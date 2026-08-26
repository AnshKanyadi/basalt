// RIFT_ORACLE -- see merge_check.h and ORACLES.txt.
#include "merge_check.h"

#include <algorithm>

#include "internal_key.h"
#include "table_format.h"

namespace rift {
namespace rig {
namespace {

struct Entry {
  std::string key;    // internal key
  std::string value;
};

// Every entry of one table image, in the order the bytes hold them.
bool Enumerate(Slice image, std::vector<Entry>* out, std::string* why) {
  sst::Footer footer;
  if (!sst::DecodeFooter(image, &footer, why)) return false;
  std::vector<sst::BlockEntry> index;
  std::vector<uint32_t> restarts;
  const Slice index_block(image.data() + footer.index.offset, footer.index.size);
  if (!sst::ParseBlock(index_block, &index, &restarts, why)) return false;
  for (const sst::BlockEntry& ie : index) {
    sst::BlockHandle h;
    if (!sst::DecodeHandle(ie.value, &h)) { *why = "index value is not a handle"; return false; }
    std::vector<sst::BlockEntry> entries;
    std::vector<uint32_t> block_restarts;
    const Slice data(image.data() + h.offset, h.size);
    if (!sst::ParseBlock(data, &entries, &block_restarts, why)) return false;
    for (const sst::BlockEntry& e : entries) {
      Entry entry;
      entry.key.assign(e.key.data(), e.key.size());
      entry.value.assign(e.value.data(), e.value.size());
      out->push_back(entry);
    }
  }
  return true;
}

MergeVerdict Violation(MergeVerdict v, const std::string& why) {
  v.outcome = RunOutcome::kContractViolation;
  v.why = why;
  return v;
}

}  // namespace

std::size_t InputEntryCount(const std::vector<std::string>& inputs) {
  std::size_t n = 0;
  for (const std::string& image : inputs) {
    std::vector<Entry> e;
    std::string why;
    if (Enumerate(Slice(image), &e, &why)) n += e.size();
  }
  return n;
}

MergeVerdict AdjudicateMerge(const VersionModel& model,
                             const std::vector<std::string>& inputs,
                             const std::string& output) {
  MergeVerdict v;
  std::string why;

  // 1. THE EXPECTATION, BUILT FROM THE INPUTS. This is the one place deriving
  //    from engine bytes is sound (B3-D2b): the INPUTS are not the thing under
  //    test -- the OUTPUT is -- and each input was validated by the classifier
  //    when it was opened.
  std::vector<Entry> expected;
  for (const std::string& image : inputs) {
    if (!Enumerate(Slice(image), &expected, &why)) {
      return Violation(v, "an input image did not parse: " + why);
    }
  }
  v.input_entries = expected.size();

  std::stable_sort(expected.begin(), expected.end(),
                   [](const Entry& a, const Entry& b) {
                     return CompareInternalKey(Slice(a.key), Slice(b.key)) < 0;
                   });

  // 2. FILTERED BY THE DROP CLAIM. Only what B3-D1 REQUIRES must survive; a
  //    permitted drop may or may not have been taken, so an entry that is
  //    neither required nor present in the output is not a violation.
  const std::set<VersionId> required = model.Required();

  // 3. THE OUTPUT, IN THE ORDER THE BYTES HOLD IT.
  std::vector<Entry> got;
  if (!Enumerate(Slice(output), &got, &why)) {
    return Violation(v, "the output image did not parse: " + why);
  }
  v.output_entries = got.size();

  // 4. ORDER. The output must be strictly ascending in the internal order --
  //    checked here as well as by the classifier, because a merge is the one
  //    producer that can emit a legal table whose ORDER came out of a wrong
  //    traversal rather than a wrong writer.
  for (std::size_t i = 1; i < got.size(); ++i) {
    if (CompareInternalKey(Slice(got[i].key), Slice(got[i - 1].key)) <= 0) {
      return Violation(v, "the merge emitted entry " + std::to_string(i) +
                              " out of order: a merge that emits every required "
                              "entry in the wrong order satisfies every set-based "
                              "check there is");
    }
  }

  // 5. EVERY REQUIRED ENTRY PRESENT, WITH ITS VALUE. The value comparison is
  //    what a set of (user_key, seq) cannot make: a merge that shifted every
  //    value by one position would pass the adjudicator entirely.
  std::size_t expected_kept = 0;
  for (const Entry& e : expected) {
    const VersionId id{ExtractUserKey(Slice(e.key)).ToString(),
                       SeqOfTag(ExtractTag(Slice(e.key)))};
    if (required.find(id) == required.end()) continue;
    ++expected_kept;
    bool found = false;
    for (const Entry& g : got) {
      if (g.key != e.key) continue;
      found = true;
      if (g.value != e.value) {
        return Violation(v, "the merge changed a value: key \"" + id.first +
                                "\" at sequence " + std::to_string(id.second) +
                                " came out holding different bytes than the input "
                                "it was merged from");
      }
      break;
    }
    if (!found) {
      return Violation(v, "the merge lost a required entry: key \"" + id.first +
                              "\" at sequence " + std::to_string(id.second));
    }
  }
  v.expected_entries = expected_kept;

  // 6. AND NOTHING INVENTED. Every output entry must come from an input.
  for (const Entry& g : got) {
    bool from_input = false;
    for (const Entry& e : expected) {
      if (e.key == g.key && e.value == g.value) { from_input = true; break; }
    }
    if (!from_input) {
      return Violation(v, "the merge emitted an entry no input contained");
    }
  }
  return v;
}

}  // namespace rig
}  // namespace rift
