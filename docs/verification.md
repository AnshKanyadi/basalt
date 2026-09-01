# Verification inventory

## Lanes that run in CI

Every lane runs on three toolchains: ubuntu-latest with clang, ubuntu-latest
with gcc, and macos-latest with clang. Twelve sanitizer jobs, plus two jobs that
are not per-toolchain.

| Lane | Binary | Asserts | Tests |
|---|---|---|---|
| `cpp-test` | `basalt_test` | The unit suite with no sanitizer. Asserts at compile time that no sanitizer is present. Also builds `basalt_tsan_harness` without running it. | 373 |
| `cpp-asan` | `basalt_test` | The unit suite under AddressSanitizer. Asserts at compile time that `__has_feature(address_sanitizer)` holds. | 373 |
| `cpp-ubsan` | `basalt_test` | The unit suite under UndefinedBehaviorSanitizer, `-fno-sanitize-recover=all`. Asserts at compile time that `__has_feature(undefined_behavior_sanitizer)` holds. | 373 |
| `cpp-tsan` | `basalt_tsan_harness` | Concurrent writer and syncer across a flush, under ThreadSanitizer. Asserts at compile time that `__has_feature(thread_sanitizer)` holds. | 3 |
| `format` | none | `git clang-format --diff` against the merge base, restricted to the line ranges the change touches, over `src include rig cmd test`. | n/a |
| `library-only` | `libbasalt.a` | Configures with `BASALT_BUILD_TESTS=OFF` and builds the archive alone. Asserts the library builds without GoogleTest and without `test/` or `rig/`. | n/a |

The compile-time sanitizer assertions are in `test/sanitizer_lane_test.cc`.
Exactly one of `BASALT_EXPECT_ASAN`, `BASALT_EXPECT_UBSAN`, `BASALT_EXPECT_TSAN`
and `BASALT_EXPECT_NO_SANITIZER` is defined per lane; a mismatch fails the build
rather than the run.

`cpp-test` builds `basalt_tsan_harness` and does not run it. The harness races
under a planted defect often enough that running it here would make `cpp-test`
red for the same reason `cpp-tsan` is, removing the control.

Two targets are defined and no CI job builds or runs them: `basalt_sweep` (the
Env kill-point sweep) and `basalt_amp` (compaction amplification). Each lane
builds only the targets it runs, and `library-only` configures with
`BASALT_BUILD_TESTS=OFF`, which does not define them.

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

Removed with it: `cmd/diff_main.cc`, `rig/differential_driver.*`,
`test/differential_driver_test.cc` (7 tests), and the fixture corpus with its
`DiffFixtures` tests (4 tests).

## What partial coverage remains

`test/env_surface_test.cc` asserts that the built code has 22 call sites
(`kExpectedCallSites`) and drives each one; `test/test_env_test.cc` asserts
`AllCallSites().size() == 22`. Both read the built code, not the source.

An added call site is caught: the count changes.
A bypassing call site is not caught: the count does not change.

`rig/differential_artifact.cc`'s format classifier keeps 25 tests
(`DiffArtifact`). Every image they parse is built by `Assemble`, a helper inside
`test/differential_artifact_test.cc` that computes its trailer with the engine's
own `wal::Crc32c`. The corpus that was built from the format document by an
independent generator, and read by a second decoder, was removed with the
differential lane.
