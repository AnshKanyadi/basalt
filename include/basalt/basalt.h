/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Ansh Kanyadi
 *
 * basalt.h -- the C ABI.
 *
 * This is basalt's stable boundary. The C++ headers next to this one
 * (basalt/db.h and the rest) are the library's implementation language; THIS
 * file is what anything else links against. A C compiler must accept it, and
 * test/c_abi_test.c exists to prove one does -- a header only ever compiled as
 * C++ is a C header by intention rather than by evidence.
 *
 * ===========================================================================
 * NO EXCEPTION LEAVES THIS BOUNDARY, AND THE ENFORCEMENT IS THE COMPILER.
 *
 * An exception unwinding out of a C frame into a caller that has no unwinder
 * is undefined behaviour, and the failure is not a crash at the boundary -- it
 * is a corrupted caller stack, diagnosed anywhere.
 *
 * The implementation is built `-fno-exceptions`, so there is nothing to catch:
 * `throw` does not compile, `try` does not compile, and `operator new` ABORTS
 * rather than throwing. A catch-all would have been WEAKER -- it converts an
 * exception into a code and loses what the exception was, and a boundary that
 * reports BASALT_INTERNAL for everything is one where every failure looks the
 * same.
 *
 * The flag is asserted by the implementation itself: src/capi/basalt_c.cc
 * `#error`s if it is compiled with exceptions enabled. That is stronger than
 * reading the build file, because it is checked on the translation unit that
 * would actually be miscompiled.
 *
 * ===========================================================================
 * MEMORY: THE CALLER OWNS EVERY BUFFER, IN BOTH DIRECTIONS.
 *
 *   INTO the library:  (pointer, length), valid for the duration of ONE call.
 *                      The library copies what it needs before returning.
 *   OUT of the library: THE CALLER SUPPLIES THE BUFFER. The library writes into
 *                      it and reports the length it needed; a caller that
 *                      under-sizes is told the exact length and asks again.
 *
 * So the library never hands out memory the caller must free, no allocation
 * crosses the boundary in either direction, and no lifetime is shared. It is a
 * property of the SHAPE and not a rule anyone has to remember.
 *
 * WHY THE SHAPE IS THIS ONE, AND NOT A NICER-LOOKING ONE. A managed-runtime
 * caller -- Go's cgo is the worked example, but the same is true of any runtime
 * with a moving or scanned heap -- may hand a pointer into its own heap to C
 * only if the memory that pointer names contains NO further pointers into that
 * heap. That single rule decides two things here, and it decides them in
 * opposite directions:
 *
 *   basalt_caps IS A STRUCT. It holds four integers and no pointers, so a
 *   caller can build one in its own heap and pass its address. Nothing about it
 *   is hazardous, and four positional integer arguments would have been worse
 *   in every way -- unreadable at the call site and unextendable without
 *   breaking the ABI.
 *
 *   basalt_iter_block TAKES ELEVEN FLAT ARGUMENTS AND NOT AN OUTPUT STRUCT.
 *   A struct holding `char* keys; char* vals; uint32_t* key_lens; ...` is the
 *   obvious tidy shape, and it is precisely the shape such a caller CANNOT
 *   BUILD: the struct would be memory in its heap containing pointers into its
 *   heap. Passing each buffer as its own argument is what keeps the call legal.
 *
 * This is written down as a property of the API rather than as a note about one
 * language, because it is one: the flat form is also what a dlopen'd consumer,
 * an FFI that cannot lay out structs, and a stable ABI all want.
 *
 * ===========================================================================
 * NOTHING IS CALLED BACK. There is no function pointer in this header.
 *
 * There is no OnDurable and no completion callback. What the library offers
 * instead is a BLOCKING basalt_db_sync, which is strictly more primitive: a
 * callback can be built from a poller and a poller cannot be built from a
 * callback. An embedder that wants a callback runs basalt_db_sync on a thread
 * it owns and dispatches the result wherever, and on whatever thread, it wants
 * it dispatched.
 *
 * That is a design position and not a concession. A library that calls out
 * decides for the embedder which thread its code runs on; this one does not
 * have an opinion, because it never has the control flow to have one with.
 *
 * ===========================================================================
 * THREADING. The library is not internally synchronised, with one exception
 * stated where it applies:
 *
 *   basalt_db_sync IS SINGLE-CALLER, AND THE LIBRARY ENFORCES IT BY ABORTING.
 *   It is the path that flushes and compacts, so it is the only path that
 *   appends to the manifest, and two concurrent callers would interleave two
 *   groups into one log. One poller per database.
 *
 * Everything else: concurrent calls on ONE handle are the caller's to serialise.
 * Distinct handles are independent.
 *
 * ===========================================================================
 * HANDLE LIFETIME. Read this; it is the part a C API most often gets wrong and
 * most often documents wrongly.
 *
 *   A CLOSED OR FREED HANDLE IS GONE. Passing it to anything afterwards is
 *   undefined behaviour, exactly as it is for a FILE* after fclose. This
 *   library does NOT detect it and does not claim to: detecting it would mean
 *   never releasing the handle, and a boundary that leaks every handle in order
 *   to diagnose a caller's bug has made a worse trade than the bug.
 *
 *   A NULL HANDLE IS NOT UNDEFINED. Every function that takes a handle returns
 *   BASALT_INVALID_ARGUMENT for NULL rather than dereferencing it, and the
 *   void-returning frees ignore it. So the discipline that makes the rule above
 *   safe is the ordinary one: null the pointer when you release it.
 *
 *   ITERATORS AND SNAPSHOTS DO NOT DIE WITH THE DATABASE. Each holds the
 *   version it was created over, so it keeps serving correct reads after
 *   basalt_db_close, and it must still be freed. This is a guarantee and not an
 *   accident of the implementation -- test/c_api_test.cc asserts it.
 *
 *   A BATCH IS INDEPENDENT OF EVERY DATABASE. It is a value. Build one, write
 *   it to any number of databases, free it when you are done.
 */
#ifndef BASALT_BASALT_H_
#define BASALT_BASALT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ status
 *
 * ONE ENUM, ONE MEANING. Codes 0..9 map 1:1 onto the C++ Status::Code -- a
 * second numbering would be a second source of truth about one fact -- and
 * static_asserts in the implementation hold them together. Codes at 100 and
 * above are the boundary's own and correspond to no engine state. */
typedef enum {
  BASALT_OK = 0,
  BASALT_NOT_FOUND = 1,
  BASALT_RECORD_TOO_LARGE = 2,
  BASALT_WAL_BUFFER_FULL = 3,
  BASALT_IO_ERROR = 4,
  BASALT_DISK_FULL = 5,
  BASALT_CORRUPTION = 6,
  BASALT_KILLED = 7,
  BASALT_INVALID_ARGUMENT = 8,
  /* Backpressure: more has been submitted than the busy threshold and no
   * poller has drained it. THE WRITE WAS NOT APPLIED. Retry after draining --
   * a caller that ignores this makes no progress rather than losing data. */
  BASALT_BUSY = 9,

  /* The boundary itself failed, and no engine state corresponds to it: an
   * allocation that could not be served, or an engine code this build of the
   * boundary cannot name. It is deliberately distinguishable from everything
   * above so it can never be mistaken for a verdict about the data. */
  BASALT_INTERNAL = 100,
  /* The caller's buffer was too small. *needed holds the required length. */
  BASALT_BUFFER_TOO_SMALL = 101
} basalt_status;

/* The spelling of a code, for logs and assertion messages. Never NULL: an
 * unrecognised value returns "BASALT_UNKNOWN" rather than a null a caller would
 * have to check before printing.
 *
 * IT TAKES AN int AND NOT A basalt_status, AND THAT IS NOT A SLIP. This
 * function exists to name a value that MIGHT NOT BE VALID -- that is what the
 * "BASALT_UNKNOWN" answer is for -- and a caller holding such a value has it
 * from a struct field, a socket, a log line or a differently-versioned build of
 * this library. Declaring the parameter as the enum would make the act of
 * PASSING that value undefined behaviour before this function ran at all: in
 * C++ an enum without a fixed underlying type has a valid range bounded by its
 * enumerators, and loading anything outside it is UB that UndefinedBehavior-
 * Sanitizer diagnoses. The fallback would be unreachable except by the route
 * that made the program undefined.
 *
 * Passing an enumerator still works unchanged; it promotes. */
const char* basalt_status_name(int status);

/* ------------------------------------------------------------------- caps
 *
 * The engine's thresholds. Fill from basalt_caps_defaults and change what you
 * mean to change.
 *
 * THERE IS NO "0 MEANS DEFAULT" RULE HERE, AND ITS ABSENCE IS THE POINT.
 * busy_bytes = 0 is a MEANINGFUL VALUE -- it disables backpressure -- so a
 * sentinel that read 0 as "give me the default" would make the one setting a
 * caller most wants to turn off the one setting a caller cannot turn off.
 * Every field means what it says; basalt_caps_defaults is how you find out what
 * the shipped numbers are. */
typedef struct basalt_caps {
  uint64_t max_record_bytes;
  uint64_t wal_buffer_bytes;
  uint64_t flush_bytes;
  uint64_t busy_bytes; /* 0 disables backpressure. A value, not a sentinel. */
} basalt_caps;

/* Writes the shipped defaults into *out. */
void basalt_caps_defaults(basalt_caps* out);

/* Whether these caps satisfy the engine's ordering rule, which basalt_db_open
 * would otherwise refuse with BASALT_INVALID_ARGUMENT:
 *
 *   wal_buffer_bytes >= 2 * max_record_bytes, and
 *   busy_bytes + max_record_bytes <= wal_buffer_bytes  (unless busy_bytes == 0)
 *
 * Exposed because a caller that is refused deserves to be able to ask WHICH
 * combination was wrong before it reaches an open. Returns non-zero for ok. */
int basalt_caps_ordered(const basalt_caps* caps);

/* ---------------------------------------------------------------- handles
 *
 * Opaque. No C++ type is nameable from C, and no caller pointer is ever
 * retained by the library. */
typedef struct basalt_env basalt_env;
typedef struct basalt_db basalt_db;
typedef struct basalt_batch basalt_batch;
typedef struct basalt_iter basalt_iter;
typedef struct basalt_snapshot basalt_snapshot;

/* ---------------------------------------------------------------- the env
 *
 * Every file operation the engine performs goes through an Env. The production
 * one talks to the filesystem; a test harness supplies one that can inject
 * faults and be killed at a chosen call. Exposing it here is what lets a C
 * consumer be tested to the same depth as a C++ one -- see basalt/basalt_cxx.h
 * for how a harness supplies its own.
 *
 * Returns NULL only if the allocation failed. */
basalt_env* basalt_env_posix(void);

/* Releases an env handle. Ignores NULL.
 *
 * AN ENV MUST OUTLIVE EVERY DATABASE OPENED OVER IT. basalt_db_open owns the
 * env it creates for itself and this does not apply; it applies to an env the
 * caller made and passed to basalt_db_open_env. */
void basalt_env_free(basalt_env* env);

/* --------------------------------------------------------------- database */

/* Opens or creates a database at `dir`, over the production filesystem env,
 * which the returned handle owns and releases on close.
 *
 * `caps` may be NULL, which means the shipped defaults. The directory is
 * created if it does not exist. `dir` need not be NUL-terminated; `dir_len` is
 * authoritative. */
basalt_status basalt_db_open(const char* dir, size_t dir_len,
                             const basalt_caps* caps, basalt_db** out);

/* Opens over an env the caller supplies and continues to own.
 *
 * THE ENV IS BORROWED, NOT TAKEN. It must outlive the returned database, and
 * the caller frees it afterwards. Splitting this from basalt_db_open rather
 * than making the env a nullable parameter of it keeps ownership legible at the
 * call site: one function always owns its env and one never does, and neither
 * has a mode. */
basalt_status basalt_db_open_env(basalt_env* env, const char* dir,
                                 size_t dir_len, const basalt_caps* caps,
                                 basalt_db** out);

/* Closes the database and releases the handle, which is invalid afterwards.
 *
 * DOES NOT SYNC, DELIBERATELY. The watermark is the engine's only durability
 * promise; a close that synced would make clean shutdown a hidden durability
 * event, and close-then-reopen would stop being indistinguishable from
 * kill-then-reopen. Call basalt_db_sync first if you want the tail durable.
 *
 * The status describes the close; the handle is released either way. */
basalt_status basalt_db_close(basalt_db* db);

/* Blocks until everything applied so far is durable, and reports the watermark
 * -- the highest sequence guaranteed to survive a crash.
 *
 * SINGLE-CALLER: see THREADING above. A second concurrent caller aborts.
 * `watermark` may be NULL. */
basalt_status basalt_db_sync(basalt_db* db, uint64_t* watermark);

/* The current durable watermark, without blocking. Monotone non-decreasing.
 * Returns 0 for a NULL handle, which is also the value before anything is
 * durable -- this call has no error channel, and inventing one for the null
 * case would be a worse API than the honest ambiguity at zero. */
uint64_t basalt_db_durable_seq(const basalt_db* db);

/* Reads one key into a caller buffer.
 *
 * Returns BASALT_NOT_FOUND if absent, BASALT_BUFFER_TOO_SMALL if `cap` is
 * short. *needed is set to the value's length on BASALT_OK AND on
 * BASALT_BUFFER_TOO_SMALL, so one grow-and-retry always suffices and a caller
 * never has to guess how much to grow by. `needed` may be NULL.
 *
 * `value_out` may be NULL when `cap` is 0, which is how you ask for the length
 * alone. */
basalt_status basalt_db_get(const basalt_db* db, const char* key,
                            size_t key_len, char* value_out, size_t cap,
                            size_t* needed);

/* Bytes the live entries in [start, end) occupy, as the engine accounts them.
 *
 * READ WHAT THIS ACTUALLY COUNTS BEFORE USING IT FOR A DISK DECISION. It walks
 * the merged view and sums the key and value bytes visible at the current
 * sequence. So it is exact, O(n) in the live entries in range, and it is LIVE
 * LOGICAL BYTES rather than bytes on disk -- it counts no block framing, no
 * filter, no footer, and nothing an overwritten or deleted version still
 * occupies in a file it has not been compacted out of.
 *
 * NULL start or end means unbounded on that side; see the bounds rule below. */
basalt_status basalt_db_approximate_disk_bytes(const basalt_db* db,
                                               const char* start,
                                               size_t start_len,
                                               const char* end, size_t end_len,
                                               uint64_t* out);

/* ------------------------------------------------------------------ batch
 *
 * ONE CALL COMMITS A WHOLE BATCH, and that is the interface's point. A boundary
 * crossing costs something in every FFI; a batch amortises it over as many
 * operations as the caller cares to put in one.
 *
 * A batch is a value with no connection to any database. Build it, write it as
 * many times as you like, free it. */

basalt_batch* basalt_batch_new(void);    /* NULL only on allocation failure */
void basalt_batch_free(basalt_batch* b); /* ignores NULL */

/* THE BOUNDS RULE, WHICH APPLIES TO EVERY (pointer, length) PAIR BELOW.
 *
 *   For a KEY or a VALUE: NULL is BASALT_INVALID_ARGUMENT. A non-NULL pointer
 *   with length 0 is the EMPTY key or the EMPTY value, both of which are legal.
 *
 *   For a BOUND -- the start/end of a range, the lower/upper of an iterator --
 *   NULL means UNBOUNDED, and a non-NULL pointer with length 0 is the EMPTY
 *   KEY, which is a perfectly ordinary key that sorts first.
 *
 * The two cases differ because only a bound has an unbounded case to express,
 * and there is no byte string that means "no bound" -- so for bounds the
 * distinction has to live in the pointer. Getting these two confused is a real
 * defect and not a hypothetical one: it is why they are stated separately here
 * rather than left to a reader to infer from one example. */

basalt_status basalt_batch_set(basalt_batch* b, const char* key, size_t key_len,
                               const char* value, size_t value_len);
basalt_status basalt_batch_delete(basalt_batch* b, const char* key,
                                  size_t key_len);
/* Deletes [start, end). Either bound may be NULL for unbounded. */
basalt_status basalt_batch_delete_range(basalt_batch* b, const char* start,
                                        size_t start_len, const char* end,
                                        size_t end_len);

/* How many operations the batch holds. 0 for NULL. */
size_t basalt_batch_count(const basalt_batch* b);
/* Empties the batch for reuse, keeping its allocation. Ignores NULL. */
void basalt_batch_clear(basalt_batch* b);

/* Applies the batch and reports the sequence at which it became visible.
 *
 * NEVER BLOCKS ON I/O. Visibility and durability are separate here: this makes
 * the writes readable, basalt_db_sync makes them survive a crash. `seq` may be
 * NULL. */
basalt_status basalt_db_write(basalt_db* db, const basalt_batch* b,
                              uint64_t* seq);

/* -------------------------------------------------------------- iterators
 *
 * BLOCKS OF PAIRS PER CALL, for the same reason the batch exists. The block
 * size is the caller's `n` and is a PARAMETER rather than a constant, because
 * the right value depends on the caller's crossing cost and nothing here knows
 * what that is.
 *
 * POSITIONING IS SEPARATE FROM FETCHING, and that split is what lets a block
 * interface serve a cursor. A caller that wants a cursor seeks once and then
 * fetches blocks in a direction, serving individual entries out of the block it
 * holds; a caller that wants a scan just fetches. Neither has to buffer the
 * whole iteration to get the other's shape. */

/* `lower` and `upper` are BOUNDS: NULL means unbounded. See the bounds rule. */
basalt_status basalt_db_iter(const basalt_db* db, const char* lower,
                             size_t lower_len, const char* upper,
                             size_t upper_len, basalt_iter** out);

/* Closes and releases. Ignores NULL. Outlives its database -- see HANDLE
 * LIFETIME. */
void basalt_iter_free(basalt_iter* it);

typedef enum {
  BASALT_SEEK_FIRST = 0,
  BASALT_SEEK_LAST = 1,
  BASALT_SEEK_GE = 2,
  BASALT_SEEK_LT = 3
} basalt_seek_mode;

/* Positions the cursor. `key` is ignored for FIRST and LAST and required for GE
 * and LT. *valid reports whether the cursor landed on an entry; it may be NULL.
 *
 * The entry the seek lands on HAS NOT BEEN CONSUMED: the next block starts with
 * it. A cursor that silently skipped the entry it was asked to seek to would be
 * a wrong answer with nothing to catch it.
 *
 * `mode` IS AN int TAKING A basalt_seek_mode VALUE, for the reason spelled out
 * at basalt_status_name: a value crossing an ABI is whatever the caller sent,
 * and a mode outside the enumerated set must be REFUSED with
 * BASALT_INVALID_ARGUMENT rather than being undefined the moment it is read.
 * An enum-typed parameter would make the refusal arm reachable only through
 * undefined behaviour, which is a validation you cannot actually perform. */
basalt_status basalt_iter_seek(basalt_iter* it, int mode, const char* key,
                               size_t key_len, int* valid);

/* Fills up to `n` pairs into caller memory.
 *
 *   key_lens[i], val_lens[i]   the lengths of pair i
 *   keys, vals                 the bytes, packed back to back
 *
 * *filled receives how many pairs were written, *keys_used and *vals_used how
 * many bytes. `forward` non-zero walks ascending, zero descending. The first
 * pair of a block is the one the cursor is currently on.
 *
 * NO PAIR IS EVER DROPPED AND NO POSITION IS EVER LOST, which is the whole of
 * the short-block contract:
 *
 *   If the next pair does not fit and NOTHING has been filled, the call returns
 *   BASALT_BUFFER_TOO_SMALL having consumed nothing, and *keys_used /
 *   *vals_used carry the capacities THAT PAIR NEEDS -- not the capacities used
 *   -- so one grow-and-retry always suffices.
 *
 *   If the next pair does not fit and something HAS been filled, the short
 *   block is returned as BASALT_OK and the pair that did not fit is held for
 *   the next call.
 *
 * Eleven arguments rather than an output struct: see MEMORY above. */
basalt_status basalt_iter_block(basalt_iter* it, int forward, size_t n,
                                uint32_t* key_lens, uint32_t* val_lens,
                                char* keys, size_t keys_cap, size_t* keys_used,
                                char* vals, size_t vals_cap, size_t* vals_used,
                                size_t* filled);

/* Whether the iteration has failed.
 *
 * A BLOCK CALL RETURNING ZERO PAIRS IS AMBIGUOUS ON ITS OWN -- it means either
 * "the range is exhausted" or "a read failed" -- and a caller that cannot tell
 * those apart will treat a corrupt table as an empty one. This is how you tell.
 * BASALT_OK means the iteration is healthy, exhausted or not. */
basalt_status basalt_iter_error(const basalt_iter* it);

/* -------------------------------------------------------------- snapshots
 *
 * A pinned view. Reads through it see the state as of the moment it was taken,
 * and it HOLDS THAT VERSION AGAINST COMPACTION until it is closed -- a
 * compaction may not drop a version this snapshot can still read. */

basalt_status basalt_db_snapshot(basalt_db* db, basalt_snapshot** out);

/* Closes and releases. The handle is invalid afterwards. */
basalt_status basalt_snapshot_close(basalt_snapshot* s);

/* As basalt_db_get, against the pinned view. */
basalt_status basalt_snapshot_get(const basalt_snapshot* s, const char* key,
                                  size_t key_len, char* value_out, size_t cap,
                                  size_t* needed);

/* As basalt_db_iter, against the pinned view.
 *
 * The iterator holds the snapshot's version, so it stays valid after the
 * snapshot is closed and must still be freed. A snapshot you can only point-
 * read is a much weaker thing than a snapshot: repeatable range reads are most
 * of what a pinned view is for. */
basalt_status basalt_snapshot_iter(const basalt_snapshot* s, const char* lower,
                                   size_t lower_len, const char* upper,
                                   size_t upper_len, basalt_iter** out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* BASALT_BASALT_H_ */
