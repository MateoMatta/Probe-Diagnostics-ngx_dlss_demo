#include "TemporalDiagnostics.h"
#include "IdentitySelfTest.h"

#if USE_DX11
#include <Windows.h>
#include <d3d11.h>
#include <donut/core/log.h>
#include <donut/engine/View.h>
#include <nvrhi/d3d11/d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace temporal_diagnostics
{
namespace
{
    struct Logger
    {
        FILE* file = nullptr;
        bool active = false;
        bool sampled = false;
        bool writeFailed = false;
        uint64_t frame = 0;
        char lastMode[16] = {};
        char lastQuality[16] = {};
        int lastTimedCycleStep = -1;
        int lastWidth = 0, lastHeight = 0, lastDisplayWidth = 0, lastDisplayHeight = 0;
        int lastPreviousValid = -1;

        Logger()
        {
            wchar_t setting[8] = {};
            const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_TRACE", setting, 8);
            if (size == 0)
            {
                if (GetLastError() != ERROR_ENVVAR_NOT_FOUND)
                    donut::log::error("Temporal diagnostics: failed to read OPTISCALER_DEMO_TRACE (%lu)", GetLastError());
                return;
            }
            if (size >= 8 || wcscmp(setting, L"1") != 0)
            {
                donut::log::error("Temporal diagnostics: OPTISCALER_DEMO_TRACE must be 1 to enable tracing");
                return;
            }
            wchar_t path[32768] = {};
            const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
            if (length == 0 || length >= std::size(path))
            {
                donut::log::error("Temporal diagnostics: failed to find executable path (%lu)", GetLastError());
                return;
            }
            wchar_t* separator = wcsrchr(path, L'\\');
            if (separator == nullptr)
            {
                donut::log::error("Temporal diagnostics: executable path has no directory");
                return;
            }
            LARGE_INTEGER counter {};
            if (!QueryPerformanceCounter(&counter) ||
                swprintf_s(separator + 1, std::size(path) - (separator + 1 - path),
                           L"TemporalInputs_%lu_%lld.log", GetCurrentProcessId(), counter.QuadPart) < 0)
            {
                donut::log::error("Temporal diagnostics: could not construct unique log path");
                return;
            }
            const errno_t error = _wfopen_s(&file, path, L"wx");
            if (error != 0 || file == nullptr)
            {
                donut::log::error("Temporal diagnostics: could not create log (errno=%d)", static_cast<int>(error));
                return;
            }
            active = true;
            donut::log::info("Temporal diagnostics enabled; writing beside the executable");
        }

        ~Logger()
        {
            if (file != nullptr && fclose(file) != 0)
                OutputDebugStringA("Temporal diagnostics: failed to close log\n");
        }

        template<typename... Args>
        void write(const char* format, Args... args)
        {
            if (!active || writeFailed)
                return;
            SYSTEMTIME now {};
            LARGE_INTEGER counter {};
            GetLocalTime(&now);
            if (!QueryPerformanceCounter(&counter))
            {
                donut::log::error("Temporal diagnostics: performance counter unavailable");
                writeFailed = true;
                return;
            }
            const int prefix = fprintf(file, "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu qpc=%lld frame=%llu ",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                GetCurrentProcessId(), counter.QuadPart, static_cast<unsigned long long>(frame));
            const int message = fprintf(file, format, args...);
            if (prefix < 0 || message < 0 || fputc('\n', file) == EOF || fflush(file) != 0)
            {
                donut::log::error("Temporal diagnostics: log write or flush failed");
                OutputDebugStringA("Temporal diagnostics: log write or flush failed\n");
                writeFailed = true;
            }
        }
    };

    Logger& logger()
    {
        static Logger instance;
        return instance;
    }

    ID3D11Resource* native(nvrhi::ITexture* texture)
    {
        return texture ? texture->getNativeObject(nvrhi::ObjectTypes::D3D11_Resource) : nullptr;
    }

    using Microsoft::WRL::ComPtr;

    bool region_fits(unsigned textureWidth, unsigned textureHeight, unsigned width, unsigned height,
                     unsigned offsetX, unsigned offsetY)
    {
        return width != 0 && height != 0 && offsetX <= textureWidth && offsetY <= textureHeight &&
            width <= textureWidth - offsetX && height <= textureHeight - offsetY;
    }

    bool log_resource_contract(ID3D11DeviceContext* context, const char* path, const char* role,
                               ID3D11Resource* resource, bool required, bool checkRegion,
                               unsigned width, unsigned height, unsigned offsetX, unsigned offsetY)
    {
        auto& log = logger();
        if (!resource)
        {
            log.write("event=resource_contract path=%s role=%s resource=null status=%s",
                      path, role, required ? "error" : "absent_optional");
            return !required;
        }
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT hr = resource->QueryInterface(IID_PPV_ARGS(&texture));
        if (FAILED(hr) || !context)
        {
            log.write("event=resource_contract path=%s role=%s resource=%p status=error hr=0x%08X context=%p",
                      path, role, static_cast<void*>(resource), static_cast<unsigned>(hr), static_cast<void*>(context));
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Device> contextDevice, resourceDevice;
        context->GetDevice(&contextDevice);
        resource->GetDevice(&resourceDevice);
        const bool sameDevice = contextDevice && resourceDevice && contextDevice.Get() == resourceDevice.Get();
        const bool fits = !checkRegion || region_fits(desc.Width, desc.Height, width, height, offsetX, offsetY);
        const bool valid = sameDevice && fits && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE;
        log.write("event=resource_contract path=%s role=%s resource=%p context=%p device=%p resource_device=%p "
                  "size=%ux%u format=%u samples=%u quality=%u mips=%u layers=%u usage=%u bind=0x%X "
                  "cpu_access=0x%X misc=0x%X region_checked=%u region_fits=%u same_device=%u status=%s",
                  path, role, static_cast<void*>(resource), static_cast<void*>(context),
                  static_cast<void*>(contextDevice.Get()), static_cast<void*>(resourceDevice.Get()),
                  desc.Width, desc.Height, static_cast<unsigned>(desc.Format), desc.SampleDesc.Count,
                  desc.SampleDesc.Quality, desc.MipLevels, desc.ArraySize, static_cast<unsigned>(desc.Usage),
                  desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags, checkRegion ? 1u : 0u,
                  fits ? 1u : 0u, sameDevice ? 1u : 0u, valid ? "pass" : "error");
        return valid;
    }

    void log_contract_selftest()
    {
        static bool tested = false;
        if (tested)
            return;
        tested = true;
        const bool passed = region_fits(10, 8, 4, 3, 6, 5) &&
            !region_fits(10, 8, 5, 3, 6, 5) && !region_fits(10, 8, 4, 4, 6, 5) &&
            !region_fits(10, 8, 0, 3, 0, 0) && !region_fits(10, 8, 1, 1, ~0u, 0);
        logger().write("event=resource_contract_selftest status=%s cases=5", passed ? "pass" : "error");
    }

    float half_to_float(uint16_t value)
    {
        const unsigned sign = value >> 15;
        const unsigned exponent = (value >> 10) & 31;
        const unsigned fraction = value & 1023;
        float result;
        if (exponent == 31)
            result = fraction ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
        else if (exponent == 0)
            result = std::ldexp(static_cast<float>(fraction), -24);
        else
            result = std::ldexp(static_cast<float>(1024 + fraction), static_cast<int>(exponent) - 25);
        return sign ? -result : result;
    }

    bool readback_enabled()
    {
        static const bool result = [] {
            wchar_t value[8] = {};
            const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_READBACK", value, 8);
            if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                return false;
            if (size != 1 || value[0] != L'1')
            {
                donut::log::error("Temporal diagnostics: OPTISCALER_DEMO_READBACK must be 1");
                return false;
            }
            return true;
        }();
        return result;
    }

    struct Stats
    {
        unsigned count = 0, nonfinite = 0, nonzero = 0;
        uint64_t sampleHash = 14695981039346656037ull;
        float x[256] {}, y[256] {};
        uint8_t stencil[256] {};
        float minX = std::numeric_limits<float>::infinity();
        float maxX = -std::numeric_limits<float>::infinity();
        float minY = std::numeric_limits<float>::infinity();
        float maxY = -std::numeric_limits<float>::infinity();
    };

    bool sample_texture(ID3D11DeviceContext* context, ID3D11Resource* resource, bool depth,
                        unsigned width, unsigned height, unsigned offsetX, unsigned offsetY,
                        const char* label, Stats& stats)
    {
        auto& log = logger();
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = resource ? resource->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
        if (FAILED(hr))
        {
            log.write("event=readback_error label=%s stage=query_texture hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        const bool validFormat = depth ? desc.Format == DXGI_FORMAT_R24G8_TYPELESS
                                       : desc.Format == DXGI_FORMAT_R16G16_FLOAT;
        if (!validFormat || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
            width == 0 || height == 0 || offsetX > desc.Width || width > desc.Width - offsetX ||
            offsetY > desc.Height || height > desc.Height - offsetY)
        {
            log.write("event=readback_error label=%s stage=unsupported_desc format=%u size=%ux%u samples=%u "
                      "mips=%u array=%u region=%u,%u,%u,%u", label, static_cast<unsigned>(desc.Format),
                      desc.Width, desc.Height, desc.SampleDesc.Count, desc.MipLevels, desc.ArraySize,
                      offsetX, offsetY, width, height);
            return false;
        }
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr))
        {
            log.write("event=readback_error label=%s stage=create_staging hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            log.write("event=readback_error label=%s stage=map hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        uint64_t fullHash = 14695981039346656037ull;
        uint64_t id0Pixels = 0, id1Pixels = 0, ambiguousPixels = 0;
        for (unsigned y = offsetY; y < offsetY + height; ++y)
            for (unsigned x = offsetX; x < offsetX + width; ++x)
            {
                uint32_t bits;
                memcpy(&bits, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4, sizeof(bits));
                fullHash ^= depth ? bits & 0x00ffffffu : bits;
                fullHash *= 1099511628211ull;
                if (depth)
                {
                    switch ((bits >> 24) & 6u)
                    {
                    case 2: ++id0Pixels; break;
                    case 4: ++id1Pixels; break;
                    case 6: ++ambiguousPixels; break;
                    }
                }
            }
        const unsigned gridX = (std::min)(width, 16u), gridY = (std::min)(height, 16u);
        for (unsigned y = 0; y < gridY; ++y)
            for (unsigned x = 0; x < gridX; ++x)
            {
                const unsigned px = offsetX + static_cast<unsigned>((static_cast<uint64_t>(2 * x + 1) * width) / (2 * gridX));
                const unsigned py = offsetY + static_cast<unsigned>((static_cast<uint64_t>(2 * y + 1) * height) / (2 * gridY));
                const auto* data = static_cast<const uint8_t*>(mapped.pData) + py * mapped.RowPitch + px * 4;
                uint32_t bits;
                memcpy(&bits, data, sizeof(bits));
                stats.sampleHash ^= depth ? bits & 0x00ffffffu : bits;
                stats.sampleHash *= 1099511628211ull;
                const float vx = depth ? static_cast<float>(bits & 0x00ffffffu) / 16777215.f
                                       : half_to_float(static_cast<uint16_t>(bits));
                const float vy = depth ? 0.f : half_to_float(static_cast<uint16_t>(bits >> 16));
                stats.x[stats.count] = vx;
                stats.y[stats.count] = vy;
                stats.stencil[stats.count] = depth ? static_cast<uint8_t>(bits >> 24) : 0;
                ++stats.count;
                if (!std::isfinite(vx) || !std::isfinite(vy))
                {
                    ++stats.nonfinite;
                    continue;
                }
                stats.minX = (std::min)(stats.minX, vx);
                stats.maxX = (std::max)(stats.maxX, vx);
                stats.minY = (std::min)(stats.minY, vy);
                stats.maxY = (std::max)(stats.maxY, vy);
                if (!depth && (vx != 0.f || vy != 0.f))
                    ++stats.nonzero;
            }
        context->Unmap(staging.Get(), 0);
        log.write("event=readback label=%s resource=%p region=%u,%u,%u,%u samples=%u nonfinite=%u nonzero=%u "
                  "min_x=%.8g max_x=%.8g min_y=%.8g max_y=%.8g sample_hash=%016llX full_hash=%016llX "
                  "id0_pixels=%llu id1_pixels=%llu ambiguous_pixels=%llu", label, static_cast<void*>(resource),
                  offsetX, offsetY, width, height, stats.count, stats.nonfinite, stats.nonzero,
                  stats.minX, stats.maxX, stats.minY, stats.maxY, static_cast<unsigned long long>(stats.sampleHash),
                  static_cast<unsigned long long>(fullHash), static_cast<unsigned long long>(id0Pixels),
                  static_cast<unsigned long long>(id1Pixels), static_cast<unsigned long long>(ambiguousPixels));
        return true;
    }

    bool synthetic_readback_test(ID3D11DeviceContext* context)
    {
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        uint32_t pixels[16 * 16];
        std::fill(std::begin(pixels), std::end(pixels), 0x4000bc00u);
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 16;
        desc.Height = 16;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = pixels;
        data.SysMemPitch = 16 * sizeof(uint32_t);
        ComPtr<ID3D11Texture2D> motion;
        HRESULT hr = device->CreateTexture2D(&desc, &data, &motion);
        if (FAILED(hr))
        {
            logger().write("event=readback_selftest status=error stage=create_motion hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth;
        hr = device->CreateTexture2D(&desc, nullptr, &depth);
        if (FAILED(hr))
        {
            logger().write("event=readback_selftest status=error stage=create_depth hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc {};
        viewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11DepthStencilView> view;
        hr = device->CreateDepthStencilView(depth.Get(), &viewDesc, &view);
        if (FAILED(hr))
        {
            logger().write("event=readback_selftest status=error stage=create_dsv hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        context->ClearDepthStencilView(view.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 73);
        Stats mv, z;
        const bool motionOk = sample_texture(context, motion.Get(), false, 16, 16, 0, 0, "synthetic_motion", mv);
        const bool depthOk = sample_texture(context, depth.Get(), true, 16, 16, 0, 0, "synthetic_depth", z);
        const bool pass = motionOk && depthOk && mv.count == 256 && z.count == 256 &&
            mv.nonfinite == 0 && z.nonfinite == 0 && mv.minX == -1.f && mv.maxX == -1.f &&
            mv.minY == 2.f && mv.maxY == 2.f && std::abs(z.minX - 0.25f) < 0.000001f &&
            std::abs(z.maxX - 0.25f) < 0.000001f &&
            std::all_of(std::begin(z.stencil), std::end(z.stencil), [](uint8_t v) { return v == 73; });
        logger().write("event=readback_selftest status=%s expected_motion=-1,2 expected_depth=0.25 stencil=73",
                       pass ? "pass" : "fail");
        return pass;
    }
}

bool enabled(nvrhi::IDevice* device)
{
    return device != nullptr && device->getGraphicsAPI() == nvrhi::GraphicsAPI::D3D11 && logger().active;
}

void log_frame_pacing(unsigned limit, int64_t intervalNs)
{
    logger().write("event=frame_pacing limit=%u interval_ns=%lld", limit, static_cast<long long>(intervalNs));
}

void log_resize_request(unsigned step, int width, int height)
{
    logger().write("event=resize_request step=%u size=%dx%d", step, width, height);
}

void log_scene_instance(const char* name, unsigned index, float x, float y, float z,
                        float sizeX, float sizeY, float sizeZ)
{
    logger().write("event=scene_instance index=%u name=%s center=%.4f,%.4f,%.4f size=%.4f,%.4f,%.4f",
                   index, name, x, y, z, sizeX, sizeY, sizeZ);
}

void begin_frame(nvrhi::IDevice* device, uint64_t frame, const char* mode, int width, int height,
                 int displayWidth, int displayHeight, int offsetX, int offsetY,
                 float jitterX, float jitterY, bool previousValid, bool diagnosticCameraMotion,
                 const char* quality, int timedCycleStep, int64_t timedCycleElapsedMs,
                 bool diagnosticReset, bool diagnosticCut, bool diagnosticRecovery,
                 int64_t frameStartIntervalNs, float animationDeltaSeconds)
{
    if (!enabled(device))
        return;
    auto& log = logger();
    if (!log.active)
        return;
    log.frame = frame;
    if (frame == 120)
    {
        wchar_t value[8] {};
        const DWORD length = GetEnvironmentVariableW(L"OPTISCALER_DEMO_IDENTITY_SELFTEST", value, 8);
        if (length != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND)
        {
            if (length != 1 || value[0] != L'1')
                log.write("event=identity_selftest status=error stage=config");
            else
            {
                ID3D11Device* nativeDevice = device->getNativeObject(nvrhi::ObjectTypes::D3D11_Device);
                const auto result = run_identity_selftest(nativeDevice);
                log.write("event=identity_selftest status=%s cases=%u pixels=%llu mismatches=%llu hr=0x%08lX",
                          result.error == 0 && result.cases == 14 && result.pixels == 106496 &&
                              result.mismatches == 0 ? "pass" : "error",
                          result.cases, static_cast<unsigned long long>(result.pixels),
                          static_cast<unsigned long long>(result.mismatches), result.error);
            }
        }
    }
    const bool modeChanged = strcmp(log.lastMode, mode) != 0 || strcmp(log.lastQuality, quality) != 0;
    const bool previousValidityChanged = static_cast<int>(previousValid) != log.lastPreviousValid;
    const bool sizeChanged = width != log.lastWidth || height != log.lastHeight ||
        displayWidth != log.lastDisplayWidth || displayHeight != log.lastDisplayHeight;
    log.sampled = modeChanged || sizeChanged || previousValidityChanged ||
                  diagnosticReset || diagnosticRecovery ||
                  timedCycleStep != log.lastTimedCycleStep || frame <= 5 || frame % 120 == 0;
    if (!log.sampled)
        return;
    strcpy_s(log.lastMode, mode);
    strcpy_s(log.lastQuality, quality);
    log.lastTimedCycleStep = timedCycleStep;
    log.lastWidth = width;
    log.lastHeight = height;
    log.lastDisplayWidth = displayWidth;
    log.lastDisplayHeight = displayHeight;
    log.lastPreviousValid = previousValid ? 1 : 0;
    log.write("event=frame mode=%s quality_setting=%s cycle_step=%d cycle_elapsed_ms=%lld render=%dx%d display=%dx%d offset=%d,%d jitter=%.6f,%.6f previous_valid=%u diagnostic_camera=%u diagnostic_reset=%u diagnostic_cut=%u diagnostic_recovery=%u",
              mode, quality, timedCycleStep, static_cast<long long>(timedCycleElapsedMs), width, height,
              displayWidth, displayHeight, offsetX, offsetY, jitterX, jitterY, previousValid ? 1u : 0u,
              diagnosticCameraMotion ? 1u : 0u, diagnosticReset ? 1u : 0u, diagnosticCut ? 1u : 0u,
              diagnosticRecovery ? 1u : 0u);
    log.write("event=frame_timing frame_start_interval_ns=%lld animation_delta_seconds=%.9f",
              static_cast<long long>(frameStartIntervalNs), animationDeltaSeconds);
    if (width <= 0 || height <= 0 || displayWidth <= 0 || displayHeight <= 0 ||
        offsetX < 0 || offsetY < 0 || !std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        frameStartIntervalNs < 0 || !std::isfinite(animationDeltaSeconds) || animationDeltaSeconds < 0.f)
        log.write("event=temporal_parameter_error path=frame error=invalid_dimensions_jitter_or_timing");
}

void clear_stencil(nvrhi::IDevice* device, nvrhi::ITexture* depth)
{
    auto& log = logger();
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    ComPtr<ID3D11Texture2D> texture;
    ID3D11Resource* resource = native(depth);
    HRESULT hr = resource ? resource->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
    D3D11_TEXTURE2D_DESC desc {};
    if (SUCCEEDED(hr))
        texture->GetDesc(&desc);
    if (FAILED(hr) || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
        desc.Format != DXGI_FORMAT_R24G8_TYPELESS || desc.SampleDesc.Count != 1 ||
        desc.MipLevels != 1 || desc.ArraySize != 1)
    {
        log.write("event=stencil_clear_error stage=validate hr=0x%08X format=%u", static_cast<unsigned>(hr),
                  static_cast<unsigned>(desc.Format));
        return;
    }
    ComPtr<ID3D11Device> nativeDevice;
    context->GetDevice(&nativeDevice);
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc {};
    viewDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    viewDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> depthView;
    hr = nativeDevice->CreateDepthStencilView(texture.Get(), &viewDesc, &depthView);
    if (FAILED(hr))
    {
        log.write("event=stencil_clear_error stage=create_view hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_STENCIL, 1.f, 0);
    if (log.sampled)
        log.write("event=stencil_clear scope=diagnostic stencil=0 depth_preserved=1");
}

void log_taa(nvrhi::IDevice* device, nvrhi::ITexture* color, nvrhi::ITexture* output,
             nvrhi::ITexture* depth, nvrhi::ITexture* motion, nvrhi::ITexture* feedback1,
             nvrhi::ITexture* feedback2, unsigned width, unsigned height, unsigned offsetX, unsigned offsetY)
{
    auto& log = logger();
    if (!log.sampled)
        return;
    log.write("event=taa_inputs ngx_evaluate=0 resolve_color=%p output=%p motion=%p motion_pass_depth=%p "
                  "feedback1=%p feedback2=%p feedback_direction=alternates",
                  static_cast<void*>(native(color)), static_cast<void*>(native(output)),
                  static_cast<void*>(native(motion)), static_cast<void*>(native(depth)),
                  static_cast<void*>(native(feedback1)), static_cast<void*>(native(feedback2)));
    log_contract_selftest();
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    log_resource_contract(context, "taa", "color", native(color), true, true, width, height, offsetX, offsetY);
    log_resource_contract(context, "taa", "depth", native(depth), true, true, width, height, offsetX, offsetY);
    log_resource_contract(context, "taa", "motion", native(motion), true, true, width, height, offsetX, offsetY);
    log_resource_contract(context, "taa", "output", native(output), true, false, width, height, offsetX, offsetY);
    log_resource_contract(context, "taa", "feedback1", native(feedback1), true, false, width, height, offsetX, offsetY);
    log_resource_contract(context, "taa", "feedback2", native(feedback2), true, false, width, height, offsetX, offsetY);
}

void verify_taa_gpu_completion(nvrhi::IDevice* device)
{
    auto& log = logger();
    if (!log.sampled || log.frame != 120)
        return;
    wchar_t setting[8] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_GPU_SYNC_TEST", setting, 8);
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return;
    if (size != 1 || setting[0] != L'1')
    {
        log.write("event=gpu_sync_error stage=config error=expected_1");
        return;
    }
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=gpu_sync_error stage=context error=missing_or_non_immediate");
        return;
    }
    ComPtr<ID3D11Device> nativeDevice;
    context->GetDevice(&nativeDevice);
    D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
    ComPtr<ID3D11Query> query;
    HRESULT hr = nativeDevice->CreateQuery(&desc, &query);
    if (FAILED(hr))
    {
        log.write("event=gpu_sync_error stage=create_query hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    log.write("event=gpu_sync_marker scope=taa_after_dispatch");
    context->End(query.Get());
    context->Flush();
    const ULONGLONG start = GetTickCount64();
    BOOL complete = FALSE;
    do
    {
        hr = context->GetData(query.Get(), &complete, sizeof(complete), D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (hr == S_OK && complete)
        {
            log.write("event=gpu_sync_complete scope=taa_after_dispatch wait_ms=%llu",
                      static_cast<unsigned long long>(GetTickCount64() - start));
            return;
        }
        if (hr != S_FALSE)
        {
            log.write("event=gpu_sync_error stage=get_data hr=0x%08X complete=%u",
                      static_cast<unsigned>(hr), complete ? 1u : 0u);
            return;
        }
        Sleep(1);
    } while (GetTickCount64() - start < 5000);
    log.write("event=gpu_sync_error stage=timeout timeout_ms=5000");
}

void verify_taa_gpu_pipeline(nvrhi::IDevice* device)
{
    auto& log = logger();
    static const int setting = [] {
        wchar_t value[8] {};
        const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_GPU_PIPELINE_TEST", value, 8);
        if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return 0;
        if (size == 1 && value[0] == L'1')
            return 1;
        logger().write("event=gpu_sync_error stage=pipeline_config error=expected_1");
        return -1;
    }();
    if (setting != 1 || log.frame < 120 || log.frame > 128)
        return;

    struct PendingQuery { ComPtr<ID3D11Query> query; uint64_t frame; };
    static std::vector<PendingQuery> pending;
    static unsigned submitted = 0, completed = 0, maxOutstanding = 0;
    static bool failed = false;
    if (failed)
        return;
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=gpu_sync_error stage=pipeline_context error=missing_or_non_immediate");
        failed = true;
        return;
    }
    if (log.frame <= 127)
    {
        ComPtr<ID3D11Device> nativeDevice;
        context->GetDevice(&nativeDevice);
        D3D11_QUERY_DESC desc { D3D11_QUERY_EVENT, 0 };
        PendingQuery item {};
        const HRESULT hr = nativeDevice->CreateQuery(&desc, &item.query);
        if (FAILED(hr))
        {
            log.write("event=gpu_sync_error stage=pipeline_create hr=0x%08X", static_cast<unsigned>(hr));
            failed = true;
            return;
        }
        item.frame = log.frame;
        context->End(item.query.Get());
        pending.push_back(item);
        ++submitted;
        maxOutstanding = (std::max)(maxOutstanding, static_cast<unsigned>(pending.size()));
        log.write("event=gpu_pipeline_submit submitted_frame=%llu outstanding=%u",
                  static_cast<unsigned long long>(item.frame), static_cast<unsigned>(pending.size()));
    }
    if (log.frame < 122)
        return;
    if (log.frame == 128)
        context->Flush();
    const ULONGLONG start = GetTickCount64();
    do
    {
        while (!pending.empty())
        {
            BOOL complete = FALSE;
            const HRESULT hr = context->GetData(pending.front().query.Get(), &complete, sizeof(complete),
                                                D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if (hr == S_FALSE)
                break;
            if (hr != S_OK || !complete)
            {
                log.write("event=gpu_sync_error stage=pipeline_get_data hr=0x%08X complete=%u",
                          static_cast<unsigned>(hr), complete ? 1u : 0u);
                failed = true;
                return;
            }
            log.write("event=gpu_pipeline_complete submitted_frame=%llu observed_frame=%llu outstanding_before=%u",
                      static_cast<unsigned long long>(pending.front().frame),
                      static_cast<unsigned long long>(log.frame), static_cast<unsigned>(pending.size()));
            pending.erase(pending.begin());
            ++completed;
        }
        if (log.frame != 128 || pending.empty())
            break;
        Sleep(1);
    } while (GetTickCount64() - start < 5000);
    if (log.frame == 128)
    {
        if (!pending.empty())
        {
            log.write("event=gpu_sync_error stage=pipeline_timeout pending=%u", static_cast<unsigned>(pending.size()));
            failed = true;
        }
        else
            log.write("event=gpu_pipeline_result submitted=%u completed=%u max_outstanding=%u",
                      submitted, completed, maxOutstanding);
    }
}

void capture_temporal(nvrhi::IDevice* device, nvrhi::ITexture* depth, nvrhi::ITexture* motion,
                      unsigned width, unsigned height, unsigned offsetX, unsigned offsetY,
                      const donut::engine::IView* view, const donut::engine::IView* previousView,
                      bool previousValid, const donut::math::float3* objectDelta0,
                      const donut::math::float3* objectDelta1, bool objectIds)
{
    auto& log = logger();
    if (!log.sampled || !readback_enabled())
        return;
    static unsigned captures = 0;
    static bool tested = false;
    static bool selftestPassed = false;
    if (captures >= 8)
        return;
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=readback_error stage=context error=missing_or_non_immediate");
        captures = 8;
        return;
    }
    if (!tested)
    {
        tested = true;
        selftestPassed = synthetic_readback_test(context);
    }
    if (!selftestPassed)
    {
        log.write("event=readback_error stage=selftest error=disabled_after_failure");
        captures = 8;
        return;
    }
    ++captures;
    LARGE_INTEGER start {}, end {}, frequency {};
    const bool timing = QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0 &&
                        QueryPerformanceCounter(&start);
    Stats mv, z;
    const bool motionOk = sample_texture(context, native(motion), false, width, height, offsetX, offsetY, "motion", mv);
    const bool depthOk = sample_texture(context, native(depth), true, width, height, offsetX, offsetY, "depth", z);
    if (motionOk && depthOk && previousValid && view != nullptr && previousView != nullptr &&
        previousView->GetViewportState().viewports.size() == 1)
    {
        using namespace donut::math;
        const auto reprojection = inverse(view->GetProjectionMatrix(false)) *
            affineToHomogeneous(inverse(view->GetViewMatrix()) * previousView->GetViewMatrix()) *
            previousView->GetProjectionMatrix(false);
        const float3 deltas[] = { objectDelta0 ? *objectDelta0 : float3(0.f),
                                  objectDelta1 ? *objectDelta1 : float3(0.f) };
        const auto reprojection0 = inverse(view->GetProjectionMatrix(false)) *
            affineToHomogeneous(inverse(view->GetViewMatrix()) * translation(-deltas[0]) * previousView->GetViewMatrix()) *
            previousView->GetProjectionMatrix(false);
        const auto reprojection1 = inverse(view->GetProjectionMatrix(false)) *
            affineToHomogeneous(inverse(view->GetViewMatrix()) * translation(-deltas[1]) * previousView->GetViewMatrix()) *
            previousView->GetProjectionMatrix(false);
        const auto previousViewport = previousView->GetViewportState().viewports[0];
        const unsigned gridX = (std::min)(width, 16u), gridY = (std::min)(height, 16u);
        unsigned compared = 0, stencilSet = 0, stencilUnset = 0, invalidProjection = 0, mismatches = 0;
        unsigned informative = 0, reversedRejected = 0, normalizedRejected = 0;
        unsigned stencilSetMismatches = 0, stencilUnsetMismatches = 0;
        unsigned objectMatches[2] = {}, objectDistinct[2] = {};
        unsigned idCompared[2] = {}, idMatches[2] = {}, idMismatch[2] = {}, idDistinct[2] = {};
        unsigned idAmbiguous = 0, idUnmarked = 0;
        float maxError = 0.f, maxExpected = 0.f;
        for (unsigned gy = 0; gy < gridY; ++gy)
            for (unsigned gx = 0; gx < gridX; ++gx)
            {
                const unsigned i = gy * gridX + gx;
                const bool marked = (z.stencil[i] & 1u) != 0;
                marked ? ++stencilSet : ++stencilUnset;
                const unsigned ids = objectIds ? (z.stencil[i] & 6u) : 0;
                if (ids == 6u) ++idAmbiguous;
                if (ids != 0 && !marked) ++idUnmarked;
                const unsigned px = offsetX + static_cast<unsigned>((static_cast<uint64_t>(2 * gx + 1) * width) / (2 * gridX));
                const unsigned py = offsetY + static_cast<unsigned>((static_cast<uint64_t>(2 * gy + 1) * height) / (2 * gridY));
                const float positionX = static_cast<float>(px) + 0.5f;
                const float positionY = static_cast<float>(py) + 0.5f;
                const float clip[4] = {
                    2.f * (positionX - static_cast<float>(offsetX)) / static_cast<float>(width) - 1.f,
                    1.f - 2.f * (positionY - static_cast<float>(offsetY)) / static_cast<float>(height),
                    z.x[i], 1.f
                };
                float prev[4] = {};
                for (unsigned col = 0; col < 4; ++col)
                    for (unsigned row = 0; row < 4; ++row)
                        prev[col] += clip[row] * reprojection[row][col];
                if (prev[3] <= 0.f || !std::isfinite(prev[0]) || !std::isfinite(prev[1]) || !std::isfinite(prev[3]) ||
                    !std::isfinite(mv.x[i]) || !std::isfinite(mv.y[i]))
                {
                    ++invalidProjection;
                    continue;
                }
                const float expectedX = (0.5f + 0.5f * prev[0] / prev[3]) *
                    previousViewport.width() + previousViewport.minX - positionX;
                const float expectedY = (0.5f - 0.5f * prev[1] / prev[3]) *
                    previousViewport.height() + previousViewport.minY - positionY;
                const float error = (std::max)(std::abs(expectedX - mv.x[i]), std::abs(expectedY - mv.y[i]));
                const float magnitude = (std::max)(std::abs(expectedX), std::abs(expectedY));
                const float tolerance = 0.02f + 0.002f * magnitude;
                if (magnitude > 2.f * tolerance)
                {
                    ++informative;
                    const float reversedError = (std::max)(std::abs(-expectedX - mv.x[i]),
                                                           std::abs(-expectedY - mv.y[i]));
                    const float normalizedError = (std::max)(std::abs(expectedX / static_cast<float>(width) - mv.x[i]),
                                                             std::abs(expectedY / static_cast<float>(height) - mv.y[i]));
                    if (reversedError > tolerance)
                        ++reversedRejected;
                    if (normalizedError > tolerance)
                        ++normalizedRejected;
                }
                if (marked && objectDelta0 && objectDelta1)
                {
                    const float4x4 matrices[] = { reprojection0, reprojection1 };
                    for (unsigned j = 0; j < 2; ++j)
                    {
                        float objPrev[4] = {};
                        for (unsigned col = 0; col < 4; ++col)
                            for (unsigned row = 0; row < 4; ++row)
                                objPrev[col] += clip[row] * matrices[j][row][col];
                        if (objPrev[3] <= 0.f || !std::isfinite(objPrev[3]))
                            continue;
                        const float ox = (0.5f + 0.5f * objPrev[0] / objPrev[3]) *
                            previousViewport.width() + previousViewport.minX - positionX;
                        const float oy = (0.5f - 0.5f * objPrev[1] / objPrev[3]) *
                            previousViewport.height() + previousViewport.minY - positionY;
                        const float objectError = (std::max)(std::abs(ox - mv.x[i]), std::abs(oy - mv.y[i]));
                        const float separation = (std::max)(std::abs(ox - expectedX), std::abs(oy - expectedY));
                        if (ids == (2u << j))
                        {
                            ++idCompared[j];
                            if (std::isfinite(objectError) && objectError <= tolerance)
                            {
                                ++idMatches[j];
                                if (separation > 2.f * tolerance && error > tolerance)
                                    ++idDistinct[j];
                            }
                            else
                                ++idMismatch[j];
                        }
                        if (std::isfinite(objectError) && separation > 2.f * tolerance)
                        {
                            ++objectDistinct[j];
                            if (objectError <= tolerance && error > tolerance)
                                ++objectMatches[j];
                        }
                    }
                }
                ++compared;
                if (!std::isfinite(error) || error > tolerance)
                {
                    ++mismatches;
                    marked ? ++stencilSetMismatches : ++stencilUnsetMismatches;
                }
                maxError = (std::max)(maxError, error);
                maxExpected = (std::max)(maxExpected, magnitude);
            }
        log.write("event=motion_reprojection_check scope=static_geometry_assumption tolerance=0.02_plus_0.002_abs "
                  "compared=%u stencil_set=%u stencil_unset=%u invalid_projection=%u mismatches=%u "
                  "set_mismatches=%u unset_mismatches=%u max_error=%.8g max_expected=%.8g",
                  compared, stencilSet, stencilUnset, invalidProjection, mismatches, stencilSetMismatches,
                  stencilUnsetMismatches, maxError, maxExpected);
        log.write("event=motion_convention_controls scope=static_geometry_assumption informative=%u reversed_rejected=%u normalized_rejected=%u",
                  informative, reversedRejected, normalizedRejected);
        if (objectDelta0 && objectDelta1)
            log.write("event=object_motion_models scope=unsegmented_grid tolerance=0.02_plus_0.002_abs "
                      "delta0=%.6f,%.6f,%.6f delta1=%.6f,%.6f,%.6f distinct0=%u matches0=%u distinct1=%u matches1=%u",
                      deltas[0].x, deltas[0].y, deltas[0].z, deltas[1].x, deltas[1].y, deltas[1].z,
                      objectDistinct[0], objectMatches[0], objectDistinct[1], objectMatches[1]);
        if (objectIds && objectDelta0 && objectDelta1)
            log.write("event=object_id_motion_check scope=depth_equal_stencil_grid "
            "compared0=%u matches0=%u mismatches0=%u distinct0=%u "
            "compared1=%u matches1=%u mismatches1=%u distinct1=%u "
            "ambiguous=%u id_without_motion_stencil=%u invalid_projection=%u",
            idCompared[0], idMatches[0], idMismatch[0], idDistinct[0],
            idCompared[1], idMatches[1], idMismatch[1], idDistinct[1],
                      idAmbiguous, idUnmarked, invalidProjection);
    }
    else
        log.write("event=motion_reprojection_check status=unavailable previous_valid=%u motion_ok=%u depth_ok=%u",
                  previousValid ? 1u : 0u, motionOk ? 1u : 0u, depthOk ? 1u : 0u);
    const bool timingOk = timing && QueryPerformanceCounter(&end);
    const double durationMs = timingOk ? 1000. * static_cast<double>(end.QuadPart - start.QuadPart) /
                                           static_cast<double>(frequency.QuadPart) : -1.;
    log.write("event=readback_complete stage=after_motion_before_temporal capture=%u motion_ok=%u depth_ok=%u duration_ms=%.3f",
              captures, motionOk ? 1u : 0u, depthOk ? 1u : 0u, durationMs);
}

void log_taa_bindings(nvrhi::IDevice* device, nvrhi::ITexture* motion,
                      nvrhi::ITexture* feedback1, nvrhi::ITexture* feedback2)
{
    auto& log = logger();
    if (!log.sampled)
        return;
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (context == nullptr || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=taa_binding_error error=missing_or_non_immediate_context");
        return;
    }
    ComPtr<ID3D11ShaderResourceView> motionView, feedbackView;
    ComPtr<ID3D11UnorderedAccessView> outputView;
    context->CSGetShaderResources(1, 1, &motionView);
    context->CSGetShaderResources(2, 1, &feedbackView);
    context->CSGetUnorderedAccessViews(1, 1, &outputView);
    ComPtr<ID3D11Resource> motionResource, inputResource, outputResource;
    if (motionView) motionView->GetResource(&motionResource);
    if (feedbackView) feedbackView->GetResource(&inputResource);
    if (outputView) outputView->GetResource(&outputResource);
    ID3D11Resource* feedbackA = native(feedback1);
    ID3D11Resource* feedbackB = native(feedback2);
    const bool valid = motionResource.Get() == native(motion) &&
        ((inputResource.Get() == feedbackA && outputResource.Get() == feedbackB) ||
         (inputResource.Get() == feedbackB && outputResource.Get() == feedbackA)) &&
        feedbackA != nullptr && feedbackB != nullptr && feedbackA != feedbackB;
    log.write("event=taa_bindings stage=after_dispatch cs_t1_motion=%p cs_t2_feedback_input=%p "
              "cs_u1_feedback_output=%p feedback1=%p feedback2=%p identity_valid=%u",
              static_cast<void*>(motionResource.Get()), static_cast<void*>(inputResource.Get()),
              static_cast<void*>(outputResource.Get()), static_cast<void*>(feedbackA),
              static_cast<void*>(feedbackB), valid ? 1u : 0u);
    if (!valid)
        log.write("event=taa_binding_error error=unexpected_or_missing_bindings");
}

void log_dlss_inputs(ID3D11DeviceContext* context, ID3D11Resource* color, ID3D11Resource* output, ID3D11Resource* depth,
                     ID3D11Resource* motion, ID3D11Resource* exposure, unsigned width, unsigned height,
                     unsigned offsetX, unsigned offsetY, float jitterX, float jitterY,
                     float motionScaleX, float motionScaleY, bool reset)
{
    auto& log = logger();
    if (!log.sampled)
        return;
    log.write("event=ngx_dx11_evaluate_inputs color=%p output=%p depth=%p motion=%p exposure=%p "
                  "render=%ux%u offset=%u,%u jitter=%.6f,%.6f motion_scale=%.6f,%.6f reset=%u",
                  static_cast<void*>(color), static_cast<void*>(output), static_cast<void*>(depth),
                  static_cast<void*>(motion), static_cast<void*>(exposure), width, height, offsetX, offsetY,
                  jitterX, jitterY, motionScaleX, motionScaleY, reset ? 1u : 0u);
    if (!std::isfinite(jitterX) || !std::isfinite(jitterY) ||
        !std::isfinite(motionScaleX) || !std::isfinite(motionScaleY))
        log.write("event=temporal_parameter_error path=dlss error=non_finite_jitter_or_motion_scale");
    log_contract_selftest();
    log_resource_contract(context, "dlss", "color", color, true, true, width, height, offsetX, offsetY);
    log_resource_contract(context, "dlss", "depth", depth, true, true, width, height, offsetX, offsetY);
    log_resource_contract(context, "dlss", "motion", motion, true, true, width, height, offsetX, offsetY);
    log_resource_contract(context, "dlss", "output", output, true, false, width, height, offsetX, offsetY);
    log_resource_contract(context, "dlss", "exposure", exposure, false, false, width, height, offsetX, offsetY);
}

void log_dlss_result(unsigned result)
{
    auto& log = logger();
    if (log.sampled)
        log.write("event=ngx_dx11_evaluate_result result=0x%08X", result);
}
}
#endif
