/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ansh Kanyadi
 *
 * THIS FILE IS COMPILED AS C, AND THAT IS ITS ENTIRE PURPOSE.
 *
 * test/c_api_test.cc exercises the behaviour, and it is C++, because that is
 * where GoogleTest lives. So every assertion about what the C ABI DOES is made
 * by a C++ compiler reading a header through `extern "C"` -- which proves
 * nothing whatever about whether a C compiler can read it. A C header that has
 * only ever been compiled as C++ is a C header by intention.
 *
 * The failure this catches is not exotic. `nullptr` in a comment example, a
 * `//` comment in a tree that predates C99 habits, a default argument, an
 * `enum class`, a struct declared without `typedef` and then used bare, `bool`
 * without <stdbool.h> -- each compiles silently as C++ and stops the header
 * dead as C. None of them is caught by any amount of C++ testing.
 *
 * IT ALSO CALLS EVERY FUNCTION, and that is not redundant with the behavioural
 * suite either. Declaring a symbol proves the header parses; CALLING it proves
 * the symbol exists in the archive with C linkage, which is a fact about the
 * build and not about the header. A function that lost its `extern "C"` links
 * fine into the C++ suite and is missing from this one.
 *
 * Correctness is not this file's job -- it asserts almost nothing about values.
 * Its job is that a C compiler and a C linker both accept the whole surface.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "basalt/basalt.h"

static int failures = 0;

static void check(int cond, const char* what) {
  if (!cond) {
    printf("  FAIL  %s\n", what);
    failures++;
  }
}

int main(void) {
  basalt_caps caps;
  basalt_db* db = NULL;
  basalt_batch* batch = NULL;
  basalt_iter* it = NULL;
  basalt_snapshot* snap = NULL;
  basalt_env* env = NULL;
  basalt_status st;
  uint64_t seq = 0;
  uint64_t mark = 0;
  uint64_t bytes = 0;
  size_t needed = 0;
  size_t filled = 0;
  size_t keys_used = 0;
  size_t vals_used = 0;
  uint32_t key_lens[8];
  uint32_t val_lens[8];
  char keys[256];
  char vals[256];
  char value[64];
  int valid = 0;
  const char* dir = "/tmp/basalt-c-abi-test";

  printf("basalt C ABI: compiled by a C compiler\n");

  /* --- version ------------------------------------------------------ */
  check(basalt_version_number() == BASALT_VERSION_NUMBER, "version number");
  check(basalt_version_string() != NULL, "version string");

  /* --- status names ------------------------------------------------- */
  check(strcmp(basalt_status_name(BASALT_OK), "BASALT_OK") == 0,
        "status name of OK");
  check(basalt_status_name(12345) != NULL, "unknown status name is not NULL");

  /* --- caps --------------------------------------------------------- */
  basalt_caps_defaults(&caps);
  check(caps.max_record_bytes > 0, "defaults give a max record size");
  check(basalt_caps_ordered(&caps) != 0, "the shipped defaults are ordered");

  /* THE GAP THE FIRST CONSUMER HID, ASSERTED FROM C. busy_bytes is reachable
   * here, so a small WAL buffer is now expressible: lower the busy threshold
   * with it and the configuration is ordered. Without the field, this exact
   * request is refused and the caller has no way to fix it. */
  caps.wal_buffer_bytes = 1024 * 1024;
  caps.max_record_bytes = 256 * 1024;
  caps.busy_bytes = 512 * 1024;
  check(basalt_caps_ordered(&caps) != 0,
        "a small WAL buffer is expressible from C");

  /* --- open --------------------------------------------------------- */
  basalt_caps_defaults(&caps);
  caps.flush_bytes = 8 * 1024;
  st = basalt_db_open(dir, strlen(dir), &caps, &db);
  check(st == BASALT_OK, "open");
  if (st != BASALT_OK) {
    printf("  cannot continue: open returned %s\n", basalt_status_name(st));
    return 1;
  }

  /* --- batch -------------------------------------------------------- */
  batch = basalt_batch_new();
  check(batch != NULL, "batch_new");
  check(basalt_batch_set(batch, "a", 1, "1", 1) == BASALT_OK, "batch_set");
  check(basalt_batch_set(batch, "b", 1, "2", 1) == BASALT_OK, "batch_set");
  check(basalt_batch_delete(batch, "gone", 4) == BASALT_OK, "batch_delete");
  check(basalt_batch_delete_range(batch, NULL, 0, "a", 1) == BASALT_OK,
        "batch_delete_range");
  check(basalt_batch_count(batch) == 4, "batch_count");
  check(basalt_db_write(db, batch, &seq) == BASALT_OK, "db_write");
  basalt_batch_clear(batch);
  check(basalt_batch_count(batch) == 0, "batch_clear");
  basalt_batch_free(batch);

  /* --- read --------------------------------------------------------- */
  check(basalt_db_get(db, "a", 1, value, sizeof value, &needed) == BASALT_OK,
        "db_get");
  check(needed == 1, "db_get length");
  check(basalt_db_get(db, "nope", 4, value, sizeof value, &needed) ==
            BASALT_NOT_FOUND,
        "db_get missing");
  check(basalt_db_approximate_disk_bytes(db, NULL, 0, NULL, 0, &bytes) ==
            BASALT_OK,
        "approximate_disk_bytes");

  /* --- durability --------------------------------------------------- */
  check(basalt_db_sync(db, &mark) == BASALT_OK, "db_sync");
  check(basalt_db_durable_seq(db) == mark,
        "durable_seq agrees with the watermark");

  /* --- iteration ---------------------------------------------------- */
  check(basalt_db_iter(db, NULL, 0, NULL, 0, &it) == BASALT_OK, "db_iter");
  check(basalt_iter_seek(it, BASALT_SEEK_FIRST, NULL, 0, &valid) == BASALT_OK,
        "iter_seek");
  check(valid == 1, "iter_seek landed");
  st = basalt_iter_block(it, 1, 8, key_lens, val_lens, keys, sizeof keys,
                         &keys_used, vals, sizeof vals, &vals_used, &filled);
  check(st == BASALT_OK, "iter_block");
  check(filled > 0, "iter_block filled something");
  check(basalt_iter_error(it) == BASALT_OK, "iter_error");
  basalt_iter_free(it);

  /* --- snapshots ---------------------------------------------------- */
  check(basalt_db_snapshot(db, &snap) == BASALT_OK, "db_snapshot");
  check(basalt_snapshot_get(snap, "a", 1, value, sizeof value, &needed) ==
            BASALT_OK,
        "snapshot_get");
  check(basalt_snapshot_iter(snap, NULL, 0, NULL, 0, &it) == BASALT_OK,
        "snapshot_iter");
  basalt_iter_free(it);
  check(basalt_snapshot_close(snap) == BASALT_OK, "snapshot_close");

  check(basalt_db_close(db) == BASALT_OK, "db_close");

  /* --- the caller-supplied env path --------------------------------- */
  env = basalt_env_posix();
  check(env != NULL, "env_posix");
  check(basalt_db_open_env(env, dir, strlen(dir), NULL, &db) == BASALT_OK,
        "db_open_env");
  check(basalt_db_close(db) == BASALT_OK, "db_close after open_env");
  basalt_env_free(env);

  /* --- null handling, which must be a code and never a dereference --- */
  check(basalt_db_close(NULL) == BASALT_INVALID_ARGUMENT, "close(NULL)");
  check(basalt_db_sync(NULL, NULL) == BASALT_INVALID_ARGUMENT, "sync(NULL)");
  check(basalt_db_durable_seq(NULL) == 0, "durable_seq(NULL)");
  check(basalt_batch_count(NULL) == 0, "batch_count(NULL)");
  check(basalt_iter_error(NULL) == BASALT_INVALID_ARGUMENT, "iter_error(NULL)");
  check(basalt_snapshot_close(NULL) == BASALT_INVALID_ARGUMENT,
        "snapshot_close(NULL)");
  basalt_batch_free(NULL);
  basalt_iter_free(NULL);
  basalt_env_free(NULL);

  if (failures != 0) {
    printf("  FAILED with %d problem(s)\n", failures);
    return 1;
  }
  printf("  ok  the whole C surface compiles as C, links, and runs\n");
  return 0;
}
