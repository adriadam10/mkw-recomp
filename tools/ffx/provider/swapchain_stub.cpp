// The runtime uses the public no-swapchain frame-generation path. Keep the
// optional backend entry point explicit and fail cleanly if it is ever called.
#include "Kits/FidelityFX/backend/vk/ffx_vk.h"

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(
    FfxFrameGenerationConfig const*)
{
    return FFX_ERROR_BACKEND_API_ERROR;
}
