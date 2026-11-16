# Valkyria Chronicles 3 (native)

The Windows port of **Valkyria Chronicles 3 Extra Edition** (PSP, Japan,
ULJM-05957), on the same host as the VC2 port: its executable translated ahead
of time to C++ by PortableKit's generator, with the VC2 port's code adapted in
`host/` (addresses mapped from VC2's by `host/vc3_region.cpp`). The English
ReTranslation v1.0.8 runs on the same code with its own executable and disc
image.

## Accepted executables

- Extra Edition `EBOOT.BIN` SHA-256: `3879146a4c4a9f31fa0435edd6c53c94e5e9e55bb838be269246cfd35e95dcbf`
- ReTranslation v1.0.8 executable SHA-256: `57f9af0e1718a40354d9d43ef16b0dcfb035596d697a4fdb3a5bb8c06a55d1ac`

## Generating the code

```powershell
.\scripts\configure-tools.ps1   # builds PortableKit's generator
.\scripts\extract-eboot.ps1     # takes EBOOT.BIN from your disc image
.\scripts\prepare-elf.ps1       # decrypts it to analysis\EBOOT.ELF
.\scripts\generate-aot.ps1      # writes generated\
```

Then build as in the repository's `docs/BUILDING.md`.

## Layout

- `host/` - the port's own code; `cmake/vc3_sites.cmake` patches the generated
  code at build time.
- `portablekit/` - PortableKit (MIT) at the revision in `PORTABLEKIT.lock`,
  with this port's changes.
- `tools/vc3_identity_probe.cpp` - a check of a disc's identity (not built by
  default).

`analysis/`, `generated/`, `out/` and everything else made from the game stay
on your PC.
