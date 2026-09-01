// The sanitizer lanes assert, at compile time, that their sanitizer is on.
//
// RISK-1, in this repo's own words: a lane too expensive for a git hook has no
// executor at all, and three lanes have been found red after running
// unattended. The dual hazard is a lane found GREEN after quietly losing the
// only thing it was checking -- a -fsanitize flag dropped by a CMake edit
// leaves cpp-asan building, running and passing while checking nothing at all.
//
// So the flag is not trusted; it is asserted. Each sanitizer lane defines its
// own BASALT_EXPECT_* macro, and the corresponding __has_feature must agree or
// the lane fails to BUILD. That is a stronger guarantee than any canary,
// because it cannot be reached by a test that was skipped.
#include <gtest/gtest.h>

#if defined(__has_feature)
#define BASALT_HAS_FEATURE(f) __has_feature(f)
#else
#define BASALT_HAS_FEATURE(f) 0
#endif

#if defined(BASALT_EXPECT_ASAN)
static_assert(BASALT_HAS_FEATURE(address_sanitizer),
              "cpp-asan lane was built WITHOUT AddressSanitizer");
#endif
#if defined(BASALT_EXPECT_UBSAN)
static_assert(BASALT_HAS_FEATURE(undefined_behavior_sanitizer),
              "cpp-ubsan lane was built WITHOUT UndefinedBehaviorSanitizer");
#endif
#if defined(BASALT_EXPECT_TSAN)
static_assert(BASALT_HAS_FEATURE(thread_sanitizer),
              "cpp-tsan lane was built WITHOUT ThreadSanitizer");
#endif

// The plain cpp-test lane asserts the converse: it must run with NO sanitizer.
// Without this, a build that silently sanitized everything would make cpp-test
// and cpp-asan the same lane, and the four reds B1.1 requires would be one red
// counted four times.
#if defined(BASALT_EXPECT_NO_SANITIZER)
static_assert(!BASALT_HAS_FEATURE(address_sanitizer) &&
                  !BASALT_HAS_FEATURE(thread_sanitizer) &&
                  !BASALT_HAS_FEATURE(undefined_behavior_sanitizer),
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
