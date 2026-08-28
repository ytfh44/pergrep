cmake_minimum_required(VERSION 3.20)
project(pergrep VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
option(PERGREP_BUILD_TESTS "Build tests" ON)
find_package(ICU REQUIRED COMPONENTS uc)
find_package(LibArchive REQUIRED)

add_library(libpergrep src/index.cpp src/regex.cpp src/search.cpp src/c_api.cpp)
target_include_directories(libpergrep PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:include>)
target_compile_options(libpergrep PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(libpergrep PUBLIC ICU::uc)
set_target_properties(libpergrep PROPERTIES OUTPUT_NAME pergrep)

add_executable(pergrep src/cli.cpp)
target_link_libraries(pergrep PRIVATE libpergrep LibArchive::LibArchive)
target_compile_options(pergrep PRIVATE -Wall -Wextra -Wpedantic)

if(PERGREP_BUILD_TESTS)
  enable_testing()
  add_executable(pergrep_tests tests/test.cpp)
  target_link_libraries(pergrep_tests PRIVATE libpergrep)
  target_compile_options(pergrep_tests PRIVATE -UNDEBUG)
  add_test(NAME pergrep_tests COMMAND pergrep_tests)
  add_executable(pergrep_c_tests tests/test_c.c)
  target_link_libraries(pergrep_c_tests PRIVATE libpergrep)
  target_compile_options(pergrep_c_tests PRIVATE -UNDEBUG)
  add_test(NAME pergrep_c_tests COMMAND pergrep_c_tests)
  add_test(NAME pergrep_cli_compat COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_compat.sh $<TARGET_FILE:pergrep>)
  add_test(NAME pergrep_flags_surface COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/flags_surface.sh $<TARGET_FILE:pergrep>)
  add_test(NAME pergrep_upstream_cases COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tests/upstream_cases.sh $<TARGET_FILE:pergrep>)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(TARGETS libpergrep pergrep EXPORT pergrepTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT pergrepTargets FILE pergrepTargets.cmake NAMESPACE pergrep:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pergrep)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pergrepConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/pergrepConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pergrep)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/pergrepConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/pergrepConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/pergrepConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pergrep)
