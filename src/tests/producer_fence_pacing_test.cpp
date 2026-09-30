#include "producer_fence_pacing.h"
#include "config.h"
#include <windows.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <thread>

namespace fgvk {
static Config configuration;
const Config& Cfg() { return configuration; }
void Log(const char*, ...) {}
}
using namespace fgvk;
namespace {
template<class H> H Handle(uintptr_t n) { return (H)n; }
const VkDevice device = Handle<VkDevice>(1);
const VkQueue queue = Handle<VkQueue>(2);
const VkFence terminal = Handle<VkFence>(103);
std::atomic<int> waitCalls{}, idleCalls{}, resetCalls{}, destroyCalls{};
std::atomic<bool> complete{};
VkResult waitResult = VK_SUCCESS, statusFailure = VK_SUCCESS, idleResult = VK_SUCCESS;
uintptr_t nextFence = 101, nextSemaphore = 201;
bool resolveWait = true, changePrefixInWait = false, blockWait = false;
bool resolveCreateFence = true;
std::atomic<uint32_t> alternateCalls{}, otherDeviceCalls{};
bool changeTokenInWait = false;
uint32_t currentToken = 101;
uint32_t ReadCurrentToken() { return currentToken; }
HANDLE entered{}, release{}, mutationEntered{}, mutationCalled{};
int failures{};
void Check(bool value, const char* name) { if (!value) { ++failures; printf("FAIL: %s\n", name); } }
VKAPI_ATTR VkResult VKAPI_CALL Status(VkDevice d, VkFence f) {
  Check(d == device, "status device");
  if (f == terminal && statusFailure != VK_SUCCESS) return statusFailure;
  return complete ? VK_SUCCESS : VK_NOT_READY;
}
VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice d, uint32_t count, const VkFence* fences, VkBool32 all, uint64_t timeout) {
  Check(d == device && count == 1 && fences[0] == terminal && all == VK_TRUE && timeout == UINT64_MAX,
      "exact live-terminal wait arguments");
  ++waitCalls;
  if (changeTokenInWait) ++currentToken;
  if (changePrefixInWait) {
    VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    ProducerFenceSubmit2(queue, 1, &info, VK_NULL_HANDLE, 0, VK_SUCCESS);
  }
  if (blockWait) { SetEvent(entered); Check(WaitForSingleObject(release, 5000) == WAIT_OBJECT_0, "bounded mocked GPU stall"); }
  if (waitResult == VK_SUCCESS) complete = true;
  return waitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL Idle(VkQueue q) {
  Check(q != VK_NULL_HANDLE, "fallback uses the caller's queue");
  ++idleCalls; if (idleResult == VK_SUCCESS) complete = true; return idleResult;
}
VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* out) {
  *out = Handle<VkFence>(nextFence++); return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice, uint32_t, const VkFence*) {
  ++resetCalls; if (mutationCalled) SetEvent(mutationCalled); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice, VkFence, const VkAllocationCallbacks*) {
  ++destroyCalls; if (mutationCalled) SetEvent(mutationCalled);
}
VKAPI_ATTR VkResult VKAPI_CALL CreateSemaphore(VkDevice, const VkSemaphoreCreateInfo*, const VkAllocationCallbacks*, VkSemaphore* out) {
  *out = Handle<VkSemaphore>(nextSemaphore++); return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL DestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks*) {}
// Distinct dispatch entry points with identical Vulkan semantics. The counter
// prevents linker folding, so mixed GIPA/GDPA tests really use different bytes.
__declspec(noinline) VKAPI_ATTR VkResult VKAPI_CALL AlternateCreateFence(VkDevice d,
    const VkFenceCreateInfo* i, const VkAllocationCallbacks* a, VkFence* f) {
  ++alternateCalls; return CreateFence(d, i, a, f);
}
__declspec(noinline) VKAPI_ATTR VkResult VKAPI_CALL AlternateResetFences(VkDevice d,
    uint32_t n, const VkFence* f) { ++alternateCalls; return Reset(d, n, f); }
__declspec(noinline) VKAPI_ATTR void VKAPI_CALL AlternateDestroyFence(VkDevice d,
    VkFence f, const VkAllocationCallbacks* a) { ++alternateCalls; Destroy(d, f, a); }
__declspec(noinline) VKAPI_ATTR VkResult VKAPI_CALL AlternateCreateSemaphore(VkDevice d,
    const VkSemaphoreCreateInfo* i, const VkAllocationCallbacks* a, VkSemaphore* s) {
  ++alternateCalls; return CreateSemaphore(d, i, a, s);
}
__declspec(noinline) VKAPI_ATTR void VKAPI_CALL AlternateDestroySemaphore(VkDevice d,
    VkSemaphore s, const VkAllocationCallbacks* a) { ++alternateCalls; DestroySemaphore(d, s, a); }
VKAPI_ATTR VkResult VKAPI_CALL OtherDeviceCreateFence(VkDevice d,
    const VkFenceCreateInfo*, const VkAllocationCallbacks*, VkFence* f) {
  Check(d == Handle<VkDevice>(42), "device-specific target never forwards another device");
  ++otherDeviceCalls; *f = Handle<VkFence>(10001); return VK_SUCCESS;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Resolve(VkDevice, const char* name) {
  if (!strcmp(name, "vkGetFenceStatus")) return (PFN_vkVoidFunction)Status;
  if (!strcmp(name, "vkWaitForFences") && resolveWait) return (PFN_vkVoidFunction)Wait;
  if (!strcmp(name, "vkQueueWaitIdle")) return (PFN_vkVoidFunction)Idle;
  if (!strcmp(name, "vkCreateFence") && resolveCreateFence) return (PFN_vkVoidFunction)CreateFence;
  if (!strcmp(name, "vkResetFences")) return (PFN_vkVoidFunction)Reset;
  if (!strcmp(name, "vkDestroyFence")) return (PFN_vkVoidFunction)Destroy;
  if (!strcmp(name, "vkCreateSemaphore")) return (PFN_vkVoidFunction)CreateSemaphore;
  if (!strcmp(name, "vkDestroySemaphore")) return (PFN_vkVoidFunction)DestroySemaphore;
  return nullptr;
}
auto ResetFunction() { return (PFN_vkResetFences)ProducerFenceWrap("vkResetFences", (PFN_vkVoidFunction)Reset); }
auto DestroyFunction() { return (PFN_vkDestroyFence)ProducerFenceWrap("vkDestroyFence", (PFN_vkVoidFunction)Destroy); }
void RegisterAlternateLookups(VkDevice resolvedDevice = VK_NULL_HANDLE) {
  ProducerFenceWrap("vkCreateFence", (PFN_vkVoidFunction)AlternateCreateFence, resolvedDevice);
  ProducerFenceWrap("vkResetFences", (PFN_vkVoidFunction)AlternateResetFences, resolvedDevice);
  ProducerFenceWrap("vkDestroyFence", (PFN_vkVoidFunction)AlternateDestroyFence, resolvedDevice);
  ProducerFenceWrap("vkCreateSemaphore", (PFN_vkVoidFunction)AlternateCreateSemaphore, resolvedDevice);
  ProducerFenceWrap("vkDestroySemaphore", (PFN_vkVoidFunction)AlternateDestroySemaphore, resolvedDevice);
}
void Setup(bool anchor = true, bool mixedStartup = false) {
  ProducerFenceResetForTest(); configuration = Config{}; configuration.x4ProducerFencePacing = true;
  alternateCalls = otherDeviceCalls = 0;
  PFN_vkCreateFence cachedCreate{};
  PFN_vkResetFences cachedReset{};
  PFN_vkCreateSemaphore cachedCreateSemaphore{};
  if (mixedStartup) {
    RegisterAlternateLookups();       // instance dispatch before device exists
    RegisterAlternateLookups(device); // third-party device dispatch in CreateDevice
    cachedCreate = (PFN_vkCreateFence)ProducerFenceWrap("vkCreateFence",
        (PFN_vkVoidFunction)AlternateCreateFence, device);
    cachedReset = (PFN_vkResetFences)ProducerFenceWrap("vkResetFences",
        (PFN_vkVoidFunction)AlternateResetFences, device);
    cachedCreateSemaphore = (PFN_vkCreateSemaphore)ProducerFenceWrap("vkCreateSemaphore",
        (PFN_vkVoidFunction)AlternateCreateSemaphore, device);
  }
  ProducerFenceInitialize(device, Resolve);
  complete = false; waitCalls = idleCalls = resetCalls = destroyCalls = 0;
  waitResult = statusFailure = idleResult = VK_SUCCESS; changePrefixInWait = blockWait = changeTokenInWait = false;
  nextFence = 101; nextSemaphore = 201;
  auto createF = cachedCreate ? cachedCreate :
      (PFN_vkCreateFence)ProducerFenceWrap("vkCreateFence", (PFN_vkVoidFunction)CreateFence);
  auto reset = cachedReset ? cachedReset : ResetFunction();
  for (int i = 0; i < 3; ++i) {
    VkFenceCreateInfo info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
    createF(device, &info, nullptr, &fence); reset(device, 1, &fence);
  }
  resetCalls = 0;
  auto createS = cachedCreateSemaphore ? cachedCreateSemaphore :
      (PFN_vkCreateSemaphore)ProducerFenceWrap("vkCreateSemaphore", (PFN_vkVoidFunction)CreateSemaphore);
  for (int i = 0; i < 3; ++i) {
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; info.pNext = i == 0 ? &type : nullptr;
    VkSemaphore semaphore{}; createS(device, &info, nullptr, &semaphore);
  }
  if (anchor) { auto b = ProducerFenceBegin(queue, 100); ProducerFenceEnd(b, 100, VK_SUCCESS); }
}
void Prefix(uint32_t token = 101, uint32_t nrCount = 34, bool wrongSr = false,
    bool resetDuringSubmit = false, VkFence lastFence = terminal, VkResult terminalResult = VK_SUCCESS) {
  ProducerFenceInput(token, Handle<VkCommandBuffer>(wrongSr ? 999 : 529),
      Handle<VkImage>(301), Handle<VkImage>(302), Handle<VkImage>(303), true);
  std::array<VkCommandBufferSubmitInfo, 65> commands{};
  for (size_t i = 0; i < commands.size(); ++i) {
    commands[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commands[i].commandBuffer = Handle<VkCommandBuffer>(500 + i);
  }
  auto record = [&](const VkSubmitInfo2& info, VkFence fence, VkResult result = VK_SUCCESS) {
    ProducerFenceSubmit2(queue, 1, &info, fence, ProducerFenceSubmitEpoch(fence), result);
  };
  VkSubmitInfo2 info{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  info.commandBufferInfoCount = 1; info.pCommandBufferInfos = commands.data();
  record(info, Handle<VkFence>(101));
  VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
  signal.semaphore = Handle<VkSemaphore>(201); signal.value = 11;
  signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  info.commandBufferInfoCount = nrCount; info.signalSemaphoreInfoCount = 1; info.pSignalSemaphoreInfos = &signal;
  record(info, Handle<VkFence>(102));
  VkSubmitInfo2 empty{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; record(empty, VK_NULL_HANDLE);
  VkSemaphoreSubmitInfo acquire{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; acquire.semaphore = Handle<VkSemaphore>(202);
  acquire.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT;
  std::array<VkSemaphoreSubmitInfo, 2> signals{signal, {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}};
  signals[0].value = 12; signals[1].semaphore = Handle<VkSemaphore>(203);
  signals[1].stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  info.commandBufferInfoCount = 2; info.waitSemaphoreInfoCount = 1; info.pWaitSemaphoreInfos = &acquire;
  info.signalSemaphoreInfoCount = 2; info.pSignalSemaphoreInfos = signals.data();
  const auto epoch = ProducerFenceSubmitEpoch(lastFence);
  if (resetDuringSubmit) ResetFunction()(device, 1, &lastFence);
  ProducerFenceSubmit2(queue, 1, &info, lastFence, epoch, terminalResult);
}
VkResult Run(uint32_t token = 101, VkQueue q = queue, bool validPresent = true, uint32_t markerToken = 0) {
  auto boundary = ProducerFenceBegin(q, token);
  currentToken = token;
  const auto result = ProducerFencePace(boundary, true, ReadCurrentToken);
  ProducerFenceEnd(boundary, markerToken ? markerToken : token,
      validPresent ? VK_SUCCESS : VK_ERROR_OUT_OF_DATE_KHR);
  return result;
}
void Fallback(const char* name) { Check(waitCalls == 0 && idleCalls == 1, name); }
void ConcurrentMutation(bool destroy) {
  entered = CreateEventA(nullptr, TRUE, FALSE, nullptr); release = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  mutationEntered = CreateEventA(nullptr, TRUE, FALSE, nullptr); mutationCalled = CreateEventA(nullptr, TRUE, FALSE, nullptr);
  std::thread owner([&] { Setup(); Prefix(); blockWait = true; Run(); });
  const bool started = WaitForSingleObject(entered, 3000) == WAIT_OBJECT_0;
  Check(started, "selected wait started");
  if (started) {
    ResetEvent(mutationCalled);
    const auto reset = ResetFunction(); const auto destroyF = DestroyFunction();
    std::thread mutation([&] {
      SetEvent(mutationEntered);
      if (destroy) destroyF(device, terminal, nullptr); else reset(device, 1, &terminal);
    });
    Check(WaitForSingleObject(mutationEntered, 1000) == WAIT_OBJECT_0, "mutation thread started");
    Check(WaitForSingleObject(mutationCalled, 30) == WAIT_TIMEOUT, "reset/destroy cannot touch borrowed fence");
    // A different registry operation completes while the GPU wait is blocked.
    ProducerFenceInput(999, Handle<VkCommandBuffer>(999), Handle<VkImage>(1), Handle<VkImage>(2), Handle<VkImage>(3), true);
    SetEvent(release); mutation.join();
    Check(WaitForSingleObject(mutationCalled, 1000) == WAIT_OBJECT_0, "mutation forwards after lease release");
  } else SetEvent(release);
  owner.join(); Check(waitCalls == 1 && idleCalls == 0, "borrowed wait remains valid during mutation request");
  for (HANDLE h : {entered, release, mutationEntered, mutationCalled}) CloseHandle(h);
  entered = release = mutationEntered = mutationCalled = nullptr;
}
}
int main() {
  for (bool enabled : {false, true}) for (bool fg : {false, true}) for (uint32_t frames = 0; frames <= 5; ++frames)
    Check(ProducerFencePacingApplies(enabled, fg, frames) == (enabled && fg && frames == 3), "only enabled normal x4 reaches gate");
  Setup(); Prefix(); Check(Run() == VK_SUCCESS && waitCalls == 1 && idleCalls == 0, "valid steady prefix selects terminal fence");
  Prefix(102); Run(102); Check(waitCalls == 2 && idleCalls == 0, "consecutive source uses fence");
  Check((PFN_vkVoidFunction)CreateFence != (PFN_vkVoidFunction)AlternateCreateFence,
      "resolver regression uses distinct forwarding addresses");
  Setup(true, true); Prefix(); Run();
  Check(waitCalls == 1 && idleCalls == 0 && alternateCalls == 0,
      "mixed pre-initialize GIPA/GDPA lookups use canonical device dispatch without poisoning");
  RegisterAlternateLookups(); RegisterAlternateLookups(device);
  for (uint32_t token = 102; token < 202; ++token) { Prefix(token); Run(token); }
  Check(waitCalls == 101 && idleCalls == 0,
      "all five alternate lookup addresses leave 100 steady prefixes eligible");
  ProducerFenceInvalidateSources(); ProducerFenceInitialize(device, Resolve);
  Prefix(202); Run(202); Prefix(203); Run(203);
  Check(waitCalls == 102 && idleCalls == 1,
      "mixed lookups preserve normal source-invalidation fallback and recovery");
  Setup(); Prefix(); Run();
  std::thread lookups([&] {
    for (uint32_t n = 0; n < 1000; ++n) { RegisterAlternateLookups(); RegisterAlternateLookups(device); }
  });
  for (uint32_t token = 102; token < 302; ++token) { Prefix(token); Run(token); }
  lookups.join();
  Check(waitCalls == 201 && idleCalls == 0 && alternateCalls == 0,
      "concurrent resolver lookups do not change canonical forwarding or poison selection");
  Setup();
  const auto otherCreate = (PFN_vkCreateFence)ProducerFenceWrap("vkCreateFence",
      (PFN_vkVoidFunction)OtherDeviceCreateFence, Handle<VkDevice>(42));
  VkFenceCreateInfo otherInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence otherFence{};
  otherCreate(Handle<VkDevice>(42), &otherInfo, nullptr, &otherFence);
  Check(otherDeviceCalls == 1 && ProducerFenceSubmitEpoch(otherFence) == 0,
      "untracked device forwards through its own table and does not create tracked epochs");
  Prefix(); Run(); Check(waitCalls == 1 && idleCalls == 0,
      "another device lookup does not poison the active device's valid fence");
  Setup(); ProducerFenceInitialize(Handle<VkDevice>(42), Resolve); Prefix(); Run();
  Fallback("second tracked device still fails closed rather than mixing lifetime evidence");
  resolveCreateFence = false; Setup(); Prefix(); Run();
  Fallback("incomplete canonical lifecycle table still fails closed"); resolveCreateFence = true;
  Setup();
  for (uintptr_t d = 10; d < 13; ++d)
    ProducerFenceWrap("vkCreateFence", (PFN_vkVoidFunction)CreateFence, Handle<VkDevice>(d));
  Check(ProducerFenceWrap("vkCreateFence", (PFN_vkVoidFunction)CreateFence, Handle<VkDevice>(13)) ==
      (PFN_vkVoidFunction)CreateFence, "dispatch-capacity overflow forwards the exact original rather than another device");
  Prefix(); Run(); Fallback("dispatch-capacity overflow remains fail closed");
  Setup(); nextFence = 101;
  auto duplicateCreate = (PFN_vkCreateFence)ProducerFenceWrap("vkCreateFence", (PFN_vkVoidFunction)CreateFence);
  VkFence duplicateFence{}; duplicateCreate(device, &otherInfo, nullptr, &duplicateFence);
  RegisterAlternateLookups(); ProducerFenceInitialize(device, Resolve);
  Prefix(); Run(); Fallback("canonical normalization never clears genuine duplicate-live-handle ambiguity");
  Setup(); Prefix(); complete = true; Run(); Check(waitCalls == 1 && idleCalls == 0, "already complete fence uses real wait");
  Setup(false); Prefix(); Run(); Fallback("first x4 boundary has no prior anchor");
  Prefix(102); Run(102); Check(waitCalls == 1 && idleCalls == 1, "initial fallback anchors next valid source");
  Setup(); Prefix(101, 34, false, false, VK_NULL_HANDLE); Run(); Fallback("missing terminal fence");
  Setup(); Prefix(); ResetFunction()(device, 1, &terminal); Run(); Fallback("fence reset epoch mismatch");
  Setup(); Prefix(101, 34, false, true); Run(); Fallback("epoch changed before successful metadata capture");
  Setup(); Prefix(); DestroyFunction()(device, terminal, nullptr); Run(); Fallback("destroyed fence");
  Setup(); Prefix(); DestroyFunction()(device, terminal, nullptr); nextFence = 103;
  auto createF = (PFN_vkCreateFence)ProducerFenceWrap("vkCreateFence", (PFN_vkVoidFunction)CreateFence);
  VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence reused{};
  createF(device, &ci, nullptr, &reused); Run(); Fallback("same handle recreated is not old producer");
  Setup(); Prefix(); Run(101, Handle<VkQueue>(99)); Fallback("wrong queue");
  Setup(); Prefix(101, 31); Run(); Fallback("31-CB prefix remains conservative");
  Setup(); Prefix(101, 32); Run(); Fallback("32-CB prefix remains conservative");
  Setup(); Prefix(101, 33); Run(); Fallback("33-CB prefix remains conservative");
  Setup(); Prefix(101, 65); Run(); Fallback("truncated prefix");
  Setup(); Prefix(101, 34, true); Run(); Fallback("SR command not covered");
  Setup(); Prefix(101, 34, false, false, terminal, VK_ERROR_DEVICE_LOST); Run(); Fallback("failed submit is not successful producer");
  Setup(); Prefix(); VkSubmitInfo2 extra{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  ProducerFenceSubmit2(queue, 1, &extra, VK_NULL_HANDLE, 0, VK_SUCCESS); Run(); Fallback("five-submit prefix");
  Setup(); Prefix(); ProducerFenceSubmit2(queue, 1, &extra, VK_NULL_HANDLE, 0, VK_ERROR_DEVICE_LOST);
  Run(); Fallback("failed extra submit cannot be hidden behind an earlier terminal fence");
  Setup(); Prefix(); Run(102); Fallback("token discontinuity");
  Setup(false); { auto b=ProducerFenceBegin(queue,UINT32_MAX); ProducerFenceEnd(b,UINT32_MAX,VK_SUCCESS); }
  Prefix(0); Run(0); Fallback("token wrap must not alias a valid continuity proof");
  Setup(); Prefix(); ProducerFenceInvalidateSources(); Run(); Fallback("mode or swapchain recreation invalidates source anchor");
  Prefix(102); Run(102); Check(waitCalls == 1, "subsequent valid source after recreate resumes fence path");
  Setup(); Prefix(); Run(); ProducerFenceInvalidateSources(); // x4 -> x3 -> x4
  Check(!ProducerFencePacingApplies(true,true,2),"x3 transition bypasses the production wait");
  Prefix(102); Run(102); Check(idleCalls==1,"return to x4 does not reuse the old x4 source anchor");
  Prefix(103); Run(103); Check(waitCalls==2,"steady x4 resumes after transition fallback");
  Setup(); Prefix(); Run(101, queue, false); Prefix(102); Run(102); Check(idleCalls == 1, "failed prior Present invalidates source anchor");
  Setup(); Prefix(); Run(101, queue, true, 102); Prefix(102); Run(102); Check(idleCalls == 1, "marker token moved during wait invalidates anchor");
  Setup(); Prefix(); std::thread wrongThread([&] { Run(); }); wrongThread.join(); Fallback("source thread mismatch");
  Setup(); Prefix(); statusFailure = VK_ERROR_DEVICE_LOST; Run(); Fallback("unusable pre-wait status");
  resolveWait = false; Setup(); Prefix(); Run(); Fallback("unresolved wait API"); resolveWait = true;
  Setup(); Prefix(); waitResult = VK_ERROR_DEVICE_LOST; Run(); Check(waitCalls == 1 && idleCalls == 1, "failed wait invokes queue-idle fallback");
  ResetFunction()(device, 1, &terminal); Check(resetCalls == 1, "failed wait releases lease");
  Setup(); Prefix(); changePrefixInWait = true; Run(); Check(waitCalls == 1 && idleCalls == 1, "prefix change during wait fails closed");
  Setup(); Prefix(); changeTokenInWait = true; Run(); Check(waitCalls == 1 && idleCalls == 1, "token front changed during wait uses queue idle before markers");
  Setup(false); idleResult = VK_ERROR_DEVICE_LOST; Check(Run() == VK_ERROR_DEVICE_LOST, "real fallback failure propagated");
  Setup(); Prefix(); Run();
  for (uint32_t token = 102; token < 6000; ++token) { Prefix(token); Run(token); }
  Check(waitCalls == 5899 && idleCalls == 0, "no diagnostic record cap limits production tracking");
  Check(ProducerFenceTotalsForTest().waits == 5899, "lightweight full-run aggregate");
  Setup(); configuration.x4ProducerFencePacing = false;
  Check(ProducerFenceWrap("vkResetFences", (PFN_vkVoidFunction)Reset) == (PFN_vkVoidFunction)Reset, "opt-out wrapper identity");
  Check(ProducerFenceSubmitEpoch(terminal) == 0, "opt-out does not inspect tracker");
  ConcurrentMutation(false); ConcurrentMutation(true);
  printf("producer fence production tests: %d failure(s)\n", failures);
  return failures ? 1 : 0;
}
