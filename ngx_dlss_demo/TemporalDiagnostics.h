#pragma once

#if USE_DX11
#include <cstdint>
#include <nvrhi/nvrhi.h>
#include <donut/core/math/math.h>

struct ID3D11Resource;
struct ID3D11DeviceContext;
namespace donut::engine { class IView; }

namespace temporal_diagnostics
{
    bool enabled(nvrhi::IDevice* device);
    void log_frame_pacing(unsigned limit, int64_t intervalNs);
    void log_resize_request(unsigned step, int width, int height);
    void log_scene_instance(const char* name, unsigned index, float x, float y, float z,
                            float sizeX, float sizeY, float sizeZ);
    void begin_frame(nvrhi::IDevice* device, uint64_t frame, const char* mode, int width, int height,
                     int displayWidth, int displayHeight, int offsetX, int offsetY,
                     float jitterX, float jitterY, bool previousValid, bool diagnosticCameraMotion,
                     const char* quality, int timedCycleStep, int64_t timedCycleElapsedMs,
                     bool diagnosticReset, bool diagnosticCut, bool diagnosticRecovery,
                     int64_t frameStartIntervalNs, float animationDeltaSeconds);
    void clear_stencil(nvrhi::IDevice* device, nvrhi::ITexture* depth);
    void log_taa(nvrhi::IDevice* device, nvrhi::ITexture* color, nvrhi::ITexture* output,
                 nvrhi::ITexture* depth, nvrhi::ITexture* motion, nvrhi::ITexture* feedback1,
                 nvrhi::ITexture* feedback2, unsigned width, unsigned height, unsigned offsetX, unsigned offsetY);
    void capture_temporal(nvrhi::IDevice* device, nvrhi::ITexture* depth, nvrhi::ITexture* motion,
                          unsigned width, unsigned height, unsigned offsetX, unsigned offsetY,
                          const donut::engine::IView* view, const donut::engine::IView* previousView,
                          bool previousValid, const donut::math::float3* objectDelta0,
                          const donut::math::float3* objectDelta1, bool objectIds);
    void log_taa_bindings(nvrhi::IDevice* device, nvrhi::ITexture* motion,
                          nvrhi::ITexture* feedback1, nvrhi::ITexture* feedback2);
    void verify_taa_gpu_completion(nvrhi::IDevice* device);
    void verify_taa_gpu_pipeline(nvrhi::IDevice* device);
    void verify_multiple_swapchains(nvrhi::IDevice* device);
    void log_dlss_inputs(ID3D11DeviceContext* context, ID3D11Resource* color, ID3D11Resource* output, ID3D11Resource* depth,
                         ID3D11Resource* motion, ID3D11Resource* exposure, unsigned width, unsigned height,
                         unsigned offsetX, unsigned offsetY, float jitterX, float jitterY,
                         float motionScaleX, float motionScaleY, bool reset);
    void log_dlss_result(unsigned result);
}
#endif
