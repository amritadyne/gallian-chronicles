# Gallian Chronicles

Native Windows ports of **Valkyria Chronicles II** and **Valkyria Chronicles 3**
(PSP). Releases come with a launcher, **Gallian Chronicles**, that sets both up and runs them.

The games are not emulated. Each one's PSP executable is statically recompiled
to C++ ([PSPRecomp](https://github.com/jessicanataliagta/PSPRecomp) and
[PortableKit](https://github.com/TeamGDB/PortableKit), both MIT), on the
player's own machine, from the player's own copy of the game. It is then run on
a native host: Vulkan rendering, SDL3 input and audio, and PortableKit's PSP
kernel services.

**No game is included.** This repository holds no game executable, game data,
disc image, generated code, game text or game artwork. You need your own copy:

| Game | What the launcher accepts |
|---|---|
| Valkyria Chronicles II | An uncompressed `.iso` of the Europe (ULES-01417), North America or Japan release |
| Valkyria Chronicles 3 | An uncompressed `.iso` of *Senjou no Valkyria 3 Extra Edition* (Japan, ULJM-05957; UMD or PlayStation Store), or of the *Valkyria Chronicles 3 ReTranslation Extra Edition* (v1.0.8) made from it |

Each disc image is checked by its hash before anything is set up.

## What the ports add

- Any window shape and resolution, with the 3D view widened (Hor+) and the
  interface kept in place; pictures can be zoomed to fill the window.
- Higher frame rates than the PSP's 30/60, with game timing kept correct.
- Modern keyboard-and-mouse and controller controls. Button prompts and
  glyphs, in menus and in text, follow the device in use (PlayStation, Xbox,
  keyboard and mouse).
- An in-game Settings screen added to the games' own menus.
- Texture packs (PPSSPP layout) and sharper text and symbols.
- Rewritable game text: the games load text override files
  (`vc2-text-overrides.json`, `vc3-text-overrides.json`) placed beside them.
- Discord status, VC2 online play (friend codes), and portable packages.

## Layout

```text
psprecomp-upstream/         The VC2 port (this project began as a PSPRecomp fork)
  profiles/vc2/             Valkyria Chronicles II: host code, build, scripts
  dependencies/             Pinned third-party sources (locks) and PortableKit patches
VC3/                        Valkyria Chronicles 3: host code, build, scripts
  portablekit/              PortableKit (MIT), vendored with VC3's changes
docs/                       Building, and what must never be committed
```

## Building

Windows 10/11 x64. See [docs/BUILDING.md](docs/BUILDING.md). In short:
install WinLibs MinGW, the Vulkan SDK, SDL3 and Python, generate each game's
C++ from your own executable, then build each with CMake and Ninja.

## Legal

Valkyria Chronicles is a trademark of SEGA. This project is not affiliated
with or endorsed by SEGA or Media.Vision. It contains no copyrighted game
material; see [docs/GAME_FILES.md](docs/GAME_FILES.md) for what stays out and
why. Code here is MIT licensed ([LICENSE](LICENSE)); third-party components
keep their own licences ([THIRD_PARTY.md](THIRD_PARTY.md)).
