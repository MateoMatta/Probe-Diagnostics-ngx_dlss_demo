#include "IdentitySelfTest.h"

#if USE_DX11
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr unsigned size = 64;
    const char shader[] = R"(
cbuffer Test : register(b0) { uint reverseOrder; uint equalDepth; uint transparent; uint partialEdge; };
struct Vertex { float4 position : SV_Position; nointerpolation uint instance : INSTANCE; };
Vertex vs(uint vertex : SV_VertexID, uint inst : SV_InstanceID)
{
    uint id = reverseOrder != 0 ? 1 - inst : inst;
    float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1),
                          float2(0,1), float2(1,0), float2(1,1) };
    float2 p = corners[vertex];
    float left = id == 0 ? 4.0 : 16.0;
    float right = id == 0 ? 48.0 : 60.0;
    if (partialEdge != 0) { left += 0.5; right += 0.5; }
    Vertex o;
    o.position = float4(2.0 * lerp(left, right, p.x) / 64.0 - 1.0,
                        1.0 - 2.0 * p.y,
                        id == 0 || equalDepth != 0 ? 0.6 : 0.4, 1.0);
    o.instance = id;
    return o;
}
struct Output { float4 color : SV_Target0; uint2 id : SV_Target1; float4 contributors : SV_Target2; };
Output ps(Vertex i)
{
    if (i.instance == 1 && i.position.y < 32.0) discard;
    Output o;
    o.color = i.instance == 0 ? float4(1,0,0,1) : float4(0,1,0,1);
    if (transparent != 0) o.color.a = 0.5;
    o.id = uint2(i.instance + 1, 123);
    o.contributors = i.instance == 0 ? float4(1,0,0,0) : float4(0,1,0,0);
    return o;
}
Texture2DMS<uint2, 4> idsMS : register(t0);
RWStructuredBuffer<uint2> samples : register(u0);
[numthreads(8,8,1)]
void cs(uint3 thread : SV_DispatchThreadID)
{
    for (uint s = 0; s < 4; ++s)
        samples[(thread.y * 64 + thread.x) * 4 + s] = idsMS.Load(thread.xy, s);
}
)";

    struct Constants { uint32_t reverse, equal, transparent, padding; };
}

long run_srgb_selftest(ID3D11Device* sourceDevice, uint8_t values[2][4])
{
    if (!sourceDevice || !values) return static_cast<long>(E_POINTER);
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = sourceDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return static_cast<long>(hr);
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return static_cast<long>(hr);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                           D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) return static_cast<long>(hr);
    const char shader[] = R"(
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
    return float4(p[id], 0, 1);
}
float4 ps() : SV_Target { return float4(0.25, 0.25, 0.25, 0.5); }
)";
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    hr = D3DCompile(shader, sizeof(shader) - 1, "srgb-selftest", nullptr, nullptr, "vs", "vs_5_0", 0, 0, &vsCode, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer())); return static_cast<long>(hr); }
    errors.Reset();
    hr = D3DCompile(shader, sizeof(shader) - 1, "srgb-selftest", nullptr, nullptr, "ps", "ps_5_0", 0, 0, &psCode, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer())); return static_cast<long>(hr); }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    hr = device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    if (SUCCEEDED(hr)) hr = device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr)) return static_cast<long>(hr);
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = 1; desc.Height = 1; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> texture, staging;
    hr = device->CreateTexture2D(&desc, nullptr, &texture);
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc {};
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11RenderTargetView> linear, srgb;
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(texture.Get(), &rtvDesc, &linear);
    rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (SUCCEEDED(hr)) hr = device->CreateRenderTargetView(texture.Get(), &rtvDesc, &srgb);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, &staging);
    D3D11_RASTERIZER_DESC rasterDesc {};
    rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE; rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    if (SUCCEEDED(hr)) hr = device->CreateRasterizerState(&rasterDesc, &raster);
    if (FAILED(hr)) return static_cast<long>(hr);
    D3D11_VIEWPORT viewport { 0, 0, 1, 1, 0, 1 };
    context->RSSetState(raster.Get()); context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11RenderTargetView* views[] = { linear.Get(), srgb.Get() };
    for (unsigned i = 0; i < 2; ++i)
    {
        const float clear[4] = { 0, 0, 0, 0 };
        context->ClearRenderTargetView(views[i], clear);
        context->OMSetRenderTargets(1, &views[i], nullptr);
        context->Draw(3, 0);
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return static_cast<long>(hr);
        memcpy(values[i], mapped.pData, 4);
        context->Unmap(staging.Get(), 0);
    }
    return static_cast<long>(S_OK);
}

IdentityTestResult run_identity_selftest(ID3D11Device* sourceDevice)
{
    IdentityTestResult result;
    auto fail = [&](HRESULT hr) { result.error = static_cast<long>(hr); return result; };
    if (!sourceDevice) return fail(E_POINTER);
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = sourceDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) return fail(hr);
    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) return fail(hr);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 1,
                           D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) return fail(hr);
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    hr = D3DCompile(shader, sizeof(shader) - 1, "identity-selftest", nullptr, nullptr,
                    "vs", "vs_5_0", 0, 0, &vsCode, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer())); return fail(hr); }
    errors.Reset();
    hr = D3DCompile(shader, sizeof(shader) - 1, "identity-selftest", nullptr, nullptr,
                    "ps", "ps_5_0", 0, 0, &psCode, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer())); return fail(hr); }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    hr = device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs);
    if (FAILED(hr)) return fail(hr);
    hr = device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps);
    if (FAILED(hr)) return fail(hr);

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = size; desc.Height = size; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> color, ids, depth, colorRead, idRead, contributors, contributorsRead;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = device->CreateTexture2D(&desc, nullptr, &color); if (FAILED(hr)) return fail(hr);
    hr = device->CreateTexture2D(&desc, nullptr, &contributors); if (FAILED(hr)) return fail(hr);
    desc.Format = DXGI_FORMAT_R32G32_UINT;
    hr = device->CreateTexture2D(&desc, nullptr, &ids); if (FAILED(hr)) return fail(hr);
    desc.Format = DXGI_FORMAT_D32_FLOAT; desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = device->CreateTexture2D(&desc, nullptr, &depth); if (FAILED(hr)) return fail(hr);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = device->CreateTexture2D(&desc, nullptr, &colorRead); if (FAILED(hr)) return fail(hr);
    hr = device->CreateTexture2D(&desc, nullptr, &contributorsRead); if (FAILED(hr)) return fail(hr);
    desc.Format = DXGI_FORMAT_R32G32_UINT;
    hr = device->CreateTexture2D(&desc, nullptr, &idRead); if (FAILED(hr)) return fail(hr);
    ComPtr<ID3D11RenderTargetView> colorView, idView, contributorsView;
    ComPtr<ID3D11DepthStencilView> depthView;
    hr = device->CreateRenderTargetView(color.Get(), nullptr, &colorView); if (FAILED(hr)) return fail(hr);
    hr = device->CreateRenderTargetView(ids.Get(), nullptr, &idView); if (FAILED(hr)) return fail(hr);
    hr = device->CreateRenderTargetView(contributors.Get(), nullptr, &contributorsView); if (FAILED(hr)) return fail(hr);
    hr = device->CreateDepthStencilView(depth.Get(), nullptr, &depthView); if (FAILED(hr)) return fail(hr);
    D3D11_BUFFER_DESC bufferDesc {};
    bufferDesc.ByteWidth = sizeof(Constants); bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> constants;
    hr = device->CreateBuffer(&bufferDesc, nullptr, &constants); if (FAILED(hr)) return fail(hr);
    D3D11_RASTERIZER_DESC rasterDesc {};
    rasterDesc.FillMode = D3D11_FILL_SOLID; rasterDesc.CullMode = D3D11_CULL_NONE;
    rasterDesc.DepthClipEnable = TRUE;
    ComPtr<ID3D11RasterizerState> raster;
    hr = device->CreateRasterizerState(&rasterDesc, &raster); if (FAILED(hr)) return fail(hr);

    ID3D11RenderTargetView* views[] = { colorView.Get(), idView.Get() };
    ID3D11Buffer* cb = constants.Get();
    D3D11_VIEWPORT viewport { 0, 0, static_cast<float>(size), static_cast<float>(size), 0, 1 };
    context->OMSetRenderTargets(2, views, depthView.Get());
    context->RSSetState(raster.Get()); context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(ps.Get(), nullptr, 0);
    context->VSSetConstantBuffers(0, 1, &cb);
    context->PSSetConstantBuffers(0, 1, &cb);
    const float clear[4] = { 0, 0, 0, 0 };
    for (unsigned equal = 0; equal != 2; ++equal)
    for (unsigned lessEqual = 0; lessEqual != 2; ++lessEqual)
    for (unsigned reverse = 0; reverse != 2; ++reverse)
    {
        D3D11_DEPTH_STENCIL_DESC depthDesc {};
        depthDesc.DepthEnable = TRUE; depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDesc.DepthFunc = lessEqual ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_LESS;
        ComPtr<ID3D11DepthStencilState> state;
        hr = device->CreateDepthStencilState(&depthDesc, &state); if (FAILED(hr)) return fail(hr);
        context->OMSetDepthStencilState(state.Get(), 0);
        context->ClearRenderTargetView(colorView.Get(), clear);
        context->ClearRenderTargetView(idView.Get(), clear);
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        Constants data { reverse, equal, 0, 0 };
        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
        context->DrawInstanced(6, 2, 0, 0);
        context->CopyResource(colorRead.Get(), color.Get());
        context->CopyResource(idRead.Get(), ids.Get());
        D3D11_MAPPED_SUBRESOURCE colorMap {}, idMap {};
        hr = context->Map(colorRead.Get(), 0, D3D11_MAP_READ, 0, &colorMap); if (FAILED(hr)) return fail(hr);
        hr = context->Map(idRead.Get(), 0, D3D11_MAP_READ, 0, &idMap);
        if (FAILED(hr)) { context->Unmap(colorRead.Get(), 0); return fail(hr); }
        for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
        {
            const bool a = x >= 4 && x < 48;
            const bool b = x >= 16 && x < 60 && y >= 32;
            unsigned expected = a ? 1 : 0;
            if (b && (!a || !equal || (lessEqual ? !reverse : reverse))) expected = 2;
            uint32_t pixel[2];
            std::memcpy(pixel, static_cast<const uint8_t*>(idMap.pData) + y * idMap.RowPitch + x * 8, 8);
            const auto* rgba = static_cast<const uint8_t*>(colorMap.pData) + y * colorMap.RowPitch + x * 4;
            if (pixel[0] != expected || pixel[1] != (expected ? 123u : 0u) ||
                rgba[0] != (expected == 1 ? 255 : 0) || rgba[1] != (expected == 2 ? 255 : 0) ||
                rgba[2] != 0 || rgba[3] != (expected ? 255 : 0))
                ++result.mismatches;
            ++result.pixels;
        }
        context->Unmap(idRead.Get(), 0);
        context->Unmap(colorRead.Get(), 0);
        ++result.cases;
    }
    D3D11_BLEND_DESC blendDesc {};
    blendDesc.IndependentBlendEnable = TRUE;
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blendDesc.RenderTarget[2].BlendEnable = TRUE;
    blendDesc.RenderTarget[2].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[2].DestBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[2].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[2].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[2].DestBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[2].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[2].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;
    hr = device->CreateBlendState(&blendDesc, &blend); if (FAILED(hr)) return fail(hr);
    D3D11_DEPTH_STENCIL_DESC transparentDepth {};
    transparentDepth.DepthEnable = TRUE; transparentDepth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    transparentDepth.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ComPtr<ID3D11DepthStencilState> transparentState;
    hr = device->CreateDepthStencilState(&transparentDepth, &transparentState); if (FAILED(hr)) return fail(hr);
    ID3D11RenderTargetView* transparentViews[] = { colorView.Get(), idView.Get(), contributorsView.Get() };
    context->OMSetRenderTargets(3, transparentViews, depthView.Get());
    context->OMSetBlendState(blend.Get(), nullptr, 0xffffffffu);
    context->OMSetDepthStencilState(transparentState.Get(), 0);
    for (unsigned reverse = 0; reverse != 2; ++reverse)
    {
        context->ClearRenderTargetView(colorView.Get(), clear);
        context->ClearRenderTargetView(idView.Get(), clear);
        context->ClearRenderTargetView(contributorsView.Get(), clear);
        context->ClearDepthStencilView(depthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        Constants data { reverse, 0, 1, 0 };
        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
        context->DrawInstanced(6, 2, 0, 0);
        context->CopyResource(colorRead.Get(), color.Get());
        context->CopyResource(idRead.Get(), ids.Get());
        context->CopyResource(contributorsRead.Get(), contributors.Get());
        D3D11_MAPPED_SUBRESOURCE colorMap {}, idMap {}, contributorsMap {};
        hr = context->Map(colorRead.Get(), 0, D3D11_MAP_READ, 0, &colorMap); if (FAILED(hr)) return fail(hr);
        hr = context->Map(idRead.Get(), 0, D3D11_MAP_READ, 0, &idMap);
        if (FAILED(hr)) { context->Unmap(colorRead.Get(), 0); return fail(hr); }
        hr = context->Map(contributorsRead.Get(), 0, D3D11_MAP_READ, 0, &contributorsMap);
        if (FAILED(hr)) { context->Unmap(idRead.Get(), 0); context->Unmap(colorRead.Get(), 0); return fail(hr); }
        for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
        {
            const bool a = x >= 4 && x < 48;
            const bool b = x >= 16 && x < 60 && y >= 32;
            const unsigned last = a && b ? (reverse ? 1u : 2u) : (a ? 1u : (b ? 2u : 0u));
            const int expectedRed = a ? (b && !reverse ? 64 : 128) : 0;
            const int expectedGreen = b ? (a && reverse ? 64 : 128) : 0;
            uint32_t pixel[2];
            std::memcpy(pixel, static_cast<const uint8_t*>(idMap.pData) + y * idMap.RowPitch + x * 8, 8);
            const auto* rgba = static_cast<const uint8_t*>(colorMap.pData) + y * colorMap.RowPitch + x * 4;
            const auto* contributor = static_cast<const uint8_t*>(contributorsMap.pData) + y * contributorsMap.RowPitch + x * 4;
            const int redError = static_cast<int>(rgba[0]) - expectedRed;
            const int greenError = static_cast<int>(rgba[1]) - expectedGreen;
            if (pixel[0] != last || pixel[1] != (last ? 123u : 0u) ||
                redError < -1 || redError > 1 || greenError < -1 || greenError > 1 || rgba[2] != 0 ||
                contributor[0] != (a ? 255 : 0) || contributor[1] != (b ? 255 : 0) ||
                contributor[2] != 0 || contributor[3] != 0)
                ++result.mismatches;
            ++result.pixels;
        }
        context->Unmap(idRead.Get(), 0);
        context->Unmap(contributorsRead.Get(), 0);
        context->Unmap(colorRead.Get(), 0);
        ++result.cases;
    }
    const DXGI_FORMAT msFormats[] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32G32_UINT, DXGI_FORMAT_D32_FLOAT };
    for (DXGI_FORMAT format : msFormats)
    {
        UINT quality = 0;
        hr = device->CheckMultisampleQualityLevels(format, 4, &quality);
        if (FAILED(hr)) return fail(hr);
        if (quality == 0) return fail(DXGI_ERROR_UNSUPPORTED);
    }
    ComPtr<ID3DBlob> csCode;
    errors.Reset();
    hr = D3DCompile(shader, sizeof(shader) - 1, "identity-selftest", nullptr, nullptr,
                    "cs", "cs_5_0", 0, 0, &csCode, &errors);
    if (FAILED(hr)) { if (errors) OutputDebugStringA(static_cast<const char*>(errors->GetBufferPointer())); return fail(hr); }
    ComPtr<ID3D11ComputeShader> cs;
    hr = device->CreateComputeShader(csCode->GetBufferPointer(), csCode->GetBufferSize(), nullptr, &cs);
    if (FAILED(hr)) return fail(hr);
    D3D11_TEXTURE2D_DESC msDesc {};
    msDesc.Width = size; msDesc.Height = size; msDesc.MipLevels = 1; msDesc.ArraySize = 1;
    msDesc.SampleDesc.Count = 4; msDesc.SampleDesc.Quality = D3D11_STANDARD_MULTISAMPLE_PATTERN;
    msDesc.Usage = D3D11_USAGE_DEFAULT;
    msDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; msDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> msColor, msIds, msDepth;
    hr = device->CreateTexture2D(&msDesc, nullptr, &msColor); if (FAILED(hr)) return fail(hr);
    msDesc.Format = DXGI_FORMAT_R32G32_UINT;
    msDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = device->CreateTexture2D(&msDesc, nullptr, &msIds); if (FAILED(hr)) return fail(hr);
    msDesc.Format = DXGI_FORMAT_D32_FLOAT; msDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    hr = device->CreateTexture2D(&msDesc, nullptr, &msDepth); if (FAILED(hr)) return fail(hr);
    ComPtr<ID3D11RenderTargetView> msColorView, msIdView;
    ComPtr<ID3D11DepthStencilView> msDepthView;
    ComPtr<ID3D11ShaderResourceView> msIdResource;
    hr = device->CreateRenderTargetView(msColor.Get(), nullptr, &msColorView); if (FAILED(hr)) return fail(hr);
    hr = device->CreateRenderTargetView(msIds.Get(), nullptr, &msIdView); if (FAILED(hr)) return fail(hr);
    hr = device->CreateDepthStencilView(msDepth.Get(), nullptr, &msDepthView); if (FAILED(hr)) return fail(hr);
    hr = device->CreateShaderResourceView(msIds.Get(), nullptr, &msIdResource); if (FAILED(hr)) return fail(hr);
    D3D11_BUFFER_DESC samplesDesc {};
    samplesDesc.ByteWidth = size * size * 4 * 8;
    samplesDesc.Usage = D3D11_USAGE_DEFAULT; samplesDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    samplesDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; samplesDesc.StructureByteStride = 8;
    ComPtr<ID3D11Buffer> samples, samplesRead;
    hr = device->CreateBuffer(&samplesDesc, nullptr, &samples); if (FAILED(hr)) return fail(hr);
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc {};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN; uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = size * size * 4;
    ComPtr<ID3D11UnorderedAccessView> samplesView;
    hr = device->CreateUnorderedAccessView(samples.Get(), &uavDesc, &samplesView); if (FAILED(hr)) return fail(hr);
    samplesDesc.Usage = D3D11_USAGE_STAGING; samplesDesc.BindFlags = 0;
    samplesDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; samplesDesc.MiscFlags = 0;
    samplesDesc.StructureByteStride = 0;
    hr = device->CreateBuffer(&samplesDesc, nullptr, &samplesRead); if (FAILED(hr)) return fail(hr);
    D3D11_DEPTH_STENCIL_DESC msDepthDesc {};
    msDepthDesc.DepthEnable = TRUE; msDepthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    msDepthDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    ComPtr<ID3D11DepthStencilState> msState;
    hr = device->CreateDepthStencilState(&msDepthDesc, &msState); if (FAILED(hr)) return fail(hr);
    rasterDesc.MultisampleEnable = TRUE;
    ComPtr<ID3D11RasterizerState> msRaster;
    hr = device->CreateRasterizerState(&rasterDesc, &msRaster); if (FAILED(hr)) return fail(hr);
    ID3D11RenderTargetView* msViews[] = { msColorView.Get(), msIdView.Get() };
    ID3D11ShaderResourceView* srv = msIdResource.Get();
    ID3D11UnorderedAccessView* uav = samplesView.Get();
    for (unsigned partial = 0; partial != 2; ++partial)
    for (unsigned reverse = 0; reverse != 2; ++reverse)
    {
        context->OMSetRenderTargets(2, msViews, msDepthView.Get());
        context->OMSetDepthStencilState(msState.Get(), 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context->RSSetState(msRaster.Get());
        context->ClearRenderTargetView(msColorView.Get(), clear);
        context->ClearRenderTargetView(msIdView.Get(), clear);
        context->ClearDepthStencilView(msDepthView.Get(), D3D11_CLEAR_DEPTH, 1, 0);
        Constants data { reverse, 1, 0, partial };
        context->UpdateSubresource(constants.Get(), 0, nullptr, &data, 0, 0);
        context->DrawInstanced(6, 2, 0, 0);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CSSetShader(cs.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &srv);
        context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
        context->Dispatch(size / 8, size / 8, 1);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11UnorderedAccessView* nullUav = nullptr;
        context->CSSetShaderResources(0, 1, &nullSrv);
        context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        context->CopyResource(samplesRead.Get(), samples.Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        hr = context->Map(samplesRead.Get(), 0, D3D11_MAP_READ, 0, &mapped); if (FAILED(hr)) return fail(hr);
        const auto* pixels = static_cast<const uint32_t*>(mapped.pData);
        for (unsigned y = 0; y < size; ++y)
        for (unsigned x = 0; x < size; ++x)
        for (unsigned s = 0; s < 4; ++s)
        {
            const int sampleX[] = { -2, 6, -6, 2 };
            const int positionX = static_cast<int>(x * 16 + 8) + sampleX[s];
            const int offset = partial ? 8 : 0;
            const bool a = positionX >= 4 * 16 + offset && positionX < 48 * 16 + offset;
            const bool b = positionX >= 16 * 16 + offset && positionX < 60 * 16 + offset && y >= 32;
            const unsigned expected = a && b ? (reverse ? 1u : 2u) : (a ? 1u : (b ? 2u : 0u));
            const unsigned index = ((y * size + x) * 4 + s) * 2;
            if (pixels[index] != expected || pixels[index + 1] != (expected ? 123u : 0u))
                ++result.mismatches;
            ++result.pixels;
        }
        context->Unmap(samplesRead.Get(), 0);
        ++result.cases;
    }
    return result;
}
#endif
