// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ansh Kanyadi
//
// The C ABI's implementation. The contract is in include/basalt/basalt.h; this
// file is about how it is held.

#include "basalt/basalt.h"

#include <cstring>
#include <memory>
#include <string>

#include "basalt/basalt_cxx.h"
#include "basalt/caps.h"
#include "basalt/check.h"
#include "basalt/db.h"
#include "basalt/env.h"
#include "basalt/posix_env.h"
#include "basalt/status.h"

// NO EXCEPTION LEAVES THIS BOUNDARY, AND THIS IS WHERE THAT IS CHECKED.
//
// The claim is about a compiler flag, and a flag cannot be asserted by any line
// of code it compiles -- so it is asserted by a line that REFUSES to compile
// without it. That is strictly stronger than a build-system lane grepping
// CMakeLists.txt for `-fno-exceptions`, which is how this was checked before
// this library existed: the grep confirms the flag is written down, this
// confirms it arrived at the translation unit that would be miscompiled
// without it. The two failures are not the same failure -- a target that stops
// picking up the flag list keeps the flag written down.
#if defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#error \
    "basalt_c must be compiled -fno-exceptions: no exception may cross the C ABI"
#endif

namespace {

using basalt::Bound;
using basalt::DB;
using basalt::Env;
using basalt::Iterator;
using basalt::IterOptions;
using basalt::Slice;
using basalt::Snapshot;
using basalt::Status;
using basalt::WriteBatch;

// ONE ENUM, ONE MEANING, AND THE COMPILER HOLDS THEM TOGETHER. A second
// numbering would be a second source of truth about one fact; these asserts are
// what stop the two drifting when a code is added between them.
static_assert(static_cast<int>(Status::Code::kOk) == BASALT_OK, "");
static_assert(static_cast<int>(Status::Code::kNotFound) == BASALT_NOT_FOUND,
              "");
static_assert(static_cast<int>(Status::Code::kRecordTooLarge) ==
                  BASALT_RECORD_TOO_LARGE,
              "");
static_assert(static_cast<int>(Status::Code::kWalBufferFull) ==
                  BASALT_WAL_BUFFER_FULL,
              "");
static_assert(static_cast<int>(Status::Code::kIoError) == BASALT_IO_ERROR, "");
static_assert(static_cast<int>(Status::Code::kDiskFull) == BASALT_DISK_FULL,
              "");
static_assert(static_cast<int>(Status::Code::kCorruption) == BASALT_CORRUPTION,
              "");
static_assert(static_cast<int>(Status::Code::kKilled) == BASALT_KILLED, "");
static_assert(static_cast<int>(Status::Code::kInvalidArgument) ==
                  BASALT_INVALID_ARGUMENT,
              "");
static_assert(static_cast<int>(Status::Code::kBusy) == BASALT_BUSY, "");

// A SWITCH AND NOT A CAST, AND THE DIFFERENCE IS THE WHOLE POINT OF THE
// FUNCTION. This is the one place two independently-declared enums have to
// agree, and a static_cast agrees with anything.
//
// The static_asserts above pin each code that EXISTS. Nothing there pins that
// the set is COMPLETE -- an assertion about the members present cannot fail on
// a member added. -Werror=switch can, and it is the mechanism status.h already
// relies on for exactly this: kBusy was added to Status::Code in the parent
// project long after the first boundary was written, that boundary compiled
// without a word, and the new code crossed as an integer no C header named.
basalt_status ToC(const Status& s) {
  switch (s.code()) {  // NO default: arm -- a new code must be mapped here
    case Status::Code::kOk:
      return BASALT_OK;
    case Status::Code::kNotFound:
      return BASALT_NOT_FOUND;
    case Status::Code::kRecordTooLarge:
      return BASALT_RECORD_TOO_LARGE;
    case Status::Code::kWalBufferFull:
      return BASALT_WAL_BUFFER_FULL;
    case Status::Code::kIoError:
      return BASALT_IO_ERROR;
    case Status::Code::kDiskFull:
      return BASALT_DISK_FULL;
    case Status::Code::kCorruption:
      return BASALT_CORRUPTION;
    case Status::Code::kKilled:
      return BASALT_KILLED;
    case Status::Code::kInvalidArgument:
      return BASALT_INVALID_ARGUMENT;
    case Status::Code::kBusy:
      return BASALT_BUSY;
  }
  // Unreachable while the switch is exhaustive, and BASALT_INTERNAL rather than
  // a cast if it ever is not: an unmapped code must arrive as the boundary's
  // own "something is wrong here", never as a number that looks like a verdict.
  return BASALT_INTERNAL;
}

// A Bound from a possibly-null pointer. NULL IS UNBOUNDED; a non-null pointer
// with length zero is THE EMPTY KEY, which is a valid key. The distinction has
// to live in the pointer because there is no byte string meaning "no bound".
Bound BoundOf(const char* p, size_t n) {
  if (p == nullptr) return Bound::Unbounded();
  return Bound::At(Slice(p, n));
}

// Copies a value into a caller buffer, reporting the length either way.
//
// *needed IS SET BEFORE THE CAPACITY IS CHECKED, deliberately: a caller told
// only "too small" has to GUESS how much to grow by, and a guess that is still
// too small loops. Reporting the requirement makes the retry exact and makes
// the caller's loop terminate in one step.
basalt_status Deliver(const std::string& v, char* out, size_t cap,
                      size_t* needed) {
  if (needed != nullptr) *needed = v.size();
  if (v.size() > cap) return BASALT_BUFFER_TOO_SMALL;
  if (!v.empty()) std::memcpy(out, v.data(), v.size());
  return BASALT_OK;
}

basalt::wal::Caps CapsOf(const basalt_caps* c) {
  basalt::wal::Caps out;
  if (c == nullptr) return out;  // NULL means the shipped defaults
  out.max_record_bytes = c->max_record_bytes;
  out.wal_buffer_bytes = c->wal_buffer_bytes;
  out.flush_bytes = c->flush_bytes;
  out.busy_bytes = c->busy_bytes;
  return out;
}

}  // namespace

// The handles. Definitions live here so the header can keep them opaque and no
// C++ type is nameable from C.

// AN ENV HANDLE IS EITHER OWNED OR BORROWED, AND IT REMEMBERS WHICH.
//
// basalt_env_posix makes one this handle owns; BorrowEnv wraps one it does not.
// A single `Env*` plus a flag would work and would put the decision at every
// use site; a unique_ptr that is simply empty for the borrowed case puts it at
// the one site that matters -- the destructor -- and makes "borrowed" mean
// "there is nothing here to delete" rather than "remember not to".
struct basalt_env {
  std::unique_ptr<Env> owned;
  Env* env = nullptr;
};

struct basalt_db {
  // Non-null only when this database made its own env, which is what makes
  // basalt_db_open and basalt_db_open_env differ by exactly one line.
  std::unique_ptr<Env> owned_env;
  std::unique_ptr<DB> db;
};

struct basalt_batch {
  WriteBatch b;
};

struct basalt_iter {
  std::unique_ptr<Iterator> it;
  // POSITIONED means a seek has placed the cursor and the entry under it has
  // not yet been returned. A cursor that silently skipped the entry it was
  // asked to seek to is a wrong answer with no failing structure anywhere.
  bool positioned = false;
  // A PAIR THE CURSOR HAS ALREADY MOVED PAST BUT THE CALLER'S BUFFER COULD NOT
  // HOLD. Held rather than dropped: reporting a short block and losing the pair
  // makes the iterator skip exactly when a caller's buffer is tight, which is
  // the least visible way to lose data.
  std::string pending_key;
  std::string pending_value;
  bool has_pending = false;
};

struct basalt_snapshot {
  std::unique_ptr<Snapshot> s;
};

namespace basalt {

basalt_env* BorrowEnv(Env* env) {
  if (env == nullptr) return nullptr;
  auto* h = new (std::nothrow) basalt_env();
  if (h == nullptr) return nullptr;
  h->env = env;  // owned stays empty: nothing here to delete
  return h;
}

}  // namespace basalt

extern "C" {

const char* basalt_status_name(int status) {
  // A switch over the INT, so an out-of-range value is an ordinary integer this
  // function declines to name rather than a value it was undefined to receive.
  switch (status) {
    case BASALT_OK:
      return "BASALT_OK";
    case BASALT_NOT_FOUND:
      return "BASALT_NOT_FOUND";
    case BASALT_RECORD_TOO_LARGE:
      return "BASALT_RECORD_TOO_LARGE";
    case BASALT_WAL_BUFFER_FULL:
      return "BASALT_WAL_BUFFER_FULL";
    case BASALT_IO_ERROR:
      return "BASALT_IO_ERROR";
    case BASALT_DISK_FULL:
      return "BASALT_DISK_FULL";
    case BASALT_CORRUPTION:
      return "BASALT_CORRUPTION";
    case BASALT_KILLED:
      return "BASALT_KILLED";
    case BASALT_INVALID_ARGUMENT:
      return "BASALT_INVALID_ARGUMENT";
    case BASALT_BUSY:
      return "BASALT_BUSY";
    case BASALT_INTERNAL:
      return "BASALT_INTERNAL";
    case BASALT_BUFFER_TOO_SMALL:
      return "BASALT_BUFFER_TOO_SMALL";
    default:
      break;
  }
  // NEVER NULL. A caller printing this has no reason to null-check a name, and
  // the one thing worse than an unrecognised code is a crash while reporting
  // one.
  return "BASALT_UNKNOWN";
}

void basalt_caps_defaults(basalt_caps* out) {
  if (out == nullptr) return;
  const basalt::wal::Caps d;
  out->max_record_bytes = d.max_record_bytes;
  out->wal_buffer_bytes = d.wal_buffer_bytes;
  out->flush_bytes = d.flush_bytes;
  out->busy_bytes = d.busy_bytes;
}

int basalt_caps_ordered(const basalt_caps* caps) {
  return CapsOf(caps).Ordered() ? 1 : 0;
}

/* ------------------------------------------------------------------- env */

basalt_env* basalt_env_posix(void) {
  auto* h = new (std::nothrow) basalt_env();
  if (h == nullptr) return nullptr;
  h->owned = basalt::NewPosixEnv();
  h->env = h->owned.get();
  return h;
}

void basalt_env_free(basalt_env* env) { delete env; }

}  // extern "C"

/* -------------------------------------------------------------- database */

// THE HELPERS BELOW ARE NOT INSIDE THE `extern "C"` BLOCK, and that is not a
// tidiness choice. A function with C linkage may not return a C++ class type --
// `OptionsOf` returning IterOptions is an error under -Wreturn-type-c-linkage,
// which is in this build's -Werror set. The internal helpers of a C boundary
// stay C++; only the boundary itself is C.
namespace {

basalt_status OpenInto(Env* env, const char* dir, size_t dir_len,
                       const basalt_caps* caps, basalt_db* handle) {
  const basalt::wal::Caps c = CapsOf(caps);
  // REFUSED HERE RATHER THAN DEEPER, so the caller gets the one code that means
  // "your configuration is inconsistent" instead of a failure from whichever
  // component first tripped over it. basalt_caps_ordered exposes the same
  // predicate so a caller can ask before it opens.
  if (!c.Ordered()) return BASALT_INVALID_ARGUMENT;
  const std::string path(dir, dir_len);
  (void)env->CreateDir(path);
  std::unique_ptr<DB> db;
  const Status s = DB::Open(env, path, c, &db);
  if (!s.ok()) return ToC(s);
  handle->db = std::move(db);
  return BASALT_OK;
}

basalt_status IterInto(std::unique_ptr<Iterator> it, basalt_iter** out) {
  auto handle = std::unique_ptr<basalt_iter>(new (std::nothrow) basalt_iter());
  if (handle == nullptr) return BASALT_INTERNAL;
  handle->it = std::move(it);
  *out = handle.release();
  return BASALT_OK;
}

IterOptions OptionsOf(const char* lower, size_t lower_len, const char* upper,
                      size_t upper_len) {
  IterOptions o;
  o.lower = BoundOf(lower, lower_len);
  o.upper = BoundOf(upper, upper_len);
  return o;
}

}  // namespace

extern "C" {

basalt_status basalt_db_open(const char* dir, size_t dir_len,
                             const basalt_caps* caps, basalt_db** out) {
  if (dir == nullptr || out == nullptr) return BASALT_INVALID_ARGUMENT;
  *out = nullptr;
  auto handle = std::unique_ptr<basalt_db>(new (std::nothrow) basalt_db());
  if (handle == nullptr) return BASALT_INTERNAL;
  handle->owned_env = basalt::NewPosixEnv();
  if (handle->owned_env == nullptr) return BASALT_INTERNAL;
  const basalt_status st =
      OpenInto(handle->owned_env.get(), dir, dir_len, caps, handle.get());
  if (st != BASALT_OK) return st;
  *out = handle.release();
  return BASALT_OK;
}

basalt_status basalt_db_open_env(basalt_env* env, const char* dir,
                                 size_t dir_len, const basalt_caps* caps,
                                 basalt_db** out) {
  if (env == nullptr || env->env == nullptr || dir == nullptr ||
      out == nullptr) {
    return BASALT_INVALID_ARGUMENT;
  }
  *out = nullptr;
  auto handle = std::unique_ptr<basalt_db>(new (std::nothrow) basalt_db());
  if (handle == nullptr) return BASALT_INTERNAL;
  // owned_env stays empty: the caller's env is borrowed and must outlive this.
  const basalt_status st = OpenInto(env->env, dir, dir_len, caps, handle.get());
  if (st != BASALT_OK) return st;
  *out = handle.release();
  return BASALT_OK;
}

basalt_status basalt_db_close(basalt_db* db) {
  if (db == nullptr) return BASALT_INVALID_ARGUMENT;
  const Status s = db->db->Close();
  // THE HANDLE IS RELEASED WHETHER OR NOT THE CLOSE SUCCEEDED. A close that
  // reported an error and kept the handle alive would leave the caller holding
  // something with no defined next move: Close does not sync, so there is
  // nothing a retry would do differently.
  delete db;
  return ToC(s);
}

basalt_status basalt_db_sync(basalt_db* db, uint64_t* watermark) {
  if (db == nullptr) return BASALT_INVALID_ARGUMENT;
  basalt::wal::SeqNum w = 0;
  const Status s = db->db->Sync(&w);
  if (watermark != nullptr) *watermark = w;
  return ToC(s);
}

uint64_t basalt_db_durable_seq(const basalt_db* db) {
  if (db == nullptr) return 0;
  return db->db->DurableSeq();
}

basalt_status basalt_db_get(const basalt_db* db, const char* key,
                            size_t key_len, char* value_out, size_t cap,
                            size_t* needed) {
  if (db == nullptr || key == nullptr) return BASALT_INVALID_ARGUMENT;
  std::string v;
  const Status s = db->db->Get(Slice(key, key_len), &v);
  if (!s.ok()) return ToC(s);
  return Deliver(v, value_out, cap, needed);
}

basalt_status basalt_db_approximate_disk_bytes(const basalt_db* db,
                                               const char* start,
                                               size_t start_len,
                                               const char* end, size_t end_len,
                                               uint64_t* out) {
  if (db == nullptr || out == nullptr) return BASALT_INVALID_ARGUMENT;
  *out = 0;
  uint64_t n = 0;
  const Status s = db->db->ApproximateDiskBytes(BoundOf(start, start_len),
                                                BoundOf(end, end_len), &n);
  if (!s.ok()) return ToC(s);
  *out = n;
  return BASALT_OK;
}

/* ----------------------------------------------------------------- batch */

basalt_batch* basalt_batch_new(void) {
  return new (std::nothrow) basalt_batch();
}

void basalt_batch_free(basalt_batch* b) { delete b; }

basalt_status basalt_batch_set(basalt_batch* b, const char* key, size_t key_len,
                               const char* value, size_t value_len) {
  if (b == nullptr || key == nullptr || value == nullptr)
    return BASALT_INVALID_ARGUMENT;
  // COPIED AT THE BOUNDARY: WriteBatch owns its bytes, so the caller's pointer
  // is not retained past this call and a managed-runtime caller's pointer rule
  // cannot be violated.
  b->b.Set(Slice(key, key_len), Slice(value, value_len));
  return BASALT_OK;
}

basalt_status basalt_batch_delete(basalt_batch* b, const char* key,
                                  size_t key_len) {
  if (b == nullptr || key == nullptr) return BASALT_INVALID_ARGUMENT;
  b->b.Delete(Slice(key, key_len));
  return BASALT_OK;
}

basalt_status basalt_batch_delete_range(basalt_batch* b, const char* start,
                                        size_t start_len, const char* end,
                                        size_t end_len) {
  if (b == nullptr) return BASALT_INVALID_ARGUMENT;
  b->b.DeleteRange(BoundOf(start, start_len), BoundOf(end, end_len));
  return BASALT_OK;
}

size_t basalt_batch_count(const basalt_batch* b) {
  if (b == nullptr) return 0;
  return b->b.Len();
}

void basalt_batch_clear(basalt_batch* b) {
  if (b == nullptr) return;
  b->b.Reset();
}

basalt_status basalt_db_write(basalt_db* db, const basalt_batch* b,
                              uint64_t* seq) {
  if (db == nullptr || b == nullptr) return BASALT_INVALID_ARGUMENT;
  basalt::wal::SeqNum s = 0;
  const Status st = db->db->Write(b->b, &s);
  if (seq != nullptr) *seq = s;
  return ToC(st);
}

/* ------------------------------------------------------------- iterators */

basalt_status basalt_db_iter(const basalt_db* db, const char* lower,
                             size_t lower_len, const char* upper,
                             size_t upper_len, basalt_iter** out) {
  if (db == nullptr || out == nullptr) return BASALT_INVALID_ARGUMENT;
  *out = nullptr;
  return IterInto(
      db->db->NewIter(OptionsOf(lower, lower_len, upper, upper_len)), out);
}

void basalt_iter_free(basalt_iter* it) {
  if (it == nullptr) return;
  (void)it->it->Close();
  delete it;
}

basalt_status basalt_iter_seek(basalt_iter* it, int mode, const char* key,
                               size_t key_len, int* valid) {
  if (it == nullptr) return BASALT_INVALID_ARGUMENT;
  it->has_pending = false;
  bool ok = false;
  switch (mode) {
    case BASALT_SEEK_FIRST:
      ok = it->it->First();
      break;
    case BASALT_SEEK_LAST:
      ok = it->it->Last();
      break;
    case BASALT_SEEK_GE:
      if (key == nullptr) return BASALT_INVALID_ARGUMENT;
      ok = it->it->SeekGE(Slice(key, key_len));
      break;
    case BASALT_SEEK_LT:
      if (key == nullptr) return BASALT_INVALID_ARGUMENT;
      ok = it->it->SeekLT(Slice(key, key_len));
      break;
    default:
      // A VALUE NO ENUMERATOR NAMES CAN ARRIVE HERE, and that is why this arm
      // exists where the Status switch above has none. `mode` crosses from C,
      // where a caller may send any integer; the Status switch is over a value
      // this library produced itself.
      //
      // It is reachable BECAUSE the parameter is an int. Typed as the enum, an
      // out-of-range mode would be undefined behaviour at the load, and this
      // line would be a refusal that could only be reached by a program that
      // had already lost its meaning.
      return BASALT_INVALID_ARGUMENT;
  }
  // THE ENTRY UNDER THE CURSOR HAS NOT BEEN RETURNED YET, so the next block
  // must start with it rather than past it.
  it->positioned = ok;
  if (valid != nullptr) *valid = ok ? 1 : 0;
  return BASALT_OK;
}

basalt_status basalt_iter_block(basalt_iter* it, int forward, size_t n,
                                uint32_t* key_lens, uint32_t* val_lens,
                                char* keys, size_t keys_cap, size_t* keys_used,
                                char* vals, size_t vals_cap, size_t* vals_used,
                                size_t* filled) {
  if (it == nullptr || filled == nullptr) return BASALT_INVALID_ARGUMENT;
  *filled = 0;
  // THE LENGTH ARRAYS ARE REQUIRED WHENEVER A PAIR COULD BE WRITTEN. Checking
  // it here rather than trusting `n` is what stops a caller that passed n > 0
  // with null arrays from getting a store through a null pointer instead of a
  // code.
  if (n > 0 && (key_lens == nullptr || val_lens == nullptr))
    return BASALT_INVALID_ARGUMENT;
  size_t ko = 0;
  size_t vo = 0;
  // The progress quantity is `*filled`, which rises by exactly one per
  // iteration and is bounded by the caller's `n`. IT DOES NOT DEPEND ON THE
  // ITERATOR TO STOP -- a cursor that failed to advance would spin a loop
  // bounded by nothing, so the bound is the caller's.
  while (*filled < n) {
    Slice k;
    Slice v;
    if (it->has_pending) {
      // THE HELD PAIR GOES FIRST, and only then does the cursor advance.
      k = Slice(it->pending_key);
      v = Slice(it->pending_value);
    } else if (it->positioned) {
      // The seek's own entry, returned once and then stepped past.
      it->positioned = false;
      k = it->it->Key();
      v = it->it->Value();
    } else {
      const bool ok = forward != 0 ? it->it->Next() : it->it->Prev();
      if (!ok) {
        if (keys_used != nullptr) *keys_used = ko;
        if (vals_used != nullptr) *vals_used = vo;
        return BASALT_OK;
      }
      k = it->it->Key();
      v = it->it->Value();
    }
    // RETURNS WITHOUT CONSUMING IF THE NEXT PAIR DOES NOT FIT, so a caller can
    // grow and retry WITHOUT LOSING A POSITION. The alternative -- a partial
    // write plus an error -- would leave the caller unable to tell which pairs
    // it had.
    if (ko + k.size() > keys_cap || vo + v.size() > vals_cap) {
      if (!it->has_pending) {
        it->pending_key.assign(k.data(), k.size());
        it->pending_value.assign(v.data(), v.size());
        it->has_pending = true;
      }
      // A PAIR THAT FITS IN NO BUFFER THIS CALLER HAS OFFERED must be reported
      // rather than held forever: with nothing filled, the caller is told to
      // grow. With something filled, the short block is the answer and the pair
      // waits.
      if (*filled == 0) {
        // THE NEEDED CAPACITIES, not the used ones -- the same idiom
        // basalt_db_get uses and for the same reason.
        if (keys_used != nullptr) *keys_used = k.size();
        if (vals_used != nullptr) *vals_used = v.size();
        return BASALT_BUFFER_TOO_SMALL;
      }
      if (keys_used != nullptr) *keys_used = ko;
      if (vals_used != nullptr) *vals_used = vo;
      return BASALT_OK;
    }
    it->has_pending = false;
    key_lens[*filled] = static_cast<uint32_t>(k.size());
    val_lens[*filled] = static_cast<uint32_t>(v.size());
    if (!k.empty()) std::memcpy(keys + ko, k.data(), k.size());
    if (!v.empty()) std::memcpy(vals + vo, v.data(), v.size());
    ko += k.size();
    vo += v.size();
    ++*filled;
  }
  if (keys_used != nullptr) *keys_used = ko;
  if (vals_used != nullptr) *vals_used = vo;
  return BASALT_OK;
}

basalt_status basalt_iter_error(const basalt_iter* it) {
  if (it == nullptr) return BASALT_INVALID_ARGUMENT;
  return ToC(it->it->Error());
}

/* ------------------------------------------------------------- snapshots */

basalt_status basalt_db_snapshot(basalt_db* db, basalt_snapshot** out) {
  if (db == nullptr || out == nullptr) return BASALT_INVALID_ARGUMENT;
  *out = nullptr;
  auto handle =
      std::unique_ptr<basalt_snapshot>(new (std::nothrow) basalt_snapshot());
  if (handle == nullptr) return BASALT_INTERNAL;
  handle->s = db->db->NewSnapshot();
  *out = handle.release();
  return BASALT_OK;
}

basalt_status basalt_snapshot_close(basalt_snapshot* s) {
  if (s == nullptr) return BASALT_INVALID_ARGUMENT;
  const Status st = s->s->Close();
  delete s;
  return ToC(st);
}

basalt_status basalt_snapshot_get(const basalt_snapshot* s, const char* key,
                                  size_t key_len, char* value_out, size_t cap,
                                  size_t* needed) {
  if (s == nullptr || key == nullptr) return BASALT_INVALID_ARGUMENT;
  std::string v;
  const Status st = s->s->Get(Slice(key, key_len), &v);
  if (!st.ok()) return ToC(st);
  return Deliver(v, value_out, cap, needed);
}

basalt_status basalt_snapshot_iter(const basalt_snapshot* s, const char* lower,
                                   size_t lower_len, const char* upper,
                                   size_t upper_len, basalt_iter** out) {
  if (s == nullptr || out == nullptr) return BASALT_INVALID_ARGUMENT;
  *out = nullptr;
  return IterInto(s->s->NewIter(OptionsOf(lower, lower_len, upper, upper_len)),
                  out);
}

}  // extern "C"
