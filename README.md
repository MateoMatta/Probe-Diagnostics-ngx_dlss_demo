# DLSS Probe Diagnostics & ReShade Integration Probe (`ngx_dlss_demo` Fork)

> **NON-COMMERCIAL RESEARCH NOTICE:**
>
> This repository (`MateoMatta/DLSS-Probe-Diagnostics-ngx_dlss_demo`) is a non-commercial, personal, and research-oriented diagnostic fork of NVIDIA's `ngx_dlss_demo` sample application. It incorporates components and APIs from [ReShade](https://github.com/crosire/reshade) (licensed under the BSD 3-Clause license) and the NVIDIA RTX SDK. This repository is intended strictly for personal research, internal testing, and sharing among friends to probe graphical inputs, temporal reprojection behavior, and API hooking interactions across DirectX 11, DirectX 12, and Vulkan. **This project is strictly non-profit and is not sold, monetized, or commercially distributed.**

## Overview & Project Goal

The primary goal of this repository is to analyze, instrument, and evaluate graphical inputs required by modern upscaling algorithms (such as NVIDIA DLSS, ReShade add-ons, and OptiScaler translation layers).

By extending NVIDIA's original sample app and pairing it with custom diagnostic instrumentation and ReShade API hooks, this project allows developers to:

1. Verify motion vector conventions, depth buffer accuracy, and stencil marking for static and moving geometry.

2. Probe resource lifecycles (creation, destruction, generation tracking) across resolution resizes.

3. Inspect DirectX 11 compute shader descriptor bindings (SRV/UAV/Samplers/CBVs) and evaluate immediate context completion timing.

4. Execute automated, reproducible diagnostic test suites via PowerShell harnesses (`dx11-diagnostic-harness.ps1`).

## Third-Party Licenses & Disclaimer

This project builds upon public sample code and open-source libraries:

* **NVIDIA RTX SDK / NGX DLSS Sample:** Copyright (c) NVIDIA Corporation. Governed by the NVIDIA RTX SDKs License (see `LICENSE.txt`).

* **ReShade API & SDK:** Copyright (c) 2014, Patrick Mours / Crosire. Governed by the BSD 3-Clause License (included in `LICENSE.txt`).

All third-party trademarks, SDKs, and libraries remain the exclusive property of their respective legal owners.

## Windows Setup & Build

**Prerequisites:**

* Windows 10 RS5 or higher

* Visual Studio 2017 (v141) or newer

* CMake 3.12+

* Python 3.7+ (64-bit)

**Pre-built Executables:**
Both `ngx_dlss_demo.exe` and `ngx_dlss_demo_diagnostics.exe` are already available pre-built inside the `bin\ngx_dlss_demo` directory.

To run the pre-built binaries directly:
1. Open a command prompt or PowerShell inside `bin\ngx_dlss_demo`.
2. Run either executable:
   ```cmd
   ngx_dlss_demo.exe -d3d11
   ```
   *OR*
   ```cmd
   ngx_dlss_demo_diagnostics.exe -d3d11
   ```
   *(Use `-d3d11`, `-d3d12`, or `-vulkan` to switch graphics backends).*

**Steps to Build from Source:**

1. Install CMake (https://cmake.org/).

2. Open CMake-GUI, set the source location to the root of this repo, and select a build output directory.

3. Ensure a **64-bit** compiler target is selected.

4. Click **Configure**.

5. Set `NGX_SDK_ROOT` to point to your local root directory of the NVIDIA NGX SDK.

6. Click **Configure** again, then **Generate**, and open the generated solution in Visual Studio.

7. Set `Donut DLSS Demo/ngx_dlss_demo` as the startup project and build.

## Diagnostic Suite & Harness Usage

This repository includes a PowerShell automation harness: `dx11-diagnostic-harness.ps1`.

### Harness Execution Example:

```powershell
$demo = (Get-Location).Path
.\dx11-diagnostic-harness.ps1 `
    -Executable (Join-Path $demo 'bin\ngx_dlss_demo\ngx_dlss_demo_diagnostics.exe') `
    -MediaPath (Join-Path $demo 'media') `
    -OutputPath (Join-Path $demo 'diagnostic-results\run-001')
```

Run this example from the repository root. The output path must not already exist; choose a new path for each run. The executable directory must also contain the ReShade `dxgi.dll`, the diagnostic `OptiScaler.addon64`, and `OptiScaler.ini`. The harness enables `TemporalInputs` tracing for every case and restores its process environment afterward. You can also pass `ngx_dlss_demo.exe` to `-Executable`.

The default suite covers 17 DX11 scenarios, including resolution changes with TemporalAA and DLSS, two additional real swapchains on the same device, late add-on registration, resource-generation correlation, and multiple outstanding GPU event queries. The probe records separate swapchain-buffer observation epochs between ReShade swapchain callbacks and correlates them with sampled bindings and the three resize stages. These epochs are not actual resource creation/destruction times or GPU synchronization. Resource observations without an observed creation event remain **unknown** and are not assigned an inferred resource generation. The tests do not establish complete lifetime coverage, synchronization across processes, a universal per-pixel scene identity, or a functioning OptiScaler backend.

### Key Diagnostic Flags & Modes:

* **`-aamode <dlss|temporal|none>`**: Sets the temporal anti-aliasing / upscaling mode.

* **`-diagnostic-camera`**: Moves the camera systematically to test reprojection.

* **`-diagnostic-objects`**: Animates Sponza mesh instances independently to test object motion vectors.

* **`-diagnostic-object-ids`**: Renders custom stencil masks for tracked objects to isolate motion vectors.

* **`-diagnostic-resize`**: Triggers runtime resolution changes (e.g., `640x360` $\rightarrow$ `800x450` $\rightarrow$ `640x360`) to test resource creation/destruction QPCs.

* **`-stress-fps`**: Raises the internal frame-pacing limit from 240 to 300 FPS for stress testing. Pass `-StressFps` to the harness to test that mode.

### Targeted Harness Options:

You can run individual test cases in isolation using flags like `-ShaderBindingsOnly`, `-OverlapOnly`, `-IdentityOnly`, `-DxgiAdapterOnly`, `-ResizeOnly`, `-ResizeDlssOnly`, `-GpuPipelineOnly`, `-MultiSwapOnly`, `-LateAddonOnly`, `-CameraOnly`, `-ColorOnly`, or `-ExposureOnly`. The latter two select the same DLSS case: it reads the full active region of the RGBA16F color input, checks that RGB is not entirely black and all channels are finite, and correlates the resource and region with the NGX parameters. A 4x4 synthetic texture verifies all four half-float channels and a nonzero region offset. It also reads the 1x1 R16F exposure input, verifies a synthetic 0.5 control, and correlates the exposure resource with NGX. These diagnostic readbacks have a GPU synchronization cost; they do not prove color-space, exposure semantics, or backend compatibility. The multi-swapchain test creates two hidden swapchains on the demo device, resizes one while the other remains live, and checks their observed epochs separately. It does not present them or validate multiple devices.

The late-registration test creates a real DX11 render-target texture, then manually registers an isolated second copy of the probe under a distinct test-only name with the running ReShade instance. It binds that texture and a second texture created after registration. The harness requires the first probe to observe the first creation, the late probe to report that texture as unknown without a fabricated creation, and the post-registration control as live. This opt-in test exercises manual late registration on the demo device; it is not evidence that ReShade automatically loads add-ons late in arbitrary games.

`-OutputOnly` runs the DLSS camera case with an intrusive output test. Before each of eight sampled evaluations it clears the entire RGBA16F output to a sentinel and reads it back as a negative control; after a successful NGX evaluation it checks that every output pixel changed from the sentinel and that values are finite and RGB is not entirely black. It also checks that the tone-map draw leaves the same output bound at pixel-shader slot 0 and the expected target at render-target slot 0. The harness correlates resources, frames, and CPU event order. This changes the output and synchronously reads it back, so it is not a performance test. Bindings and a draw do not alone prove pixel-level influence on the displayed image, image quality, or a functioning OptiScaler backend.

`-InfluenceOnly` runs a separate intrusive DLSS case at frame 120. It saves the DLSS output and its tone-mapped RGBA8 target, clears the output to black and verifies the clear, renders tone mapping again without recomputing exposure, then restores the source and renders a third time. It requires at least 1% of target pixels to change in RGB and exact restoration of the original target. This checks causal influence on the demo's tone-mapped texture, not the presented back buffer or general image quality. Synchronous readbacks and extra draws make it unsuitable for performance measurements.

`-CombinedOnly` combines the input color, exposure, output sentinel and tone-map influence controls in the same DLSS run. At frame 120 it checks PS/RTV bindings and reads the swapchain back buffer before presentation. It tests independent 16x16 typeless D24S8, D32 and D16 textures with DSV/SRV views and known clear values; the D24S8 test verifies depth-only and stencil-only clears preserve the other component. On a separate DX11 device using the same adapter, a shader writes known linear values to UNORM and sRGB render-target views and checks the resulting RGB and alpha bytes. This checks output-merger sRGB encoding, not sRGB sampling or the demo's display color pipeline. FP16 controls include a value above 1, but do not validate HDR display, pre-exposure, or other backends. These are bounded tests, not exhaustive state or format coverage, and synchronous readbacks affect performance.

The GPU pipeline test polls each event query immediately with `D3D11_ASYNC_GETDATA_DONOTFLUSH` and records `not_ready` or `complete`, separately from the count of unretired queries. `not_ready` means the result was not available at that poll; it does not prove that the GPU has started executing it or that frames overlap on the GPU. The final bounded drain flushes and checks completion.

## Linux Support

**Build Steps:**

1. Update packman dependencies:

   ```bash
   cd donut/packman
   sh update-dependencies.sh
   ```

2. Build via CMake:

   ```bash
   cd ../..
   mkdir build && cd build
   cmake -DNGX_SDK_ROOT=<PATH_TO_NGX_SDK> ..
   make -j
   ```

**Run:**

```bash
export DISPLAY=:0
./ngx_dlss_demo/ngx_dlss_demo -vulkan -w 1920 -h 1080
```