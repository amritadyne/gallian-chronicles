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

## Audio

### Language audio (Valkyria Chronicles II)

Play one release with another release's voices, for example Japanese voices on the European release. In the launcher: Audio > **Voice disc image**, choose the other release's `.iso`, then turn **Replace language audio** on. Spoken lines and the movies' sound come from that disc; the picture, text and anything that disc lacks stay the release's own. Keep that `.iso` where it is: the game reads it while you play.

### Soundtrack replacement

The ports can replace each game's background music with the official soundtrack albums, in higher quality. No music is included: you supply your own copies of the albums. Any format FFmpeg reads works (FLAC, MP3, M4A, ...); file names and numbering don't matter. Tracks you don't have keep the game's own music.

In the launcher: Audio > **Soundtrack** > *Import from a folder…*, and choose the folder with your album files (subfolders are searched too, so one folder can hold several albums). The import takes a few minutes. Each track is lined up with the game's own and level-matched, and the result is kept in the game's data folder (`soundtrack`); your files are only read. **On** / **Off** switch it, and F6 in game switches between the album and the game's own music to compare. The report, `soundtrack.txt` in that folder, lists what was found.

### Soundtrack replacement: requirements

Disc-track numbers below follow the albums' own numbering.

#### Valkyria Chronicles II

Every in-game track comes from one album (37 tracks). *Graduation* and the title music are shortened edits in the game; the launcher rebuilds them from the album tracks.

**Valkyria Chronicles 2 Original Soundtrack** (37 tracks)

- 1-01 Valkyria Chronicles 2 Main Theme
- 1-02 Exciting Battle
- 1-03 Winter in Lanseal
- 1-04 Everyday Life of The Class
- 1-05 Practice Battle
- 1-06 Royal Military Academy
- 1-08 Briefing
- 1-09 Liberation Battle
- 1-10 Counter Battle
- 1-11 Reward
- 1-13 Spring in Lanseal
- 1-14 Are We A Spoiled Class
- 1-15 Outside Battle
- 1-16 War Preparedness
- 1-17 Snowfield Battle
- 1-18 Theme of The Rebels
- 1-19 Summer in Lanseal
- 1-20 Street Battle
- 1-21 Defense Battle
- 1-22 Conflict
- 1-23 Desert Battle
- 1-24 Decisive Battle
- 1-25 Defeat
- 2-02 Days With Classmates
- 2-03 Laevatein Cup Finals
- 2-04 Theme of Class G
- 2-06 Autumn in Lanseal
- 2-07 Close Battle
- 2-08 Tear at The War
- 2-09 Mine Battle
- 2-10 Faint Youth
- 2-11 Offensive and Defensive Battle
- 2-12 Theme of Valkyria
- 2-14 Ship Battle
- 2-15 Gassenarl's Ambition
- 2-16 Final Decisive Battle
- 2-17 Graduation

#### Valkyria Chronicles 3

VC3 reuses music from all three games, so it draws on three albums. *Valkyria Chronicles 3 Main Theme* (1-01) and *Nameless Heroes* (2-12) are also used, in a different part of the game's audio that is not replaced yet.

**Valkyria Chronicles 3 Original Soundtrack** (29 tracks)

- 1-02 Fierce Battlefield
- 1-03 In Search Of The Next Battle
- 1-04 Counter Offence!
- 1-05 The Looming Sound of Ammunition Boots
- 1-06 We Are 'The Nameless'
- 1-07 Daily Lives Of 'The Nameless'
- 1-08 Signs Of Unrest
- 1-09 Swirling Conspiracy
- 1-10 Fear And Guts
- 1-11 Seesawing
- 1-12 Calamity Raven
- 1-13 Critical Moment!
- 1-14 A Momentary Rest
- 1-15 Damn It!
- 1-16 Are the Customers Pigeons
- 1-17 Dark Hero
- 1-18 That Is 'The Nameless'
- 1-19 Connecting Hearts
- 2-01 Valkyria Chronicles 3 Main Theme (Short Ver.)
- 2-02 Riela Marceris
- 2-03 As A Squad
- 2-04 'The Nameless', Takeoff!
- 2-05 With Pride In Hearts
- 2-06 OPEN FIRE!
- 2-07 Risking Everything On This Stroke
- 2-08 Must-win Battle
- 2-09 With You
- 2-10 Steady Preparedness
- 2-11 Final Decisive Battle

**Valkyria Chronicles 2 Original Soundtrack** (23 tracks)

- 1-04 Everyday Life of The Class
- 1-05 Practice Battle
- 1-06 Royal Military Academy
- 1-08 Briefing
- 1-09 Liberation Battle
- 1-11 Reward
- 1-14 Are We A Spoiled Class
- 1-15 Outside Battle
- 1-16 War Preparedness
- 1-17 Snowfield Battle
- 1-18 Theme of The Rebels
- 1-22 Conflict
- 1-23 Desert Battle
- 1-25 Defeat
- 2-02 Days With Classmates
- 2-03 Laevatein Cup Finals
- 2-04 Theme of Class G
- 2-07 Close Battle
- 2-08 Tear at The War
- 2-10 Faint Youth
- 2-11 Offensive and Defensive Battle
- 2-12 Theme of Valkyria
- 2-14 Ship Battle

**Valkyria Chronicles Original Soundtrack** (20 tracks)

- 1-04 Gallant Fight
- 1-05 Randgrith City
- 1-06 Everyday Training
- 1-08 Hope for Tomorrow
- 1-12 Strategy Instructions
- 1-13 Urgent Instructions
- 1-14 Hard Fight
- 1-17 The Gallian War
- 1-20 War in the Empire
- 1-24 Valkyria's Omen
- 2-01 Empty Loneliness
- 2-04 A Moment of Relief
- 2-05 People of Darcus
- 2-06 7th Platoon of the Volunteer Army
- 2-09 Anxiety
- 2-11 Recollection of That Day
- 2-12 The Limits of the Quarrel
- 2-14 Composition of the Confrontation
- 2-16 Quiet Chat
- 2-18 Chris Draws Near

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
