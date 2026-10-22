#include "core/live_folders.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "core/strings.h"

namespace orbit {
namespace {

// ---------------------------------------------------------------------------
// A small, tolerant XML reader: elements (local names), attributes and text.
// Text chunks are kept in document order, so an element's textContent is the
// contiguous run of chunks between its start and end.

struct XmlNode {
  std::string name;  // local name (prefix dropped)
  std::vector<std::pair<std::string, std::string>> attrs;
  size_t parent = 0;
  size_t chunk_begin = 0, chunk_end = 0;  // text chunks inside it
  size_t node_end = 0;                    // one past its last descendant
};

constexpr size_t kMaxNodes = 200000;
constexpr size_t kMaxDepth = 256;

void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// &amp; &lt; &gt; &quot; &apos; &nbsp; &#N; &#xN; (unknown ones stay as written).
std::string DecodeEntities(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '&') {
      out += s[i];
      continue;
    }
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12) {
      out += '&';
      continue;
    }
    const std::string_view name = s.substr(i + 1, semi - i - 1);
    uint32_t cp = 0;
    bool ok = true;
    if (name == "amp") cp = '&';
    else if (name == "lt") cp = '<';
    else if (name == "gt") cp = '>';
    else if (name == "quot") cp = '"';
    else if (name == "apos") cp = '\'';
    else if (name == "nbsp") cp = 0xA0;
    else if (name.size() > 1 && name[0] == '#') {
      const bool hex = name[1] == 'x' || name[1] == 'X';
      const std::string_view digits = name.substr(hex ? 2 : 1);
      ok = !digits.empty();
      for (char c : digits) {
        int v = -1;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
        if (v < 0 || cp > 0x10FFFF) {
          ok = false;
          break;
        }
        cp = cp * (hex ? 16u : 10u) + static_cast<uint32_t>(v);
      }
    } else {
      ok = false;
    }
    if (!ok) {
      out += '&';
      continue;
    }
    AppendUtf8(out, cp);
    i = semi;
  }
  return out;
}

std::string LocalName(std::string_view qname) {
  const size_t colon = qname.rfind(':');
  return std::string(colon == std::string_view::npos ? qname : qname.substr(colon + 1));
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

class Xml {
 public:
  explicit Xml(std::string_view s) { Parse(s); }

  size_t root() const { return root_; }  // 0: none
  const XmlNode& node(size_t i) const { return nodes_[i]; }

  std::string Text(size_t n) const {
    std::string out;
    for (size_t c = nodes_[n].chunk_begin; c < nodes_[n].chunk_end; ++c) out += chunks_[c];
    return out;
  }
  std::string Attr(size_t n, std::string_view name) const {
    for (const auto& [k, v] : nodes_[n].attrs) {
      if (LocalName(k) == name) return v;
    }
    return {};
  }
  bool HasAttr(size_t n, std::string_view name) const {
    for (const auto& [k, v] : nodes_[n].attrs) {
      if (LocalName(k) == name) return true;
    }
    return false;
  }
  // Descendants named |name|, in document order (querySelectorAll).
  std::vector<size_t> All(size_t n, std::string_view name) const {
    std::vector<size_t> out;
    for (size_t i = n + 1; i < nodes_[n].node_end; ++i) {
      if (nodes_[i].name == name) out.push_back(i);
    }
    return out;
  }
  size_t First(size_t n, std::string_view name) const {  // querySelector; 0: none
    for (size_t i = n + 1; i < nodes_[n].node_end; ++i) {
      if (nodes_[i].name == name) return i;
    }
    return 0;
  }
  size_t Child(size_t n, std::string_view name) const {  // "n > name"
    for (size_t i = n + 1; i < nodes_[n].node_end; ++i) {
      if (nodes_[i].parent == n && nodes_[i].name == name) return i;
    }
    return 0;
  }

 private:
  void Close(size_t n) {
    nodes_[n].chunk_end = chunks_.size();
    nodes_[n].node_end = nodes_.size();
  }

  void Parse(std::string_view s) {
    nodes_.push_back({});  // 0: the document
    std::vector<size_t> stack{0};
    size_t i = 0;
    if (s.substr(0, 3) == "\xEF\xBB\xBF") i = 3;
    while (i < s.size() && nodes_.size() < kMaxNodes) {
      if (s[i] != '<') {
        const size_t lt = s.find('<', i);
        const size_t end = lt == std::string_view::npos ? s.size() : lt;
        if (stack.size() > 1) chunks_.push_back(DecodeEntities(s.substr(i, end - i)));
        i = end;
        continue;
      }
      const std::string_view rest = s.substr(i);
      auto skip_to = [&](std::string_view terminator) {
        const size_t e = s.find(terminator, i);
        i = e == std::string_view::npos ? s.size() : e + terminator.size();
      };
      if (rest.substr(0, 4) == "<!--") {
        skip_to("-->");
      } else if (rest.substr(0, 9) == "<![CDATA[") {
        const size_t e = s.find("]]>", i + 9);
        const size_t end = e == std::string_view::npos ? s.size() : e;
        if (stack.size() > 1) chunks_.emplace_back(s.substr(i + 9, end - i - 9));
        i = e == std::string_view::npos ? s.size() : e + 3;
      } else if (rest.substr(0, 2) == "<?") {
        skip_to("?>");
      } else if (rest.substr(0, 2) == "<!") {
        const size_t gt = s.find('>', i);
        const size_t bracket = s.find('[', i);
        if (bracket != std::string_view::npos && gt != std::string_view::npos && bracket < gt) {
          skip_to("]>");
        } else {
          skip_to(">");
        }
      } else if (rest.substr(0, 2) == "</") {
        const size_t gt = s.find('>', i);
        if (gt == std::string_view::npos) break;
        std::string_view qname = s.substr(i + 2, gt - i - 2);
        while (!qname.empty() && IsSpace(qname.back())) qname.remove_suffix(1);
        const std::string name = LocalName(qname);
        for (size_t d = stack.size(); d-- > 1;) {
          if (nodes_[stack[d]].name == name) {
            while (stack.size() > d) {
              Close(stack.back());
              stack.pop_back();
            }
            break;
          }
        }
        i = gt + 1;
      } else {
        i = ParseStartTag(s, i, stack);
      }
    }
    while (!stack.empty()) {
      Close(stack.back());
      stack.pop_back();
    }
    for (size_t n = 1; n < nodes_.size(); ++n) {
      if (nodes_[n].parent == 0) {
        root_ = n;
        break;
      }
    }
  }

  size_t ParseStartTag(std::string_view s, size_t i, std::vector<size_t>& stack) {
    size_t p = i + 1;
    const size_t name_begin = p;
    while (p < s.size() && !IsSpace(s[p]) && s[p] != '>' && s[p] != '/') ++p;
    if (p == name_begin) return i + 1;  // a stray '<'
    XmlNode n;
    n.name = LocalName(s.substr(name_begin, p - name_begin));
    bool self_closing = false;
    while (p < s.size()) {
      while (p < s.size() && IsSpace(s[p])) ++p;
      if (p >= s.size()) break;
      if (s[p] == '>') {
        ++p;
        break;
      }
      if (s[p] == '/') {
        self_closing = true;
        ++p;
        continue;
      }
      const size_t k = p;
      while (p < s.size() && !IsSpace(s[p]) && s[p] != '=' && s[p] != '>' && s[p] != '/') ++p;
      std::string key(s.substr(k, p - k));
      while (p < s.size() && IsSpace(s[p])) ++p;
      std::string value;
      if (p < s.size() && s[p] == '=') {
        ++p;
        while (p < s.size() && IsSpace(s[p])) ++p;
        if (p < s.size() && (s[p] == '"' || s[p] == '\'')) {
          const char q = s[p];
          const size_t e = s.find(q, p + 1);
          const size_t end = e == std::string_view::npos ? s.size() : e;
          value = DecodeEntities(s.substr(p + 1, end - p - 1));
          p = e == std::string_view::npos ? s.size() : e + 1;
        } else {
          const size_t v = p;
          while (p < s.size() && !IsSpace(s[p]) && s[p] != '>') ++p;
          value = DecodeEntities(s.substr(v, p - v));
        }
      }
      if (!key.empty()) n.attrs.emplace_back(std::move(key), std::move(value));
    }
    if (stack.size() >= kMaxDepth) return p;
    n.parent = stack.back();
    n.chunk_begin = chunks_.size();
    nodes_.push_back(std::move(n));
    const size_t id = nodes_.size() - 1;
    if (self_closing) {
      Close(id);
    } else {
      stack.push_back(id);
    }
    return p;
  }

  std::vector<XmlNode> nodes_;
  std::vector<std::string> chunks_;
  size_t root_ = 0;
};

std::wstring Trimmed(const std::string& utf8) {
  return std::wstring(TrimWhitespace(FromUtf8(utf8)));
}

bool IsHttpUrl(const std::wstring& url) {
  return StartsWithNoCase(url, L"http://") || StartsWithNoCase(url, L"https://");
}

// ---------------------------------------------------------------------------
// Dates

int64_t DaysFromCivil(int64_t y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

class Cursor {
 public:
  explicit Cursor(std::wstring_view s) : s_(s) {}
  bool done() const { return i_ >= s_.size(); }
  wchar_t peek() const { return done() ? 0 : s_[i_]; }
  void skip_spaces() {
    while (!done() && (std::iswspace(s_[i_]) || s_[i_] == L',')) ++i_;
  }
  bool eat(wchar_t c) {
    if (peek() != c) return false;
    ++i_;
    return true;
  }
  bool number(int& out, int max_digits) {
    int n = 0, v = 0;
    while (!done() && n < max_digits && s_[i_] >= L'0' && s_[i_] <= L'9') {
      v = v * 10 + (s_[i_] - L'0');
      ++i_;
      ++n;
    }
    out = v;
    return n > 0;
  }
  std::wstring word() {
    std::wstring w;
    while (!done() && std::iswalpha(s_[i_])) w += static_cast<wchar_t>(std::towlower(s_[i_++]));
    return w;
  }
  size_t pos() const { return i_; }

 private:
  std::wstring_view s_;
  size_t i_ = 0;
};

bool ValidClock(int y, int mo, int d, int h, int mi, int sec) {
  return y >= 1970 && y <= 9999 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 &&
         h <= 24 && mi >= 0 && mi <= 59 && sec >= 0 && sec <= 60;
}

int64_t ToEpochMs(int y, int mo, int d, int h, int mi, int sec, int64_t offset_minutes) {
  const int64_t days = DaysFromCivil(y, mo, d);
  const int64_t seconds = days * 86400 + h * 3600 + mi * 60 + sec - offset_minutes * 60;
  return seconds * 1000;
}

// "+0200", "-05:00", "GMT", "EST", "Z"; false when it is none of these.
bool ParseZone(Cursor& c, int64_t& offset) {
  offset = 0;
  if (c.eat(L'+') || c.peek() == L'-') {
    const bool negative = c.eat(L'-');
    int hh = 0, mm = 0;
    if (!c.number(hh, 2)) return false;
    c.eat(L':');
    c.number(mm, 2);
    offset = (hh * 60 + mm) * (negative ? -1 : 1);
    return true;
  }
  const std::wstring z = c.word();
  static const struct { const wchar_t* name; int hours; } kZones[] = {
      {L"gmt", 0},  {L"ut", 0},   {L"utc", 0},  {L"z", 0},    {L"est", -5}, {L"edt", -4},
      {L"cst", -6}, {L"cdt", -5}, {L"mst", -7}, {L"mdt", -6}, {L"pst", -8}, {L"pdt", -7}};
  for (const auto& zone : kZones) {
    if (z == zone.name) {
      offset = zone.hours * 60;
      return true;
    }
  }
  return z.empty();
}

int64_t ParseRfc822(std::wstring_view s) {
  static const wchar_t* kMonths[] = {L"jan", L"feb", L"mar", L"apr", L"may", L"jun",
                                     L"jul", L"aug", L"sep", L"oct", L"nov", L"dec"};
  Cursor c(s);
  c.skip_spaces();
  if (std::iswalpha(c.peek())) c.word();  // weekday
  c.skip_spaces();
  int day = 0;
  if (!c.number(day, 2)) return 0;
  c.skip_spaces();
  const std::wstring month_name = c.word();
  int month = 0;
  for (int m = 0; m < 12; ++m) {
    if (month_name.substr(0, 3) == kMonths[m]) month = m + 1;
  }
  if (!month) return 0;
  c.skip_spaces();
  const size_t year_at = c.pos();
  int year = 0;
  if (!c.number(year, 4)) return 0;
  if (c.pos() - year_at == 2) year += year < 50 ? 2000 : 1900;
  c.skip_spaces();
  int h = 0, mi = 0, sec = 0;
  if (c.number(h, 2)) {
    if (!c.eat(L':') || !c.number(mi, 2)) return 0;
    if (c.eat(L':')) c.number(sec, 2);
  }
  c.skip_spaces();
  int64_t offset = 0;
  if (!c.done() && !ParseZone(c, offset)) return 0;
  if (!ValidClock(year, month, day, h, mi, sec)) return 0;
  return ToEpochMs(year, month, day, h, mi, sec, offset);
}

int64_t ParseIso8601(std::wstring_view s) {
  Cursor c(s);
  c.skip_spaces();
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
  if (!c.number(y, 4) || !c.eat(L'-') || !c.number(mo, 2) || !c.eat(L'-') || !c.number(d, 2)) return 0;
  int64_t offset = 0;
  if (c.eat(L'T') || c.eat(L't') || c.eat(L' ')) {
    if (!c.number(h, 2) || !c.eat(L':') || !c.number(mi, 2)) return 0;
    if (c.eat(L':')) c.number(sec, 2);
    if (c.eat(L'.')) {
      int frac = 0;
      c.number(frac, 9);
    }
    if (!c.done() && !(c.eat(L'Z') || c.eat(L'z')) && !ParseZone(c, offset)) return 0;
  }
  if (!ValidClock(y, mo, d, h, mi, sec)) return 0;
  return ToEpochMs(y, mo, d, h, mi, sec, offset);
}

// ---------------------------------------------------------------------------
// State text: "key=value" lines; "\\" and "\n" escaped.

std::wstring Escape(const std::wstring& v) {
  std::wstring out;
  for (wchar_t ch : v) {
    if (ch == L'\\') out += L"\\\\";
    else if (ch == L'\n') out += L"\\n";
    else if (ch == L'\r') out += L"\\r";
    else out += ch;
  }
  return out;
}

std::wstring Unescape(std::wstring_view v) {
  std::wstring out;
  for (size_t i = 0; i < v.size(); ++i) {
    if (v[i] == L'\\' && i + 1 < v.size()) {
      ++i;
      out += v[i] == L'n' ? L'\n' : v[i] == L'r' ? L'\r' : v[i];
    } else {
      out += v[i];
    }
  }
  return out;
}

std::wstring JoinSet(const std::set<std::wstring>& set) {
  std::wstring out;
  for (const auto& s : set) {
    if (!out.empty()) out += L',';
    out += s;
  }
  return out;
}

std::set<std::wstring> SplitSet(std::wstring_view v) {
  std::set<std::wstring> out;
  size_t start = 0;
  while (start <= v.size()) {
    const size_t comma = v.find(L',', start);
    const size_t end = comma == std::wstring_view::npos ? v.size() : comma;
    std::wstring item(TrimWhitespace(v.substr(start, end - start)));
    if (!item.empty()) out.insert(std::move(item));
    if (comma == std::wstring_view::npos) break;
    start = comma + 1;
  }
  return out;
}

int64_t ToInt64(std::wstring_view v) {
  int64_t n = 0;
  bool negative = false;
  size_t i = 0;
  if (!v.empty() && v[0] == L'-') {
    negative = true;
    i = 1;
  }
  for (; i < v.size() && v[i] >= L'0' && v[i] <= L'9' && n < (INT64_MAX / 10 - 10); ++i) {
    n = n * 10 + (v[i] - L'0');
  }
  return negative ? -n : n;
}

// ---------------------------------------------------------------------------
// GitHub pages

std::wstring Origin(const std::wstring& url) {
  const size_t scheme = url.find(L"://");
  if (scheme == std::wstring::npos) return L"https://github.com";
  const size_t slash = url.find(L'/', scheme + 3);
  return url.substr(0, slash == std::wstring::npos ? url.size() : slash);
}

std::wstring Resolve(const std::wstring& href, const std::wstring& base) {
  if (IsHttpUrl(href)) return href;
  if (!href.empty() && href[0] == L'/') return Origin(base) + href;
  return {};
}

// "/owner/repo/(issues|pull)/123..." -> repo "owner/repo", number "123".
bool MatchIssuePath(const std::wstring& url, std::wstring& repo, std::wstring& number) {
  const size_t scheme = url.find(L"://");
  const size_t path_at = scheme == std::wstring::npos ? 0 : url.find(L'/', scheme + 3);
  if (path_at == std::wstring::npos) return false;
  std::vector<std::wstring> parts;
  size_t i = path_at + 1;
  while (i <= url.size() && parts.size() < 4) {
    size_t e = url.find_first_of(L"/?#", i);
    if (e == std::wstring::npos) e = url.size();
    parts.push_back(url.substr(i, e - i));
    if (e >= url.size() || url[e] != L'/') break;
    i = e + 1;
  }
  if (parts.size() < 4 || parts[0].empty() || parts[1].empty()) return false;
  if (parts[2] != L"issues" && parts[2] != L"pull") return false;
  if (parts[3].empty() || !std::all_of(parts[3].begin(), parts[3].end(), [](wchar_t c) {
        return c >= L'0' && c <= L'9';
      })) {
    return false;
  }
  repo = parts[0] + L"/" + parts[1];
  number = parts[3];
  return true;
}

// The text of an HTML fragment: tags dropped, entities decoded, trimmed.
std::wstring HtmlText(std::string_view html) {
  std::string text;
  bool in_tag = false;
  for (char c : html) {
    if (c == '<') in_tag = true;
    else if (c == '>') in_tag = false;
    else if (!in_tag) text += c;
  }
  std::wstring out = Trimmed(DecodeEntities(text));
  // Collapse runs of whitespace (titles are laid out on one line).
  std::wstring collapsed;
  for (wchar_t ch : out) {
    if (std::iswspace(ch)) {
      if (!collapsed.empty() && collapsed.back() != L' ') collapsed += L' ';
    } else {
      collapsed += ch;
    }
  }
  return collapsed;
}

// The last non-empty text run inside an element (its lastChild text).
std::wstring LastTextRun(std::string_view html) {
  std::wstring last;
  size_t i = 0;
  while (i < html.size()) {
    const size_t lt = html.find('<', i);
    const size_t end = lt == std::string_view::npos ? html.size() : lt;
    std::wstring run = Trimmed(DecodeEntities(html.substr(i, end - i)));
    if (!run.empty()) last = run;
    if (lt == std::string_view::npos) break;
    const size_t gt = html.find('>', lt);
    if (gt == std::string_view::npos) break;
    i = gt + 1;
  }
  return last;
}

struct Anchor {
  size_t tag_begin = 0, content_begin = 0, content_end = 0, end = 0;
  std::string tag;  // "<a ...>"
};

// The <a> element whose start tag contains |at|.
std::optional<Anchor> AnchorAround(std::string_view html, size_t at) {
  size_t lt = html.rfind('<', at);
  if (lt == std::string_view::npos || lt + 2 >= html.size()) return std::nullopt;
  if ((html[lt + 1] != 'a' && html[lt + 1] != 'A') || !IsSpace(html[lt + 2])) return std::nullopt;
  const size_t gt = html.find('>', at);
  if (gt == std::string_view::npos) return std::nullopt;
  const size_t close = html.find("</a>", gt);
  if (close == std::string_view::npos) return std::nullopt;
  Anchor a;
  a.tag_begin = lt;
  a.content_begin = gt + 1;
  a.content_end = close;
  a.end = close + 4;
  a.tag = std::string(html.substr(lt, gt + 1 - lt));
  return a;
}

std::string TagAttr(const std::string& tag, std::string_view name) {
  size_t p = 0;
  while ((p = tag.find(name, p)) != std::string::npos) {
    const bool boundary = p > 0 && IsSpace(tag[p - 1]);
    size_t q = p + name.size();
    while (q < tag.size() && IsSpace(tag[q])) ++q;
    if (boundary && q < tag.size() && tag[q] == '=') {
      ++q;
      while (q < tag.size() && IsSpace(tag[q])) ++q;
      if (q < tag.size() && (tag[q] == '"' || tag[q] == '\'')) {
        const size_t e = tag.find(tag[q], q + 1);
        if (e != std::string::npos) return DecodeEntities(std::string_view(tag).substr(q + 1, e - q - 1));
      }
      return {};
    }
    p += name.size();
  }
  return {};
}

void AddGithubItem(GithubPage& page, const std::wstring& url, std::wstring title,
                   std::wstring author) {
  std::wstring repo, number;
  if (!MatchIssuePath(url, repo, number)) return;
  page.repos.insert(repo);
  LiveItem item;
  item.id = repo + L"#" + number;
  item.title = std::move(title);
  item.subtitle = std::move(author);
  item.url = url;
  page.items.push_back(std::move(item));
}

}  // namespace

// ---------------------------------------------------------------------------

std::optional<Feed> ParseFeed(std::string_view utf8) {
  const Xml xml(utf8);
  const size_t root = xml.root();
  if (!root) return std::nullopt;
  const std::string& kind = xml.node(root).name;
  if (kind != "rss" && kind != "feed" && kind != "RDF") return std::nullopt;
  Feed feed;
  // Zen: isAtom = a "feed > entry" exists (the metadata uses "feed" alone).
  const bool atom_meta = kind == "feed";
  bool atom = false;
  for (size_t e : xml.All(0, "entry")) atom |= xml.node(xml.node(e).parent).name == "feed";
  if (atom_meta) {
    if (size_t t = xml.Child(root, "title")) feed.title = Trimmed(xml.Text(t));
    for (size_t l : xml.All(root, "link")) {
      if (xml.node(l).parent != root || !xml.HasAttr(l, "href")) continue;
      const std::string rel = xml.Attr(l, "rel");
      if (rel.empty() || rel == "alternate") {
        feed.link = Trimmed(xml.Attr(l, "href"));
        if (rel == "alternate") break;
      }
    }
  } else {
    const size_t channel = xml.First(0, "channel");
    if (channel) {
      if (size_t t = xml.Child(channel, "title")) feed.title = Trimmed(xml.Text(t));
      for (size_t l : xml.All(channel, "link")) {
        if (xml.node(l).parent != channel) continue;
        const std::wstring text = Trimmed(xml.Text(l));
        if (!text.empty()) {
          feed.link = text;
          break;
        }
      }
    }
  }
  for (size_t e : xml.All(0, atom ? "entry" : "item")) {
    LiveItem item;
    if (size_t t = xml.First(e, "title")) item.title = Trimmed(xml.Text(t));
    if (size_t l = xml.First(e, "link")) {
      item.url = Trimmed(atom ? xml.Attr(l, "href") : xml.Text(l));
    }
    const size_t guid = xml.First(e, atom ? "id" : "guid");
    item.id = guid ? Trimmed(xml.Text(guid)) : std::wstring();
    if (item.id.empty()) item.id = item.url;
    // Zen reads "updated" / "pubDate"; Orbit also accepts Atom "published"
    // and RDF's dc:date, which would otherwise drop every item.
    size_t date = xml.First(e, atom ? "updated" : "pubDate");
    if (!date) date = xml.First(e, atom ? "published" : "date");
    if (date) {
      const std::wstring text = Trimmed(xml.Text(date));
      item.has_date = !text.empty();
      item.date_ms = ParseFeedDate(text);
    }
    feed.items.push_back(std::move(item));
  }
  return feed;
}

int64_t ParseFeedDate(std::wstring_view s) {
  s = TrimWhitespace(s);
  if (s.empty()) return 0;
  if (s.size() >= 10 && s[4] == L'-' && s[7] == L'-') return ParseIso8601(s);
  return ParseRfc822(s);
}

std::vector<LiveItem> FilterFeedItems(const std::vector<LiveItem>& items, int64_t now_ms,
                                      int64_t time_range_ms, int max_items) {
  std::vector<LiveItem> out;
  const int64_t cutoff = now_ms - time_range_ms;
  for (const LiveItem& item : items) {
    if (static_cast<int>(out.size()) >= max_items) break;
    if (item.url.empty() || !item.has_date || !IsHttpUrl(item.url)) continue;
    if (time_range_ms && (!item.date_ms || item.date_ms < cutoff)) continue;
    out.push_back(item);
  }
  return out;
}

const wchar_t* LiveKindName(LiveFolderState::Kind kind) {
  switch (kind) {
    case LiveFolderState::Kind::kGithubPulls: return L"github:pull-requests";
    case LiveFolderState::Kind::kGithubIssues: return L"github:issues";
    case LiveFolderState::Kind::kRss: break;
  }
  return L"rss";
}

std::optional<LiveFolderState::Kind> LiveKindFromName(std::wstring_view name) {
  if (name == L"rss") return LiveFolderState::Kind::kRss;
  if (name == L"github:pull-requests") return LiveFolderState::Kind::kGithubPulls;
  if (name == L"github:issues") return LiveFolderState::Kind::kGithubIssues;
  return std::nullopt;
}

std::wstring DefaultGithubUrl(LiveFolderState::Kind kind) {
  return kind == LiveFolderState::Kind::kGithubPulls ? L"https://github.com/pulls"
                                                     : L"https://github.com/issues/assigned";
}

std::wstring SerializeLiveState(const LiveFolderState& s) {
  std::wstring out;
  auto put = [&](const wchar_t* key, const std::wstring& value) {
    out += key;
    out += L'=';
    out += Escape(value);
    out += L'\n';
  };
  put(L"kind", LiveKindName(s.kind));
  put(L"url", s.url);
  put(L"interval", std::to_wstring(s.interval_ms));
  put(L"max_items", std::to_wstring(s.max_items));
  put(L"time_range", std::to_wstring(s.time_range_ms));
  put(L"author_me", s.author_me ? L"1" : L"0");
  put(L"assigned_me", s.assigned_me ? L"1" : L"0");
  put(L"review_requested", s.review_requested ? L"1" : L"0");
  put(L"include_drafts", s.include_drafts ? L"1" : L"0");
  put(L"json_api", std::to_wstring(s.json_api));
  put(L"repos", JoinSet(s.repos));
  put(L"repo_excludes", JoinSet(s.repo_excludes));
  put(L"last_fetched", std::to_wstring(s.last_fetched));
  put(L"last_error", s.last_error);
  return out;
}

std::optional<LiveFolderState> ParseLiveState(const std::wstring& text) {
  LiveFolderState s;
  bool have_kind = false;
  size_t start = 0;
  while (start < text.size()) {
    size_t nl = text.find(L'\n', start);
    if (nl == std::wstring::npos) nl = text.size();
    const std::wstring_view line(text.data() + start, nl - start);
    start = nl + 1;
    const size_t eq = line.find(L'=');
    if (eq == std::wstring_view::npos) continue;
    const std::wstring_view key = line.substr(0, eq);
    const std::wstring value = Unescape(line.substr(eq + 1));
    if (key == L"kind") {
      const auto kind = LiveKindFromName(value);
      if (!kind) return std::nullopt;
      s.kind = *kind;
      have_kind = true;
    } else if (key == L"url") {
      s.url = value;
    } else if (key == L"interval") {
      s.interval_ms = std::clamp<int64_t>(ToInt64(value), 60 * 1000, 24LL * 3600 * 1000);
    } else if (key == L"max_items") {
      s.max_items = static_cast<int>(std::clamp<int64_t>(ToInt64(value), 1, 200));
    } else if (key == L"time_range") {
      s.time_range_ms = std::max<int64_t>(0, ToInt64(value));
    } else if (key == L"author_me") {
      s.author_me = value == L"1";
    } else if (key == L"assigned_me") {
      s.assigned_me = value == L"1";
    } else if (key == L"review_requested") {
      s.review_requested = value == L"1";
    } else if (key == L"include_drafts") {
      s.include_drafts = value == L"1";
    } else if (key == L"json_api") {
      s.json_api = static_cast<int>(std::clamp<int64_t>(ToInt64(value), -1, 1));
    } else if (key == L"repos") {
      s.repos = SplitSet(value);
    } else if (key == L"repo_excludes") {
      s.repo_excludes = SplitSet(value);
    } else if (key == L"last_fetched") {
      s.last_fetched = std::max<int64_t>(0, ToInt64(value));
    } else if (key == L"last_error") {
      s.last_error = value;
    }
  }
  if (!have_kind) return std::nullopt;
  if (s.kind != LiveFolderState::Kind::kRss && s.url.empty()) s.url = DefaultGithubUrl(s.kind);
  return s;
}

std::wstring LiveErrorLabel(const std::wstring& id) {
  if (id == kLiveFailedFetch) return L"Failed to update";
  if (id == kLiveGithubNoAuth) return L"Not signed in to GitHub";
  if (id == kLiveGithubNoFilter) return L"Filter is not set";
  return {};
}

std::wstring LiveErrorTooltip(const std::wstring& id) {
  if (id == kLiveFailedFetch) return L"Failed to update. Try again.";
  if (id == kLiveGithubNoAuth) return L"Sign back in to GitHub.";
  if (id == kLiveGithubNoFilter) return L"No filter set, nothing will be fetched.";
  return {};
}

std::vector<std::wstring> GithubQueries(const LiveFolderState& s) {
  const bool pulls = s.kind == LiveFolderState::Kind::kGithubPulls;
  std::wstring base = pulls ? L"is:pr" : L"is:issue";
  base += L" is:open archived:false sort:updated-desc";
  if (pulls && !s.include_drafts) base += L" draft:false";
  for (const auto& repo : s.repo_excludes) {
    const std::wstring_view r = TrimWhitespace(repo);
    if (!r.empty()) base += L" -repo:" + std::wstring(r);
  }
  std::vector<std::wstring> filters;
  if (s.author_me) filters.push_back(L"author:@me");
  if (s.assigned_me) filters.push_back(L"assignee:@me");
  if (s.review_requested && pulls) filters.push_back(L"review-requested:@me");
  if (filters.empty()) return {};
  std::vector<std::wstring> out;
  if (pulls && s.json_api != 1) {
    for (const auto& f : filters) out.push_back(base + L" " + f);
    return out;
  }
  std::wstring any;
  for (const auto& f : filters) any += (any.empty() ? L"" : L" OR ") + f;
  out.push_back(base + L" (" + any + L")");
  return out;
}

GithubPage ParseGithubIssuesHtml(std::string_view html, const std::wstring& base_url) {
  GithubPage page;
  constexpr std::string_view kTitle = "data-testid=\"issue-listitem-title-link\"";
  constexpr std::string_view kAuthor = "data-testid=\"author-filter-link\"";
  size_t at = 0;
  while ((at = html.find(kTitle, at)) != std::string_view::npos) {
    const auto a = AnchorAround(html, at);
    at += kTitle.size();
    if (!a) continue;
    const std::wstring url = Resolve(FromUtf8(TagAttr(a->tag, "href")), base_url);
    const std::wstring title = HtmlText(html.substr(a->content_begin, a->content_end - a->content_begin));
    // The author link of the same row: before the next title link.
    const size_t next = html.find(kTitle, a->end);
    std::wstring author;
    const size_t au = html.find(kAuthor, a->end);
    if (au != std::string_view::npos && (next == std::string_view::npos || au < next)) {
      const size_t gt = html.find('>', au);
      const size_t close = gt == std::string_view::npos ? gt : html.find("</a>", gt);
      if (close != std::string_view::npos) author = LastTextRun(html.substr(gt + 1, close - gt - 1));
    }
    AddGithubItem(page, url, title, author);
  }
  return page;
}

GithubPage ParseGithubPullsHtml(std::string_view html, const std::wstring& base_url) {
  GithubPage page;
  constexpr std::string_view kId = "id=\"issue_";
  constexpr std::string_view kOpenedBy = "opened-by";
  size_t at = 0;
  while ((at = html.find(kId, at)) != std::string_view::npos) {
    const auto a = AnchorAround(html, at);
    at += kId.size();
    if (!a) continue;  // the row's <div id="issue_...">
    const std::wstring url = Resolve(FromUtf8(TagAttr(a->tag, "href")), base_url);
    const std::wstring title = HtmlText(html.substr(a->content_begin, a->content_end - a->content_begin));
    std::wstring author;
    const size_t by = html.find(kOpenedBy, a->end);
    const size_t next = html.find(kId, a->end);
    if (by != std::string_view::npos && (next == std::string_view::npos || by < next)) {
      const size_t link = html.find("<a ", by);
      const size_t gt = link == std::string_view::npos ? link : html.find('>', link);
      const size_t close = gt == std::string_view::npos ? gt : html.find("</a>", gt);
      if (close != std::string_view::npos) author = HtmlText(html.substr(gt + 1, close - gt - 1));
    }
    AddGithubItem(page, url, title, author);
    at = a->end;
  }
  return page;
}

std::wstring LiveCompositeId(int64_t folder_id, const std::wstring& item_id) {
  return std::to_wstring(folder_id) + L":" + item_id;
}

LiveDiff DiffLiveItems(int64_t folder_id, const std::vector<std::wstring>& existing,
                       const std::vector<LiveItem>& items, std::set<std::wstring>& dismissed) {
  LiveDiff diff;
  std::set<std::wstring> wanted;
  for (const LiveItem& item : items) wanted.insert(LiveCompositeId(folder_id, item.id));
  std::set<std::wstring> present;
  for (const std::wstring& id : existing) {
    if (!wanted.count(id)) {
      diff.remove.push_back(id);
    } else {
      present.insert(id);
    }
  }
  if (!wanted.empty()) {
    const std::wstring prefix = std::to_wstring(folder_id) + L":";
    for (auto it = dismissed.begin(); it != dismissed.end();) {
      if (it->rfind(prefix, 0) == 0 && !wanted.count(*it)) {
        it = dismissed.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const LiveItem& item : items) {
    const std::wstring id = LiveCompositeId(folder_id, item.id);
    if (present.count(id) || dismissed.count(id)) continue;
    present.insert(id);  // a feed listing an item twice adds it once
    diff.add.push_back(item);
  }
  return diff;
}

std::wstring FormatTimeAgo(int64_t then_ms, int64_t now_ms) {
  if (then_ms == 0) return L"-";
  const double seconds = static_cast<double>(then_ms - now_ms) / 1000.0;
  const double abs_seconds = std::fabs(seconds);
  struct Unit { const wchar_t* name; double size; const wchar_t* zero; const wchar_t* last; const wchar_t* next; };
  static const Unit kUnits[] = {
      {L"day", 86400, L"today", L"yesterday", L"tomorrow"},
      {L"hour", 3600, L"this hour", nullptr, nullptr},
      {L"minute", 60, L"this minute", nullptr, nullptr},
      {L"second", 1, L"now", nullptr, nullptr},
  };
  const Unit* unit = &kUnits[3];
  for (const Unit& u : kUnits) {
    if (abs_seconds >= u.size) {
      unit = &u;
      break;
    }
  }
  const int64_t n = static_cast<int64_t>(std::floor(seconds / unit->size + 0.5));  // Math.round
  if (n == 0) return unit->zero;
  if (n == -1 && unit->last) return unit->last;
  if (n == 1 && unit->next) return unit->next;
  const int64_t count = n < 0 ? -n : n;
  std::wstring text = std::to_wstring(count) + L" " + unit->name + (count == 1 ? L"" : L"s");
  return n < 0 ? text + L" ago" : L"in " + text;
}

}  // namespace orbit
