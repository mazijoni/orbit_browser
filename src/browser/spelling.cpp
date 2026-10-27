#include "browser/spelling.h"

#include <windows.h>
#include <spellcheck.h>
#include <wrl/client.h>

namespace orbit::spelling {

std::vector<std::wstring> Suggest(const std::wstring& word, const std::wstring& language, size_t max) {
  std::vector<std::wstring> out;
  if (word.empty() || word.size() > 100 || language.empty()) return out;
  Microsoft::WRL::ComPtr<ISpellCheckerFactory> factory;
  if (FAILED(::CoCreateInstance(__uuidof(SpellCheckerFactory), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
    return out;
  }
  BOOL supported = FALSE;
  if (FAILED(factory->IsSupported(language.c_str(), &supported)) || !supported) return out;
  Microsoft::WRL::ComPtr<ISpellChecker> checker;
  if (FAILED(factory->CreateSpellChecker(language.c_str(), &checker))) return out;
  Microsoft::WRL::ComPtr<IEnumString> suggestions;
  if (FAILED(checker->Suggest(word.c_str(), &suggestions)) || !suggestions) return out;
  LPOLESTR item = nullptr;
  while (out.size() < max && suggestions->Next(1, &item, nullptr) == S_OK) {
    if (item && item != word) out.emplace_back(item);
    ::CoTaskMemFree(item);
    item = nullptr;
  }
  return out;
}

}  // namespace orbit::spelling
