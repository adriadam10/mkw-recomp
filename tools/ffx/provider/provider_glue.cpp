// Standalone provider registry used by the runtime's public no-swapchain path.
// FidelityFX-SDK v2.4.0 is MIT licensed; see out/licenses in build/ffx.
#include "Kits/FidelityFX/api/internal/ffx_provider.h"
#include "Kits/FidelityFX/framegeneration/fsr3/include/ffx_provider_fsr3framegeneration.h"

static ffxProvider* const providers[] = {
    &ffxProvider_Fsr3FrameGeneration::GetInstance(),
};

ffxProvider* GetProvider(
    ffxStructType_t descType,
    uint64_t overrideId,
    void* device,
    std::optional<ffxProviderExternal>& extProviderSlot)
{
    return GetProvider(
        descType,
        overrideId,
        device,
        extProviderSlot,
        std::span<ffxProvider* const>(providers));
}

uint64_t GetProviderVersions(
    ffxStructType_t descType,
    void* device,
    uint64_t capacity,
    uint64_t* versionIds,
    const char** versionNames,
    std::optional<ffxProviderExternal>& extProviderSlot)
{
    return GetProviderVersions(
        descType,
        device,
        capacity,
        versionIds,
        versionNames,
        extProviderSlot,
        std::span<ffxProvider* const>(providers));
}
