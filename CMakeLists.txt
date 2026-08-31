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
target_link_libraries(libpergrep PUBLIC ICU::uc)
set_target_properties(libpergrep PROPERTIES OUTPUT_NAME pergrep)

add_executable(pergrep src/cli.cpp)
target_link_libraries(pergrep PRIVATE libpergrep LibArchive::LibArchive)
if(MSVC)
  target_compile_options(pergrep PRIVATE /W4)
else()
  target_compile_options(pergrep PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  target_compile_definitions(pergrep PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
  # wmain is the entry point on Windows so argv arrives as UTF-16 (paths with
  # non-ANSI characters survive); select wmainCRTStartup explicitly.
  target_link_options(pergrep PRIVATE /ENTRY:wmainCRTStartup)
endif()

if(MSVC)
  target_compile_options(libpergrep PRIVATE /W4)
else()
  target_compile_options(libpergrep PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(PERGREP_BUILD_TESTS)
  enable_testing()
  add_executable(pergrep_tests tests/test.cpp)
  target_link_libraries(pergrep_tests PRIVATE libpergrep)
  if(MSVC)
    target_compile_options(pergrep_tests PRIVATE /W4 /UNDEBUG)
  else()
    target_compile_options(pergrep_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  endif()
  add_test(NAME pergrep_tests COMMAND pergrep_tests)

  add_executable(pergrep_region_verifier_tests tests/region_verifier_test.cpp)
  target_link_libraries(pergrep_region_verifier_tests PRIVATE libpergrep)
  if(MSVC)
    target_compile_options(pergrep_region_verifier_tests PRIVATE /W4 /UNDEBUG)
  else()
    target_compile_options(pergrep_region_verifier_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  endif()
  add_test(NAME pergrep_region_verifier_tests COMMAND pergrep_region_verifier_tests)
  add_executable(pergrep_c_tests tests/test_c.c)
  target_link_libraries(pergrep_c_tests PRIVATE libpergrep)
  if(MSVC)
    target_compile_options(pergrep_c_tests PRIVATE /W4 /UNDEBUG)
  else()
    target_compile_options(pergrep_c_tests PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
  endif()
  add_test(NAME pergrep_c_tests COMMAND pergrep_c_tests)
  add_executable(pergrep_bench bench/bench.cpp)
  target_sources(pergrep_bench PRIVATE bench/workload_matrix.hpp)
  target_link_libraries(pergrep_bench PRIVATE libpergrep)
  if(MSVC)
    target_compile_options(pergrep_bench PRIVATE /W4 /O2)
  else()
    target_compile_options(pergrep_bench PRIVATE -Wall -Wextra -Wpedantic -O3)
  endif()

  # The shell suites require a real POSIX bash. On Windows that means Git for
  # Windows' bash; the WSL bash (C:\Windows\System32\bash.exe) cannot run
  # Windows-path scripts, so exclude it explicitly.
  if(WIN32)
    find_program(PERGREP_BASH bash
      HINTS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin"
      PATHS "C:/Program Files/Git/bin" "C:/Program Files/Git/usr/bin"
      NO_DEFAULT_PATH)
    if(NOT PERGREP_BASH)
      find_program(PERGREP_BASH bash)
    endif()
  else()
    find_program(PERGREP_BASH bash)
  endif()
  if(PERGREP_BASH)
    if(WIN32)
      # Git for Windows' bash expects Unix-style paths; convert them via cygpath.
      find_program(PERGREP_CYGPATH cygpath
        HINTS "C:/Program Files/Git/usr/bin" "C:/Program Files/Git/bin"
        PATHS "C:/Program Files/Git/usr/bin" "C:/Program Files/Git/bin"
        NO_DEFAULT_PATH)
      if(PERGREP_CYGPATH)
        execute_process(COMMAND ${PERGREP_CYGPATH} -u ${CMAKE_CURRENT_SOURCE_DIR}/tests
                        OUTPUT_VARIABLE PERGREP_TESTS_UNIX OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(STRIP "${PERGREP_TESTS_UNIX}" PERGREP_TESTS_UNIX)
        # The pergrep binary path is passed as-is; Git for Windows converts
        # Windows-style paths automatically when executing them.
        add_test(NAME pergrep_cli_compat COMMAND ${PERGREP_BASH} ${PERGREP_TESTS_UNIX}/cli_compat.sh $<TARGET_FILE:pergrep>)
        add_test(NAME pergrep_flags_surface COMMAND ${PERGREP_BASH} ${PERGREP_TESTS_UNIX}/flags_surface.sh $<TARGET_FILE:pergrep>)
        add_test(NAME pergrep_upstream_cases COMMAND ${PERGREP_BASH} ${PERGREP_TESTS_UNIX}/upstream_cases.sh $<TARGET_FILE:pergrep>)
      endif()
    else()
      add_test(NAME pergrep_cli_compat COMMAND ${PERGREP_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_compat.sh $<TARGET_FILE:pergrep>)
      add_test(NAME pergrep_flags_surface COMMAND ${PERGREP_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/flags_surface.sh $<TARGET_FILE:pergrep>)
      add_test(NAME pergrep_upstream_cases COMMAND ${PERGREP_BASH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/upstream_cases.sh $<TARGET_FILE:pergrep>)
    endif()
  endif()
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
