#include "core/split_layout.h"
#include "test.h"

using orbit::BuildSplitTree;
using orbit::LayoutSplit;
using orbit::ParseSplit;
using orbit::RemoveSplitKey;
using orbit::ResizeSplit;
using orbit::SerializeSplit;
using orbit::SplitGrid;
using orbit::SplitHandle;
using orbit::SplitKeys;
using orbit::SplitNode;
using orbit::SplitPane;

namespace {
bool Near(float a, float b) { return a - b < 0.01f && b - a < 0.01f; }
}  // namespace

TEST(Split_TreesFollowZenGridTypes) {
  const SplitNode two = BuildSplitTree({1, 2}, SplitGrid::kGrid);
  EXPECT_TRUE(two.dir == SplitNode::Dir::kRow && two.children.size() == 2 && two.children[0].leaf());
  const SplitNode four = BuildSplitTree({1, 2, 3, 4}, SplitGrid::kGrid);
  EXPECT_TRUE(four.children.size() == 2 && four.children[0].dir == SplitNode::Dir::kColumn &&
              four.children[0].children.size() == 2 && Near(four.children[0].size, 50.f));
  const SplitNode three = BuildSplitTree({1, 2, 3}, SplitGrid::kGrid);
  EXPECT_TRUE(three.children.size() == 2 && three.children[1].leaf() &&
              three.children[1].key == 3);
  const SplitNode h = BuildSplitTree({1, 2, 3}, SplitGrid::kHorizontal);
  EXPECT_TRUE(h.dir == SplitNode::Dir::kColumn && h.children.size() == 3 &&
              Near(h.children[0].size, 100.f / 3.f));
  EXPECT_TRUE((SplitKeys(four) == std::vector<int64_t>{1, 2, 3, 4}));
}

TEST(Split_LayoutLeavesGapsAndHandles) {
  const SplitNode root = BuildSplitTree({1, 2}, SplitGrid::kVertical);
  std::vector<SplitPane> panes;
  std::vector<SplitHandle> handles;
  LayoutSplit(root, {0, 0, 210, 100}, 10, &panes, &handles);
  EXPECT_TRUE(panes.size() == 2 && handles.size() == 1);
  if (panes.size() == 2 && handles.size() == 1) {
    EXPECT_TRUE(Near(panes[0].rect.right, 100) && Near(panes[1].rect.left, 110) &&
                Near(panes[1].rect.right, 210));
    EXPECT_TRUE(handles[0].vertical && Near(handles[0].rect.left, 100) &&
                Near(handles[0].rect.right, 110) && handles[0].path.empty());
  }
  const SplitNode grid = BuildSplitTree({1, 2, 3, 4}, SplitGrid::kGrid);
  panes.clear();
  handles.clear();
  LayoutSplit(grid, {0, 0, 100, 100}, 0, &panes, &handles);
  // One vertical handle between the columns, one horizontal in each column.
  EXPECT_TRUE(panes.size() == 4 && handles.size() == 3);
  int horizontal = 0;
  for (const auto& h : handles) horizontal += !h.vertical;
  EXPECT_TRUE(horizontal == 2);
}

TEST(Split_NodeRectMatchesLayout) {
  const SplitNode grid = BuildSplitTree({1, 2, 3, 4}, SplitGrid::kGrid);
  std::vector<SplitPane> panes;
  LayoutSplit(grid, {0, 0, 210, 210}, 10, &panes, nullptr);
  const orbit::SplitRect col = orbit::SplitNodeRect(grid, {0, 0, 210, 210}, 10, {1});
  const orbit::SplitRect leaf = orbit::SplitNodeRect(grid, {0, 0, 210, 210}, 10, {1, 1});
  EXPECT_TRUE(Near(col.left, 110) && Near(col.right, 210) && Near(col.top, 0));
  EXPECT_TRUE(panes.size() == 4 && Near(leaf.top, panes[3].rect.top) &&
              Near(leaf.left, panes[3].rect.left));
}

TEST(Split_ResizeCascadesWithMinimum) {
  SplitNode root = BuildSplitTree({1, 2, 3}, SplitGrid::kVertical);
  const std::vector<float> start = {40.f, 30.f, 30.f};
  // Dragging the second handle 50% right: child 2 gives 23 (down to 7),
  // nothing else lies to its right, so child 1 only grows by 23.
  EXPECT_TRUE(ResizeSplit(root, {}, 1, start, 50.f, 7.f));
  EXPECT_TRUE(Near(root.children[1].size, 53.f) && Near(root.children[2].size, 7.f) &&
              Near(root.children[0].size, 40.f));
  // Dragging it 45% left (from the start sizes): child 1 gives 23 (down to
  // 7), child 0 gives the other 22, and child 2 grows by all 45.
  EXPECT_TRUE(ResizeSplit(root, {}, 1, start, -45.f, 7.f));
  EXPECT_TRUE(Near(root.children[1].size, 7.f) && Near(root.children[0].size, 18.f) &&
              Near(root.children[2].size, 75.f));
  EXPECT_TRUE(!ResizeSplit(root, {}, 2, start, 5.f, 7.f));  // no handle after the last child
  EXPECT_TRUE(!ResizeSplit(root, {0}, 0, start, 5.f, 7.f));  // a leaf has no handles
}

TEST(Split_RemoveCollapsesAndRedistributes) {
  SplitNode grid = BuildSplitTree({1, 2, 3, 4}, SplitGrid::kGrid);
  EXPECT_TRUE(RemoveSplitKey(grid, 2));
  // The first column is left with one tab: it becomes a leaf of the row.
  EXPECT_TRUE(grid.children.size() == 2 && grid.children[0].leaf() && grid.children[0].key == 1 &&
              Near(grid.children[0].size, 50.f));
  EXPECT_TRUE(!RemoveSplitKey(grid, 99));
  SplitNode three = BuildSplitTree({1, 2, 3}, SplitGrid::kVertical);
  EXPECT_TRUE(RemoveSplitKey(three, 3));
  EXPECT_TRUE(three.children.size() == 2 && Near(three.children[0].size, 50.f));
  EXPECT_TRUE(RemoveSplitKey(three, 1));
  EXPECT_TRUE(three.leaf() && three.key == 2);  // a lone tab: no split any more
}

TEST(Split_SerializeRoundTripsAndRejectsGarbage) {
  SplitNode grid = BuildSplitTree({12, 13, 14}, SplitGrid::kGrid);
  const std::wstring text = SerializeSplit(grid);
  EXPECT_WEQ(text, L"r(50.00:c(50.00:12,50.00:13),50.00:14)");
  SplitNode parsed;
  EXPECT_TRUE(ParseSplit(text, &parsed));
  EXPECT_TRUE(SerializeSplit(parsed) == text);
  for (const wchar_t* bad : {L"", L"12", L"r(", L"r(50:1)", L"r(50:1,50:)", L"x(50:1,50:2)",
                             L"r(50:1,50:2)x", L"r(-5:1,50:2)", L"r(50:0,50:2)"}) {
    EXPECT_TRUE(!ParseSplit(bad, &parsed));
  }
}
