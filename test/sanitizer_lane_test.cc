// The sanitizer lanes assert, at compile time, that their sanitizer is on.
//
// RISK-1, in this repo's own words: a lane too expensive for a git hook has no
// executor at all, and three lanes have been found red after running
// unattended. The dual hazard is a lane found GREEN after quietly losing the
// only thing it was checking -- a -fsanitize flag dropped by a CMake edit
// leaves cpp-asan building, running and passing while checking nothing at all.
//
// So the flag is not trusted; it is asserted. Each sanitizer lane defines its
// own BASALT_EXPECT_* macro, the build is probed for the sanitizer that macro
// names, and the two must agree or the lane fails to BUILD. That is a stronger
// guarantee than any canary, because it cannot be reached by a test that was
// skipped.
//
// ---------------------------------------------------------------------------
// HOW A SANITIZER IS DETECTED, AND WHY IT TAKES THREE MECHANISMS.
//
// This file was clang-only until it was run under gcc, and the way it failed is
// the reason the fallback below is now a hard error. `__has_feature` is a clang
// extension. gcc 14 adopted it; gcc 13 has not got it. Under gcc 13 the old
// `#else #define BASALT_HAS_FEATURE(f) 0` made every probe answer "no
// sanitizer", so all four lanes failed to build at once -- which is the guard
// erring in the safe direction, and is the only reason this was noticed rather
// than believed.
//
//   A FALLBACK THAT ANSWERS "NO" IS A FALLBACK THAT CANNOT TELL "ABSENT" FROM
//   "UNDETECTABLE". Here it answered "no" for a lane that had the sanitizer,
//   which is loud. Reverse the polarity of one assertion and the same fallback
//   is silent.
//
// So detection is per-sanitizer, and each answers from whatever that toolchain
// actually exposes:
//
//   AddressSanitizer   clang: __has_feature(address_sanitizer)
//                      gcc:   __SANITIZE_ADDRESS__
//   ThreadSanitizer    clang: __has_feature(thread_sanitizer)
//                      gcc:   __SANITIZE_THREAD__
//   UBSan              clang and gcc >= 14: __has_feature(...)
//                      gcc < 14:            NOTHING. See below.
//
// UBSAN IS THE ONE THE COMPILER WILL NOT ANSWER. gcc defines no predefined
// macro for it that can be relied on: `__SANITIZE_UNDEFINED__` is absent under
// `-fsanitize=undefined` on gcc 16.1 as well as on gcc 13, so it is not a
// version gap that waiting fixes. The build system is therefore the witness,
// through BASALT_UBSAN_FLAGS_ADDED.
//
// THAT DEFINE IS A WEAKER KIND OF EVIDENCE AND IS TREATED AS ONE. It is a claim
// by CMake rather than an observation of the compiler, so it is emitted inside
// the SAME add_compile_options() argument list as -fsanitize=undefined itself:
// dropping the flag and keeping the define means deleting one token from the
// middle of a list whose remaining tokens are right beside it. See the UBSan
// branch in CMakeLists.txt.
//
// And where the compiler CAN see UBSan -- clang, gcc >= 14 -- the two are
// required to agree. That is a tripwire on the define itself: on any toolchain
// with __has_feature, a BASALT_UBSAN_FLAGS_ADDED that has drifted away from the
// flag fails the build. gcc 13 cannot run that check, which is stated here
// rather than left to be assumed.
#include <gtest/gtest.h>

#if defined(__has_feature)
#define BASALT_HAS_FEATURE(f) __has_feature(f)
#define BASALT_HAS_FEATURE_AVAILABLE 1
#else
// NOT a silent 0. A toolchain with neither __has_feature nor the gcc macros
// cannot be probed at all, and the lanes below must not pass by defaulting.
#define BASALT_HAS_FEATURE(f) 0
#define BASALT_HAS_FEATURE_AVAILABLE 0
#endif

#if BASALT_HAS_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#define BASALT_ASAN_ON 1
#else
#define BASALT_ASAN_ON 0
#endif

#if BASALT_HAS_FEATURE(thread_sanitizer) || defined(__SANITIZE_THREAD__)
#define BASALT_TSAN_ON 1
#else
#define BASALT_TSAN_ON 0
#endif

#if defined(BASALT_UBSAN_FLAGS_ADDED)
#define BASALT_UBSAN_CLAIMED 1
#else
#define BASALT_UBSAN_CLAIMED 0
#endif

#if BASALT_HAS_FEATURE(undefined_behavior_sanitizer) || BASALT_UBSAN_CLAIMED
#define BASALT_UBSAN_ON 1
#else
#define BASALT_UBSAN_ON 0
#endif

// A toolchain that exposes no probe for ASan or TSan is refused outright rather
// than allowed to answer "absent" for everything. Both gcc and clang satisfy
// one of these; a third compiler that satisfies neither must be taught how to
// answer before its lanes mean anything.
#if !BASALT_HAS_FEATURE_AVAILABLE && !defined(__GNUC__)
#error "no sanitizer probe for this compiler: teach it before trusting a lane"
#endif

#if defined(BASALT_EXPECT_ASAN)
static_assert(BASALT_ASAN_ON,
              "cpp-asan lane was built WITHOUT AddressSanitizer");
#endif
#if defined(BASALT_EXPECT_UBSAN)
static_assert(BASALT_UBSAN_ON,
              "cpp-ubsan lane was built WITHOUT UndefinedBehaviorSanitizer");
#endif
#if defined(BASALT_EXPECT_TSAN)
static_assert(BASALT_TSAN_ON,
              "cpp-tsan lane was built WITHOUT ThreadSanitizer");
#endif

// THE DEFINE IS CHECKED AGAINST THE COMPILER WHEREVER THE COMPILER CAN ANSWER.
// This is what keeps BASALT_UBSAN_FLAGS_ADDED honest: it fires if the define
// outlives the flag, and it fires if the flag is added without the define.
// Preprocessor, not static_assert: __has_feature and defined() are only legal
// in a preprocessor expression, and writing them in a C++ one compiles to
// something else entirely on the toolchains where it compiles at all.
#if BASALT_HAS_FEATURE_AVAILABLE
#if BASALT_HAS_FEATURE(undefined_behavior_sanitizer) != BASALT_UBSAN_CLAIMED
#error "BASALT_UBSAN_FLAGS_ADDED and -fsanitize=undefined have drifted apart; they are emitted from one add_compile_options() call and must arrive together"
#endif
#endif

// The plain cpp-test lane asserts the converse: it must run with NO sanitizer.
// Without this, a build that silently sanitized everything would make cpp-test
// and cpp-asan the same lane, and the four reds B1.1 requires would be one red
// counted four times.
#if defined(BASALT_EXPECT_NO_SANITIZER)
static_assert(!BASALT_ASAN_ON && !BASALT_TSAN_ON && !BASALT_UBSAN_ON,
              "cpp-test lane was built WITH a sanitizer; it is meant to be the "
              "uninstrumented control for the other three");
#endif

// Exactly one expectation macro must be defined, or a lane is running with no
// declared identity at all.
namespace {
constexpr int kExpectationsDefined = 0
#if defined(BASALT_EXPECT_ASAN)
                                     + 1
#endif
#if defined(BASALT_EXPECT_UBSAN)
                                     + 1
#endif
#if defined(BASALT_EXPECT_TSAN)
                                     + 1
#endif
#if defined(BASALT_EXPECT_NO_SANITIZER)
                                     + 1
#endif
    ;
static_assert(kExpectationsDefined == 1,
              "exactly one BASALT_EXPECT_* macro must be defined by the lane");
}  // namespace

TEST(SanitizerLane, DeclaresExactlyOneExpectation) {
  EXPECT_EQ(kExpectationsDefined, 1);
}
