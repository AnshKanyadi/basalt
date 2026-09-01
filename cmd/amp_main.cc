// usage: basalt_amp
//
// THE NUMBER, THE THRESHOLD, THE CONCLUSION -- IN THAT ORDER, and the order is
// the point. A measurement printed without the threshold it was compared against
// invites the reader to supply their own, and a conclusion printed without the
// number invites them to take it on trust.
//
// DESIGN-B3 section 8.1 fixed the threshold BEFORE this program existed, and
// section 8.2b fixed what to do with each outcome. Neither is decided here.
#include <cstdio>
#include <vector>

#include "amplification.h"
#include "basalt/caps.h"

int main() {
  // THE SHIPPED CAPS. The measurement is about the engine as it will run, and a
  // lowered flush threshold would move the crossing point with it -- which is
  // exactly why `CrossingPointBytes` derives from the caps rather than being a
  // constant, so a reader who changes them cannot compare against a threshold
  // that followed from a setting they have just changed.
  basalt::wal::Caps caps;

  // THREE SIZES SPANNING THE PREDICTED CROSSING POINT, per section 8.1: the
  // deliverable is the CURVE and where it crosses, not one figure. A single
  // point cannot distinguish "the model is right" from "the number happened to
  // land there".
  const uint64_t crossing = basalt::rig::CrossingPointBytes(caps);
  const std::vector<uint64_t> sizes = {crossing / 8, crossing / 2, crossing};

  const basalt::rig::AmpResult r =
      basalt::rig::MeasureAmplification(caps, sizes);

  std::printf("\n  amplification (flush threshold %llu bytes, L0 trigger 4)\n",
              static_cast<unsigned long long>(caps.flush_bytes));
  std::printf("  ----------------------------------------------------------\n");
  std::printf("   %12s %10s %8s %8s %8s %8s\n", "live bytes", "on disk",
              "space", "write", "read", "L0 left");
  for (const basalt::rig::AmpPoint& p : r.points) {
    std::printf("   %12llu %10llu %8.2f %8.2f %8.2f %8llu\n",
                static_cast<unsigned long long>(p.live_bytes),
                static_cast<unsigned long long>(p.disk_bytes), p.space, p.write,
                p.read, static_cast<unsigned long long>(p.l0_at_end));
  }
  std::printf("  ----------------------------------------------------------\n");
  std::printf("   crossing point   : %llu bytes of live data\n",
              static_cast<unsigned long long>(r.crossing_bytes));
  std::printf("   measured above it: %s\n", r.above_crossing ? "yes" : "no");
  std::printf("   predicted WA     : 2 + D/(K*F), so %.2f at the crossing point\n",
              2.0 + 8.0);
  std::printf("\n");
  // THE CONCLUSION IS PRINTED, NOT LEFT TO THE READER -- and it is the one
  // DESIGN-B3 section 8.2b fixed in advance for each outcome.
  if (!r.above_crossing) {
    std::printf("   CONCLUSION: the measured sizes sit BELOW the crossing point, so the\n"
                "   question is NOT DECIDABLE ON EVIDENCE and (b) wins on Amendment A6's\n"
                "   rule rather than on a benchmark. This is a result, not an inconclusive:\n"
                "   the instrument finished and returned a number.\n");
    return 0;
  }
  double at_crossing = 0.0;
  uint64_t l0_left = 0;
  for (const basalt::rig::AmpPoint& p : r.points) {
    if (p.live_bytes >= r.crossing_bytes) { at_crossing = p.write; l0_left = p.l0_at_end; }
  }
  if (at_crossing > 10.0) {
    std::printf("   CONCLUSION: write amplification is %.2f at the crossing point, ABOVE\n"
                "   the 10x threshold section 3 fixed in advance. (c) is reopened.\n",
                at_crossing);
  } else {
    std::printf("   CONCLUSION: write amplification is %.2f at the crossing point, BELOW\n"
                "   the 10x threshold section 3 fixed in advance -- so (b) holds AT THE SIZE\n"
                "   the threshold named, and wins on the measurement rather than on A6's\n"
                "   rule alone. The model over-predicted, which is the safe direction for a\n"
                "   threshold to be wrong.\n", at_crossing);
    if (l0_left > 0) {
      std::printf("\n   CAVEAT, STATED RATHER THAN LEFT TO BE ASSUMED: %llu L0 file(s) were\n"
                  "   still uncompacted when the workload stopped, so this number has not\n"
                  "   paid for their compaction and is a SNAPSHOT MID-CYCLE. The steady-state\n"
                  "   value is higher by at most one L0 generation.\n",
                  static_cast<unsigned long long>(l0_left));
    }
  }
  return 0;
}
