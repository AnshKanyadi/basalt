# Verification inventory

## Lanes that run in CI

Every lane runs on three toolchains: ubuntu-latest with clang, ubuntu-latest
with gcc, and macos-latest with clang. Twelve sanitizer jobs and three sweep
jobs, plus two jobs that are not per-toolchain.

| Lane | Binary | Asserts | Tests |
|---|---|---|---|
| `cpp-test` | `basalt_test` | The unit suite with no sanitizer. Asserts at compile time that no sanitizer is present. Also builds `basalt_tsan_harness` without running it. | 411 |
| `cpp-asan` | `basalt_test` | The unit suite under AddressSanitizer. Asserts at compile time that `__has_feature(address_sanitizer)` holds. | 411 |
| `cpp-ubsan` | `basalt_test` | The unit suite under UndefinedBehaviorSanitizer, `-fno-sanitize-recover=all`. Asserts at compile time that `__has_feature(undefined_behavior_sanitizer)` holds. | 411 |
| `cpp-tsan` | `basalt_tsan_harness` | Concurrent writer and syncer across a flush, under ThreadSanitizer. Asserts at compile time that `__has_feature(thread_sanitizer)` holds. | 3 |
| `sweep` | `basalt_sweep` | The kill-point sweep, one step per regime. Asserts every kill point recovered to a promised watermark, that both elements of the recovery set were observed, and that the per-call-kind census totals the visit count. Exits non-zero on any of the three. | 305 / 990 / 3545 kill points |
| `c-abi` | `basalt_c_abi_test` | The C header compiled by a **C** compiler at `-std=c99 -Wall -Wextra -Werror`, calling every function in it. Asserts the header is valid C and every symbol has C linkage in the archive. Run by `ctest`. | 1 binary, 40 checks |
| `format` | none | `git clang-format --diff` against the merge base, restricted to the line ranges the change touches, over `src include rig cmd test`. | n/a |
| `library-only` | `libbasalt.a`, `libbasalt_c.a` | Configures with `BASALT_BUILD_TESTS=OFF` and builds the archives alone. Asserts both the library and the C ABI build without GoogleTest and without `test/` or `rig/`. | n/a |

Inside the 377, four tests (`DiffFixtures`) read the static corpus at
`test/fixtures/differential/` — 21 artifact images built field by field from the
format document by a generator with its own crc32c implementation. They are the
only assertions in `test/differential_artifact_test.cc` whose expected bytes
were not produced by the code under test: the other 25 (`DiffArtifact`) assemble
their images with the engine's own `wal::Crc32c`. A missing corpus fails the
tests rather than skipping them.

The compile-time sanitizer assertions are in `test/sanitizer_lane_test.cc`.
Exactly one of `BASALT_EXPECT_ASAN`, `BASALT_EXPECT_UBSAN`, `BASALT_EXPECT_TSAN`
and `BASALT_EXPECT_NO_SANITIZER` is defined per lane; a mismatch fails the build
rather than the run.

THAT GUARD WAS CLANG-ONLY UNTIL 2026-09-01 AND DID NOT VERIFY THE GCC LANES. It
probed with `__has_feature`, a clang extension that gcc did not adopt until 14.
Under the CI compiler (gcc 13.3) the probe fell back to a constant 0, so:

- `cpp-asan`, `cpp-ubsan` and `cpp-tsan` under gcc failed to build from the day
  the gcc jobs were added — the guard reporting "no sanitizer" for lanes that
  had one. Those three lanes never ran under gcc.
- `cpp-test` under gcc passed, and passed VACUOUSLY: its assertion is that no
  sanitizer is present, and a probe wired to 0 satisfies it without being able
  to detect one. A gcc build that had silently acquired a sanitizer would not
  have been caught.

Detection is now per-sanitizer: `__has_feature` on clang, `__SANITIZE_ADDRESS__`
and `__SANITIZE_THREAD__` on gcc, and for UBSan — which gcc exposes no reliable
predefined macro for, on 13 or on 16.1 — a `BASALT_UBSAN_FLAGS_ADDED` token
emitted from inside the same `add_compile_options()` argument list as
`-fsanitize=undefined`. On any toolchain that can see UBSan for itself, the two
are required to agree, which fails the build if the define and the flag drift
apart; gcc 13 cannot run that cross-check.

`cpp-test` builds `basalt_tsan_harness` and does not run it. The harness races
under a planted defect often enough that running it here would make `cpp-test`
red for the same reason `cpp-tsan` is, removing the control.

`basalt_sweep` now has its own job, above. It had none: each lane builds only
the targets it runs and `library-only` configures with `BASALT_BUILD_TESTS=OFF`,
which does not define the tool at all, so the program behind the durability
claim was built by nothing and run by nothing. All three regimes take about 17
seconds together, uninstrumented, which is why it is on push rather than on a
schedule.

The sweep needs no seed and no iteration count. Its workload keys are authored,
its torn-write prefixes are a fixed list, and the number of points follows from
the workload rather than from a budget, so the three regimes' point counts are
fixed at 305, 990 and 3545 and three runs of each are byte-identical.

`basalt_amp` remains defined and unrun, and that is deliberate rather than an
oversight of the same kind. It is an INSTRUMENT, not a lane: it measures write,
read and space amplification at three sizes spanning the predicted crossing
point and prints the conclusion DESIGN-B3 section 8.2b fixed in advance for each
outcome. It returns 0 on every path, including the one where amplification is
above the threshold and "(c) is reopened" -- there is no exit code to gate a
merge on, and adding one would be inventing a pass/fail line that section 8.1
deliberately did not draw. Its own logic is asserted in
`test/amplification_test.cc`, inside the 377. It takes 34 seconds and is run by
hand when the question is asked.

## Verification that did not survive extraction

### `scripts/cpp-scan.sh` — source scope scanner (836 lines across three scripts)

Not ported. It enforced four rules.

| Rule | Bug class it caught |
|---|---|
| Env seam 1:1:1 shape: one public non-virtual wrapper, one private `Do*` pure virtual, one `CallSite` enumerator, names matching, and `AllCallSites()` bound to the same set | A filesystem operation reaching the OS without passing a registered call site, so `TestEnv` cannot inject a fault at it and the kill-point sweep cannot visit it. |
| Oracle independence: a file registered in `ORACLES.txt` may not include anything from `src/` outside `ARTIFACTS.txt` | An oracle that consults the engine's beliefs rather than parsing its artifacts, so a wrong engine is judged correct by a judge reading the engine's own answer. |
| Decider registries: every function deciding evidentiary status is enumerated in `DECIDERS.txt` and asserted in both directions | A run classifier that is wrong in its untested direction — conservative, banking too little — so runs stop counting as evidence with every lane still green. |
| Part 7: the shipping archive is actually compiled `-fno-exceptions` | The flag being dropped from the build, making exceptions possible on a boundary whose design assumes they cannot occur. |

`CPP-HATCHES.txt` (the exemption registry), `scan-fixtures/` and `scan-blind/`
(23 files, one blinding patch per rule to prove each rule still fires) were
removed with it.

### Mutation catalogue — 155 patches

Not ported; the runner (`scripts/cpp-mutants.sh`) is in the parent project. Each
patch planted one defect, named the lane that had to go red, and named a control
lane that had to stay green, separating "the lane caught it" from "the patch
broke the build".

Bug class it caught: a lane that has stopped detecting the class it was built
for, found by planting the defect rather than by trusting the lane.

**BM17a-bypassing-env-method** is the case the remaining suite does not cover. It
adds a public virtual to the `Env` base class that reaches the filesystem without
going through the wrapper / `Do*` / `CallSite` chain. The 1:1:1 source assertion
caught it. `test/env_surface_test.cc` cannot: it asserts the *number* of call
sites in the built code, and a bypassing method registers no call site, so the
count is unchanged. The other three in the family were
`BM17b-unregistered-wrapper`, `BM17c-mispointed-callsite` and
`BM17d-callsite-missing-from-list`.

### Differential lane

Removed. The C++ half ran a seeded workload, killed at a chosen Env call
ordinal, reopened, and emitted an artifact. It reached no verdict; the verdict
required a second process holding the reference model, which stayed in the
parent project.

Bug class it caught: a divergence between this engine's recovered state and the
reference model's — visible only by comparison against an independent
implementation, not by any assertion this engine can make about itself.

Removed with it: `cmd/diff_main.cc`, `rig/differential_driver.*`, and
`test/differential_driver_test.cc` (7 tests).

The fixture corpus was NOT removed with it and is listed under the CI lanes
above. It needs neither the driver nor the judge: it is a directory of bytes and
an expected reading of them.

## What partial coverage remains

`test/env_surface_test.cc` asserts that the built code has 22 call sites
(`kExpectedCallSites`) and drives each one; `test/test_env_test.cc` asserts
`AllCallSites().size() == 22`. Both read the built code, not the source.

An added call site is caught: the count changes.
A bypassing call site is not caught: the count does not change.

`rig/differential_artifact.cc`'s format classifier has 29 tests. 25
(`DiffArtifact`) build their image with `Assemble`, a helper inside
`test/differential_artifact_test.cc` that computes its trailer with the engine's
own `wal::Crc32c`; those agree with the engine by construction. 4
(`DiffFixtures`) read the corpus and do not.

THE CORPUS IS FROZEN. The generator that produced it stayed in the parent
project, as did the second decoder that read the same files. A fixture cannot be
regenerated if it is corrupted or deleted, and no fixture can be added for a
refusal rule introduced after the split. A format rule added from here on is
covered only by the 25 circular tests. Writing its fixture with `Assemble` would
not change that: an image built by the code under test asserts nothing about the
document.

## Known gaps

There is no benchmark in this repository. The one that existed measured a native
column whose only purpose was to be differenced against a cross-language column
that stayed in the parent project; it was removed with that column, along with
the pinned key stream that existed to make the two comparable. No throughput or
latency figure is produced or checked by anything here.
