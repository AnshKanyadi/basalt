#include "env_guard.h"

#include "basalt/check.h"

namespace basalt {
namespace {

thread_local uint64_t g_env_calls = 0;
thread_local int g_mutex_depth = 0;

void AbortOnViolation(const char* what) { BASALT_UNREACHABLE(what); }

GuardViolationFn g_handler = &AbortOnViolation;

}  // namespace

uint64_t EnvCallsOnThisThread() { return g_env_calls; }
void NoteEnvCall() { ++g_env_calls; }

MutexHeldMarker::MutexHeldMarker() { ++g_mutex_depth; }
MutexHeldMarker::~MutexHeldMarker() { --g_mutex_depth; }
int MutexDepthOnThisThread() { return g_mutex_depth; }

void SetGuardViolationHandler(GuardViolationFn fn) {
  g_handler = (fn == nullptr) ? &AbortOnViolation : fn;
}
void ReportGuardViolation(const char* what) { g_handler(what); }

}  // namespace basalt
