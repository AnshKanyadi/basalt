// SHA-256, for the WAL byte digest.
//
// The digest earns its own line in section 10.2: same workload, same WAL bytes,
// pinned. It catches THREE THINGS FOR ONE TEST -- ambient randomness,
// uninitialized padding, and any float that reached a serialization path -- and
// it is why MSan stays declined, since MSan's value here (uninitialized bytes
// reaching the disk) is covered at a fraction of the cost.
//
// It is the C++ analogue of Track A's trace hash: not a security primitive, a
// change detector, but a wide one -- a 64-bit hash would be adequate and
// SHA-256 removes the argument entirely.
#ifndef RIFT_WAL_SHA256_H_
#define RIFT_WAL_SHA256_H_

#include <cstddef>
#include <cstdint>
#include <string>

namespace rift {
namespace wal {

// Lowercase hex, 64 characters.
std::string Sha256Hex(const char* data, std::size_t n);

}  // namespace wal
}  // namespace rift

#endif  // RIFT_WAL_SHA256_H_
