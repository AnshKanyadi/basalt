#include "fault_controller.h"

namespace rift {

FaultController::~FaultController() = default;

namespace {
class NoFaults final : public FaultController {
 public:
  Status Intercept(CallSite, HandleId) override { return Status::Ok(); }
};
}  // namespace

FaultController* NoFaultController() {
  static NoFaults* const instance = new NoFaults();
  return instance;
}

}  // namespace rift
