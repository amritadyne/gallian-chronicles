# Game files: what never goes in this repository

Everything below comes from, or is made from, a copy of the game. It stays on
the player's PC, and the root `.gitignore` excludes it.

| Never committed | Why |
|---|---|
| Disc images (`*.iso`, `*.cso`), `EBOOT.BIN`, `EBOOT.ELF`, `PSP_GAME/` | The games themselves |
| `*/generated/`, `*/generated-variants/`, `*/overlays/` | C++ translated from the game's executable |
| `*/game/`, `*/analysis/`, `*/progress/`, `*/captures/`, `logs/`, `editions/` | Installed data, saves, dumps, captures and logs made from the game |
| `*-text-overrides*.json`, `*-text-index*.tsv`, `*-script-originals*.json` | The games' text and rewrites of it |
| DLC (`*.EDAT`, `*.PBP`) | Downloaded game content |
| Texture dumps and texture packs | Game artwork (packs are installed by the player) |
| `profiles/vc2/resources/vc2native.ico`; `VC3/resources/vc3.ico` | The games' artwork. The games build without them (no icon) |

Facts about the games are fine: addresses, hashes of the accepted disc
images, message ids, texture hashes, and descriptions of how the game works.
Reverse-engineering notes must not quote game text or include dumped data.
