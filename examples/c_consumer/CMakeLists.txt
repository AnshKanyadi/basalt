# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Ansh Kanyadi
#
# A CONSUMER OF THE INSTALLED PACKAGE, WRITTEN IN C, THAT KNOWS NO C++.
#
# This is not a demo. It is the only check in the tree that exercises what an
# embedder actually does -- find_package against an INSTALLED prefix, rather
# than a target that happens to be in the same build -- and it is here because
# doing it for the first time immediately failed.
#
# WHAT IT CAUGHT. `basalt` is a static archive of C++ objects. A C++ consumer
# links it and the driver supplies the C++ runtime unasked; a C consumer links
# it and gets hundreds of undefined std:: symbols. Every in-tree test is C++, so
# nothing here could see that, and the library's first consumer had quietly
# patched around it by writing `-lstdc++` into its own source.
#
# A library whose C API only links for people who already know it is C++ has not
# finished shipping a C API.
cmake_minimum_required(VERSION 3.16)
project(basalt_c_consumer C)

# VERSIONED ON PURPOSE. An unversioned find_package would pass without ever
# consulting basaltConfigVersion.cmake, so the version file would be installed,
# untested, and free to be wrong.
find_package(basalt 0.1 REQUIRED)

add_executable(basalt_c_consumer main.c)

# basalt::basalt_c AND NOTHING ELSE. Not basalt::basalt, not a C++ runtime, not
# an include path. If this line ever needs a companion, the package is asking
# its consumers to know something it should have told CMake itself.
target_link_libraries(basalt_c_consumer PRIVATE basalt::basalt_c)
set_target_properties(basalt_c_consumer PROPERTIES C_STANDARD 99
                                                   C_STANDARD_REQUIRED ON
                                                   C_EXTENSIONS OFF)
