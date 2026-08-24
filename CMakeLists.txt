# Rift C++ storage engine (Track B).
#
# B1.0 scope: this file exists to build the VENDORED GoogleTest and nothing
# else. There is no engine code yet and no test of ours yet -- that is B1.1,
# deliberately, because B1.1 is the step at which a failing C++ test of ours
# first turns a lane red (DESIGN-B1 section 14.1, threshold 2). Keeping B1.0
# free of our code is what keeps that threshold at B1.1 where it is claimed.
#
# NO NETWORK. There is no FetchContent, no ExternalProject, no download step,
# and there must never be one: `make cpp-ci` runs every lane with networking
# disabled precisely so that adding one is a red lane rather than a surprise
# for whoever first builds this on a plane. DESIGN-B1 section 9.2.
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

# ---------------------------------------------------------------- googletest
#
# gmock is not built: we do not use it, and building it doubles the framework's
# compile time in every one of four sanitizer lanes. This is a build-config
# choice and NOT a modification of the vendored tree -- the tree on disk is the
# complete upstream tree at the pinned commit, which scripts/cpp-vendor-check.sh
# asserts by hash on every run of `make cpp-vendor-check`.
set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

# The engine is built -fno-exceptions -fno-rtti (DESIGN-B1 section 9.1:
# no exception may cross into Go, ever, and the flag makes that structural
# rather than a review habit). GoogleTest must be told the same thing, in the
# same terms, or its inline code and ours disagree about GTEST_HAS_EXCEPTIONS
# across a translation-unit boundary -- an ODR violation that would present as
# something far stranger than a build error.
add_subdirectory("${RIFT_THIRD_PARTY}/googletest"
                 "${CMAKE_BINARY_DIR}/googletest" EXCLUDE_FROM_ALL)
target_compile_options(gtest      PRIVATE -fno-exceptions -fno-rtti)
target_compile_options(gtest_main PRIVATE -fno-exceptions -fno-rtti)
target_compile_definitions(gtest      PUBLIC GTEST_HAS_EXCEPTIONS=0 GTEST_HAS_RTTI=0)
target_compile_definitions(gtest_main PUBLIC GTEST_HAS_EXCEPTIONS=0 GTEST_HAS_RTTI=0)
