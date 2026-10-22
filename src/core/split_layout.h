// Split view layout tree (Zen's nsSplitNode / nsSplitLeafNode in
// ZenViewSplitter.mjs), without UI dependencies so it can be unit-tested.
//
// A node lays its children out side by side (kRow) or stacked (kColumn);
// each child's `size` is its share of the parent, in percent. Leaves carry
// an opaque key (Orbit uses the Tab pointer in memory and the tab id on disk).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace orbit {

enum class SplitGrid { kGrid, kVertical, kHorizontal };  // Ctrl+Alt+G / V / H

struct SplitNode {
  enum class Dir { kRow, kColumn };
  Dir dir = Dir::kRow;
  float size = 100.f;  // percent of the parent along the parent's axis
  int64_t key = 0;     // leaves only
  std::vector<SplitNode> children;

  bool leaf() const { return children.empty(); }
};

struct SplitRect {
  float left = 0.f, top = 0.f, right = 0.f, bottom = 0.f;
};

struct SplitPane {
  int64_t key = 0;
  SplitRect rect;
};

// The gap between children `index` and `index + 1` of the node at `path`
// (child indices from the root). `vertical` handles resize horizontally.
struct SplitHandle {
  std::vector<int> path;
  int index = 0;
  bool vertical = true;
  SplitRect rect;
};

// ZenViewSplitter.calculateLayoutTree: grid = pairs stacked in columns
// side by side (two tabs: side by side); vertical = one row; horizontal = one column.
SplitNode BuildSplitTree(const std::vector<int64_t>& keys, SplitGrid grid);

// Panes and handles inside `bounds`, `gap` apart.
void LayoutSplit(const SplitNode& root, const SplitRect& bounds, float gap,
                 std::vector<SplitPane>* panes, std::vector<SplitHandle>* handles);

// handleSplitterMouseDown: moves the handle by `delta_percent` of its
// node's extent. The children on the shrinking side give way in turn down
// to `min_percent` (zen.splitView.min-resize-width = 7); the neighbour on
// the other side grows by what they gave. Applied to `start` sizes so a drag
// can be replayed from its origin. Returns false for a bad path.
bool ResizeSplit(SplitNode& root, const std::vector<int>& path, int index,
                 const std::vector<float>& start, float delta_percent, float min_percent);
SplitNode* SplitNodeAt(SplitNode& root, const std::vector<int>& path);
// The rectangle LayoutSplit gives the node at `path` (bounds for the root).
SplitRect SplitNodeRect(const SplitNode& root, const SplitRect& bounds, float gap,
                        const std::vector<int>& path);

// Removes a leaf; nodes left with one child are replaced by it. Returns
// false when the key is not in the tree.
bool RemoveSplitKey(SplitNode& root, int64_t key);
std::vector<int64_t> SplitKeys(const SplitNode& root);  // in layout order
void ReplaceSplitKeys(SplitNode& root, const std::vector<int64_t>& from,
                      const std::vector<int64_t>& to);

// Text form for storage: "r(50:12,50:c(50:13,50:14))" (r = row, c = column).
std::wstring SerializeSplit(const SplitNode& root);
bool ParseSplit(const std::wstring& text, SplitNode* out);

}  // namespace orbit
