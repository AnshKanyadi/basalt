// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Ansh Kanyadi
//
// WHICH IMPLEMENTATION OF THE ENGINE A HARNESS IS DRIVING.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS: A DURABILITY CLAIM THAT STOPS AT THE C BOUNDARY HAS A HOLE
// IN IT.
//
// The kill-point sweep is the check behind this library's durability claim. It
// enumerates every point a write can be interrupted at -- every Env call, by
// global ordinal -- kills there, recovers, and adjudicates what survived
// against what was promised. Run against the C++ DB it is strong evidence.
//
// It is evidence about the C++ DB. Every embedder that reaches this engine
// through basalt/basalt.h runs a DIFFERENT amount of code on the way to the
// same Env calls: a boundary that copies, a handle that owns, an iterator that
// holds a pair back when a buffer is short. None of that is swept by a sweep
// that calls DB::Write directly, and all of it is on the path a C consumer's
// data actually takes.
//
// So the sweep is parameterised over the surface instead of hardcoding one, and
// the lane runs both. The alternative -- asserting that the boundary is "just a
// translation layer" and therefore needs no crash coverage -- is exactly the
// kind of claim that is true until it is not, and the day it stops being true
// nothing would say so.
//
// ---------------------------------------------------------------------------
// THIS IS A DRIVER AND NOT AN ORACLE, which is what makes it allowed to call
// the engine at all. ORACLES.txt: "the DRIVER may call the engine freely -- it
// has to, it is what makes the engine run -- and it is the JUDGE that may not
// consult it." Nothing here reaches a verdict. It runs a workload and reports
// what came back; rig/exactness_oracle.cc decides what that was worth.
#ifndef BASALT_RIG_ENGINE_SURFACE_H_
#define BASALT_RIG_ENGINE_SURFACE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "basalt/caps.h"
#include "basalt/env.h"
#include "basalt/status.h"

namespace basalt {
namespace rig {

enum class SweepSurface : uint8_t {
  kCxx,  // basalt::DB directly -- the library's own interface
  kC,    // basalt/basalt.h -- what every non-C++ embedder crosses
};

const char* SweepSurfaceName(SweepSurface s);

// One open database, driven through one of the two interfaces.
//
// THE OPERATIONS ARE THE SWEEP'S, NOT THE ENGINE'S. This is deliberately not a
// mirror of DB: it is exactly what the sweep's workload does and nothing more,
// so a surface is small enough to be obviously faithful. A wider interface here
// would be a second copy of the engine's API maintained for one caller.
//
// EVERY OPERATION REPORTS A Status::Code AND NOT A Status. The C surface cannot
// produce a Status -- codes are all that cross the boundary -- and the sweep
// adjudicates on the code alone (see PredicateSatisfied in sweep.cc). Returning
// the richer type would mean one surface inventing message strings the other
// one really has, which is a difference between the surfaces that comes from
// the harness rather than from the engine.
class EngineSurface {
 public:
  virtual ~EngineSurface() = default;

  // Opens over `env`, which is BORROWED: the surface never takes ownership and
  // `env` must outlive it. The sweep owns its TestEnvironment and reuses it
  // across an open/close pair, so a surface that freed it would take the
  // harness's own fault ledger with it.
  virtual Status::Code Open(Env* env, const std::string& dir,
                            const wal::Caps& caps) = 0;
  virtual bool IsOpen() const = 0;

  virtual Status::Code Put(const std::string& key,
                           const std::string& value) = 0;
  virtual Status::Code Sync(uint64_t* watermark) = 0;
  virtual uint64_t DurableSeq() const = 0;

  // A snapshot held across part of the workload. It is not in the submission
  // log and does not need to be -- no crash preserves a live-process object --
  // but it changes the ENGINE'S path: with a reader holding them, a
  // compaction's input files go on the obsolete list instead of being deleted
  // in place, and that path is what the sweep is here to visit.
  virtual void TakeSnapshot() = 0;
  virtual void ReleaseSnapshot() = 0;

  // Everything visible now, by iteration. Only ever called on a database opened
  // with no faults planned, so it does not need a failure channel.
  virtual std::map<std::string, std::string> ExtractState() const = 0;

  virtual void Close() = 0;
};

std::unique_ptr<EngineSurface> NewSurface(SweepSurface which);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_ENGINE_SURFACE_H_
