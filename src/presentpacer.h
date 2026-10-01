#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
namespace fgvk {
// Present pacing: before RenderSubmitEnd/PresentStart, wait until the GPU has finished all work
// already submitted to the present queue (the game's frame). With x4 MFG, PR #6's A/B showed
// that marking PresentStart only once the frame's GPU work is complete turns burst-shaped
// display intervals smooth; on our machine every paced frame took its queue-idle fallback
// (waits=0 fallbacks=10278), so the full-queue wait is all that matters.
//
// Mechanism: one empty submit carrying our own fence. A vkQueueSubmit fence signal includes
// every command earlier in submission order on that queue, so it is a queue-idle wait that
// (a) has a timeout - a slow/hung frame skips pacing instead of freezing the present thread -
// and (b) touches none of the game's sync objects.
struct PacerVk {
  VkDevice device{};
  PFN_vkCreateFence createFence{};
  PFN_vkResetFences resetFences{};
  PFN_vkGetFenceStatus getFenceStatus{};
  PFN_vkWaitForFences waitForFences{};
  PFN_vkQueueSubmit queueSubmit{};
};

enum class PaceResult { Waited, TimedOut, SkippedBusy, Unavailable };

class PresentPacer {
public:
  // Caller holds the queue (we run inside the game's vkQueuePresentKHR, which requires it).
  PaceResult Pace(const PacerVk& vk, VkQueue queue, uint64_t timeoutNs){
    if(!vk.device || !queue || !vk.createFence || !vk.resetFences || !vk.getFenceStatus ||
       !vk.waitForFences || !vk.queueSubmit) return PaceResult::Unavailable;
    if(vk.device != device_){ device_ = vk.device; fence_ = VK_NULL_HANDLE; pending_ = false; }  // new device: old fence died with it
    if(!fence_){
      VkFenceCreateInfo ci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      if(vk.createFence(device_, &ci, nullptr, &fence_) != VK_SUCCESS){ fence_ = VK_NULL_HANDLE; return PaceResult::Unavailable; }
    }
    if(pending_){   // an earlier wait timed out: never reset a fence the queue may still signal
      VkResult s = vk.getFenceStatus(device_, fence_);
      if(s == VK_NOT_READY) return PaceResult::SkippedBusy;
      if(s != VK_SUCCESS) return PaceResult::Unavailable;   // device lost: present reports it
      pending_ = false;
    }
    if(vk.resetFences(device_, 1, &fence_) != VK_SUCCESS) return PaceResult::Unavailable;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};          // no work: only the fence signal
    if(vk.queueSubmit(queue, 1, &si, fence_) != VK_SUCCESS) return PaceResult::Unavailable;
    pending_ = true;
    VkResult w = vk.waitForFences(device_, 1, &fence_, VK_TRUE, timeoutNs);
    if(w == VK_SUCCESS){ pending_ = false; return PaceResult::Waited; }
    return w == VK_TIMEOUT ? PaceResult::TimedOut : PaceResult::Unavailable;
  }
private:
  VkDevice device_{};
  VkFence fence_{};
  bool pending_{};
};
}
