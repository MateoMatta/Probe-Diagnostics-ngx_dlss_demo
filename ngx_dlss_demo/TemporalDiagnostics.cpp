#include "TemporalDiagnostics.h"
#include "IdentitySelfTest.h"

#if USE_DX11
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
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

    struct ColorStats
    {
        uint64_t count = 0, nonfinite = 0, nonzero = 0, sentinel = 0;
        float minimum[4] = { INFINITY, INFINITY, INFINITY, INFINITY };
        float maximum[4] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };
    };

    bool sample_color(ID3D11DeviceContext* context, ID3D11Resource* resource,
                      unsigned width, unsigned height, unsigned offsetX, unsigned offsetY,
                      const char* label, ColorStats& stats)
    {
        auto& log = logger();
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = resource ? resource->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
        if (FAILED(hr) || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            log.write("event=color_readback_error label=%s stage=resource_or_context hr=0x%08X",
                      label, static_cast<unsigned>(hr));
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Device> device, owner;
        context->GetDevice(&device);
        texture->GetDevice(&owner);
        if (!device || owner.Get() != device.Get() || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
            desc.SampleDesc.Count != 1 || desc.ArraySize != 1 || desc.MipLevels != 1 ||
            !region_fits(desc.Width, desc.Height, width, height, offsetX, offsetY))
        {
            log.write("event=color_readback_error label=%s stage=unsupported_desc format=%u size=%ux%u "
                      "samples=%u mips=%u array=%u region=%u,%u,%u,%u same_device=%u",
                      label, static_cast<unsigned>(desc.Format), desc.Width, desc.Height, desc.SampleDesc.Count,
                      desc.MipLevels, desc.ArraySize, offsetX, offsetY, width, height, owner.Get() == device.Get());
            return false;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr))
        {
            log.write("event=color_readback_error label=%s stage=create_staging hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            log.write("event=color_readback_error label=%s stage=map hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        for (unsigned y = offsetY; y < offsetY + height; ++y)
            for (unsigned x = offsetX; x < offsetX + width; ++x)
            {
                uint16_t bits[4];
                memcpy(bits, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * sizeof(bits), sizeof(bits));
                if (bits[0] == 0xd000 && bits[1] == 0xd000 && bits[2] == 0xd000 && bits[3] == 0xd000)
                    ++stats.sentinel;
                bool finite = true, nonzero = false;
                for (unsigned c = 0; c < 4; ++c)
                {
                    const float value = half_to_float(bits[c]);
                    if (!std::isfinite(value))
                        finite = false;
                    else
                    {
                        stats.minimum[c] = (std::min)(stats.minimum[c], value);
                        stats.maximum[c] = (std::max)(stats.maximum[c], value);
                        nonzero = nonzero || (c < 3 && value != 0.f);
                    }
                }
                ++stats.count;
                if (!finite) ++stats.nonfinite;
                if (nonzero) ++stats.nonzero;
            }
        context->Unmap(staging.Get(), 0);
        log.write("event=color_readback label=%s resource=%p region=%u,%u,%u,%u pixels=%llu nonfinite=%llu nonzero=%llu "
                  "min=%.8g,%.8g,%.8g,%.8g max=%.8g,%.8g,%.8g,%.8g",
                  label, static_cast<void*>(resource), offsetX, offsetY, width, height,
                  static_cast<unsigned long long>(stats.count), static_cast<unsigned long long>(stats.nonfinite),
                  static_cast<unsigned long long>(stats.nonzero), stats.minimum[0], stats.minimum[1],
                  stats.minimum[2], stats.minimum[3], stats.maximum[0], stats.maximum[1], stats.maximum[2], stats.maximum[3]);
        return true;
    }

    bool synthetic_color_test(ID3D11DeviceContext* context)
    {
        uint16_t pixels[4][4][4] {};
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
            {
                pixels[y][x][0] = 0x4900; // 10 outside the selected region
                if (x >= 1 && x < 3 && y >= 1 && y < 3)
                {
                    pixels[y][x][0] = 0x3400; // 0.25
                    pixels[y][x][1] = 0x3800; // 0.5
                    pixels[y][x][2] = 0x3c00; // 1
                    pixels[y][x][3] = 0x4000; // 2
                }
            }
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 4; desc.Height = 4; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = pixels; data.SysMemPitch = sizeof(pixels[0]);
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT hr = device->CreateTexture2D(&desc, &data, &texture);
        if (FAILED(hr))
        {
            logger().write("event=color_selftest status=error stage=create hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        ColorStats stats;
        const bool read = sample_color(context, texture.Get(), 2, 2, 1, 1, "synthetic", stats);
        const float expected[] = { 0.25f, 0.5f, 1.f, 2.f };
        bool passed = read && stats.count == 4 && stats.nonfinite == 0 && stats.nonzero == 4;
        for (unsigned c = 0; c < 4; ++c)
            passed = passed && stats.minimum[c] == expected[c] && stats.maximum[c] == expected[c];
        logger().write("event=color_selftest status=%s region=1,1,2,2 expected=0.25,0.5,1,2", passed ? "pass" : "error");
        return passed;
    }

    bool sample_exposure(ID3D11DeviceContext* context, ID3D11Resource* resource, const char* label, float& value)
    {
        auto& log = logger();
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT query = resource ? resource->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
        if (FAILED(query) || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            log.write("event=exposure_readback_error label=%s stage=resource_or_context hr=0x%08X",
                      label, static_cast<unsigned>(query));
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Device> device, owner;
        context->GetDevice(&device);
        texture->GetDevice(&owner);
        if (!device || owner.Get() != device.Get() || desc.Format != DXGI_FORMAT_R16_FLOAT ||
            desc.Width != 1 || desc.Height != 1 || desc.SampleDesc.Count != 1 ||
            desc.ArraySize != 1 || desc.MipLevels != 1)
        {
            log.write("event=exposure_readback_error label=%s stage=unsupported_desc format=%u size=%ux%u "
                      "samples=%u mips=%u array=%u same_device=%u",
                      label, static_cast<unsigned>(desc.Format), desc.Width, desc.Height, desc.SampleDesc.Count,
                      desc.MipLevels, desc.ArraySize, owner.Get() == device.Get());
            return false;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        HRESULT hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr))
        {
            log.write("event=exposure_readback_error label=%s stage=create_staging hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            log.write("event=exposure_readback_error label=%s stage=map hr=0x%08X", label, static_cast<unsigned>(hr));
            return false;
        }
        uint16_t bits;
        memcpy(&bits, mapped.pData, sizeof(bits));
        context->Unmap(staging.Get(), 0);
        value = half_to_float(bits);
        log.write("event=exposure_readback label=%s resource=%p bits=0x%04X value=%.8g finite=%u",
                  label, static_cast<void*>(resource), static_cast<unsigned>(bits), value, std::isfinite(value) ? 1u : 0u);
        return true;
    }

    bool synthetic_exposure_test(ID3D11DeviceContext* context)
    {
        const uint16_t expectedBits = 0x3800;
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R16_FLOAT; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
        D3D11_SUBRESOURCE_DATA data {};
        data.pSysMem = &expectedBits; data.SysMemPitch = sizeof(expectedBits);
        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        ComPtr<ID3D11Texture2D> texture;
        const HRESULT hr = device->CreateTexture2D(&desc, &data, &texture);
        if (FAILED(hr))
        {
            logger().write("event=exposure_selftest status=error stage=create hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        float value = 0.f;
        const bool passed = sample_exposure(context, texture.Get(), "synthetic", value) && value == 0.5f;
        logger().write("event=exposure_selftest status=%s expected=0.5", passed ? "pass" : "error");
        return passed;
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
    static unsigned pendingPolls = 0, completePolls = 0, unretired = 0;
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
        BOOL immediateComplete = FALSE;
        const HRESULT immediateHr = context->GetData(item.query.Get(), &immediateComplete, sizeof(immediateComplete),
                                                      D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if (immediateHr == S_FALSE)
            ++pendingPolls;
        else if (immediateHr == S_OK && immediateComplete)
            ++completePolls;
        else
        {
            log.write("event=gpu_sync_error stage=pipeline_immediate_poll hr=0x%08X complete=%u",
                      static_cast<unsigned>(immediateHr), immediateComplete ? 1u : 0u);
            failed = true;
            return;
        }
        log.write("event=gpu_pipeline_immediate submitted_frame=%llu status=%s",
                  static_cast<unsigned long long>(item.frame), immediateHr == S_FALSE ? "not_ready" : "complete");
    }
    if (log.frame < 122)
    {
        unretired = static_cast<unsigned>(pending.size());
        return;
    }
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
        if (pending.empty())
            log.write("event=gpu_pipeline_immediate_summary not_ready=%u complete=%u unretired_before_first_drain=%u",
                      pendingPolls, completePolls, unretired);
    }
}

void verify_late_addon_resource(nvrhi::IDevice* device)
{
    auto& log = logger();
    if (log.frame != 120)
        return;
    wchar_t path[32768] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_LATE_ADDON_PATH", path, _countof(path));
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return;
    if (size == 0 || size >= _countof(path))
    {
        log.write("event=late_addon_error stage=path");
        return;
    }
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=late_addon_error stage=context");
        return;
    }
    ComPtr<ID3D11Device> nativeDevice;
    context->GetDevice(&nativeDevice);
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 32; desc.Height = 32; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = nativeDevice->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr))
    {
        log.write("event=late_addon_error stage=create_texture hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    ComPtr<ID3D11RenderTargetView> view;
    hr = nativeDevice->CreateRenderTargetView(texture.Get(), nullptr, &view);
    if (FAILED(hr))
    {
        log.write("event=late_addon_error stage=create_view hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    log.write("event=late_addon_texture_created resource=%p", static_cast<void*>(texture.Get()));
    HMODULE reshadeModule = GetModuleHandleW(L"dxgi.dll");
    HMODULE addon = LoadLibraryW(path);
    if (!reshadeModule || !addon)
    {
        log.write("event=late_addon_error stage=load error=%lu", GetLastError());
        if (addon) FreeLibrary(addon);
        return;
    }
    using Init = bool (*)(HMODULE, HMODULE);
    using Uninit = void (*)(HMODULE, HMODULE);
    const auto init = reinterpret_cast<Init>(GetProcAddress(addon, "AddonInit"));
    const auto uninit = reinterpret_cast<Uninit>(GetProcAddress(addon, "AddonUninit"));
    auto* name = reinterpret_cast<const char**>(GetProcAddress(addon, "NAME"));
    if (!init || !uninit || !name)
    {
        log.write("event=late_addon_error stage=missing_exports");
        FreeLibrary(addon);
        return;
    }
    *name = "OptiScaler Late Registration Diagnostic";
    if (!init(addon, reshadeModule))
    {
        log.write("event=late_addon_error stage=registration");
        FreeLibrary(addon);
        return;
    }
    log.write("event=late_addon_registered resource=%p", static_cast<void*>(texture.Get()));
    ComPtr<ID3D11Texture2D> control;
    ComPtr<ID3D11RenderTargetView> controlView;
    hr = nativeDevice->CreateTexture2D(&desc, nullptr, &control);
    if (SUCCEEDED(hr))
        hr = nativeDevice->CreateRenderTargetView(control.Get(), nullptr, &controlView);
    if (FAILED(hr))
    {
        log.write("event=late_addon_error stage=control_create hr=0x%08X", static_cast<unsigned>(hr));
        uninit(addon, reshadeModule);
        FreeLibrary(addon);
        return;
    }
    log.write("event=late_addon_control_created resource=%p", static_cast<void*>(control.Get()));
    ComPtr<ID3D11RenderTargetView> previous[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
    ID3D11RenderTargetView* saved[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] {};
    ComPtr<ID3D11DepthStencilView> depth;
    context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, &depth);
    for (unsigned i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        previous[i].Attach(saved[i]);
    ID3D11RenderTargetView* target = view.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    log.write("event=late_addon_bound resource=%p", static_cast<void*>(texture.Get()));
    target = controlView.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    log.write("event=late_addon_control_bound resource=%p", static_cast<void*>(control.Get()));
    context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, saved, depth.Get());
    controlView.Reset();
    control.Reset();
    uninit(addon, reshadeModule);
    FreeLibrary(addon);
    log.write("event=late_addon_result status=complete");
}

void verify_multiple_swapchains(nvrhi::IDevice* device)
{
    auto& log = logger();
    if (log.frame != 120)
        return;
    wchar_t setting[8] {};
    const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_MULTISWAP_TEST", setting, 8);
    if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
        return;
    if (size != 1 || setting[0] != L'1')
    {
        log.write("event=multiswap_error stage=config");
        return;
    }

    struct TestWindows
    {
        HWND windows[2] {};
        ~TestWindows() { for (HWND window : windows) if (window) DestroyWindow(window); }
    } windows;
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=multiswap_error stage=context");
        return;
    }
    ComPtr<ID3D11Device> nativeDevice;
    context->GetDevice(&nativeDevice);
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr))
    {
        log.write("event=multiswap_error stage=factory hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    ComPtr<IDXGISwapChain> chains[2];
    for (unsigned i = 0; i < 2; ++i)
    {
        windows.windows[i] = CreateWindowExW(0, L"STATIC", L"OptiScaler diagnostic swapchain",
                                             WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                             GetModuleHandleW(nullptr), nullptr);
        if (!windows.windows[i])
        {
            log.write("event=multiswap_error stage=window index=%u error=%lu", i, GetLastError());
            return;
        }
        DXGI_SWAP_CHAIN_DESC desc {};
        desc.BufferDesc.Width = 64;
        desc.BufferDesc.Height = 64;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1;
        desc.OutputWindow = windows.windows[i];
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = factory->CreateSwapChain(nativeDevice.Get(), &desc, &chains[i]);
        if (FAILED(hr))
        {
            log.write("event=multiswap_error stage=create index=%u hr=0x%08X", i, static_cast<unsigned>(hr));
            return;
        }
        log.write("event=multiswap_created index=%u chain=%p hwnd=%p", i,
                  static_cast<void*>(chains[i].Get()), static_cast<void*>(windows.windows[i]));
    }
    if (chains[0].Get() == chains[1].Get())
    {
        log.write("event=multiswap_error stage=duplicate_chain");
        return;
    }
    hr = chains[0]->ResizeBuffers(1, 96, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr))
    {
        log.write("event=multiswap_error stage=resize hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    DXGI_SWAP_CHAIN_DESC desc[2] {};
    for (unsigned i = 0; i < 2; ++i)
    {
        hr = chains[i]->GetDesc(&desc[i]);
        if (FAILED(hr))
        {
            log.write("event=multiswap_error stage=description index=%u hr=0x%08X", i, static_cast<unsigned>(hr));
            return;
        }
    }
    if (desc[0].BufferDesc.Width != 96 || desc[0].BufferDesc.Height != 64 ||
        desc[1].BufferDesc.Width != 64 || desc[1].BufferDesc.Height != 64)
    {
        log.write("event=multiswap_error stage=dimensions first=%ux%u second=%ux%u",
                  desc[0].BufferDesc.Width, desc[0].BufferDesc.Height,
                  desc[1].BufferDesc.Width, desc[1].BufferDesc.Height);
        return;
    }
    log.write("event=multiswap_result status=pass chains=2 first=96x64 second=64x64");
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
    if (readback_enabled())
    {
        static bool tested = false;
        static bool passed = false;
        static unsigned captures = 0;
        if (!tested && context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            tested = true;
            passed = synthetic_color_test(context);
        }
        if (!passed)
            log.write("event=color_readback_error label=dlss_color stage=selftest_or_context");
        else if (captures < 8)
        {
            ColorStats stats;
            if (sample_color(context, color, width, height, offsetX, offsetY, "dlss_color", stats))
                ++captures;
        }
        if (exposure)
        {
            static bool exposureTested = false;
            static bool exposurePassed = false;
            static unsigned exposureCaptures = 0;
            if (!exposureTested && context && context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
            {
                exposureTested = true;
                exposurePassed = synthetic_exposure_test(context);
            }
            if (!exposurePassed)
                log.write("event=exposure_readback_error label=dlss_exposure stage=selftest_or_context");
            else if (exposureCaptures < 8)
            {
                float value = 0.f;
                if (sample_exposure(context, exposure, "dlss_exposure", value))
                    ++exposureCaptures;
            }
        }
    }
}

bool output_test_enabled()
{
    static const bool result = [] {
        wchar_t value[8] = {};
        const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_OUTPUT_TEST", value, 8);
        if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
            return false;
        if (size != 1 || value[0] != L'1')
        {
            donut::log::error("Temporal diagnostics: OPTISCALER_DEMO_OUTPUT_TEST must be 1");
            return false;
        }
        return true;
    }();
    return result;
}

bool prepare_dlss_output_test(ID3D11DeviceContext* context, ID3D11Resource* output)
{
    auto& log = logger();
    static unsigned captures = 0;
    if (!log.sampled || !output_test_enabled() || captures >= 8)
        return false;
    ++captures;
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT query = output ? output->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
    if (FAILED(query) || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=output_test_error stage=resource_or_context hr=0x%08X", static_cast<unsigned>(query));
        return false;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    ComPtr<ID3D11Device> device, owner;
    context->GetDevice(&device);
    texture->GetDevice(&owner);
    if (!device || owner.Get() != device.Get() || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        !(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
    {
        log.write("event=output_test_error stage=unsupported_desc format=%u size=%ux%u bind=0x%X",
                  static_cast<unsigned>(desc.Format), desc.Width, desc.Height, desc.BindFlags);
        return false;
    }
    ComPtr<ID3D11UnorderedAccessView> uav;
    const HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav);
    if (FAILED(hr))
    {
        log.write("event=output_test_error stage=create_uav hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    const float sentinel[4] = { -32.f, -32.f, -32.f, -32.f };
    context->ClearUnorderedAccessViewFloat(uav.Get(), sentinel);
    ColorStats before;
    if (!sample_color(context, output, desc.Width, desc.Height, 0, 0, "output_sentinel", before) ||
        before.sentinel != before.count || before.count != static_cast<uint64_t>(desc.Width) * desc.Height)
    {
        log.write("event=output_test_error stage=sentinel_control pixels=%llu sentinel=%llu",
                  static_cast<unsigned long long>(before.count), static_cast<unsigned long long>(before.sentinel));
        return false;
    }
    log.write("event=output_test_control resource=%p pixels=%llu sentinel=%llu",
              static_cast<void*>(output), static_cast<unsigned long long>(before.count),
              static_cast<unsigned long long>(before.sentinel));
    return true;
}

void verify_dlss_output_test(ID3D11DeviceContext* context, ID3D11Resource* output, bool evaluationSucceeded)
{
    auto& log = logger();
    ComPtr<ID3D11Texture2D> texture;
    const HRESULT hr = output ? output->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
    if (FAILED(hr) || !evaluationSucceeded)
    {
        log.write("event=output_test_error stage=evaluate_or_resource hr=0x%08X success=%u",
                  static_cast<unsigned>(hr), evaluationSucceeded ? 1u : 0u);
        return;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    ColorStats after;
    if (!sample_color(context, output, desc.Width, desc.Height, 0, 0, "dlss_output", after))
    {
        log.write("event=output_test_error stage=readback");
        return;
    }
    const bool valid = after.count == static_cast<uint64_t>(desc.Width) * desc.Height &&
        after.nonfinite == 0 && after.nonzero != 0 && after.sentinel == 0;
    log.write("event=output_test_result resource=%p size=%ux%u pixels=%llu sentinel=%llu nonfinite=%llu nonzero=%llu status=%s",
              static_cast<void*>(output), desc.Width, desc.Height, static_cast<unsigned long long>(after.count),
              static_cast<unsigned long long>(after.sentinel), static_cast<unsigned long long>(after.nonfinite),
              static_cast<unsigned long long>(after.nonzero), valid ? "pass" : "error");
}

void verify_dlss_consumer(nvrhi::IDevice* device, nvrhi::ITexture* source, nvrhi::ITexture* target)
{
    auto& log = logger();
    if (!log.sampled || !output_test_enabled())
        return;
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=output_test_error stage=consumer_context");
        return;
    }
    ComPtr<ID3D11ShaderResourceView> sourceView;
    ComPtr<ID3D11RenderTargetView> targetView;
    context->PSGetShaderResources(0, 1, &sourceView);
    context->OMGetRenderTargets(1, &targetView, nullptr);
    ComPtr<ID3D11Resource> boundSource, boundTarget;
    if (sourceView) sourceView->GetResource(&boundSource);
    if (targetView) targetView->GetResource(&boundTarget);
    const bool valid = native(source) && native(target) &&
        boundSource.Get() == native(source) && boundTarget.Get() == native(target);
    log.write("event=output_test_consumer source=%p ps_t0=%p target=%p om_rtv0=%p status=%s",
              static_cast<void*>(native(source)), static_cast<void*>(boundSource.Get()),
              static_cast<void*>(native(target)), static_cast<void*>(boundTarget.Get()), valid ? "pass" : "error");
}

namespace
{
    struct InfluenceTest
    {
        ComPtr<ID3D11Texture2D> original;
        std::vector<uint8_t> before, black;
        unsigned width = 0, height = 0;
        bool blackRead = false;
    };

    InfluenceTest& influence_test()
    {
        static InfluenceTest test;
        return test;
    }

    bool influence_enabled()
    {
        static const bool result = [] {
            wchar_t value[8] = {};
            const DWORD size = GetEnvironmentVariableW(L"OPTISCALER_DEMO_INFLUENCE_TEST", value, 8);
            if (size == 0 && GetLastError() == ERROR_ENVVAR_NOT_FOUND)
                return false;
            if (size != 1 || value[0] != L'1')
            {
                donut::log::error("Temporal diagnostics: OPTISCALER_DEMO_INFLUENCE_TEST must be 1");
                return false;
            }
            return true;
        }();
        return result;
    }

    bool read_ldr(ID3D11DeviceContext* context, ID3D11Resource* resource, std::vector<uint8_t>& pixels,
                  unsigned& width, unsigned& height)
    {
        ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = resource ? resource->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
        if (FAILED(hr) || !context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
        {
            logger().write("event=influence_error stage=ldr_resource hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        ComPtr<ID3D11Device> device, owner;
        context->GetDevice(&device);
        texture->GetDevice(&owner);
        if (!device || owner.Get() != device.Get() || desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM ||
            desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1)
        {
            logger().write("event=influence_error stage=ldr_desc format=%u size=%ux%u", static_cast<unsigned>(desc.Format), desc.Width, desc.Height);
            return false;
        }
        width = desc.Width; height = desc.Height;
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr))
        {
            logger().write("event=influence_error stage=ldr_staging hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr))
        {
            logger().write("event=influence_error stage=ldr_map hr=0x%08X", static_cast<unsigned>(hr));
            return false;
        }
        pixels.resize(static_cast<size_t>(width) * height * 4);
        for (unsigned y = 0; y < height; ++y)
            memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                   static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                   static_cast<size_t>(width) * 4);
        context->Unmap(staging.Get(), 0);
        return true;
    }
}

bool begin_tonemap_influence_test(nvrhi::IDevice* device, nvrhi::ITexture* source, nvrhi::ITexture* target)
{
    auto& log = logger();
    if (!influence_enabled() || log.frame != 120)
        return false;
    auto& test = influence_test();
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!read_ldr(context, native(target), test.before, test.width, test.height))
        return false;
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = native(source) ? native(source)->QueryInterface(IID_PPV_ARGS(&texture)) : E_POINTER;
    if (FAILED(hr) || !context)
    {
        log.write("event=influence_error stage=source hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    ComPtr<ID3D11Device> owner, contextDevice;
    texture->GetDevice(&owner); context->GetDevice(&contextDevice);
    if (!owner || owner.Get() != contextDevice.Get() || desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT ||
        desc.MipLevels != 1 || desc.ArraySize != 1 || desc.SampleDesc.Count != 1 ||
        !(desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS))
    {
        log.write("event=influence_error stage=source_desc format=%u bind=0x%X", static_cast<unsigned>(desc.Format), desc.BindFlags);
        return false;
    }
    ComPtr<ID3D11UnorderedAccessView> uav;
    hr = owner->CreateUnorderedAccessView(texture.Get(), nullptr, &uav);
    if (FAILED(hr))
    {
        log.write("event=influence_error stage=uav hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    desc.BindFlags = 0; desc.MiscFlags = 0;
    hr = owner->CreateTexture2D(&desc, nullptr, &test.original);
    if (FAILED(hr))
    {
        log.write("event=influence_error stage=backup hr=0x%08X", static_cast<unsigned>(hr));
        return false;
    }
    context->CopyResource(test.original.Get(), texture.Get());
    const float black[4] = { 0.f, 0.f, 0.f, 0.f };
    context->ClearUnorderedAccessViewFloat(uav.Get(), black);
    ColorStats cleared;
    if (!sample_color(context, texture.Get(), desc.Width, desc.Height, 0, 0, "influence_black", cleared) ||
        cleared.nonzero != 0 || cleared.nonfinite != 0 ||
        cleared.count != static_cast<uint64_t>(desc.Width) * desc.Height)
    {
        context->CopyResource(texture.Get(), test.original.Get());
        test.original.Reset();
        log.write("event=influence_error stage=black_control");
        return false;
    }
    log.write("event=influence_begin source=%p target=%p pixels=%llu black_rgb=%llu",
              static_cast<void*>(texture.Get()), static_cast<void*>(native(target)),
              static_cast<unsigned long long>(cleared.count), static_cast<unsigned long long>(cleared.nonzero));
    return true;
}

void restore_tonemap_influence_source(nvrhi::IDevice* device, nvrhi::ITexture* source, nvrhi::ITexture* target)
{
    auto& test = influence_test();
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    unsigned width = 0, height = 0;
    test.blackRead = read_ldr(context, native(target), test.black, width, height) &&
        width == test.width && height == test.height;
    if (!test.blackRead)
        logger().write("event=influence_error stage=black_target");
    context->CopyResource(native(source), test.original.Get());
    test.original.Reset();
}

void finish_tonemap_influence_test(nvrhi::IDevice* device, nvrhi::ITexture* target)
{
    auto& test = influence_test();
    auto& log = logger();
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    std::vector<uint8_t> restored;
    unsigned width = 0, height = 0;
    if (!test.blackRead || !read_ldr(context, native(target), restored, width, height) ||
        width != test.width || height != test.height || restored.size() != test.before.size() ||
        test.black.size() != test.before.size())
    {
        log.write("event=influence_error stage=restored_target");
        return;
    }
    uint64_t changed = 0, restorationMismatch = 0;
    for (size_t i = 0; i < restored.size(); i += 4)
    {
        if (test.before[i] != test.black[i] || test.before[i + 1] != test.black[i + 1] ||
            test.before[i + 2] != test.black[i + 2])
            ++changed;
        if (test.before[i] != restored[i] || test.before[i + 1] != restored[i + 1] ||
            test.before[i + 2] != restored[i + 2] || test.before[i + 3] != restored[i + 3])
            ++restorationMismatch;
    }
    const uint64_t total = static_cast<uint64_t>(width) * height;
    const bool valid = changed * 100 >= total && restorationMismatch == 0;
    log.write("event=influence_result target=%p size=%ux%u pixels=%llu changed_rgb=%llu restore_mismatch=%llu status=%s",
              static_cast<void*>(native(target)), width, height, static_cast<unsigned long long>(total),
              static_cast<unsigned long long>(changed), static_cast<unsigned long long>(restorationMismatch),
              valid ? "pass" : "error");
}

void verify_combined_diagnostics(nvrhi::IDevice* device, nvrhi::ITexture* source, nvrhi::ITexture* backBuffer)
{
    auto& log = logger();
    if (!influence_enabled() || !output_test_enabled() || log.frame != 120)
        return;
    ID3D11DeviceContext* context = device->getNativeObject(nvrhi::ObjectTypes::D3D11_DeviceContext);
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE)
    {
        log.write("event=combined_error stage=context");
        return;
    }
    ComPtr<ID3D11Device> nativeDevice;
    context->GetDevice(&nativeDevice);
    ComPtr<ID3D11ShaderResourceView> sourceView;
    ComPtr<ID3D11RenderTargetView> targetView;
    context->PSGetShaderResources(0, 1, &sourceView);
    context->OMGetRenderTargets(1, &targetView, nullptr);
    ComPtr<ID3D11Resource> boundSource, boundTarget;
    if (sourceView) sourceView->GetResource(&boundSource);
    if (targetView) targetView->GetResource(&boundTarget);
    std::vector<uint8_t> pixels;
    unsigned width = 0, height = 0;
    if (!nativeDevice || !native(source) || !native(backBuffer) ||
        boundSource.Get() != native(source) || boundTarget.Get() != native(backBuffer) ||
        !read_ldr(context, native(backBuffer), pixels, width, height))
    {
        log.write("event=combined_error stage=backbuffer_or_binding");
        return;
    }
    uint64_t nonblack = 0;
    for (size_t i = 0; i < pixels.size(); i += 4)
        if (pixels[i] || pixels[i + 1] || pixels[i + 2]) ++nonblack;
    log.write("event=combined_backbuffer source=%p ps_t0=%p target=%p om_rtv0=%p size=%ux%u pixels=%llu nonblack=%llu status=%s",
              static_cast<void*>(native(source)), static_cast<void*>(boundSource.Get()),
              static_cast<void*>(native(backBuffer)), static_cast<void*>(boundTarget.Get()), width, height,
              static_cast<unsigned long long>(static_cast<uint64_t>(width) * height),
              static_cast<unsigned long long>(nonblack), nonblack ? "pass" : "error");

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 16; desc.Height = 16; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = nativeDevice->CreateTexture2D(&desc, nullptr, &texture);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateDepthStencilView(texture.Get(), &dsvDesc, &dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc {};
    srvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateShaderResourceView(texture.Get(), &srvDesc, &srv);
    if (FAILED(hr))
    {
        log.write("event=combined_error stage=depth_views hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    ComPtr<ID3D11Resource> dsvResource, srvResource;
    dsv->GetResource(&dsvResource); srv->GetResource(&srvResource);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.25f, 73);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.75f, 0);
    Stats stats;
    const bool read = sample_texture(context, texture.Get(), true, 16, 16, 0, 0, "combined_depth", stats);
    bool valid = read && dsvResource.Get() == texture.Get() && srvResource.Get() == texture.Get() &&
        stats.count == 256 && stats.nonfinite == 0;
    for (unsigned i = 0; i < stats.count; ++i)
        valid = valid && std::fabs(stats.x[i] - 0.75f) < 0.000001f && stats.stencil[i] == 73;
    log.write("event=combined_depth format=d24s8 views=dsv,srv depth=0.75 stencil=73 samples=%u status=%s",
              stats.count, valid ? "pass" : "error");
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_STENCIL, 0.f, 19);
    Stats stencilStats;
    const bool stencilRead = sample_texture(context, texture.Get(), true, 16, 16, 0, 0, "combined_stencil", stencilStats);
    valid = stencilRead && stencilStats.count == 256 && stencilStats.nonfinite == 0;
    for (unsigned i = 0; i < stencilStats.count; ++i)
        valid = valid && std::fabs(stencilStats.x[i] - 0.75f) < 0.000001f && stencilStats.stencil[i] == 19;
    log.write("event=combined_stencil depth=0.75 stencil=19 samples=%u status=%s",
              stencilStats.count, valid ? "pass" : "error");
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    ComPtr<ID3D11Texture2D> depth32;
    hr = nativeDevice->CreateTexture2D(&desc, nullptr, &depth32);
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    ComPtr<ID3D11DepthStencilView> dsv32;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateDepthStencilView(depth32.Get(), &dsvDesc, &dsv32);
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    ComPtr<ID3D11ShaderResourceView> srv32;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateShaderResourceView(depth32.Get(), &srvDesc, &srv32);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage32;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateTexture2D(&desc, nullptr, &stage32);
    if (FAILED(hr))
    {
        log.write("event=combined_error stage=depth32_setup hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    dsvResource.Reset(); srvResource.Reset();
    dsv32->GetResource(&dsvResource); srv32->GetResource(&srvResource);
    context->ClearDepthStencilView(dsv32.Get(), D3D11_CLEAR_DEPTH, 0.125f, 0);
    context->CopyResource(stage32.Get(), depth32.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    hr = context->Map(stage32.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        log.write("event=combined_error stage=depth32_map hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    valid = dsvResource.Get() == depth32.Get() && srvResource.Get() == depth32.Get();
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
        {
            float value;
            memcpy(&value, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * sizeof(float), sizeof(value));
            valid = valid && value == 0.125f;
        }
    context->Unmap(stage32.Get(), 0);
    log.write("event=combined_depth format=d32 views=dsv,srv depth=0.125 samples=256 status=%s", valid ? "pass" : "error");

    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0; desc.Format = DXGI_FORMAT_R16_TYPELESS;
    ComPtr<ID3D11Texture2D> depth16;
    hr = nativeDevice->CreateTexture2D(&desc, nullptr, &depth16);
    dsvDesc.Format = DXGI_FORMAT_D16_UNORM;
    ComPtr<ID3D11DepthStencilView> dsv16;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateDepthStencilView(depth16.Get(), &dsvDesc, &dsv16);
    srvDesc.Format = DXGI_FORMAT_R16_UNORM;
    ComPtr<ID3D11ShaderResourceView> srv16;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateShaderResourceView(depth16.Get(), &srvDesc, &srv16);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> stage16;
    if (SUCCEEDED(hr)) hr = nativeDevice->CreateTexture2D(&desc, nullptr, &stage16);
    if (FAILED(hr))
    {
        log.write("event=combined_error stage=depth16_setup hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    dsvResource.Reset(); srvResource.Reset();
    dsv16->GetResource(&dsvResource); srv16->GetResource(&srvResource);
    context->ClearDepthStencilView(dsv16.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
    context->CopyResource(stage16.Get(), depth16.Get());
    hr = context->Map(stage16.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr))
    {
        log.write("event=combined_error stage=depth16_map hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    valid = dsvResource.Get() == depth16.Get() && srvResource.Get() == depth16.Get();
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
        {
            uint16_t value;
            memcpy(&value, static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * sizeof(value), sizeof(value));
            valid = valid && (value == 32767 || value == 32768);
        }
    context->Unmap(stage16.Get(), 0);
    log.write("event=combined_depth format=d16 views=dsv,srv depth=0.5 samples=256 status=%s", valid ? "pass" : "error");

    uint8_t values[2][4] {};
    hr = static_cast<HRESULT>(run_srgb_selftest(nativeDevice.Get(), values));
    if (FAILED(hr))
    {
        log.write("event=combined_error stage=srgb_draw hr=0x%08X", static_cast<unsigned>(hr));
        return;
    }
    valid = true;
    for (unsigned c = 0; c < 3; ++c)
        valid = valid && std::abs(static_cast<int>(values[0][c]) - 64) <= 1 &&
            std::abs(static_cast<int>(values[1][c]) - 137) <= 1;
    valid = valid && std::abs(static_cast<int>(values[0][3]) - 128) <= 1 &&
        std::abs(static_cast<int>(values[1][3]) - 128) <= 1;
    log.write("event=combined_srgb linear=%u,%u,%u,%u srgb=%u,%u,%u,%u status=%s",
              values[0][0], values[0][1], values[0][2], values[0][3],
              values[1][0], values[1][1], values[1][2], values[1][3], valid ? "pass" : "error");
}

void log_dlss_result(unsigned result)
{
    auto& log = logger();
    if (log.sampled)
        log.write("event=ngx_dx11_evaluate_result result=0x%08X", result);
}
}
#endif
