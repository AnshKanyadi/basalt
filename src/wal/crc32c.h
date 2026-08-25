// CRC-32C (Castagnoli), the checksum every WAL fragment carries.
#ifndef RIFT_WAL_CRC32C_H_
#define RIFT_WAL_CRC32C_H_

#include <cstddef>
#include <cstdint>

namespace rift {
namespace wal {

// Software table-driven, reflected, polynomial 0x82F63B78. No hardware path:
// the SSE4.2 and ARMv8 CRC instructions produce the same values, but a code
// path selected by CPUID is a code path that differs between the machine that
// wrote a WAL and the machine that reads it -- and if the two ever disagreed,
// the symptom would be a checksum failure that reproduces on one machine and
// not the other, which is the exact shape of bug this project exists to
// eliminate. It is measured at B5 before any hardware path is considered.
uint32_t Crc32c(const char* data, std::size_t n);

// The canonical CRC-32C check value: Crc32c("123456789") == 0xE3069283.
inline constexpr uint32_t kCrc32cCheckValue = 0xE3069283u;

}  // namespace wal
}  // namespace rift

#endif  // RIFT_WAL_CRC32C_H_
