add_executable(orbit_unit_tests
  unit/test_main.cpp
  unit/test_url_fixup.cpp
  unit/test_omnibox_results.cpp
  unit/test_motion.cpp
  unit/test_split_layout.cpp
  unit/test_shortcuts.cpp
  unit/test_space_routing.cpp
  unit/test_live_folders.cpp
  unit/test_space_theme.cpp
  unit/test_boosts.cpp
  unit/test_strings.cpp
  unit/test_store.cpp
  unit/test_services.cpp
)
target_compile_options(orbit_unit_tests PRIVATE ${ORBIT_CXX_FLAGS})
target_link_libraries(orbit_unit_tests PRIVATE orbit_core)
add_test(NAME unit COMMAND orbit_unit_tests)
