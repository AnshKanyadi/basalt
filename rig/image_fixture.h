// ONE CONSTRUCTION PATH FOR DURABLE IMAGES, so a fixture cannot omit something
// the engine's own invariants require.
//
// NOT AN ORACLE. It CONSTRUCTS; it never judges, so it carries no BASALT_ORACLE
// marker and is free to use Env. The rule an oracle lives under -- parse
// artifacts, never consult beliefs -- is about what a VERDICT may rest on, and
// this produces no verdict.
//
// ---------------------------------------------------------------------------
// WHY IT EXISTS: TWO FIXTURE DEFECTS OF ONE SHAPE.
//
//   B3.0  a fixture wrote a table, synced it, and never synced the DIRECTORY --
//         so the table's NAME was not durable and the image did not contain it.
//   B3.4  a fixture handed the adjudicator a bare .sst with NO MANIFEST -- so
//         the table was an orphan, referred to by nothing.
//
// BOTH TIMES THE CHECKER WAS RIGHT. A fixture that does not describe what its
// author meant produces A CORRECT VERDICT ABOUT THE WRONG THING, and it presents
// as a checker bug -- which is the expensive way to find out, because the
// debugging starts in the wrong component.
//
// What the two omissions share: each left out something THE ENGINE'S OWN
// INVARIANTS REQUIRE. A durable table has a durable name; a live table is named
// by the manifest. So the class is made unreachable by building images THROUGH
// THE ENGINE'S OWN CONSTRUCTION PATH rather than by hand -- there is one place
// that knows the whole sequence, and a fixture cannot forget half of it.
#ifndef BASALT_RIG_IMAGE_FIXTURE_H_
#define BASALT_RIG_IMAGE_FIXTURE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "internal_key.h"
#include "test_env.h"

namespace basalt {
namespace rig {

// One version, as a fixture wants to state it.
struct FixtureCell {
  std::string user_key;
  uint64_t seq = 0;
  bool deletion = false;
  std::string value;
};

// Builds a durable image holding one table per group in `tables`, each named by
// the manifest, each with a durable directory entry.
//
// Entries within a group must already be in TABLE ORDER -- user key ascending,
// tag descending. A group that is not is a fixture testing TableBuilder's
// BASALT_CHECK rather than whatever it meant to test, so the builder's refusal is
// left in place rather than sorted around.
testenv::DurableImage BuildImage(const std::string& dir,
                                 const std::vector<std::vector<FixtureCell>>& tables);

// The same, for a caller that already has table BYTES and wants them named.
testenv::DurableImage ImageHoldingTables(const std::string& dir,
                                         const std::vector<std::string>& table_bytes);

}  // namespace rig
}  // namespace basalt

#endif  // BASALT_RIG_IMAGE_FIXTURE_H_
