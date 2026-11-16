# Building Gallian Chronicles

Windows 10 or 11, 64-bit, with a Vulkan-capable GPU. Everything builds with
MinGW (GCC) and Ninja; Visual Studio is not needed.

## 1. Tools

- **WinLibs MinGW** (GCC, CMake, Ninja): `winget install BrechtSanders.WinLibs.POSIX.UCRT`.
  Its `bin` folder must be on `PATH`.
- **Python 3** (the renderer's shaders are built with it): `winget install Python.Python.3.12`
- **Vulkan SDK 1.4.363.1**, installed or extracted to `work\vulkan-sdk\1.4.363.1`.
- **Git**.

## 2. Third-party sources

- SDL3 3.4.18 (MinGW development package) into
  `psprecomp-upstream\third_party\SDL3-3.4.18-mingw\` (see `dependencies\sdl3.lock`).
- For VC2, PortableKit at the pinned revision with the project's patches:

  ```bat
  git clone https://github.com/TeamGDB/PortableKit.git psprecomp-upstream\third_party\PortableKit
  git -C psprecomp-upstream\third_party\PortableKit checkout --detach 33800c4060c53f3233e08f8ab2cc4590a0872063
  for %f in (psprecomp-upstream\dependencies\portablekit-patches\*.patch) do git -C psprecomp-upstream\third_party\PortableKit apply "%~ff"
  ```

  VC3 builds against its own vendored copy in `VC3\portablekit`.
- FFmpeg and libsodium are downloaded and hash-checked by the builds.

## 3. Your game's code

The ports are compiled from your own copy of each game; nothing generated is in
the repository.

- **VC2:** set up your disc image with the game's
  `--install <image.iso> --in-place`, then generate the C++ corpus with
  `psprecomp-upstream\profiles\vc2\scripts\generate_aot.ps1` into
  `profiles\vc2\generated\`.
- **VC3:** extract and prepare the executable with `VC3\scripts\extract-eboot.ps1`
  and `prepare-elf.ps1`, then `VC3\scripts\generate-aot.ps1` into `VC3\generated\`.

## 4. Build

From the repository's root, with `SDL3` set to
`psprecomp-upstream\third_party\SDL3-3.4.18-mingw\SDL3-3.4.18\x86_64-w64-mingw32`
and `VULKAN_SDK` to the Vulkan SDK's folder:

```bat
set OPTS=-G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTABLEKIT_RENDERER=ON -DPORTABLEKIT_FFMPEG=bundled ^
  -DPSPRECOMP_BUILD_TESTS=OFF -DPSPRECOMP_GENERATED_OPT_LEVEL=2 -DPSPRECOMP_GENERATED_JOBS=12 ^
  -DPSPRECOMP_AOT_PRODUCTION_FASTPATHS=ON -DSDL3_DIR=%SDL3%\lib\cmake\SDL3 -DVulkan_ROOT=%VULKAN_SDK%

rem Valkyria Chronicles II (and its friend-code tool)
cmake -S psprecomp-upstream\profiles\vc2 -B psprecomp-upstream\out\vc2-release %OPTS%
cmake --build psprecomp-upstream\out\vc2-release --target VC2Native VC2OnlineCode
copy %SDL3%\bin\SDL3.dll psprecomp-upstream\out\vc2-release\bin\

rem Valkyria Chronicles 3
cmake -S VC3 -B VC3\out\vc3-release %OPTS% -DCMAKE_RC_COMPILER=windres
cmake --build VC3\out\vc3-release --target VC3Native
copy %SDL3%\bin\SDL3.dll VC3\out\vc3-release\bin\

```

The games are `bin\Valkyria Chronicles II.exe` and `bin\Valkyria Chronicles 3.exe`
in their build folders. The first build
of each game compiles a few hundred generated units (10 to 20 minutes on a
16-thread PC); later builds only rebuild what changed. On PCs with less than
32 GB of memory, lower `PSPRECOMP_GENERATED_JOBS`.

## 5. Play

Run the game you built: the first time, it asks for your `.iso` and sets
itself up from it. Keep the image where it is: the game reads it while you
play. Or put your builds in a release's folder (from the Releases page), in
place of its game executables, and use its launcher.
