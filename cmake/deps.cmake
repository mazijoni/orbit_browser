# Pinned third-party dependencies. Fetched and verified by scripts/fetch_deps.ps1.
# Keep these values in sync with that script.

set(ORBIT_CEF_VERSION "152.0.14+g8a0b5b9+chromium-152.0.7977.167")
set(ORBIT_SQLITE_VERSION "3.54.1")

set(ORBIT_CEF_DIR "${CMAKE_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution")
set(ORBIT_SQLITE_DIR "${CMAKE_SOURCE_DIR}/third_party/sqlite" CACHE PATH "SQLite amalgamation")

if(NOT EXISTS "${ORBIT_CEF_DIR}/include/cef_version.h")
  message(FATAL_ERROR "CEF not found at ${ORBIT_CEF_DIR}. Run scripts/fetch_deps.ps1 first.")
endif()
file(STRINGS "${ORBIT_CEF_DIR}/include/cef_version.h" _cef_ver REGEX "^#define CEF_VERSION \"")
string(FIND "${_cef_ver}" "\"${ORBIT_CEF_VERSION}\"" _cef_ver_pos)
if(_cef_ver_pos EQUAL -1)
  message(FATAL_ERROR "CEF version mismatch: expected ${ORBIT_CEF_VERSION}, found ${_cef_ver}")
endif()
if(NOT EXISTS "${ORBIT_SQLITE_DIR}/sqlite3.c")
  message(FATAL_ERROR "SQLite not found at ${ORBIT_SQLITE_DIR}. Run scripts/fetch_deps.ps1 first.")
endif()
