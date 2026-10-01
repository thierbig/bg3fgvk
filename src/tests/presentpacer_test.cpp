// Unit test for the present pacer's fence state machine, against mocked Vulkan entry points.
#include "presentpacer.h"
#include <cstdio>
#include <cstdint>
static int fails = 0;
static void expect(const char* name, bool ok){
  if(!ok){ printf("FAIL %s\n", name); fails++; } else printf("ok   %s\n", name);
}

namespace {
struct Mock {
  int creates=0, resets=0, submits=0, waits=0, statusCalls=0;
  VkResult createResult=VK_SUCCESS, waitResult=VK_SUCCESS, statusResult=VK_SUCCESS, submitResult=VK_SUCCESS;
  bool fenceSignaledAtReset=false;   // a reset while the queue may still signal is the bug to catch
  bool fenceInFlight=false;
  uint32_t lastSubmitCount=0, lastCmdCount=99;
  VkFence lastFence{};
} m;
const VkFence kFence = (VkFence)(uintptr_t)0xF00D;
const VkDevice kDev = (VkDevice)(uintptr_t)0xD1;
const VkQueue kQueue = (VkQueue)(uintptr_t)0x51;
VKAPI_ATTR VkResult VKAPI_CALL CreateFence(VkDevice, const VkFenceCreateInfo* ci, const VkAllocationCallbacks*, VkFence* f){
  m.creates++; if(m.createResult!=VK_SUCCESS) return m.createResult;
  if(ci->flags & VK_FENCE_CREATE_SIGNALED_BIT) m.fenceSignaledAtReset=true;
  *f=kFence; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL ResetFences(VkDevice, uint32_t, const VkFence*){
  m.resets++; if(m.fenceInFlight) m.fenceSignaledAtReset=true; return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL GetFenceStatus(VkDevice, VkFence){
  m.statusCalls++; if(m.statusResult==VK_SUCCESS) m.fenceInFlight=false; return m.statusResult;
}
VKAPI_ATTR VkResult VKAPI_CALL WaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t){
  m.waits++; if(m.waitResult==VK_SUCCESS) m.fenceInFlight=false; return m.waitResult;
}
VKAPI_ATTR VkResult VKAPI_CALL QueueSubmit(VkQueue, uint32_t n, const VkSubmitInfo* s, VkFence f){
  m.submits++; m.lastSubmitCount=n; m.lastCmdCount=n?s[0].commandBufferCount:99; m.lastFence=f;
  if(m.submitResult==VK_SUCCESS) m.fenceInFlight=true; return m.submitResult;
}
fgvk::PacerVk Vk(VkDevice d=kDev){ return {d, CreateFence, ResetFences, GetFenceStatus, WaitForFences, QueueSubmit}; }
}

int test_presentpacer(){
  using fgvk::PaceResult;
  { m=Mock{}; fgvk::PresentPacer p;
    expect("waits on an empty fenced submit", p.Pace(Vk(),kQueue,1)==PaceResult::Waited);
    expect("submit carries no work", m.lastSubmitCount==1 && m.lastCmdCount==0 && m.lastFence==kFence);
    p.Pace(Vk(),kQueue,1);
    expect("fence created once and reused", m.creates==1 && m.submits==2 && m.waits==2); }

  { m=Mock{}; fgvk::PresentPacer p; m.waitResult=VK_TIMEOUT;
    expect("timeout reported", p.Pace(Vk(),kQueue,1)==PaceResult::TimedOut);
    m.statusResult=VK_NOT_READY;
    expect("still in flight -> frame skipped", p.Pace(Vk(),kQueue,1)==PaceResult::SkippedBusy);
    expect("no reset/submit while in flight", m.submits==1 && !m.fenceSignaledAtReset);
    m.statusResult=VK_SUCCESS; m.waitResult=VK_SUCCESS;
    expect("signalled later -> pacing resumes", p.Pace(Vk(),kQueue,1)==PaceResult::Waited);
    expect("never reset a fence the queue may still signal", !m.fenceSignaledAtReset); }

  { m=Mock{}; fgvk::PresentPacer p; m.waitResult=VK_ERROR_DEVICE_LOST;
    expect("device lost -> unavailable", p.Pace(Vk(),kQueue,1)==PaceResult::Unavailable);
    m.statusResult=VK_ERROR_DEVICE_LOST;
    expect("stays unavailable, no new submit", p.Pace(Vk(),kQueue,1)==PaceResult::Unavailable && m.submits==1); }

  { m=Mock{}; fgvk::PresentPacer p; m.submitResult=VK_ERROR_OUT_OF_HOST_MEMORY;
    expect("submit failure -> unavailable, no wait", p.Pace(Vk(),kQueue,1)==PaceResult::Unavailable && m.waits==0);
    m.submitResult=VK_SUCCESS;
    expect("recovers on the next frame", p.Pace(Vk(),kQueue,1)==PaceResult::Waited); }

  { m=Mock{}; fgvk::PresentPacer p; m.createResult=VK_ERROR_OUT_OF_DEVICE_MEMORY;
    expect("fence create failure -> unavailable", p.Pace(Vk(),kQueue,1)==PaceResult::Unavailable && m.submits==0); }

  { m=Mock{}; fgvk::PresentPacer p; m.waitResult=VK_TIMEOUT; p.Pace(Vk(),kQueue,1);
    m.waitResult=VK_SUCCESS; m.fenceInFlight=false;
    expect("new device -> fresh fence, old pending state dropped",
           p.Pace(Vk((VkDevice)(uintptr_t)0xD2),kQueue,1)==PaceResult::Waited && m.creates==2 && m.statusCalls==0); }

  { m=Mock{}; fgvk::PresentPacer p; fgvk::PacerVk vk=Vk(); vk.queueSubmit=nullptr;
    expect("missing entry point -> unavailable", p.Pace(vk,kQueue,1)==PaceResult::Unavailable && m.creates==0); }
  return fails;
}
