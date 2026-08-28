#include "status.h"

namespace rift {

const char* CodeName(Status::Code code) {
  // NO `default:` ARM. See status.h. Adding an enumerator must break this
  // build, which is the entire point of the closed enum.
  switch (code) {
    case Status::Code::kOk:              return "kOk";
    case Status::Code::kNotFound:        return "kNotFound";
    case Status::Code::kRecordTooLarge:  return "kRecordTooLarge";
    case Status::Code::kWalBufferFull:   return "kWalBufferFull";
    case Status::Code::kBusy:            return "kBusy";
    case Status::Code::kIoError:         return "kIoError";
    case Status::Code::kDiskFull:        return "kDiskFull";
    case Status::Code::kCorruption:      return "kCorruption";
    case Status::Code::kKilled:          return "kKilled";
    case Status::Code::kInvalidArgument: return "kInvalidArgument";
  }
  RIFT_UNREACHABLE("Status::Code holds a value no enumerator names");
}

std::string Status::ToString() const {
  std::string out = CodeName(code_);
  if (!message_.empty()) {
    out += ": ";
    out += message_;
  }
  return out;
}

}  // namespace rift
