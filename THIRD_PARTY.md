# Third-party components

| Component | Licence | Where | How it arrives |
|---|---|---|---|
| PSPRecomp | MIT | `psprecomp-upstream/LICENSE` | This project began as a fork of it; its framework is not used |
| PortableKit | MIT | `VC3/portablekit/` (its `LICENSE`); VC2 uses a checkout in `psprecomp-upstream/third_party/PortableKit` | VC3: vendored here. VC2: cloned at the revision in `psprecomp-upstream/dependencies/portablekit.lock`, then `dependencies/portablekit-patches` applied |
| SDL3 3.4.18 (MinGW) | zlib | `psprecomp-upstream/third_party/SDL3-3.4.18-mingw/` | Downloaded; see `dependencies/sdl3.lock` |
| FFmpeg 7.1 (shared, LGPL build) | LGPL 2.1+ | beside the built games | Downloaded by the build; see `dependencies/ffmpeg.lock` (the games ship `FFmpeg-LICENSE.txt` and `FFmpeg-SOURCE.txt`) |
| libsodium 1.0.22 | ISC | `psprecomp-upstream/third_party/libsodium-1.0.22-mingw/` | Downloaded by VC2's build; see `dependencies/libsodium.lock` |
| Dear ImGui | MIT | inside PortableKit | With PortableKit |
| Vulkan SDK 1.4.363.1 | Apache 2.0 / various | `work/vulkan-sdk/` | Installed by the developer, not redistributed |
| GCC runtime (libgcc, libstdc++, winpthread) | GPL with the GCC Runtime Library Exception | beside the built executables | Copied from the WinLibs MinGW used to build |

Nothing in this table is game material. Game material never enters the
repository: see `docs/GAME_FILES.md`.
