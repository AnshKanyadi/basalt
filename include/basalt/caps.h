// The two caps, their derivations, and the invariant between them.
//
// THE DERIVATIONS LIVE HERE, AT THE DEFINITION SITE, and not in prose
// elsewhere. Section 8.4 requires it for a reason worth repeating: there has to
// be exactly one place to correct.
#ifndef BASALT_CAPS_H_
#define BASALT_CAPS_H_

#include <cstdint>

namespace basalt {
namespace wal {

// kMaxRecordBytes -- 64 MiB.
//
// DeleteRange(nil, nil) is the clear half of snapshot application's
// clear-then-ingest, the case Amendment A3 was ruled for. Through B2 it expands
// to ONE POINT DELETE PER LIVE KEY in a single record, and batches are atomic
// so it cannot be chunked.
//
// The derivation: a point delete costs 1 + 4 + |key| bytes by the frozen
// formula, so at a 50-byte key it is 55 bytes, and 64 MiB / 55 is roughly
// 1.22 MILLION POINT DELETES in one record. That is the size of range this cap
// permits before the tripwire fires, and it is chosen to be comfortably beyond
// any range a v1 replica movement would clear while still being a number rather
// than "unbounded".
//
// The cost is scheduled to end: section 8.6, B3's real range tombstones make
// the record O(1) in the range rather than O(keys), and this cap stops being
// reachable by a legal DeleteRange at all.
inline constexpr uint64_t kMaxRecordBytes = 64ull * 1024 * 1024;

// kWalBufferBytes -- 256 MiB, and THE INVARIANT IS THE DERIVATION.
//
//     kWalBufferBytes >= 2 * kMaxRecordBytes
//
// A buffer cap below the maximum legal record size would make the tripwire fire
// on LEGAL INPUT -- which is the same inversion section 5.4 rejected candidate
// (a) for, an engine refusing the normal case in the name of the abnormal one.
// The 2x is margin: at exactly 1x, a single maximum-size record would fill the
// buffer and the next legal Apply before a Sync would fail. The default pair
// satisfies the invariant with 4x.
//
// Asserted at construction, not documented. A cap pair that violates it is a
// configuration that cannot be built.
inline constexpr uint64_t kWalBufferBytes = 256ull * 1024 * 1024;

// kFlushBytes -- 4 MiB of memtable memory as the engine accounts it (B1-D6a),
// which is what makes the trigger answerable at all.
//
// The number balances two costs that pull opposite ways: a larger memtable
// means fewer, bigger SSTables and less write amplification, and it means a
// longer replay after a crash, because everything not yet flushed is replayed
// from the WAL.
//
// IT IS NOT THE ONLY BOUND ON MEMTABLE MEMORY, and that matters: a caller that
// never Syncs never flushes, because the flush runs on the Sync path -- the
// only blocking entry point the frozen interface has. Such a caller reaches
// kWalBufferBytes first, since unsynced WAL bytes and memtable bytes grow
// together, so memtable memory is bounded by that cap PLUS this threshold and
// not by this threshold alone.
//
// It joins Caps for the reason the other two are there: the sweep sets it low
// so that a short workload actually flushes, and a run at a non-default value
// is a DIFFERENT REGIME that never aggregates with a default one.
//
// THE MEASUREMENT THAT WOULD MOVE IT: B5's numbers, showing flush frequency or
// recovery time attributed by profile rather than inferred.
inline constexpr uint64_t kFlushBytes = 4ull * 1024 * 1024;

// kBusyBytes -- 192 MiB, three quarters of the buffer cap, and IT IS A
// DIFFERENT QUANTITY FROM THE TRIPWIRE'S.
//
// The tripwire counts `buffered_bytes_`: bytes accepted and not yet handed to a
// Sync. Sync ZEROES that at the swap and then does its I/O with the lock
// released, so for the whole duration of a slow fsync the engine counts nothing
// while memory holds both the in-flight batch AND everything arriving behind
// it. That window is exactly "the poller is behind", and it is invisible to a
// byte counter that only sees what has not been handed over yet.
//
// So this threshold is charged against BUFFERED PLUS IN-FLIGHT, which is the
// same quantity a harness computes from its own record as
//
//     owed = submitted - drained
//
// and that identity is what makes section 7.6.1's bidirectional predicate
// constructible without asking the engine anything (B5-D4, ruling 4).
//
// ZERO MEANS DISABLED, and it is a real regime rather than a hole. With
// backpressure ON the tripwire is UNREACHABLE through Apply -- a policy in
// front of a last resort is what makes the last resort unreached, which is what
// "tripwire, not policy" meant. The tripwire's own tests therefore run at
// busy_bytes = 0, stated in each of them as a regime and visible in the diff,
// rather than being quietly starved of the state they assert.
inline constexpr uint64_t kBusyBytes = 192ull * 1024 * 1024;

struct Caps {
  uint64_t max_record_bytes = kMaxRecordBytes;
  uint64_t wal_buffer_bytes = kWalBufferBytes;
  uint64_t flush_bytes = kFlushBytes;
  uint64_t busy_bytes = kBusyBytes;  // 0 disables backpressure

  bool IsDefault() const {
    return max_record_bytes == kMaxRecordBytes &&
           wal_buffer_bytes == kWalBufferBytes && flush_bytes == kFlushBytes &&
           busy_bytes == kBusyBytes;
  }
  // TWO CLAUSES, AND THE SECOND IS WHY THE FIRST IS NOT ENOUGH. A busy
  // threshold within one maximum record of the tripwire would let a single
  // legal record cross BOTH lines at once, so the caller is told to back off by
  // the same Apply that would have hit the cap -- backpressure arriving too
  // late to be backpressure. The margin makes the policy strictly earlier than
  // the tripwire, which is the entire ordering between them.
  bool Ordered() const {
    if (wal_buffer_bytes < 2 * max_record_bytes) return false;
    if (busy_bytes == 0) return true;  // disabled
    return busy_bytes + max_record_bytes <= wal_buffer_bytes;
  }
};

}  // namespace wal
}  // namespace basalt

#endif  // BASALT_CAPS_H_
