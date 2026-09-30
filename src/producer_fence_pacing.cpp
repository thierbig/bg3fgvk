#include "producer_fence_pacing.h"
#include "config.h"
#include "log.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace fgvk {
namespace {
constexpr size_t kLast = 4, kCommands = 64, kSync = 16, kObjects = 512;
template<class H> uint64_t Bits(H handle) { return (uint64_t)(uintptr_t)handle; }
struct Object {
  uint64_t handle{}, epoch{};
  uint32_t borrowers{};
  bool timeline{}, retiring{};
};
struct Point {
  uint64_t handle{}, epoch{}, value{}, stage{};
  uint32_t submitIndex{};
  bool timeline{};
};
struct Submit {
  uint64_t sequence{}, order{}, fence{}, epoch{};
  uint32_t thread{}, api{}, count{}, commandCount{}, waitCount{}, signalCount{};
  bool truncated{};
  std::array<uint64_t, kCommands> commands{};
  std::array<Point, kSync> waits{}, signals{};
};
struct Queue {
  uint64_t handle{}, sequence{}, source{}, endedSource{}, endedSequence{}, endedOrder{};
  uint32_t endedToken{}, endedThread{};
  bool endedValid{};
  std::array<Submit, kLast> last{};
};
struct Input {
  uint32_t token{};
  uint64_t command{}, depth{}, motion{}, hudless{};
  bool hudlessTagged{};
};
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_borrowCv = CONDITION_VARIABLE_INIT;
struct Guard {
  Guard() { AcquireSRWLockExclusive(&g_lock); }
  ~Guard() { ReleaseSRWLockExclusive(&g_lock); }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;
};
std::array<Object, kObjects> g_fences{}, g_semaphores{};
std::array<Queue, 4> g_queues{};
std::array<Input, 64> g_inputs{};
uint64_t g_epoch{}, g_order{}, g_generation{};
bool g_ambiguous{};
VkDevice g_device{};
PFN_vkGetFenceStatus g_status{};
PFN_vkWaitForFences g_wait{};
PFN_vkQueueWaitIdle g_idle{};
struct Dispatch {
  VkDevice device{};
  bool canonical{};
  PFN_vkCreateFence createFence{};
  PFN_vkResetFences resetFences{};
  PFN_vkDestroyFence destroyFence{};
  PFN_vkCreateSemaphore createSemaphore{};
  PFN_vkDestroySemaphore destroySemaphore{};
};
// Early instance lookups may precede device creation. Device lookup targets
// are never used to forward another device's lifecycle calls.
Dispatch g_instanceDispatch{};
std::array<Dispatch, 4> g_dispatches{};
std::atomic<uint64_t> g_waits{0}, g_fallbacks{0};
std::atomic<uint64_t> g_loggedWaits{0}, g_loggedFallbacks{0};

bool Enabled() { return Cfg().x4ProducerFencePacing; }
Dispatch* FindDispatch(VkDevice device, bool create = false) {
  for (auto& dispatch : g_dispatches) if (dispatch.device == device) return &dispatch;
  if (create) {
    for (auto& dispatch : g_dispatches) if (!dispatch.device) {
      dispatch.device = device; return &dispatch;
    }
    g_ambiguous = true;
  }
  return nullptr;
}
template<class F> struct Forwarder { F function; bool tracked; };
template<class F> Forwarder<F> Forward(VkDevice device, F Dispatch::* member) {
  Guard guard;
  auto* dispatch = FindDispatch(device);
  const auto function = dispatch && dispatch->*member ? dispatch->*member : g_instanceDispatch.*member;
  return {function, Enabled() && device == g_device};
}
uint64_t Advance(uint64_t& value) {
  if (value == UINT64_MAX) { g_ambiguous = true; return 0; }
  return ++value;
}
Object* Find(std::array<Object, kObjects>& objects, uint64_t handle) {
  if (!handle) return nullptr;
  for (auto& object : objects) if (object.handle == handle) return &object;
  return nullptr;
}
Queue* FindQueue(uint64_t handle, bool create = false) {
  if (!handle) return nullptr;
  for (auto& queue : g_queues) if (queue.handle == handle) return &queue;
  if (create) {
    for (auto& queue : g_queues) if (!queue.handle) { queue.handle = handle; return &queue; }
    g_ambiguous = true;
  }
  return nullptr;
}
void Created(std::array<Object, kObjects>& objects, uint64_t handle, bool timeline) {
  if (!handle) return;
  Object* object = Find(objects, handle);
  // An unexpected duplicate live handle must never replace a leased payload.
  if (object) { g_ambiguous = true; return; }
  for (auto& candidate : objects) if (!candidate.handle) { object = &candidate; break; }
  if (!object) { g_ambiguous = true; return; }
  const uint64_t epoch = Advance(g_epoch);
  *object = {handle, epoch, 0, timeline, false};
}
void WaitForBorrowers(Object& object) {
  // SleepConditionVariableSRW releases the registry lock while waiting.
  while (object.borrowers) SleepConditionVariableSRW(&g_borrowCv, &g_lock, INFINITE, 0);
}
Point Sync(VkSemaphore handle, uint64_t value, uint64_t stage, uint32_t submitIndex) {
  Point point{Bits(handle), 0, value, stage, submitIndex, false};
  if (const auto* object = Find(g_semaphores, point.handle)) {
    point.epoch = object->epoch; point.timeline = object->timeline;
  }
  return point;
}
void Store(VkQueue handle, Submit& submit, uint64_t submittedEpoch) {
  auto* queue = FindQueue(Bits(handle), true);
  if (!queue) return;
  submit.sequence = Advance(queue->sequence);
  submit.order = Advance(g_order);
  submit.thread = GetCurrentThreadId();
  if (const auto* object = Find(g_fences, submit.fence))
    if (submittedEpoch && !object->retiring && object->epoch == submittedEpoch)
      submit.epoch = submittedEpoch;
  if (submit.sequence) queue->last[(submit.sequence - 1) % kLast] = submit;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice device, const VkFenceCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkFence* out) {
  const auto call = Forward(device, &Dispatch::createFence);
  if (!call.function) return VK_ERROR_INITIALIZATION_FAILED;
  const auto result = call.function(device, info, allocator, out);
  if (result == VK_SUCCESS && Enabled()) {
    Guard guard;
    if (device == g_device) Created(g_fences, Bits(*out), false);
  }
  return result;
}
VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice device, uint32_t count, const VkFence* fences) {
  const auto call = Forward(device, &Dispatch::resetFences);
  if (!call.function) return VK_ERROR_INITIALIZATION_FAILED;
  if (!call.tracked) return call.function(device, count, fences);
  Guard guard;
  for (uint32_t i = 0; i < count; ++i)
    if (auto* object = Find(g_fences, Bits(fences[i]))) object->retiring = true;
  for (uint32_t i = 0; i < count; ++i)
    if (auto* object = Find(g_fences, Bits(fences[i]))) WaitForBorrowers(*object);
  // Even a failed reset invalidates old submission evidence, fail closed.
  for (uint32_t i = 0; i < count; ++i)
    if (auto* object = Find(g_fences, Bits(fences[i]))) object->epoch = Advance(g_epoch);
  const auto result = call.function(device, count, fences);
  for (uint32_t i = 0; i < count; ++i)
    if (auto* object = Find(g_fences, Bits(fences[i]))) object->retiring = false;
  return result;
}
VKAPI_ATTR void VKAPI_CALL DestroyFence(VkDevice device, VkFence fence,
    const VkAllocationCallbacks* allocator) {
  const auto call = Forward(device, &Dispatch::destroyFence);
  if (!call.function) { Log("X4 producer fence pacing: missing destroy-fence dispatcher"); return; }
  if (!call.tracked) { call.function(device, fence, allocator); return; }
  Guard guard;
  if (auto* object = Find(g_fences, Bits(fence))) {
    object->retiring = true; WaitForBorrowers(*object); *object = {};
  }
  call.function(device, fence, allocator);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice device, const VkSemaphoreCreateInfo* info,
    const VkAllocationCallbacks* allocator, VkSemaphore* out) {
  const auto call = Forward(device, &Dispatch::createSemaphore);
  if (!call.function) return VK_ERROR_INITIALIZATION_FAILED;
  const auto result = call.function(device, info, allocator, out);
  if (result == VK_SUCCESS && Enabled()) {
    bool timeline = false, complete = true;
    auto* next = (const VkBaseInStructure*)info->pNext;
    for (uint32_t i = 0; next; ++i, next = next->pNext) {
      if (i == 32) { complete = false; break; }
      if (next->sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO)
        timeline = ((const VkSemaphoreTypeCreateInfo*)next)->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE;
    }
    Guard guard;
    if (device != g_device) return result;
    if (complete) Created(g_semaphores, Bits(*out), timeline);
    else g_ambiguous = true;
  }
  return result;
}
VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice device, VkSemaphore semaphore,
    const VkAllocationCallbacks* allocator) {
  const auto call = Forward(device, &Dispatch::destroySemaphore);
  if (!call.function) { Log("X4 producer fence pacing: missing destroy-semaphore dispatcher"); return; }
  if (!call.tracked) { call.function(device, semaphore, allocator); return; }
  Guard guard;
  if (auto* object = Find(g_semaphores, Bits(semaphore))) *object = {};
  call.function(device, semaphore, allocator);
}

// RAII guarantees reset/destroy protection is released on every wait outcome.
struct FenceLease {
  Object* object;
  explicit FenceLease(Object* value) : object(value) {} // borrowed under Guard
  ~FenceLease() {
    Guard guard;
    --object->borrowers; WakeAllConditionVariable(&g_borrowCv);
  }
  FenceLease(const FenceLease&) = delete;
  FenceLease& operator=(const FenceLease&) = delete;
};
bool WaitTerminal(const ProducerFenceBoundary& boundary, bool requireHudless) {
  Object* leased = nullptr;
  uint64_t handle = 0, epoch = 0;
  VkDevice device{};
  PFN_vkWaitForFences wait{};
  {
    Guard guard;
    if (g_ambiguous || !g_wait || !g_status || boundary.thread != GetCurrentThreadId() ||
        boundary.generation != g_generation) return false;
    auto* queue = FindQueue(boundary.queue);
    if (!queue || !queue->endedValid || !queue->endedOrder || !queue->endedToken ||
        queue->endedThread != boundary.thread || queue->endedSource == UINT64_MAX ||
        boundary.source != queue->endedSource + 1 || queue->endedToken == UINT32_MAX ||
        boundary.token != queue->endedToken + 1 || queue->sequence != boundary.sequence ||
        queue->endedSequence > UINT64_MAX - kLast ||
        queue->sequence != queue->endedSequence + kLast) return false;
    const auto& input = g_inputs[boundary.token % g_inputs.size()];
    if (!boundary.token || input.token != boundary.token || !input.command || !input.depth || !input.motion ||
        (requireHudless && (!input.hudless || !input.hudlessTagged))) return false;
    std::array<const Submit*, kLast> submits{};
    for (size_t i = 0; i < kLast; ++i) {
      const auto& submit = queue->last[(queue->endedSequence + i) % kLast];
      submits[i] = &submit;
      if (submit.sequence != queue->endedSequence + i + 1 || submit.api != 2 || submit.count != 1 ||
          submit.truncated || submit.thread != boundary.thread ||
          submit.order <= queue->endedOrder || submit.order > boundary.order) return false;
    }
    const auto& first = *submits[0]; const auto& nr = *submits[1];
    const auto& empty = *submits[2]; const auto& terminal = *submits[3];
    // Preserve the validated selector. Shape differences take queue idle.
    if ((first.commandCount != 1 && first.commandCount != 2) || first.waitCount || first.signalCount ||
        nr.commandCount != 34 || nr.waitCount || nr.signalCount != 1 ||
        empty.commandCount || empty.waitCount || empty.signalCount || empty.fence ||
        terminal.commandCount != 2 || terminal.waitCount != 1 || terminal.signalCount != 2 ||
        std::count(nr.commands.begin(), nr.commands.begin() + nr.commandCount, input.command) != 1) return false;
    const auto& a = nr.signals[0]; const auto& b = terminal.signals[0];
    const auto& acquire = terminal.waits[0]; const auto& binary = terminal.signals[1];
    if (!a.timeline || !b.timeline || !a.epoch || a.handle != b.handle || a.epoch != b.epoch ||
        a.stage != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT || b.stage != VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT ||
        a.submitIndex || b.submitIndex || a.value == UINT64_MAX || b.value != a.value + 1 ||
        acquire.timeline || !acquire.epoch || binary.timeline || !binary.epoch ||
        binary.stage != VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT) return false;
    for (const auto* point : {&a, &acquire, &binary}) {
      const auto* object = Find(g_semaphores, point->handle);
      if (!object || object->epoch != point->epoch || object->timeline != point->timeline) return false;
    }
    for (const auto* submit : {&first, &nr, &terminal}) {
      const auto* object = Find(g_fences, submit->fence);
      if (!object || !submit->epoch || object->epoch != submit->epoch || object->retiring) return false;
    }
    if (terminal.fence == first.fence || terminal.fence == nr.fence) return false;
    leased = Find(g_fences, terminal.fence);
    handle = terminal.fence; epoch = terminal.epoch;
    device = g_device; wait = g_wait;
    const auto status = g_status(device, (VkFence)(uintptr_t)handle);
    if (status != VK_SUCCESS && status != VK_NOT_READY) return false;
    ++leased->borrowers;
  }
  FenceLease lease(leased);
  const VkFence fence = (VkFence)(uintptr_t)handle;
  // Neither the registry lock nor the frame-token mutex is held here.
  const auto result = wait(device, 1, &fence, VK_TRUE, UINT64_MAX);
  {
    Guard guard;
    const auto* current = Find(g_fences, handle);
    const auto* queue = FindQueue(boundary.queue);
    if (result != VK_SUCCESS || current != leased || current->epoch != epoch || !queue ||
        queue->sequence != boundary.sequence || boundary.generation != g_generation) return false;
  }
  return true;
}
}

void ProducerFenceInitialize(VkDevice device, PFN_vkGetDeviceProcAddr resolver) {
  if (!Enabled()) return;
  if (!device || !resolver) { Guard guard; g_ambiguous = true; return; }
  // Caller uses the existing interposer re-entry scope. Resolve outside the
  // registry lock, then publish the correctly layered device table atomically.
  Dispatch resolved{}; resolved.device = device; resolved.canonical = true;
  resolved.createFence = (PFN_vkCreateFence)resolver(device, "vkCreateFence");
  resolved.resetFences = (PFN_vkResetFences)resolver(device, "vkResetFences");
  resolved.destroyFence = (PFN_vkDestroyFence)resolver(device, "vkDestroyFence");
  resolved.createSemaphore = (PFN_vkCreateSemaphore)resolver(device, "vkCreateSemaphore");
  resolved.destroySemaphore = (PFN_vkDestroySemaphore)resolver(device, "vkDestroySemaphore");
  const auto status = (PFN_vkGetFenceStatus)resolver(device, "vkGetFenceStatus");
  const auto wait = (PFN_vkWaitForFences)resolver(device, "vkWaitForFences");
  const auto idle = (PFN_vkQueueWaitIdle)resolver(device, "vkQueueWaitIdle");
  Guard guard;
  auto* dispatch = FindDispatch(device, true);
  const bool complete = resolved.createFence && resolved.resetFences && resolved.destroyFence &&
      resolved.createSemaphore && resolved.destroySemaphore;
  if (dispatch && !dispatch->canonical) {
    // Keep any already-published forwarding target if canonical resolution
    // fails, but do not allow incomplete observation to justify a fence wait.
#define PUBLISH(field) if (resolved.field) dispatch->field = resolved.field;
    PUBLISH(createFence) PUBLISH(resetFences) PUBLISH(destroyFence)
    PUBLISH(createSemaphore) PUBLISH(destroySemaphore)
#undef PUBLISH
    dispatch->canonical = true;
  }
  if (!complete) {
    g_ambiguous = true;
    Log("X4 producer fence pacing: incomplete canonical lifecycle dispatch; queue-idle fallback retained");
  }
  // A second device cannot silently reuse first-device object/dispatch evidence.
  if (g_device && g_device != device) {
    g_ambiguous = true;
    // Never select old-device fences, but use the active game's queue dispatcher
    // for the conservative fallback rather than a stale device's function table.
    g_idle = idle;
    Log("X4 producer fence pacing: second device; fence selection disabled");
    return;
  }
  g_device = device;
  g_status = status; g_wait = wait; g_idle = idle;
  Log("X4 producer fence pacing enabled (terminal fence with conservative queue-idle fallback); dispatch-fix=1 canonical-lifecycle=%d observer-ambiguous=%d", complete && dispatch ? 1 : 0, g_ambiguous ? 1 : 0);
}
PFN_vkVoidFunction ProducerFenceWrap(const char* name, PFN_vkVoidFunction original, VkDevice resolvedDevice) {
  if (!Enabled() || !name || !original) return original;
  Guard guard;
  // GIPA trampolines and GDPA device functions may legitimately differ. Only
  // device/object/epoch evidence governs ambiguity, not entry-point equality.
  // Wrappers cached during CreateDevice remain valid after canonical publish.
#define WRAP(api, field, wrapper) if (!strcmp(name, api)) { \
    auto* dispatch = resolvedDevice ? FindDispatch(resolvedDevice, true) : &g_instanceDispatch; \
    if (!dispatch) return original; \
    if (!dispatch->field) dispatch->field = (decltype(dispatch->field))original; \
    return (PFN_vkVoidFunction)wrapper; }
  WRAP("vkCreateFence", createFence, CreateFence)
  WRAP("vkResetFences", resetFences, ResetFences)
  WRAP("vkDestroyFence", destroyFence, DestroyFence)
  WRAP("vkCreateSemaphore", createSemaphore, CreateSemaphore)
  WRAP("vkDestroySemaphore", destroySemaphore, DestroySemaphore)
#undef WRAP
  return original;
}
uint64_t ProducerFenceSubmitEpoch(VkFence fence) {
  if (!Enabled()) return 0;
  Guard guard;
  const auto* object = Find(g_fences, Bits(fence));
  return object && !object->retiring ? object->epoch : 0;
}
void ProducerFenceSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo*, VkFence fence,
    uint64_t epoch, VkResult result) {
  if (!Enabled()) return;
  // A legacy submit makes this prefix ineligible; no speculative coverage.
  Guard guard;
  Submit submit{}; submit.api = result == VK_SUCCESS ? 1 : 0;
  submit.count = count; submit.fence = Bits(fence);
  Store(queue, submit, epoch);
}
void ProducerFenceSubmit2(VkQueue queue, uint32_t count, const VkSubmitInfo2* infos, VkFence fence,
    uint64_t epoch, VkResult result) {
  if (!Enabled()) return;
  Guard guard;
  if (result != VK_SUCCESS) {
    // Preserve an invalidation event, never treat failed/possibly partial work
    // as a successful producer or allow an earlier terminal fence to cover it.
    Submit failed{}; Store(queue, failed, 0); return;
  }
  Submit submit{}; submit.api = 2; submit.count = count; submit.fence = Bits(fence);
  for (uint32_t i = 0; i < count; ++i) {
    const auto& info = infos[i];
    for (uint32_t j = 0; j < info.commandBufferInfoCount; ++j) {
      if (submit.commandCount < kCommands) submit.commands[submit.commandCount] = Bits(info.pCommandBufferInfos[j].commandBuffer);
      else submit.truncated = true;
      ++submit.commandCount;
    }
    for (uint32_t j = 0; j < info.waitSemaphoreInfoCount; ++j) {
      const auto& point = info.pWaitSemaphoreInfos[j];
      if (submit.waitCount < kSync) submit.waits[submit.waitCount] = Sync(point.semaphore, point.value, point.stageMask, i);
      else submit.truncated = true;
      ++submit.waitCount;
    }
    for (uint32_t j = 0; j < info.signalSemaphoreInfoCount; ++j) {
      const auto& point = info.pSignalSemaphoreInfos[j];
      if (submit.signalCount < kSync) submit.signals[submit.signalCount] = Sync(point.semaphore, point.value, point.stageMask, i);
      else submit.truncated = true;
      ++submit.signalCount;
    }
  }
  Store(queue, submit, epoch);
}
void ProducerFenceInput(uint32_t token, VkCommandBuffer command, VkImage depth, VkImage motion,
    VkImage hudless, bool hudlessTagged) {
  if (!Enabled()) return;
  Guard guard;
  g_inputs[token % g_inputs.size()] = {token, Bits(command), Bits(depth), Bits(motion), Bits(hudless), hudlessTagged};
}
void ProducerFenceInvalidateSources() {
  if (!Enabled()) return;
  Guard guard;
  Advance(g_generation);
  for (auto& queue : g_queues) queue.endedValid = false;
  g_inputs = {};
}
ProducerFenceBoundary ProducerFenceBegin(VkQueue handle, uint32_t token) {
  Guard guard;
  auto* queue = FindQueue(Bits(handle), true);
  return {Bits(handle), queue ? Advance(queue->source) : 0, queue ? queue->sequence : 0,
    g_order, g_generation, token, GetCurrentThreadId()};
}
VkResult ProducerFencePace(const ProducerFenceBoundary& boundary, bool requireHudless,
    uint32_t (*currentSubmittedToken)()) {
  if (WaitTerminal(boundary, requireHudless) && currentSubmittedToken &&
      currentSubmittedToken() == boundary.token) {
    if (g_waits.fetch_add(1) == 0)
      Log("X4 producer fence pacing: first terminal fence selected source=%llu token=%u submit=%llu", boundary.source, boundary.token, boundary.sequence);
    return VK_SUCCESS;
  }
  ++g_fallbacks;
  PFN_vkQueueWaitIdle idle{};
  { Guard guard; idle = g_idle; }
  return idle ? idle((VkQueue)(uintptr_t)boundary.queue) : VK_ERROR_INITIALIZATION_FAILED;
}
void ProducerFenceEnd(const ProducerFenceBoundary& boundary, uint32_t markerToken, VkResult result) {
  Guard guard;
  if (auto* queue = FindQueue(boundary.queue)) {
    queue->endedSource = boundary.source; queue->endedSequence = boundary.sequence;
    queue->endedToken = boundary.token; queue->endedThread = boundary.thread;
    queue->endedOrder = Advance(g_order);
    queue->endedValid = !g_ambiguous && boundary.generation == g_generation && markerToken == boundary.token &&
      queue->sequence == boundary.sequence && (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
  }
}
void ProducerFenceLogTotals() {
  if (!Enabled()) return;
  const auto waits = g_waits.load(), fallbacks = g_fallbacks.load();
  if (g_loggedWaits.exchange(waits) == waits && g_loggedFallbacks.load() == fallbacks) return;
  g_loggedFallbacks.store(fallbacks);
  Log("X4 producer fence pacing: terminal fence waits=%llu queue-idle fallbacks=%llu", waits, fallbacks);
}
#ifdef FGVK_PRODUCER_FENCE_HOST_TEST
void ProducerFenceResetForTest() {
  Guard guard;
  g_fences = {}; g_semaphores = {}; g_queues = {}; g_inputs = {};
  g_epoch = g_order = g_generation = 0; g_ambiguous = false; g_device = {};
  g_status = nullptr; g_wait = nullptr; g_idle = nullptr;
  g_instanceDispatch = {}; g_dispatches = {};
  g_waits = 0; g_fallbacks = 0; g_loggedWaits = 0; g_loggedFallbacks = 0;
}
ProducerFenceTotals ProducerFenceTotalsForTest() { return {g_waits.load(), g_fallbacks.load()}; }
#endif
}
