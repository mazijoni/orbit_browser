# orbit_core: platform code with no CEF dependency (unit-testable).
add_library(orbit_core STATIC
  core/bookmark_html.cpp
  core/external_scheme.cpp
  core/log.cpp
  core/process_metrics.cpp
  core/strings.cpp
  core/url_fixup.cpp
  core/omnibox_results.cpp
  core/motion.cpp
  core/split_layout.cpp
  core/shortcuts.cpp
  core/space_routing.cpp
  core/live_folders.cpp
  core/space_theme.cpp
  core/boosts.cpp
  core/json.cpp
  core/crypto.cpp
  browser/importer.cpp
  persistence/database.cpp
  persistence/schema.cpp
  persistence/store.cpp
  persistence/store_services.cpp
)
target_include_directories(orbit_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(orbit_core PRIVATE ${ORBIT_CXX_FLAGS})
target_compile_definitions(orbit_core PUBLIC ${ORBIT_DEFINES})
target_link_libraries(orbit_core PUBLIC sqlite3 psapi bcrypt crypt32)

# orbit.dll: the browser. Loaded by orbit.exe (CEF's bootstrap.exe, renamed)
# which provides the Windows sandbox for all Chromium sub-processes.
add_library(orbit SHARED
  app/application.cpp
  app/installer.cpp
  app/main.cpp
  app/self_test.cpp
  app/test_http_server.cpp
  app/orbit_app.cpp
  app/orbit.rc
  browser/browser_client.cpp
  browser/library_client.cpp
  browser/extension_bridge.cpp
  browser/extension_popup.cpp
  browser/default_client.cpp
  browser/find_bar.cpp
  browser/live_folders.cpp
  browser/text_fetch.cpp
  browser/boost_apply.cpp
  browser/library_page.cpp
  browser/theme_page.cpp
  browser/boost_page.cpp
  browser/external_protocol.cpp
  browser/internal_api.cpp
  browser/internal_page_content.cpp
  browser/internal_pages_ui.cpp
  browser/downloads.cpp
  browser/internal_pages.cpp
  browser/permissions.cpp
  browser/password_vault.cpp
  browser/profile.cpp
  browser/tab.cpp
  ui/accessibility.cpp
  ui/browser_window.cpp
  ui/browser_window_paint.cpp
  ui/menu.cpp
  ui/graphics.cpp
  ui/theme.cpp
  ui/toast.cpp
  ui/download_flight.cpp
  ui/share.cpp
)
SET_LIBRARY_TARGET_PROPERTIES(orbit)
target_include_directories(orbit PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(orbit PRIVATE /permissive-)
configure_file(app/version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.h" @ONLY)
target_include_directories(orbit PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_dependencies(orbit libcef_dll_wrapper)
target_link_libraries(orbit PRIVATE
  orbit_core
  libcef_dll_wrapper
  "${CEF_LIB_RELEASE}"
  ${CEF_STANDARD_LIBS}
  comctl32 d2d1 dwrite dwmapi oleaut32 shell32 ole32 propsys uiautomationcore user32 uxtheme dbghelp runtimeobject)

# Assemble a runnable layout in bin/: orbit.exe + orbit.dll + CEF runtime.
add_executable(stamp_resources "${CMAKE_SOURCE_DIR}/tools/stamp_resources.cpp")
target_compile_options(stamp_resources PRIVATE /W4 /WX /utf-8)
target_compile_definitions(stamp_resources PRIVATE UNICODE _UNICODE)
set_target_properties(stamp_resources PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
add_dependencies(orbit stamp_resources)

# Developer tool: native UI Automation probe used by tools/uia_check.ps1.
add_executable(uia_probe "${CMAKE_SOURCE_DIR}/tools/uia_probe.cpp")
target_compile_options(uia_probe PRIVATE /W4 /WX /utf-8)
target_compile_definitions(uia_probe PRIVATE UNICODE _UNICODE)
target_link_libraries(uia_probe PRIVATE ole32 oleaut32 uiautomationcore)
set_target_properties(uia_probe PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
add_custom_command(TARGET orbit POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CEF_BINARY_DIR_RELEASE}/bootstrap.exe" "${ORBIT_OUT_DIR}/orbit.exe"
  COMMAND "$<TARGET_FILE:stamp_resources>" "$<TARGET_FILE:orbit>" "${ORBIT_OUT_DIR}/orbit.exe"
  COMMENT "Installing bootstrap as orbit.exe with Orbit's icon and version")
foreach(f ${CEF_BINARY_FILES})
  add_custom_command(TARGET orbit POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_BINARY_DIR_RELEASE}/${f}" "${ORBIT_OUT_DIR}/${f}")
endforeach()
foreach(f ${CEF_RESOURCE_FILES})
  if(IS_DIRECTORY "${CEF_RESOURCE_DIR}/${f}")
    add_custom_command(TARGET orbit POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${CEF_RESOURCE_DIR}/${f}" "${ORBIT_OUT_DIR}/${f}")
  else()
    add_custom_command(TARGET orbit POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CEF_RESOURCE_DIR}/${f}" "${ORBIT_OUT_DIR}/${f}")
  endif()
endforeach()
# The sandbox's LPAC renderer/GPU processes need read access to the binaries.
add_custom_command(TARGET orbit POST_BUILD
  COMMAND icacls "${ORBIT_OUT_DIR}" /grant "*S-1-15-2-2:(OI)(CI)(RX)" /Q > NUL
  COMMENT "Setting LPAC ACLs")
