# basalt -- an LSM storage engine.
#
# NO NETWORK. There is no FetchContent, no ExternalProject, and no download
# step, and there must never be one: a build step that reaches the network
# fails in exactly the situation where "it builds from a clean clone" matters
# most, which is a stranger checking our work. GoogleTest is vendored whole at
# third_party/googletest; see its VERSION file for the pin.
cmake_minimum_required(VERSION 3.16)
project(basalt CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Tests, the rig and the tools are built when basalt is the top-level project
# and not when it is somebody's add_subdirectory. An embedder pays for the
# library and nothing else.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(BASALT_IS_TOP_LEVEL ON)
else()
  set(BASALT_IS_TOP_LEVEL OFF)
endif()
option(BASALT_BUILD_TESTS "Build the test suite, the rig and the tools" ${BASALT_IS_TOP_LEVEL})

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ------------------------------------------------------------------- lanes
#
# One build directory per lane, one sanitizer per build directory. The lanes
# are not variations on a theme: the uninstrumented lane is the CONTROL that
# makes the other three mean something, which is why it asserts it has no
# sanitizer just as firmly as they assert they have one
# (test/sanitizer_lane_test.cc).
set(BASALT_SANITIZER "none" CACHE STRING "none | address | undefined | thread")
set_property(CACHE BASALT_SANITIZER PROPERTY STRINGS none address undefined thread)

# Sanitizer flags are set GLOBALLY, and set HERE -- before the vendored
# googletest is added -- so the framework is instrumented too. Half-instrumented
# ASan is worse than none: it reports the halves it can see and stays quiet
# about the rest, which reads as a clean run.
if(BASALT_SANITIZER STREQUAL "address")
  add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address)
  set(BASALT_SANITIZER_DEFINE BASALT_EXPECT_ASAN=1)
elseif(BASALT_SANITIZER STREQUAL "undefined")
  # -fno-sanitize-recover=all is load-bearing. Without it UBSan PRINTS a
  # diagnostic and carries on, the process exits 0, and the lane is green with
  # undefined behaviour in its own log. A lane that reports a defect by writing
  # it down and passing anyway is decoration.
  add_compile_options(-fsanitize=undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=undefined)
  set(BASALT_SANITIZER_DEFINE BASALT_EXPECT_UBSAN=1)
elseif(BASALT_SANITIZER STREQUAL "thread")
  add_compile_options(-fsanitize=thread -g)
  add_link_options(-fsanitize=thread)
  set(BASALT_SANITIZER_DEFINE BASALT_EXPECT_TSAN=1)
elseif(BASALT_SANITIZER STREQUAL "none")
  set(BASALT_SANITIZER_DEFINE BASALT_EXPECT_NO_SANITIZER=1)
else()
  message(FATAL_ERROR
          "BASALT_SANITIZER must be none|address|undefined|thread, got '${BASALT_SANITIZER}'")
endif()

# ------------------------------------------------------------ our own flags
#
# -Werror=switch is named explicitly even though -Werror already implies it.
# It is not a style preference: it is the mechanism that makes Status::Code and
# RunOutcome CLOSED. Removing it turns "somebody added an enumerator and forgot
# to classify it" from a build failure into a runtime surprise, and the enum
# stops being closed the moment it does.
#
# -fno-exceptions -fno-rtti: no exception may leave this library, and the flag
# makes that structural rather than a review habit. Status is the only error
# channel; a violated precondition aborts.
set(BASALT_CXX_FLAGS -Wall -Wextra -Werror -Werror=switch -fno-exceptions -fno-rtti)

# The internal include roots. Public headers are reached as <basalt/x.h> from
# the `include` directory below; these are for the flat includes the
# implementation uses among itself, and they are PRIVATE -- nothing outside the
# library gets them by linking it.
set(BASALT_INTERNAL_INCLUDES
  src src/env src/env/posix src/memtable src/wal src/sst)

# --------------------------------------------------------------- the library
add_library(basalt STATIC
  src/status.cc
  src/slice.cc
  src/internal_key.cc
  src/merged_iter.cc
  src/env/call_site.cc
  src/env/fault_controller.cc
  src/env/env.cc
  src/env/env_guard.cc
  src/env/read_whole_file.cc
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
  src/sst/bloom.cc
  src/sst/table_format.cc
  src/sst/concat_iter.cc
  src/sst/range_tombstone.cc
  src/sst/table.cc
  src/sst/table_builder.cc
  src/sst/manifest.cc
  src/sst/table_check.cc
  src/compaction.cc
  src/db.cc
)
add_library(basalt::basalt ALIAS basalt)
target_include_directories(basalt
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt PRIVATE ${BASALT_CXX_FLAGS})

include(GNUInstallDirs)
install(TARGETS basalt ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/basalt DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(NOT BASALT_BUILD_TESTS)
  return()
endif()

# ---------------------------------------------------------------- googletest
#
# gmock is not built: we do not use it, and building it doubles the framework's
# compile time in every one of four lanes. This is a build-config choice and
# NOT a modification of the vendored tree, which is upstream's complete tree at
# the pinned commit.
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# The library is built -fno-exceptions -fno-rtti. GoogleTest must be told the
# same thing, in the same terms, or its inline code and ours disagree about
# GTEST_HAS_EXCEPTIONS across a translation-unit boundary -- an ODR violation
# that would present as something far stranger than a build error.
add_subdirectory(third_party/googletest "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
target_compile_options(gtest      PRIVATE -fno-exceptions -fno-rtti)
target_compile_options(gtest_main PRIVATE -fno-exceptions -fno-rtti)
target_compile_definitions(gtest      PUBLIC GTEST_HAS_EXCEPTIONS=0 GTEST_HAS_RTTI=0)
target_compile_definitions(gtest_main PUBLIC GTEST_HAS_EXCEPTIONS=0 GTEST_HAS_RTTI=0)

# ------------------------------------------------------------- harness-side
#
# TestEnv lives under src/ so the scope scan covers it -- it executes during a
# run and its fault schedules must be as deterministic as the engine's -- but it
# is a SEPARATE archive and never links into `basalt`.
add_library(basalt_testenv STATIC src/env/test/test_env.cc)
target_include_directories(basalt_testenv PUBLIC src/env/test PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_link_libraries(basalt_testenv PUBLIC basalt)
target_compile_options(basalt_testenv PRIVATE ${BASALT_CXX_FLAGS})

# What the rig uses to decide what a run may be counted as. A separate archive,
# deliberately: the oracle must not be reachable from the thing it judges.
add_library(basalt_rig STATIC
  rig/run_outcome.cc
  rig/version_model.cc
  rig/drop_check.cc
  rig/image_fixture.cc
  rig/manifest_image.cc
  rig/merge_check.cc
  rig/durable_mirror.cc
  rig/cap_adjudication.cc
  rig/poller.cc
  rig/regime.cc
  rig/exactness_oracle.cc
  rig/sweep.cc
  rig/amplification.cc
  rig/differential_artifact.cc
  rig/differential_driver.cc
)
target_include_directories(basalt_rig PUBLIC rig PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_link_libraries(basalt_rig PUBLIC basalt_testenv)
target_compile_options(basalt_rig PRIVATE ${BASALT_CXX_FLAGS})

# ----------------------------------------------------------------- binaries
enable_testing()

add_executable(basalt_test
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
  test/db_test.cc
  test/db_flush_test.cc
  test/sst_bloom_test.cc
  test/concat_iter_test.cc
  test/range_tombstone_test.cc
  test/sst_format_test.cc
  test/sst_manifest_test.cc
  test/sst_writer_test.cc
  test/oracle_test.cc
  test/drop_check_test.cc
  test/merge_check_test.cc
  test/compaction_test.cc
  test/db_compaction_test.cc
  test/amplification_test.cc
  test/differential_artifact_test.cc
  test/rng_test.cc
  test/differential_driver_test.cc
  test/poller_test.cc
  test/bench_keys_test.cc
)
# basalt_rig carries basalt_testenv which carries basalt, both PUBLIC-ly;
# naming any of them again makes the linker warn about a duplicate archive.
target_link_libraries(basalt_test PRIVATE basalt_rig gtest_main)
target_include_directories(basalt_test PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt_test PRIVATE ${BASALT_CXX_FLAGS})
target_compile_definitions(basalt_test PRIVATE ${BASALT_SANITIZER_DEFINE}
  BASALT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
add_test(NAME basalt_test COMMAND basalt_test)

# The thread lane runs THIS and not the unit suite: the unit suite is
# single-threaded, so a green run of it under TSan proves nothing.
add_executable(basalt_tsan_harness
  test/sanitizer_lane_test.cc
  test/tsan_harness_test.cc
)
target_link_libraries(basalt_tsan_harness PRIVATE basalt gtest_main)
target_include_directories(basalt_tsan_harness PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt_tsan_harness PRIVATE ${BASALT_CXX_FLAGS})
target_compile_definitions(basalt_tsan_harness PRIVATE ${BASALT_SANITIZER_DEFINE})

add_executable(basalt_sweep cmd/sweep_main.cc)
target_link_libraries(basalt_sweep PRIVATE basalt_rig)
target_include_directories(basalt_sweep PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt_sweep PRIVATE ${BASALT_CXX_FLAGS})

add_executable(basalt_diff cmd/diff_main.cc)
target_link_libraries(basalt_diff PRIVATE basalt_rig)
target_include_directories(basalt_diff PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt_diff PRIVATE ${BASALT_CXX_FLAGS})

add_executable(basalt_bench cmd/bench_main.cc)
target_link_libraries(basalt_bench PRIVATE basalt)
target_include_directories(basalt_bench PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt_bench PRIVATE ${BASALT_CXX_FLAGS})

add_executable(basalt_amp cmd/amp_main.cc)
target_link_libraries(basalt_amp PRIVATE basalt_rig)
target_include_directories(basalt_amp PRIVATE ${BASALT_INTERNAL_INCLUDES})
target_compile_options(basalt_amp PRIVATE ${BASALT_CXX_FLAGS})
