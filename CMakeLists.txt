# Rift C++ storage engine (Track B).
#
# NO NETWORK. There is no FetchContent, no ExternalProject, no download step,
# and there must never be one: `make cpp-ci` runs every lane with networking
# disabled precisely so that adding one is a red lane rather than a surprise for
# whoever first builds this on a plane. DESIGN-B1 section 9.2.
cmake_minimum_required(VERSION 3.16)
project(rift_engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(RIFT_THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/../third_party" CACHE PATH
    "Root of the vendored dependency trees")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ------------------------------------------------------------------- lanes
#
# One build directory per lane, one sanitizer per build directory. The lanes are
# not variations on a theme: cpp-test is the UNINSTRUMENTED control that makes
# the other three mean something, which is why it asserts it has no sanitizer
# just as firmly as they assert they have one (test/sanitizer_lane_test.cc).
set(RIFT_SANITIZER "none" CACHE STRING "none | address | undefined | thread")
set_property(CACHE RIFT_SANITIZER PROPERTY STRINGS none address undefined thread)

# Sanitizer flags are set GLOBALLY, and set HERE -- before the vendored
# googletest is added -- so the framework is instrumented too. Half-instrumented
# ASan is worse than none: it reports the halves it can see and stays quiet
# about the rest, which reads as a clean run.
if(RIFT_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
  set(RIFT_SANITIZER_DEFINE RIFT_EXPECT_ASAN=1)
elseif(RIFT_SANITIZER STREQUAL "undefined")
  # -fno-sanitize-recover=all is load-bearing. Without it UBSan PRINTS a
  # diagnostic and carries on, the process exits 0, and the lane is green with
  # undefined behaviour in its own log. A lane that reports a defect by writing
  # it down and passing anyway is decoration.
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=undefined)
  set(RIFT_SANITIZER_DEFINE RIFT_EXPECT_UBSAN=1)
elseif(RIFT_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
  set(RIFT_SANITIZER_DEFINE RIFT_EXPECT_TSAN=1)
elseif(RIFT_SANITIZER STREQUAL "none")
  set(RIFT_SANITIZER_DEFINE RIFT_EXPECT_NO_SANITIZER=1)
else()
  message(FATAL_ERROR
          "RIFT_SANITIZER must be none|address|undefined|thread, got '${RIFT_SANITIZER}'")
endif()

# ---------------------------------------------------------------- googletest
#
# gmock is not built: we do not use it, and building it doubles the framework's
# compile time in every one of four lanes. This is a build-config choice and NOT
# a modification of the vendored tree -- the tree on disk is the complete
# upstream tree at the pinned commit, which scripts/cpp-vendor-check.sh asserts
# by hash on every run of `make cpp-vendor-check`.
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# The engine is built -fno-exceptions -fno-rtti (DESIGN-B1 section 9.1: no
# exception may cross into Go, ever, and the flag makes that structural rather
# than a review habit). GoogleTest must be told the same thing, in the same
# terms, or its inline code and ours disagree about GTEST_HAS_EXCEPTIONS across
# a translation-unit boundary -- an ODR violation that would present as
# something far stranger than a build error.
add_subdirectory("${RIFT_THIRD_PARTY}/googletest"
                 "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
target_compile_options(gtest      PRIVATE -fno-exceptions -fno-rtti)
target_compile_options(gtest_main PRIVATE -fno-exceptions -fno-rtti)
target_compile_definitions(gtest      PUBLIC GTEST_HAS_EXCEPTIONS=0 GTEST_HAS_RTTI=0)
target_compile_definitions(gtest_main PUBLIC GTEST_HAS_EXCEPTIONS=0 GTEST_HAS_RTTI=0)

# ------------------------------------------------------------ our own flags
#
# -Werror=switch is named explicitly even though -Werror already implies it.
# It is not a style preference: it is the mechanism that makes Status::Code and
# RunOutcome CLOSED (DESIGN-B1 sections 7.5, 7.6). Removing it turns "somebody
# added an enumerator and forgot to classify it" from a build failure into a
# runtime surprise, and the enum stops being closed the moment it does.
set(RIFT_CXX_FLAGS -Wall -Wextra -Werror -Werror=switch -fno-exceptions -fno-rtti)

# The shipping archive. CLAUDE.md: CMake produces a static archive and cgo links
# it (B5). Nothing from rig/ may ever enter it -- the oracle must not be
# reachable from the thing it judges.
add_library(rift_engine STATIC
  src/status.cc
  src/slice.cc
  src/env/call_site.cc
  src/env/fault_controller.cc
  src/env/env.cc
  src/env/env_guard.cc
  src/env/posix/posix_raw.cc
  src/env/posix/posix_env.cc
  src/memtable/arena.cc
  src/memtable/tower.cc
  src/memtable/memtable.cc
  src/wal/crc32c.cc
  src/wal/format.cc
  src/wal/reader.cc
  src/wal/writer.cc
  src/wal/sha256.cc
  src/wal/wal.cc
  src/wal/recovery.cc
)
target_include_directories(rift_engine PUBLIC src src/env src/env/posix src/memtable src/wal)
target_compile_options(rift_engine PRIVATE ${RIFT_CXX_FLAGS})

# Harness-side types. A SEPARATE archive, deliberately: it is what the rig uses
# to decide what a run may be counted as, and it never links into Go.
# TestEnv lives under src/ so the A5 scope scan covers it -- it executes during
# a run and its fault schedules must be as deterministic as the engine's -- but
# it is a SEPARATE archive and never links into rift_engine or into Go.
add_library(rift_testenv STATIC
  src/env/test/test_env.cc
)
target_include_directories(rift_testenv PUBLIC src src/env src/env/test)
target_link_libraries(rift_testenv PUBLIC rift_engine)
target_compile_options(rift_testenv PRIVATE ${RIFT_CXX_FLAGS})

add_library(rift_rig STATIC
  rig/run_outcome.cc
  rig/durable_mirror.cc
  rig/cap_adjudication.cc
  rig/regime.cc
)
target_include_directories(rift_rig PUBLIC rig src src/env src/env/test src/wal)
target_link_libraries(rift_rig PUBLIC rift_testenv)
target_compile_options(rift_rig PRIVATE ${RIFT_CXX_FLAGS})

# ----------------------------------------------------------------- binaries
add_executable(rift_engine_test
  test/sanitizer_lane_test.cc
  test/status_test.cc
  test/lane_canary_test.cc
  test/run_outcome_test.cc
  test/env_surface_test.cc
  test/posix_raw_test.cc
  test/test_env_test.cc
  test/real_exit_kill_test.cc
  test/tower_test.cc
  test/memtable_test.cc
  test/wal_reader_test.cc
  test/wal_test.cc
  test/rig_caps_test.cc
  test/recovery_test.cc
)
# rift_rig carries rift_testenv which carries rift_engine, both PUBLIC-ly;
# naming any of them again makes the linker warn about a duplicate archive.
target_link_libraries(rift_engine_test PRIVATE rift_rig gtest_main)
target_compile_options(rift_engine_test PRIVATE ${RIFT_CXX_FLAGS})
target_compile_definitions(rift_engine_test PRIVATE ${RIFT_SANITIZER_DEFINE})

# The cpp-tsan lane runs THIS and not the unit suite (DESIGN-B1 section 6.4).
add_executable(rift_tsan_harness
  test/sanitizer_lane_test.cc
  test/tsan_harness_test.cc
)
target_link_libraries(rift_tsan_harness PRIVATE rift_engine gtest_main)
target_include_directories(rift_tsan_harness PRIVATE src/memtable)
target_compile_options(rift_tsan_harness PRIVATE ${RIFT_CXX_FLAGS})
target_compile_definitions(rift_tsan_harness PRIVATE ${RIFT_SANITIZER_DEFINE})
