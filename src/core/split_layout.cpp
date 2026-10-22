#include "core/split_layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cwchar>

namespace orbit {
namespace {

SplitNode Leaf(int64_t key, float size) {
  SplitNode n;
  n.key = key;
  n.size = size;
  return n;
}

void Layout(const SplitNode& node, const SplitRect& r, float gap, std::vector<int>& path,
            std::vector<SplitPane>* panes, std::vector<SplitHandle>* handles) {
  if (node.leaf()) {
    if (panes) panes->push_back({node.key, r});
    return;
  }
  const bool row = node.dir == SplitNode::Dir::kRow;
  const size_t n = node.children.size();
  float total = 0.f;
  for (const auto& c : node.children) total += std::max(c.size, 0.f);
  if (total <= 0.f) total = 1.f;
  const float extent = (row ? r.right - r.left : r.bottom - r.top) - gap * static_cast<float>(n - 1);
  float pos = row ? r.left : r.top;
  for (size_t i = 0; i < n; ++i) {
    const float len = extent * std::max(node.children[i].size, 0.f) / total;
    SplitRect child = r;
    if (row) {
      child.left = pos;
      child.right = i + 1 == n ? r.right : pos + len;
    } else {
      child.top = pos;
      child.bottom = i + 1 == n ? r.bottom : pos + len;
    }
    path.push_back(static_cast<int>(i));
    Layout(node.children[i], child, gap, path, panes, handles);
    path.pop_back();
    pos += len;
    if (i + 1 < n && handles) {
      SplitHandle h;
      h.path = path;
      h.index = static_cast<int>(i);
      h.vertical = row;
      h.rect = row ? SplitRect{pos, r.top, pos + gap, r.bottom}
                   : SplitRect{r.left, pos, r.right, pos + gap};
      handles->push_back(std::move(h));
    }
    pos += gap;
  }
}

void Keys(const SplitNode& node, std::vector<int64_t>* out) {
  if (node.leaf()) {
    out->push_back(node.key);
    return;
  }
  for (const auto& c : node.children) Keys(c, out);
}

// Collapses a node left with one child into that child (keeping its size).
void Hoist(SplitNode& node) {
  if (node.children.size() != 1) return;
  const float size = node.size;
  SplitNode only = std::move(node.children[0]);
  node = std::move(only);
  node.size = size;
}

bool Remove(SplitNode& node, int64_t key) {
  for (size_t i = 0; i < node.children.size(); ++i) {
    SplitNode& child = node.children[i];
    if (child.leaf()) {
      if (child.key != key) continue;
      // The space goes to the remaining children, in proportion.
      node.children.erase(node.children.begin() + static_cast<std::ptrdiff_t>(i));
      float total = 0.f;
      for (const auto& o : node.children) total += o.size;
      for (auto& o : node.children) o.size = total > 0.f ? o.size * 100.f / total : 100.f;
      Hoist(node);
      return true;
    }
    if (Remove(child, key)) {
      Hoist(child);
      return true;
    }
  }
  return false;
}

void Serialize(const SplitNode& node, std::wstring* out) {
  if (node.leaf()) {
    *out += std::to_wstring(node.key);
    return;
  }
  *out += node.dir == SplitNode::Dir::kRow ? L"r(" : L"c(";
  for (size_t i = 0; i < node.children.size(); ++i) {
    if (i) *out += L',';
    wchar_t size[32];
    std::swprintf(size, 32, L"%.2f:", static_cast<double>(node.children[i].size));
    *out += size;
    Serialize(node.children[i], out);
  }
  *out += L')';
}

bool Parse(const std::wstring& s, size_t& i, SplitNode* out, int depth) {
  if (depth > 8 || i >= s.size()) return false;
  if (s[i] == L'r' || s[i] == L'c') {
    out->dir = s[i] == L'r' ? SplitNode::Dir::kRow : SplitNode::Dir::kColumn;
    if (++i >= s.size() || s[i] != L'(') return false;
    ++i;
    while (true) {
      const wchar_t* start = s.c_str() + i;
      wchar_t* end = nullptr;
      const float size = std::wcstof(start, &end);
      const size_t used = static_cast<size_t>(end - start);
      if (used == 0 || i + used >= s.size() || s[i + used] != L':' || !(size > 0.f) ||
          !std::isfinite(size)) {
        return false;
      }
      i += used + 1;
      SplitNode child;
      if (!Parse(s, i, &child, depth + 1)) return false;
      child.size = size;
      out->children.push_back(std::move(child));
      if (i >= s.size()) return false;
      if (s[i] == L')') {
        ++i;
        return out->children.size() >= 2;
      }
      if (s[i] != L',') return false;
      ++i;
    }
  }
  const wchar_t* start = s.c_str() + i;
  wchar_t* end = nullptr;
  const long long key = std::wcstoll(start, &end, 10);
  const size_t used = static_cast<size_t>(end - start);
  if (used == 0 || key <= 0) return false;
  out->key = key;
  i += used;
  return true;
}

}  // namespace

SplitNode BuildSplitTree(const std::vector<int64_t>& keys, SplitGrid grid) {
  SplitNode root;
  const float n = static_cast<float>(std::max<size_t>(keys.size(), 1));
  if (grid == SplitGrid::kVertical || (grid == SplitGrid::kGrid && keys.size() == 2)) {
    root.dir = SplitNode::Dir::kRow;
    for (int64_t k : keys) root.children.push_back(Leaf(k, 100.f / n));
  } else if (grid == SplitGrid::kHorizontal) {
    root.dir = SplitNode::Dir::kColumn;
    for (int64_t k : keys) root.children.push_back(Leaf(k, 100.f / n));
  } else {
    root.dir = SplitNode::Dir::kRow;
    const float width = 100.f / std::ceil(n / 2.f);
    for (size_t i = 0; i + 1 < keys.size(); i += 2) {
      SplitNode column;
      column.dir = SplitNode::Dir::kColumn;
      column.size = width;
      column.children = {Leaf(keys[i], 50.f), Leaf(keys[i + 1], 50.f)};
      root.children.push_back(std::move(column));
    }
    if (keys.size() % 2) root.children.push_back(Leaf(keys.back(), width));
  }
  return root;
}

void LayoutSplit(const SplitNode& root, const SplitRect& bounds, float gap,
                 std::vector<SplitPane>* panes, std::vector<SplitHandle>* handles) {
  std::vector<int> path;
  Layout(root, bounds, gap, path, panes, handles);
}

SplitNode* SplitNodeAt(SplitNode& root, const std::vector<int>& path) {
  SplitNode* node = &root;
  for (int i : path) {
    if (i < 0 || static_cast<size_t>(i) >= node->children.size()) return nullptr;
    node = &node->children[static_cast<size_t>(i)];
  }
  return node;
}

SplitRect SplitNodeRect(const SplitNode& root, const SplitRect& bounds, float gap,
                        const std::vector<int>& path) {
  const SplitNode* node = &root;
  SplitRect r = bounds;
  for (int index : path) {
    if (node->leaf() || index < 0 || static_cast<size_t>(index) >= node->children.size()) break;
    const bool row = node->dir == SplitNode::Dir::kRow;
    const size_t n = node->children.size();
    float total = 0.f;
    for (const auto& c : node->children) total += std::max(c.size, 0.f);
    if (total <= 0.f) total = 1.f;
    const float extent =
        (row ? r.right - r.left : r.bottom - r.top) - gap * static_cast<float>(n - 1);
    float pos = row ? r.left : r.top;
    for (int i = 0; i < index; ++i) {
      pos += extent * std::max(node->children[static_cast<size_t>(i)].size, 0.f) / total + gap;
    }
    const float len = extent * std::max(node->children[static_cast<size_t>(index)].size, 0.f) / total;
    const bool last = static_cast<size_t>(index) + 1 == n;
    if (row) {
      r = {pos, r.top, last ? r.right : pos + len, r.bottom};
    } else {
      r = {r.left, pos, r.right, last ? r.bottom : pos + len};
    }
    node = &node->children[static_cast<size_t>(index)];
  }
  return r;
}

bool ResizeSplit(SplitNode& root, const std::vector<int>& path, int index,
                 const std::vector<float>& start, float delta_percent, float min_percent) {
  SplitNode* node = SplitNodeAt(root, path);
  if (!node || node->leaf() || start.size() != node->children.size() || index < 0 ||
      static_cast<size_t>(index) + 1 >= node->children.size()) {
    return false;
  }
  auto& kids = node->children;
  for (size_t i = 0; i < kids.size(); ++i) kids[i].size = start[i];
  const bool shrink_left = delta_percent < 0.f;
  float reducing = std::fabs(delta_percent);
  const int n = static_cast<int>(kids.size());
  for (int i = index + (shrink_left ? 0 : 1); 0 <= i && i < n; i += shrink_left ? -1 : 1) {
    const float current = start[static_cast<size_t>(i)];
    const float next = std::max(min_percent, current - reducing);
    kids[static_cast<size_t>(i)].size = std::min(current, next);
    reducing -= current - kids[static_cast<size_t>(i)].size;
    if (reducing <= 0.f) break;
  }
  const float increase = std::fabs(delta_percent) - std::max(reducing, 0.f);
  const size_t grow = static_cast<size_t>(index + (shrink_left ? 1 : 0));
  kids[grow].size = start[grow] + increase;
  return true;
}

bool RemoveSplitKey(SplitNode& root, int64_t key) {
  if (root.leaf()) return false;
  return Remove(root, key);
}

std::vector<int64_t> SplitKeys(const SplitNode& root) {
  std::vector<int64_t> out;
  Keys(root, &out);
  return out;
}

void ReplaceSplitKeys(SplitNode& root, const std::vector<int64_t>& from,
                      const std::vector<int64_t>& to) {
  if (root.leaf()) {
    for (size_t i = 0; i < from.size() && i < to.size(); ++i) {
      if (root.key == from[i]) {
        root.key = to[i];
        return;
      }
    }
    return;
  }
  for (auto& c : root.children) ReplaceSplitKeys(c, from, to);
}

std::wstring SerializeSplit(const SplitNode& root) {
  std::wstring out;
  Serialize(root, &out);
  return out;
}

bool ParseSplit(const std::wstring& text, SplitNode* out) {
  SplitNode node;
  size_t i = 0;
  if (!Parse(text, i, &node, 0) || i != text.size() || node.leaf()) return false;
  *out = std::move(node);
  return true;
}

}  // namespace orbit
