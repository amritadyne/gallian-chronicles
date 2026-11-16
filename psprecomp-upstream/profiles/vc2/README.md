# Valkyria Chronicles II (native)

The Windows port of **Valkyria Chronicles II** (PSP): the game's executable,
translated ahead of time to C++ by PortableKit's generator, run on PortableKit's
host (Vulkan rendering, SDL3 input and audio, PSP kernel services), with this
profile's own code in `host/`.

## Supported releases

| Release | Disc | Prepared `EBOOT.ELF` SHA-256 | Code |
|---|---|---|---|
| Europe | ULES-01417 | `5bc43ae81b3a35f2fe4a72180e7130598f3b856d7fb849ec7cdc49646187cefe` | built into the executable |
| North America | ULUS-10515 | `f016e74871eec117954aba4e42738741c5d743d0bb6cfcc51f89714c1c0944af` | `corpora/us.dll` (`generated-variants/us`) |
| Japan | NPJH-50145 | `582381436a2afadf94b4b210dc83ae338782da746102612f85fd4b2fc03f09a3` | `corpora/jp.dll` (`generated-variants/jp`) |

Load base `0x08804000`, 32 MiB of guest memory. The installer
(`--install <image.iso> --in-place`) checks the disc's executable against these
hashes before anything is set up.

## Layout

- `host/` - the port: rendering on the host (native GU), wide screens and
  zoom, frame-rate independence, controls and prompts, text rewrites, the
  in-game Settings screen, EXTRAS, Discord status and online play.
- `cmake/` - patches applied to the generated code at build time (step, LOD and
  camera sites; the Japanese release's addresses) and libsodium.
- `config/` - the profile's NIDs and identity.
- `scripts/generate_aot.ps1` - generates `generated/` from your prepared ELF.
- `tools/vc2_online_code.cpp` - the friend-code tool for online play.
- `research/` - the engine map and the differences between releases.
- `NATIVE_RENDERING_PLAN.md`, `SYSTEM_REQUIREMENTS.md`.

`generated/`, `generated-variants/`, `game/` and everything else made from the
game stay on your PC (see the repository's `docs/GAME_FILES.md`). Building:
the repository's `docs/BUILDING.md`.
