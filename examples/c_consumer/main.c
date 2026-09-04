/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ansh Kanyadi
 *
 * The smallest honest use of basalt from C: open, write, sync, read, iterate,
 * close. It asserts rather than prints so it can be a CI step, and it returns
 * non-zero on any failure.
 */
#include <basalt/basalt.h>

#include <stdio.h>
#include <string.h>

static int fail(const char* what, basalt_status s) {
  printf("  FAIL  %s -> %s\n", what, basalt_status_name(s));
  return 1;
}

int main(void) {
  const char* dir = "/tmp/basalt-c-consumer-db";
  basalt_caps caps;
  basalt_db* db = NULL;
  basalt_batch* b = NULL;
  basalt_iter* it = NULL;
  basalt_status s;
  uint64_t seq = 0, mark = 0;
  char value[64];
  size_t needed = 0;
  int valid = 0;
  uint32_t key_lens[4], val_lens[4];
  char keys[128], vals[128];
  size_t filled = 0, keys_used = 0, vals_used = 0;

  printf("basalt %s, linked from an installed package by a C program\n",
         basalt_version_string());

  /* The header and the archive must agree; see basalt/basalt.h on versions. */
  if (basalt_version_number() != BASALT_VERSION_NUMBER) {
    printf("  FAIL  header says %d, library says %d\n", BASALT_VERSION_NUMBER,
           basalt_version_number());
    return 1;
  }

  basalt_caps_defaults(&caps);
  caps.flush_bytes = 64 * 1024;
  if (!basalt_caps_ordered(&caps)) return fail("caps", BASALT_INVALID_ARGUMENT);

  s = basalt_db_open(dir, strlen(dir), &caps, &db);
  if (s != BASALT_OK) return fail("open", s);

  b = basalt_batch_new();
  if (b == NULL) return fail("batch_new", BASALT_INTERNAL);
  basalt_batch_set(b, "alpha", 5, "one", 3);
  basalt_batch_set(b, "beta", 4, "two", 3);
  s = basalt_db_write(db, b, &seq);
  basalt_batch_free(b);
  if (s != BASALT_OK) return fail("write", s);

  s = basalt_db_sync(db, &mark);
  if (s != BASALT_OK) return fail("sync", s);
  if (mark < seq) return fail("watermark below the write", BASALT_INTERNAL);

  s = basalt_db_get(db, "alpha", 5, value, sizeof value, &needed);
  if (s != BASALT_OK) return fail("get", s);
  if (needed != 3 || memcmp(value, "one", 3) != 0)
    return fail("get returned the wrong value", BASALT_INTERNAL);

  s = basalt_db_iter(db, NULL, 0, NULL, 0, &it);
  if (s != BASALT_OK) return fail("iter", s);
  s = basalt_iter_seek(it, BASALT_SEEK_FIRST, NULL, 0, &valid);
  if (s != BASALT_OK || !valid) return fail("seek", s);
  s = basalt_iter_block(it, 1, 4, key_lens, val_lens, keys, sizeof keys,
                        &keys_used, vals, sizeof vals, &vals_used, &filled);
  if (s != BASALT_OK) return fail("block", s);
  if (filled != 2) {
    printf("  FAIL  expected 2 pairs, got %zu\n", filled);
    return 1;
  }
  s = basalt_iter_error(it);
  basalt_iter_free(it);
  if (s != BASALT_OK) return fail("iter_error", s);

  s = basalt_db_close(db);
  if (s != BASALT_OK) return fail("close", s);

  printf("  ok  open, write, sync, get, iterate, close -- all from C\n");
  return 0;
}
