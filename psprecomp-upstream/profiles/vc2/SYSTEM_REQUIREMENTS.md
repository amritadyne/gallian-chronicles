# System requirements (minimum spec = build of 2026-11-07)

The build of 2026-11-07 (all three releases, native GU and native draws
available, original frame rates) defines the minimum spec. Later work —
native resolution and aspect, true HFR, texture packs — adds recommended tiers
above it and must not raise this floor.

## Minimum

| | Minimum | Basis |
|---|---|---|
| OS | Windows 10/11, 64-bit | x86-64 build; MinGW UCRT runtime, part of Windows 10 and later |
| CPU | x86-64 with SSE2, 4 threads; about Intel Core i3-6100 / AMD Ryzen 3 1200 | No AVX or `-march` flags in the build. Game logic on one main thread; renderer, audio and video decoding beside it. **Model estimate, not yet measured** |
| GPU | Vulkan 1.1 + `VK_KHR_swapchain`; no other required features; about Intel HD 520, AMD GCN 1.0, NVIDIA GTX 650 | `apiVersion = VK_API_VERSION_1_1`; `robustBufferAccess` used only when offered. **Performance not yet measured** |
| VRAM | 1 GB | 108 MiB vertex buffer; small render targets at 2×; PSP-sized textures (texture packs raise this) |
| RAM | 4 GB system, ~400 MB free | 124 MB executable, 112 MB per US/JP corpus, 32 MiB guest memory, decoders, caches |
| Storage | ~1.5 GB for one release (200 MB program, 1.1 GB disc image used in place, data folder), +112 MB per further release | `out/vc2-release/bin`, data folders |
| Display | 960×544 window (2×) | default `video.window_scale` |
| Input | Keyboard and mouse, or an SDL-supported gamepad | |

## Performance target at minimum

The game's own rates, steady: 60 fps in menus, story scenes and the battle map,
30 fps in 3D combat and movies, at 2× internal resolution with VSync on.

## Evidence

- Measured: Vulkan version and features (renderer source), instruction set
  (build flags), file sizes, and the rates above on the development PC (AMD
  Radeon RX 7700 XT/7800 XT class, device 0x747E, Vulkan 1.4).
- To measure: the heaviest 3D battle on a low-end machine (ideally an Intel
  HD 520/620-class laptop) with `VC2_TRACE_TIMING=1` and the FPS counter. If
  it holds 30 fps, the CPU/GPU estimates stand.
