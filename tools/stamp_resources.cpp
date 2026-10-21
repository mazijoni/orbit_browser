// Build tool: copies the icon and version resources from orbit.dll into
// orbit.exe (CEF's bootstrap.exe, renamed) so Explorer, the taskbar and the
// installer show Orbit's identity. Must run before code signing.
//
// Usage: stamp_resources <source.dll> <target.exe>

#include <windows.h>

#include <cstdio>
#include <vector>

namespace {

struct Resource {
  LPCWSTR type;
  WORD id;
  std::vector<BYTE> data;
  WORD language;
};

BOOL CALLBACK EnumLang(HMODULE module, LPCWSTR type, LPCWSTR name, WORD language,
                       LONG_PTR param) {
  auto* out = reinterpret_cast<std::vector<Resource>*>(param);
  HRSRC res = ::FindResourceExW(module, type, name, language);
  HGLOBAL handle = res ? ::LoadResource(module, res) : nullptr;
  const void* bytes = handle ? ::LockResource(handle) : nullptr;
  if (!bytes || !IS_INTRESOURCE(name)) return TRUE;
  const DWORD size = ::SizeofResource(module, res);
  Resource r{type, static_cast<WORD>(reinterpret_cast<ULONG_PTR>(name)),
             std::vector<BYTE>(static_cast<const BYTE*>(bytes),
                               static_cast<const BYTE*>(bytes) + size),
             language};
  out->push_back(std::move(r));
  return TRUE;
}

BOOL CALLBACK EnumName(HMODULE module, LPCWSTR type, LPWSTR name, LONG_PTR param) {
  ::EnumResourceLanguagesW(module, type, name, &EnumLang, param);
  return TRUE;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc != 3) {
    std::fwprintf(stderr, L"usage: stamp_resources <source.dll> <target.exe>\n");
    return 2;
  }
  HMODULE source = ::LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE |
                                                          LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (!source) {
    std::fwprintf(stderr, L"cannot load %s (%lu)\n", argv[1], ::GetLastError());
    return 1;
  }
  std::vector<Resource> resources;
  for (LPCWSTR type : {RT_ICON, RT_GROUP_ICON, RT_VERSION}) {
    ::EnumResourceNamesW(source, type, &EnumName, reinterpret_cast<LONG_PTR>(&resources));
  }
  ::FreeLibrary(source);

  // Remove the target's existing icons/version first (bootstrap's own ids may
  // differ from ours), then add ours.
  HMODULE target_view = ::LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_AS_DATAFILE |
                                                               LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  std::vector<Resource> existing;
  if (target_view) {
    for (LPCWSTR type : {RT_ICON, RT_GROUP_ICON, RT_VERSION}) {
      ::EnumResourceNamesW(target_view, type, &EnumName, reinterpret_cast<LONG_PTR>(&existing));
    }
    ::FreeLibrary(target_view);
  }
  HANDLE update = ::BeginUpdateResourceW(argv[2], FALSE);
  if (!update) {
    std::fwprintf(stderr, L"cannot open %s for update (%lu)\n", argv[2], ::GetLastError());
    return 1;
  }
  for (const auto& r : existing) {
    ::UpdateResourceW(update, r.type, MAKEINTRESOURCEW(r.id), r.language, nullptr, 0);
  }
  for (const auto& r : resources) {
    if (!::UpdateResourceW(update, r.type, MAKEINTRESOURCEW(r.id), r.language,
                           const_cast<BYTE*>(r.data.data()), static_cast<DWORD>(r.data.size()))) {
      std::fwprintf(stderr, L"UpdateResource failed (%lu)\n", ::GetLastError());
      ::EndUpdateResourceW(update, TRUE);
      return 1;
    }
  }
  if (!::EndUpdateResourceW(update, FALSE)) {
    std::fwprintf(stderr, L"EndUpdateResource failed (%lu)\n", ::GetLastError());
    return 1;
  }
  std::fwprintf(stdout, L"stamped %zu resource(s) into %s\n", resources.size(), argv[2]);
  return 0;
}
