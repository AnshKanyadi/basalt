/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ansh Kanyadi
 *
 * basalt_cxx.h -- the C++ interop seam for the C ABI.
 *
 * WHY THIS EXISTS AT ALL, since a C API that needs a C++ header to be useful
 * has usually failed at something: it is NOT needed to USE the C API.
 * basalt/basalt.h is complete on its own and basalt_env_posix covers every
 * production case. This header covers one thing that header cannot:
 *
 *   HANDING THE C API AN Env THAT ALREADY EXISTS AS A C++ OBJECT.
 *
 * Two callers want that, and they are the same caller wearing two hats:
 *
 *   A TEST HARNESS. The engine's fault injection, its kill mechanism and its
 *   call ordinals all live in the Env seam. A harness that cannot put its own
 *   Env behind the C API cannot test the C API at the depth it tests the C++
 *   one -- and the difference between those two depths is exactly the size of
 *   the hole in any durability claim made about the boundary. basalt's own
 *   kill-point sweep runs both surfaces through this function.
 *
 *   A C++ EMBEDDER SHIPPING A C PLUGIN ABI. It has an Env; its plugins speak C.
 *
 * WHY IT IS NOT IN basalt.h. That header must compile as C, and this cannot:
 * it names a C++ class. Keeping them apart is what lets test/c_abi_test.c
 * assert the first one really is C, rather than asserting it about a file with
 * a C++-only half that a `#ifdef __cplusplus` quietly skipped.
 */
#ifndef BASALT_BASALT_CXX_H_
#define BASALT_BASALT_CXX_H_

#ifndef __cplusplus
#error "basalt/basalt_cxx.h is C++ only; C consumers want basalt/basalt.h"
#endif

#include "basalt/basalt.h"
#include "basalt/env.h"

namespace basalt {

// Wraps an existing Env in a handle the C API accepts.
//
// BORROWED, AND THE NAME SAYS SO. The returned handle does not own `env` and
// never deletes it. `env` must outlive the handle, and the handle must outlive
// every database opened over it. Release the handle with basalt_env_free, which
// frees the wrapper and leaves `env` alone.
//
// Returns nullptr if `env` is null or the allocation failed.
basalt_env* BorrowEnv(Env* env);

}  // namespace basalt

#endif  // BASALT_BASALT_CXX_H_
