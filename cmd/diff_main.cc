// usage: basalt_diff <regime> <seed> [kill_ordinal] > artifact.diff
//
// Runs one differential schedule and writes an UNJUDGED artifact to stdout.
// It reaches no verdict: reaching one requires the reference model, which is
// judged in a separate process.
//
// THE COMMITS COME FROM THE ENVIRONMENT, NOT FROM THIS PROGRAM. `BASALT_ENGINE_COMMIT`
// and `BASALT_MODEL_COMMIT` are set by the lane that runs it, because a binary
// cannot know the commit it was built from without being rebuilt on every
// commit -- and a stale constant compiled in would be worse than an absent one.
// The format refuses an artifact naming no commit, so a lane that forgets to
// set them produces a file the judge will not accept.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "differential_artifact.h"
#include "differential_driver.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: basalt_diff <default|flush|compact> <seed> [kill_ordinal]\n");
    return 2;
  }
  basalt::rig::DiffRunOptions o;
  if (std::strcmp(argv[1], "default") == 0) {
    o.regime = basalt::rig::DiffRegime::kDefault;
  } else if (std::strcmp(argv[1], "flush") == 0) {
    o.regime = basalt::rig::DiffRegime::kFlush;
  } else if (std::strcmp(argv[1], "compact") == 0) {
    o.regime = basalt::rig::DiffRegime::kCompact;
  } else {
    std::fprintf(stderr, "unknown regime \"%s\"\n", argv[1]);
    return 2;
  }
  o.seed = std::strtoull(argv[2], nullptr, 10);
  if (argc > 3) o.kill_ordinal = std::strtoull(argv[3], nullptr, 10);

  const char* engine = std::getenv("BASALT_ENGINE_COMMIT");
  const char* model = std::getenv("BASALT_MODEL_COMMIT");
  if (engine != nullptr) o.engine_commit = engine;
  if (model != nullptr) o.model_commit = model;

  const basalt::rig::DiffArtifact a = basalt::rig::RunDifferential(o);
  if (!a.reopen_error.empty()) {
    // THE REOPEN FAILED, so there is no recovered state to judge. Reporting it
    // here rather than emitting an artifact claiming an empty recovery is the
    // difference between "the engine lost everything" and "the rig could not
    // ask" -- HARNESS-006's distinction, and the rig must not blame the engine
    // for its own inability to look.
    std::fprintf(stderr, "REOPEN FAILED: %s\n", a.reopen_error.c_str());
    return 3;
  }
  const std::string bytes = basalt::rig::EncodeDiffArtifact(a);
  std::fwrite(bytes.data(), 1, bytes.size(), stdout);
  return 0;
}
