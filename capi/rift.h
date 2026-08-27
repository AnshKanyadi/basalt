/* THE C BOUNDARY. extern "C", error codes, and no C++ type crosses.
 *
 * ---------------------------------------------------------------------------
 * NO EXCEPTION CROSSES THIS BOUNDARY, AND IT IS ENFORCED RATHER THAN PROMISED.
 *
 * An exception unwinding through a C frame into Go is undefined behaviour, and
 * the failure is not a crash at the boundary -- it is a corrupted Go stack,
 * diagnosed anywhere. Every entry point below is wrapped in a catch-all that
 * converts to RIFT_INTERNAL.
 *
 * THE CATCH IS A BACKSTOP AND NOT THE DESIGN. It converts an exception into a
 * code and LOSES WHAT IT WAS, so the engine's own discipline stands: this
 * engine does not throw, RIFT_CHECK aborts rather than throws, and Status is
 * the error channel everywhere. The catch is for std::bad_alloc and for a
 * future contributor.
 *
 * ---------------------------------------------------------------------------
 * BUFFER OWNERSHIP, AND THE RULE THAT MAKES cgo's POINTER RULE UNVIOLATABLE.
 *
 * C may not store a Go pointer beyond the call. The design that CANNOT violate
 * that is the one where C never receives a Go pointer it could store:
 *
 *   INTO the engine: (ptr, len) valid for one call, COPIED at the boundary.
 *   OUT of the engine: THE CALLER SUPPLIES THE BUFFER. The engine writes into
 *   it and returns the length it needed; a caller that under-sizes is told the
 *   length and asks again.
 *
 * So nothing the engine allocates ever reaches the caller, nothing must be
 * freed across the boundary, and no lifetime is shared. It is a property of the
 * shape rather than a rule someone has to remember.
 *
 * ---------------------------------------------------------------------------
 * NO C-TO-GO CALLBACKS. `[A1]` prohibits them. There is no `OnDurable` here;
 * there is a blocking `rift_db_sync`, and db.h's divergence 1 records why that
 * is the more primitive of the two: a callback can be built from a poller and a
 * poller cannot be built from a callback.
 */
#ifndef RIFT_CAPI_RIFT_H_
#define RIFT_CAPI_RIFT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ONE ENUM, ONE MEANING. These map 1:1 onto Status::Code -- a second numbering
 * would be a second source of truth about one fact, and a static_assert in the
 * implementation holds them together. */
typedef enum {
  RIFT_OK = 0,
  RIFT_NOT_FOUND = 1,
  RIFT_RECORD_TOO_LARGE = 2,
  RIFT_WAL_BUFFER_FULL = 3,
  RIFT_IO_ERROR = 4,
  RIFT_DISK_FULL = 5,
  RIFT_CORRUPTION = 6,
  RIFT_KILLED = 7,
  RIFT_INVALID_ARGUMENT = 8,
  /* NOT A Status::Code. It is the boundary's own: an exception reached the
   * catch-all, or a handle was used after it was closed. It exists so those
   * are DISTINGUISHABLE from anything the engine can report. */
  RIFT_INTERNAL = 100,
  /* The caller's buffer was too small; *needed holds the required length. */
  RIFT_BUFFER_TOO_SMALL = 101
} rift_status;

/* Opaque handles. A C++ object is never named by the caller and a caller
 * pointer is never stored by C. */
typedef struct rift_db rift_db;
typedef struct rift_batch rift_batch;
typedef struct rift_iter rift_iter;
typedef struct rift_snapshot rift_snapshot;

/* ------------------------------------------------------------------ database */

/* `caps_*` of 0 mean "the shipped default", so a caller that does not care
 * cannot accidentally configure a regime. */
rift_status rift_db_open(const char* dir, size_t dir_len, uint64_t caps_flush_bytes,
                         uint64_t caps_wal_buffer_bytes, uint64_t caps_max_record_bytes,
                         rift_db** out);
rift_status rift_db_close(rift_db* db);

/* Blocking. B5's poller owns this; see the header note and DESIGN-B5 section 4.
 * SINGLE-CALLER, and the engine aborts on a second concurrent one. */
rift_status rift_db_sync(rift_db* db, uint64_t* watermark);

uint64_t rift_db_durable_seq(const rift_db* db);

/* Reads into a caller buffer. Returns RIFT_BUFFER_TOO_SMALL and sets *needed
 * when `cap` is short; *needed is always set on RIFT_OK too, so a caller can
 * check it either way. */
rift_status rift_db_get(const rift_db* db, const char* key, size_t key_len,
                        char* value_out, size_t cap, size_t* needed);

/* --------------------------------------------------------------------- batch
 *
 * ONE CALL COMMITS A WHOLE BATCH. That is the interface's whole point: per-call
 * cgo overhead is real, and BENCHMARKS.md must measure what this amortises. */

rift_batch* rift_batch_new(void);
void rift_batch_free(rift_batch* b);
rift_status rift_batch_set(rift_batch* b, const char* key, size_t key_len,
                           const char* value, size_t value_len);
rift_status rift_batch_delete(rift_batch* b, const char* key, size_t key_len);
/* `start`/`end` NULL means UNBOUNDED. An empty non-NULL pointer is the EMPTY
 * KEY, which is a valid key -- db.h's divergence 3 surviving into C, where the
 * distinction is carried by the pointer rather than by a flag. */
rift_status rift_batch_delete_range(rift_batch* b, const char* start, size_t start_len,
                                    const char* end, size_t end_len);
rift_status rift_db_write(rift_db* db, rift_batch* b, uint64_t* seq);

/* ----------------------------------------------------------------- iterators
 *
 * BLOCKS OF N PAIRS PER CALL, for the same reason the batch exists. The block
 * size is the caller's `n` and is a PARAMETER rather than a constant, because
 * BENCHMARKS.md is supposed to find it rather than assume it. */

rift_status rift_db_iter(const rift_db* db, const char* lower, size_t lower_len,
                         const char* upper, size_t upper_len, rift_iter** out);
void rift_iter_free(rift_iter* it);

/* Fills up to `n` pairs into caller memory laid out as:
 *   key_lens[i], val_lens[i]  -- the lengths of pair i
 *   keys, vals                -- the bytes, packed back to back
 * Sets *filled to how many pairs were written and *keys_used / *vals_used to
 * how many bytes. Returns RIFT_BUFFER_TOO_SMALL without consuming anything if
 * the NEXT pair does not fit, so a caller can grow and retry without losing a
 * position. */
rift_status rift_iter_next_block(rift_iter* it, size_t n,
                                 uint32_t* key_lens, uint32_t* val_lens,
                                 char* keys, size_t keys_cap, size_t* keys_used,
                                 char* vals, size_t vals_cap, size_t* vals_used,
                                 size_t* filled);

/* ----------------------------------------------------------------- snapshots */

rift_status rift_db_snapshot(rift_db* db, rift_snapshot** out);
rift_status rift_snapshot_close(rift_snapshot* s);
rift_status rift_snapshot_get(const rift_snapshot* s, const char* key, size_t key_len,
                              char* value_out, size_t cap, size_t* needed);

/* ------------------------------------------------------------------ testing
 *
 * THE BACKSTOP MUST BE SEEN TO FIRE. A backstop nobody has watched fire is a
 * backstop nobody has tested, so this throws through a boundary function on
 * purpose and the caller asserts RIFT_INTERNAL comes back. It is compiled
 * always -- a test-only build would test a different binary. */
rift_status rift_test_throw(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* RIFT_CAPI_RIFT_H_ */
