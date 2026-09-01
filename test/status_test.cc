#include "basalt/status.h"

#include <cstddef>
#include <iterator>
#include <set>
#include <string>

#include <gtest/gtest.h>

namespace basalt {
namespace {

// Every enumerator, written out. The REAL gate against an unclassified
// enumerator is the compiler: CodeName's switch has no `default:` arm, so
// adding a code fails the build before it reaches any test. This list exists
// for the properties a switch cannot state -- that the names are distinct, and
// that the count is what the design says it is.
constexpr Status::Code kAllCodes[] = {
    Status::Code::kOk,        Status::Code::kNotFound,
    Status::Code::kRecordTooLarge, Status::Code::kWalBufferFull,
    Status::Code::kIoError,   Status::Code::kDiskFull,
    Status::Code::kCorruption, Status::Code::kKilled,
    Status::Code::kInvalidArgument,
};

// Nine: kOk, kNotFound, and the seven codes of DESIGN-B1 section 7.6's table.
// kBusy is deliberately absent until B5 supplies a bidirectional predicate.
constexpr std::size_t kExpectedCodeCount = 9;

TEST(Status, EveryCodeIsNamedAndNamesAreDistinct) {
  ASSERT_EQ(std::size(kAllCodes), kExpectedCodeCount);
  std::set<std::string> names;
  for (Status::Code c : kAllCodes) {
    const char* n = CodeName(c);
    ASSERT_NE(n, nullptr);
    EXPECT_NE(std::string(n), "") << "an unnamed code reads as a blank cell in every report";
    names.insert(n);
  }
  EXPECT_EQ(names.size(), kExpectedCodeCount)
      << "two codes share a name, so a report cannot say which one happened";
}

TEST(Status, DefaultIsOkAndCarriesNoMessage) {
  Status s;
  EXPECT_TRUE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kOk);
  EXPECT_EQ(s.message(), "");
  EXPECT_EQ(s.ToString(), "kOk");
}

TEST(Status, ErrorsCarryTheirCodeAndMessage) {
  Status s = Status::Corruption("000001.log block 3 offset 96");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kCorruption);
  EXPECT_EQ(s.message(), "000001.log block 3 offset 96");
  EXPECT_EQ(s.ToString(), "kCorruption: 000001.log block 3 offset 96");
}

// kNotFound is an error code but a normal result. The distinction is not
// pedantry: it is why kNotFound carries no harness-side predicate in section
// 7.6's table while every other code does.
TEST(Status, NotFoundIsNotOkButIsAnOrdinaryResult) {
  Status s = Status::NotFound("k");
  EXPECT_FALSE(s.ok());
  EXPECT_EQ(s.code(), Status::Code::kNotFound);
}

}  // namespace
}  // namespace basalt
