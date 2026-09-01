#include "basalt/fault_controller.h"

namespace basalt {

FaultController::~FaultController() = default;

namespace {
class NoFaults final : public FaultController {
 public:
  Status Intercept(CallSite, HandleId) override { return Status::Ok(); }
  Status AfterEffect(CallSite, HandleId, Status s) override { return s; }
};
}  // namespace

FaultController* NoFaultController() {
  static NoFaults* const instance = new NoFaults();
  return instance;
}

}  // namespace basalt
