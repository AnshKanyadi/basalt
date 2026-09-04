# Basalt

An LSM-tree storage engine in C++, with a C ABI. You link it and get durable ordered key-value storage that survives being killed at any point in a write.

## What is in it

A write-ahead log with CRC-checked records and recovery, a skiplist memtable over an arena allocator, SSTables with bloom filters and range tombstones, compaction across two levels, and a manifest that tracks versions across restarts. Snapshots hold their version against compaction until they are closed. The public entry point is `DB::Open` in `include/basalt/db.h`.

C++17. No exceptions, no RTTI, no runtime dependencies. The build reaches no network at any point: GoogleTest is vendored in the tree at a pinned commit, there is no `FetchContent` or download step, and an offline lane recomputes the vendored tree's hash against the pin on every push.

`include/basalt/basalt.h` is the C ABI: 29 functions, opaque handles, and no C++ type or exception crossing it. It is a first-class part of the library with its own archive (`basalt::basalt_c`), its own tests, and its own crash coverage — the kill-point sweep runs every regime through it as well as through `DB`, because a durability claim that stops at the C boundary has a hole in it. The caller owns every buffer in both directions, so nothing the library allocates ever reaches the caller and no lifetime is shared.

Compaction is two levels and the manifest decoder refuses a third rather than guessing at it. L0 holds flush output, where files overlap and every one must be consulted. L1 is a single non-overlapping run, which is what lets the read path binary-search it and lets a merge take it as one source. Multi-level leveled compaction is a policy change on top of this structure, not something the engine does today.

All filesystem access goes through an `Env` seam: 22 call sites, each one public non-virtual wrapper, one private pure virtual, and one enumerator, with the enumerator list bound to the same set. The test build substitutes an `Env` that can fail, stall, or die at any individual call, which is how the crash tests reach states a real filesystem will not produce on demand. The engine reads no clock at all.

## Building

```
cmake -S . -B build
cmake --build build
./build/basalt_test
```

`-DBASALT_BUILD_TESTS=OFF` builds the library alone, with no GoogleTest dependency.

Sanitizer lanes are selected at configure time with `-DBASALT_SANITIZER=none|address|undefined|thread`. One sanitizer per build directory: the flags are applied globally before GoogleTest is added, so the framework is instrumented along with the engine.

To use it from another project, either `add_subdirectory` this tree or install it and `find_package(basalt)`. Both give you `basalt::basalt` for C++ and `basalt::basalt_c` for the C ABI, and switching between them changes nothing else.

The version is **0.1.1**, and the leading zero is the honest part: semver's `0.y.z` promises nothing across a minor bump, and this API was added and reshaped in the round that gained its first external consumer. The installed package carries a version file with `SameMinorVersion` compatibility, so `find_package(basalt 0.1)` succeeds and `find_package(basalt 0.2)` fails rather than answering a question it cannot. A CI lane installs the library to a prefix and builds a C program against it, because an in-tree target inherits include paths and link languages that an installed one carries only if the export wrote them down.

## Verification

Every lane runs on macOS with clang, Ubuntu with clang, and Ubuntu with GCC.

| Lane | Binary | What it runs |
| --- | --- | --- |
| `vendor` | none | The vendored GoogleTest tree hashed and compared to its recorded pin, offline |
| `cpp-test` | `basalt_test` | 419 tests across 52 suites, uninstrumented |
| `cpp-asan` | `basalt_test` | The same suite under AddressSanitizer |
| `cpp-ubsan` | `basalt_test` | The same suite under UndefinedBehaviorSanitizer, `-fno-sanitize-recover=all` |
| `cpp-tsan` | `basalt_tsan_harness` | Two race tests across a flush, plus the lane guard |
| `c-abi` | `basalt_c_abi_test` | The C header compiled by a **C** compiler at `-std=c99 -Wall -Wextra -Werror`, calling every function in it |
| `sweep` | `basalt_sweep` | 4,840 kill points across three regimes through `DB`, and 4,855 through the C ABI |
| `differential` | `basalt_diff` | One clean and one killed run, each producing a parseable artifact |
| `installed-c-consumer` | `examples/c_consumer` | The installed package consumed from C via `find_package`, and a wrong version refused |

The durability claim rests on the sweep. It identifies a kill point by global `Env`-call ordinal, which makes the set complete by construction with nothing to annotate and therefore nothing to forget. Every ordinal is visited in five modes: killed before the effect, killed after the effect but before the caller learns of it, and a torn `Sync` at each of three fixed prefixes. The third family exists because measurement showed the first two can never leave a batch on disk without its group marker, so on their own they cannot detect an engine that commits uncommitted batches. Detections for that class were zero until it was added.

After each injection the harness reopens the database and adjudicates the result itself rather than trusting the engine's report, requiring the recovered state to be one of the two states the write could legitimately have reached. The three regimes cover the WAL path, a flush, and a compaction with a snapshot held across it, which is the path that deletes data. The sweep takes no seed and no iteration count: the workload is authored and the point counts follow from it, so 305, 990 and 3,545 are fixed, and all three toolchains produce the same numbers.

Every regime is also swept through the C ABI, where the counts are 310, 995 and 3,550. The difference is exactly five per regime and is accounted for rather than tolerated: one ordinal, the `CreateDir` the C open performs, at five modes. `kEnvCreateDir` appears in the C census and in no other, so the C surface visits five kill points the C++ surface cannot reach and recovers from all of them. The two ordinal spaces are different and their numbers never aggregate, which is why the surface is named in the machine-readable output and not only in the heading.

The kill is an in-process dead flag rather than a real process death. Every subsequent `Env` call becomes a no-op and the durable image freezes, so code that ignores a `Status` can still only touch a frozen filesystem. That leaves a blind spot: the engine keeps running, so a bug where recovery reads live memory instead of disk could be masked. The size of that blind spot is measured rather than assumed. `real_exit_kill_test.cc` forks, runs one identical workload at one identical kill ordinal both ways, and requires the two durable images to be equal.

A corpus of 21 byte images checks the differential artifact decoder against images built field by field from the format document by a generator that implements CRC-32C itself and links no part of this project. It is the only set of images the decoder did not help produce; every other artifact test builds its expected bytes with the engine's own CRC and agrees with it by construction.

`test/sanitizer_lane_test.cc` asserts at compile time that each lane was built with the sanitizer it claims, and that the control lane has none. Without it, a lane that quietly lost its flags would keep reporting green. Detection is per-compiler, since `__has_feature` is a clang extension that GCC only adopted in version 14, and GCC has no predefined macro for UBSan on any version tested. In that case CMake emits the witness define from inside the same argument list that adds the flag, so the two cannot drift apart. A compiler that can answer neither probe fails to compile rather than defaulting to "no sanitizer present."

## Known limitations

**No benchmarks.** There are no published numbers for this engine, and none should be inferred from the absence. `basalt_amp` measures write, read and space amplification and prints a conclusion, but it returns success on every path including the one above the threshold, so it is an instrument rather than a lane and is run by hand.

**A call site can still bypass the seam.** The `Env` surface test asserts the total number of registered call sites, so adding one is caught. A public method that reaches the filesystem without registering one leaves the count unchanged and is not. The source scanner that caught that class stayed with the project this engine was written for and has not been ported. This is the largest single gap in the verification here.

**The fixture corpus is frozen.** The independent generator that produced those byte images did not come across. The fixtures still verify the decoder, but they cannot be regenerated or extended here.

**`ApproximateDiskBytes` measures live logical bytes.** It walks the merged view and sums key and value sizes, counting no block framing, no filter, no footer, and nothing an overwritten version still occupies in a file it has not been compacted out of.

## Origin

The engine was written as the storage layer for Rift, a distributed transactional key-value store, and split out with its history intact so other projects can depend on it. **Rift now depends on this library as a pinned submodule and carries no engine source**; its own copy is deleted. A local-first sync engine is the second consumer under development.

The C API removed during the split has been rebuilt here, as a first-class part of the library rather than one consumer's appendage — designed for any C ABI consumer, not only for cgo. Having a second consumer immediately found what one had hidden: a cap field the old boundary could not reach, so a legal configuration was unopenable; a documented use-after-close check that did not exist; enum-typed parameters whose validation arm was unreachable without undefined behaviour; and an export that never declared the C++ runtime a C consumer needs, which worked only because the first consumer had hardcoded the flag in its own source.

## License

Apache-2.0.
