#include "core/bookmark_html.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "core/strings.h"

namespace orbit {
namespace {

constexpr int kMaxDepth = 32;
constexpr size_t kMaxNodes = 100000;

void WriteNodes(const std::vector<BookmarkNode>& nodes, int depth, std::string& out) {
  const std::string indent(static_cast<size_t>(depth) * 4, ' ');
  for (const auto& n : nodes) {
    if (n.is_folder) {
      out += indent + "<DT><H3>" + HtmlEscape(n.title) + "</H3>\n";
      out += indent + "<DL><p>\n";
      WriteNodes(n.children, depth + 1, out);
      out += indent + "</DL><p>\n";
    } else {
      out += indent + "<DT><A HREF=\"" + HtmlEscape(n.url) + "\">" + HtmlEscape(n.title) +
             "</A>\n";
    }
  }
}

std::string ToLower(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Decodes the handful of entities that appear in bookmark files.
std::string DecodeEntities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out.push_back(s[i]);
      continue;
    }
    const size_t semi = s.find(';', i);
    if (semi == std::string_view::npos || semi - i > 10) {
      out.push_back('&');
      continue;
    }
    const std::string ent = ToLower(s.substr(i + 1, semi - i - 1));
    std::string rep;
    if (ent == "amp") rep = "&";
    else if (ent == "lt") rep = "<";
    else if (ent == "gt") rep = ">";
    else if (ent == "quot") rep = "\"";
    else if (ent == "#39" || ent == "apos") rep = "'";
    else if (!ent.empty() && ent[0] == '#') {
      const bool hex = ent.size() > 1 && ent[1] == 'x';
      const unsigned long cp = std::strtoul(ent.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
      if (cp > 0 && cp < 0x110000 && !(cp >= 0xD800 && cp <= 0xDFFF)) {
        std::wstring w;
        if (cp >= 0x10000) {
          w.push_back(static_cast<wchar_t>(0xD800 + ((cp - 0x10000) >> 10)));
          w.push_back(static_cast<wchar_t>(0xDC00 + ((cp - 0x10000) & 0x3FF)));
        } else {
          w.push_back(static_cast<wchar_t>(cp));
        }
        rep = ToUtf8(w);
      }
    }
    if (rep.empty()) {
      out.push_back('&');
      continue;
    }
    out += rep;
    i = semi;
  }
  return out;
}

std::string ExtractAttribute(std::string_view tag, std::string_view name) {
  const std::string lower = ToLower(tag);
  size_t pos = 0;
  while ((pos = lower.find(name, pos)) != std::string::npos) {
    const bool boundary = pos > 0 && std::isspace(static_cast<unsigned char>(lower[pos - 1]));
    size_t eq = pos + name.size();
    while (eq < lower.size() && std::isspace(static_cast<unsigned char>(lower[eq]))) ++eq;
    if (!boundary || eq >= lower.size() || lower[eq] != '=') {
      pos += name.size();
      continue;
    }
    size_t v = eq + 1;
    while (v < tag.size() && std::isspace(static_cast<unsigned char>(tag[v]))) ++v;
    if (v >= tag.size()) return {};
    const char quote = tag[v];
    if (quote == '"' || quote == '\'') {
      const size_t end = tag.find(quote, v + 1);
      return std::string(tag.substr(v + 1, end == std::string_view::npos ? std::string_view::npos
                                                                         : end - v - 1));
    }
    size_t end = v;
    while (end < tag.size() && !std::isspace(static_cast<unsigned char>(tag[end])) &&
           tag[end] != '>') {
      ++end;
    }
    return std::string(tag.substr(v, end - v));
  }
  return {};
}

// Case-insensitive search for an ASCII |needle| (given in lower case).
size_t FindNoCase(std::string_view hay, size_t from, std::string_view needle) {
  for (size_t i = from; i + needle.size() <= hay.size(); ++i) {
    size_t k = 0;
    while (k < needle.size() &&
           std::tolower(static_cast<unsigned char>(hay[i + k])) == needle[k]) {
      ++k;
    }
    if (k == needle.size()) return i;
  }
  return std::string_view::npos;
}

bool IsImportableUrl(std::string_view url) {
  const std::string lower = ToLower(url.substr(0, 8));
  return lower.starts_with("http://") || lower.starts_with("https://") ||
         lower.starts_with("ftp://") || lower.starts_with("file:");
}

}  // namespace

std::string ExportBookmarksHtml(const std::vector<BookmarkNode>& roots) {
  std::string out =
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
      "<!-- This is an automatically generated file.\n"
      "     It will be read and overwritten.\n"
      "     DO NOT EDIT! -->\n"
      "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
      "<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n";
  WriteNodes(roots, 1, out);
  out += "</DL><p>\n";
  return out;
}

std::vector<BookmarkNode> ImportBookmarksHtml(std::string_view html) {
  std::vector<BookmarkNode> roots;
  // Stack of open folders; the outermost <DL> belongs to the document itself.
  std::vector<std::vector<BookmarkNode>*> stack;
  std::vector<BookmarkNode>* current = &roots;
  bool pending_folder = false;
  std::wstring pending_title;
  int dl_depth = 0;
  size_t nodes = 0;

  size_t i = 0;
  while (i < html.size() && nodes < kMaxNodes) {
    const size_t lt = html.find('<', i);
    if (lt == std::string_view::npos) break;
    const size_t gt = html.find('>', lt);
    if (gt == std::string_view::npos) break;
    const std::string_view tag = html.substr(lt + 1, gt - lt - 1);
    // Search for the end of the name from index 1 so "/dl" keeps its slash.
    const std::string name = ToLower(tag.substr(0, tag.find_first_of(" \t\r\n/>", 1)));
    i = gt + 1;

    if (name == "h3") {
      const size_t end = FindNoCase(html, i, "</h3>");
      if (end == std::string_view::npos) break;
      pending_title = FromUtf8(DecodeEntities(html.substr(i, end - i)));
      pending_folder = true;
      i = end + 5;
    } else if (name == "dl") {
      ++dl_depth;
      if (dl_depth == 1) continue;  // document-level list
      if (static_cast<int>(stack.size()) >= kMaxDepth) continue;
      BookmarkNode folder;
      folder.is_folder = true;
      folder.title = pending_folder ? pending_title : L"Imported";
      pending_folder = false;
      current->push_back(std::move(folder));
      ++nodes;
      stack.push_back(current);
      current = &current->back().children;
    } else if (name == "/dl") {
      if (dl_depth > 1 && !stack.empty()) {
        current = stack.back();
        stack.pop_back();
      }
      dl_depth = std::max(0, dl_depth - 1);
    } else if (name == "a") {
      const std::string href = DecodeEntities(ExtractAttribute(tag, "href"));
      const size_t end = FindNoCase(html, i, "</a>");
      const std::string_view text =
          end == std::string_view::npos ? std::string_view() : html.substr(i, end - i);
      if (end != std::string_view::npos) i = end + 4;
      if (!IsImportableUrl(href)) continue;
      BookmarkNode link;
      link.url = FromUtf8(href);
      link.title = FromUtf8(DecodeEntities(text));
      if (link.title.empty()) link.title = link.url;
      current->push_back(std::move(link));
      ++nodes;
    }
  }
  return roots;
}

}  // namespace orbit
