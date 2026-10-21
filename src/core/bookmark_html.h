#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace orbit {

// Netscape bookmark file format (exported/imported by Chrome, Edge, Firefox).
struct BookmarkNode {
  std::wstring title;
  std::wstring url;  // empty for folders
  bool is_folder = false;
  std::vector<BookmarkNode> children;
};

// Serializes |roots| as a Netscape bookmark document (UTF-8).
std::string ExportBookmarksHtml(const std::vector<BookmarkNode>& roots);

// Parses a Netscape bookmark document. Tolerant of the variations produced by
// common browsers; only http(s), ftp and file URLs are kept (javascript:,
// data: etc. are dropped). Folder nesting is capped to defend against
// pathological input.
std::vector<BookmarkNode> ImportBookmarksHtml(std::string_view utf8);

}  // namespace orbit
