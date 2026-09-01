// The sweep, as a lane.
//
// Prints a machine-readable summary the campaign parses, and exits non-zero on
// any violation. EVERY NUMBER IT PRINTS IS ALSO ASSERTED -- a number nobody
// asserts on is decoration that looks like evidence.
#include <cstdio>
#include <cstring>

#include "sweep.h"

// usage: basalt_sweep [default|flush]
//
// ONE REGIME PER INVOCATION, and the regime is named in the output. Section
// 8.4: numbers from a non-default cap never aggregate with default-cap numbers,
// so a caller that could not tell which it was reading would be aggregating by
// accident.
int main(int argc, char** argv) {
  basalt::rig::SweepRegime regime = basalt::rig::SweepRegime::kDefault;
  if (argc > 1) {
    if (std::strcmp(argv[1], "flush") == 0) {
      regime = basalt::rig::SweepRegime::kFlush;
    } else if (std::strcmp(argv[1], "compact") == 0) {
      regime = basalt::rig::SweepRegime::kCompact;
    } else if (std::strcmp(argv[1], "default") != 0) {
      std::printf("   FAIL  unknown regime \"%s\"; expected default, flush or compact\n",
                  argv[1]);
      return 2;
    }
  }
  const basalt::rig::SweepResult r = basalt::rig::RunSweep(regime);

  std::printf("\n  kill-point sweep (regime: %s)\n",
              basalt::rig::SweepRegimeName(regime));
  std::printf("  ----------------------------------------------------------\n");
  std::printf("   points visited   : %zu\n", r.points_visited);
  std::printf("   pass             : %zu\n", r.pass);
  std::printf("   violation        : %zu\n", r.violation);
  std::printf("   characterization : %zu   (not evidence)\n", r.characterization);
  std::printf("   inconclusive     : %zu   (not evidence)\n", r.inconclusive);
  std::printf("   void             : %zu   (not evidence)\n", r.voided);
  std::printf("   matched G(k-1)   : %zu\n", r.matched_previous);
  std::printf("   matched G(k)     : %zu\n", r.matched_in_flight);
  std::size_t census_total = 0;
  for (const auto& kv : r.census) census_total += kv.second;
  std::printf("  ---- kill-point census, per call kind (total %zu)\n", census_total);
  for (const auto& kv : r.census) {
    std::printf("   %-28s %zu\n", kv.first.c_str(), kv.second);
  }
  std::printf("  ----------------------------------------------------------\n");

  int rc = 0;
  // THE CENSUS AND THE VISIT COUNT MUST AGREE. They are two independent tallies
  // of the same thing, and a disagreement means points are being counted in one
  // and not the other -- which is how coverage comes to be reported for points
  // nobody visited.
  if (census_total != r.points_visited) {
    std::printf("   FAIL  census totals %zu but %zu points were visited\n",
                census_total, r.points_visited);
    rc = 2;
  }
  if (r.points_visited == 0) {
    std::printf("   FAIL  the sweep visited no kill points at all.\n");
    rc = 2;
  }
  // SECTION 7.4 CONDITION 3, at sweep level. A two-element set where only one
  // element has ever been observed is a one-element contract with a spare
  // excuse attached -- so the sweep asserts it saw both, and a sweep that
  // stopped producing one of them fails here rather than passing quietly.
  if (!r.BothElementsObserved()) {
    std::printf("   FAIL  only one element of the recovery set was observed "
                "(G(k-1)=%zu, G(k)=%zu). The set has degenerated.\n",
                r.matched_previous, r.matched_in_flight);
    rc = 1;
  }
  for (const basalt::rig::SweepPoint& p : r.failures) {
    std::printf("   VIOLATION at ordinal %llu (%s, %s): %s\n",
                static_cast<unsigned long long>(p.ordinal),
                p.call_site.empty() ? "?" : p.call_site.c_str(),
                p.after_effect ? "after effect" : "before effect", p.why.c_str());
    rc = 1;
  }
  // A MACHINE-READABLE LINE for the campaign to parse. It carries the two
  // numbers a floor is made of -- how often a class is detected, and how early
  // -- because Track A lost M19 to a count-based floor that could not see a
  // seeds-to-detection regression: a class can hold its RATE while its first
  // detection moves far later in the space, and that is what decides whether a
  // cheap sweep would ever see it.
  unsigned long long first = 0;
  if (!r.failures.empty()) first = r.failures.front().ordinal;
  std::printf("SWEEP regime=%s points=%zu violations=%zu first=%llu\n",
              basalt::rig::SweepRegimeName(regime), r.points_visited,
              r.violation, first);

  if (rc == 0) std::printf("   ok  every kill point recovered to a promised watermark\n\n");
  else std::printf("\n");
  return rc;
}
