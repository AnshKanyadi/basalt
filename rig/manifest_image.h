// A MANIFEST'S BYTES, REPLAYED. PURE: no Env, no rotation, no install.
//
// NOT AN ORACLE and not a fixture -- it is the parse both of those need, and it
// exists as one function because the alternative is what B3-D2a warns about
// from the other direction.
//
// `Manifest::Open` is the engine's path and it is an ACT WITH AN OPINION: it
// verifies, rotates, deletes the manifest it replaced and installs a new
// CURRENT. A TEST THAT USED IT TO OBSERVE A RUNNING ENGINE DESTROYED THE
// MANIFEST IT WAS OBSERVING, and the engine's next append failed on a vanished
// file. The artifact/belief split is usually argued as a rule about what a
// VERDICT may rest on; this is the same split showing up as a rule about what
// an OBSERVATION may cost.
#ifndef RIFT_RIG_MANIFEST_IMAGE_H_
#define RIFT_RIG_MANIFEST_IMAGE_H_

#include <string>

#include "manifest_format.h"
#include "slice.h"

namespace rift {
namespace rig {

bool ReplayManifestImage(Slice image, sst::ManifestState* out, std::string* why);

}  // namespace rig
}  // namespace rift

#endif  // RIFT_RIG_MANIFEST_IMAGE_H_
