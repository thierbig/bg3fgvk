#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace fgvk {
// The upstream branch has no transition-pin/private Streamline hooks. This
// opt-in applies only after an eOn/three-generated-frames options call succeeds.
constexpr bool ProducerFencePacingApplies(bool enabled, bool fgOn, uint32_t generated) {
  return enabled && fgOn && generated == 3;
}
void ProducerFenceInitialize(VkDevice, PFN_vkGetDeviceProcAddr);
// GDPA callers identify the device; GIPA callers leave resolvedDevice null.
PFN_vkVoidFunction ProducerFenceWrap(const char* name, PFN_vkVoidFunction original,
                                   VkDevice resolvedDevice = VK_NULL_HANDLE);
uint64_t ProducerFenceSubmitEpoch(VkFence);
void ProducerFenceSubmit(VkQueue, uint32_t count, const VkSubmitInfo*, VkFence,
                         uint64_t epoch, VkResult result);
void ProducerFenceSubmit2(VkQueue, uint32_t count, const VkSubmitInfo2*, VkFence,
                          uint64_t epoch, VkResult result);
void ProducerFenceInput(uint32_t token, VkCommandBuffer, VkImage depth, VkImage motion,
                        VkImage hudless, bool hudlessTagged);
// Invalidates source associations, not Vulkan object lifetimes. No GPU wait.
void ProducerFenceInvalidateSources();
struct ProducerFenceBoundary {
  uint64_t queue{}, source{}, sequence{}, order{}, generation{};
  uint32_t token{}, thread{};
};
ProducerFenceBoundary ProducerFenceBegin(VkQueue, uint32_t submittedToken);
// A validated terminal fence wait, or the conservative queue-idle fallback.
// Returns the actual Vulkan result; neither path changes GPU submissions.
VkResult ProducerFencePace(const ProducerFenceBoundary&, bool requireHudless,
                          uint32_t (*currentSubmittedToken)());
void ProducerFenceEnd(const ProducerFenceBoundary&, uint32_t markerToken, VkResult presentResult);
void ProducerFenceLogTotals(); // existing device-idle / teardown paths only
#ifdef FGVK_PRODUCER_FENCE_HOST_TEST
void ProducerFenceResetForTest();
struct ProducerFenceTotals { uint64_t waits{}, fallbacks{}; };
ProducerFenceTotals ProducerFenceTotalsForTest();
#endif
}
