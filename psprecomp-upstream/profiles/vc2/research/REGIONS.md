# Regions: what differs between the releases (2026-11-07)

The port is to support all three releases. Every game address in the profile
is written as the European release's and translated for the running edition
by `vc2::at()` (`host/vc2_region.hpp`). Before a replacement goes in at a
translated function address, `vc2::code_matches()` checks the code there
against a fingerprint of the European function (FNV-1a of its first eight
instructions, address fields masked); a mismatch leaves the game's own code in
place and is logged.

## Europe (ULES01417) — the reference

- Encrypted EBOOT.BIN `98b4c162…`, executable `5bc43ae8…`, tag `0xD91613F0`.

## US (ULUS10515)

Read by mounting each image read-only (listing
every file with size and SHA-256, copying PARAM.SFO, UMD_DATA.BIN and the
encrypted EBOOT.BIN to `work/disc-info/`).

- PARAM.SFO: DISC_ID `ULUS10515`, parental level 5 (EU 7); otherwise the same
  (APP_VER 01.00, system 6.20, same title).
- Disc files: 46 on each disc, all identical except PARAM.SFO, UMD_DATA.BIN,
  SYSDIR/BOOT.BIN, SYSDIR/EBOOT.BIN and USRDIR/ODIN.CPK (same size,
  456,855,968 bytes, different content; not yet compared inside).
- Encrypted EBOOT.BIN `7ce669b3…` (6,840,592 bytes), same tag `0xD91613F0`, so
  the profile's key decrypts it. Executable `f016e748…` (6,840,252 bytes).
- Executable against Europe's: `.text` is 0x10 bytes longer. The only change
  in behaviour is in one function: two added calls to the function at ELF
  0x2F8298, with arguments 0 and 1, at ELF 0x2296E4 and in a new branch after
  0x2297C4. The other differences are register renaming in that function, two
  alignment words removed at ELF 0x465D78, and relocated jump targets.
- Address translation, European address → US address:

  | European range (runtime) | shift |
  |---|---|
  | below 0x08A2D6E4 | 0 |
  | 0x08A2D6E4 – 0x08A2D7CC | +0x08 |
  | 0x08A2D7CC – 0x08C69D78 | +0x18 |
  | 0x08C69D78 – 0x08D36A00 (rest of .text, stubs, .rodata, .data) | +0x10 |
  | 0x08D36A00 and above (.bss, heap, VRAM) | 0 |

  Checked for all 57 function addresses the profile used on 2026-11-07: the
  first 16 instructions match with address fields masked. The GU library
  (0x08C86000–0x08C89300) moves by +0x10; the GU state block (.bss) does not.
- PortableKit: the US release is a `ProfileVariant` with its own disc id
  (patch 0010, editions from other discs). Its corpus is built from
  `generated-variants/us/` into `corpora/us.dll`; without it, the US
  executable runs under the interpreter. US saves are `ULUS10515DATA`.

### US: the port's features (2026-11-12)

Everything Europe has, at the US addresses: high frame rate, loading screens,
morale gauge, Modern controls and camera, aim, LOD, text rewrites and button
glyphs, Enable Extras, OPTIONS without Installed Data, the title's SETTINGS,
tutorial pictures, Discord. The checks that said "Europe only" now say "not
Japan".

- Host hooks: every European address goes through `vc2::at()`; the code
  checks (extras, text) compare only words that relocation leaves alone, and
  were checked against the US executable (the differing words are the masked
  ones).
- Generated-code sites (`cmake/vc2_step_sites.cmake`): `VC2_EDITION=us`
  patches `generated-variants/us` at the shifted labels, units and return
  addresses (`vc2_edition_hex`, `vc2_edition_unit`, `vc2_edition_text`); the
  calls pass the European site address, which the profile's tables use. All
  34 step sites and every hook site were found in the US corpus (checked on a
  copy, 2026-11-12; running it again changes nothing).
- The US corpus is a library that needs nothing from the executable, so the
  hook names it calls are forwarders (`host/vc2_corpus_shim.cpp`, built into
  `corpora/us.dll`) through a table (`host/vc2_corpus_hooks.hpp`) that
  `vc2::bind_corpus_hooks()` (`host/vc2_corpus_bind.cpp`) hands it when the
  image is loaded. Log: `[vc2-region] hooks handed to us.dll`. Before that,
  and in a library built without the shim, they do what the original
  instructions did.
- Not checked yet: whether the US text matches Europe's (the rewrites are
  keyed by a hash of the original text; a different text is left as it is),
  and the texture pack for US (its folder is chosen per data folder).

## Japan (NPJH50145)

Read 2026-11-07 (the disc listing, and BOOT.BIN compared with EU).

- PARAM.SFO: DISC_ID `NPJH50145`, DISC_VERSION 1.01, system 6.10,
  title 戦場のヴァルキュリア２ ガリア王立士官学校.
- Disc files: the same 46 names as Europe's. Different: the executable, nearly
  every movie (MV00–MV30 except a few), `OD_VOICE.AFS` (+39 MB, Japanese
  voices), `ODIN.CPK`, the icons and backgrounds, OPNSSMP.BIN and the system
  update.
- `SYSDIR/BOOT.BIN` is zero-filled on every release (EU and JP checked), so it
  cannot be used to check a decryption.
- Encrypted EBOOT.BIN `5334d22f…` (6,832,656 bytes), tag `0xD91612F0`: a
  different key from Europe's and the US release's. The published 16-byte key
  for that tag (supplied 2026-11-07) decrypts it to a valid PSP executable
  (type 0xFFA0, 61 sections, module `odin_psp`): executable `58238143…`,
  6,832,316 bytes, `.text` 0x4AB2F4 (Europe 0x4AC9E0).
- A separate build, not a patch of Europe's: addresses do not follow one
  shift table. Every one of the 57 function addresses the profile uses was
  found in the JP code by its first 16 instructions (address fields masked),
  each exactly once after checking the five near-identical small GU functions
  at the library's shift:
  - the GU library (0x08C86000–0x08C89300, all 51 entry points, the state
    switch at 0x08C880F8 and the stall update at 0x08C89118): −0x15F0;
  - CRI's notifier return (0x08C3B910): −0x15E8.
- Data: the data segment starts at ELF 0x52C018 (Europe 0x52D548), and the
  GU and CRI globals sit 0x30 further into it, so both move by −0x1500:
  GU state block 0x08D599C0 (Europe 0x08D5AEC0), context pointer 0x08D59A08,
  CRI error globals from 0x08D550A4 (Europe 0x08D565A4).
- The sceNetAdhocMatching import stub (CRI trace's callback): ELF 0x4AB784
  (Europe 0x4ACE80, a stub section start; the profile's 0x4ACEC0 is inside it).
- Not yet located: the startup CPK buffer (heap, diagnostic only) and the
  frame-list buffers 0x09BDF500/0x09C1F4C0 (heap, diagnostic only).

Support (2026-11-07): edition `jp` in `vc2_profile.cpp`; explicit table of
65 addresses in `vc2_region.cpp` (57 functions, 9 data/stub/limit entries);
PortableKit patch 0011 gives the edition its own tag and key and a Japanese
console (language 0, circle confirms). Data and saves in
`game/portablekit-data-jp`; corpus `generated-variants/jp` → `corpora/jp.dll`.
Set up and played from the launcher. The startup CPK buffer is
not in the table (diagnostic only; reads nothing on JP).

What JP needed beyond the US work:
- An explicit address table, not ranges: `vc2::at()` gains per-edition
  symbol entries, with anything not in JP's table reported unknown so its hook
  stays off.
- Per-edition decryption tag and key (an extension of patch 0010).
- Per-edition console settings: system language 0 (Japanese) and confirm
  button 0 (circle) are what a Japanese console reports; both are profile-wide
  today (`GameProfile::system_language`, `confirm_button`).
- Saves: the game's own save folder name, to be read from the executable.
- Text: Japanese text and fonts are the game's own; to be checked in play.

### Japan: the port's features (2026-11-15)

The address table is now complete: every European address the profile's host
code and its corpus patches use (about 240), made by aligning the two
executables:

- The two executables are aligned: every 12-instruction stretch that occurs
  exactly once in each, address fields masked (the fields their relocations
  touch, j/jal targets), pairs its instructions. 1,104,477 of Europe's
  1,225,336 instructions pair up.
- Code addresses come from that alignment; three step sites sit in repeated
  code but inside a stretch that moved as one (same shift on both sides), and
  one function (EU 0x08AFA0D4, an EXTRAS step, rewritten in Japan's build) from
  the 14 aligned calls that reach it (JP 0x08AF8FC4).
- Data addresses come from the relocation targets of the paired instructions
  (23,801 targets agree); an address inside a structure takes the shift of the
  nearest known one below it (the .bss moved by -0x1500 throughout).
- The earlier hand-made entries (GU library, CRI, the matching stub) agree.

`host/vc2_region.cpp` kJapan holds all of them; `cmake/vc2_jp_addresses.cmake`
the patches' share. With `VC2_EDITION=jp`, `cmake/vc2_step_sites.cmake` patches
`generated-variants/jp` (checked on a copy of its units: all 34 step sites, the
LOD sites, the camera, aim, preview, title, fine aim, movement, morale and
glyph sites; a second run changes nothing). The glyph end site is in the next
unit in Japan's corpus, so each glyph site now finds its own unit. jp.dll gets
the hook forwarders (`host/vc2_corpus_shim.cpp`) and
`bind_corpus_hooks()` hands them to it.

Now on in Japan too: high frame rate and the step fixes, the morale gauge, the
right stick and mouse camera, aim, Modern controls, LOD, the game's volumes,
Enable Extras and OPTIONS without Installed Data, Discord's activity, the tab
key badges, the glyph trace.

Japan-specific:
- Japanese consoles confirm with Circle: with the keyboard and mouse, the keys'
  Cross and Circle trade places in Japan (left click and Space confirm, Esc
  goes back; `VC2_JP_SWAP_CONFIRM=0` keeps the European keys). Gamepads keep
  the console's layout (B / Circle confirms).

Still Europe and US only:
- Text rewrites (English).
- The title menu's SETTINGS in place of INSTALL, and the Options screen's
  Settings entry: both are drawn from the English menu's own letters and
  found by the word "Options"; Japan's menus are in Japanese.
