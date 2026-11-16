# VC2 engine map — native rendering phase 0 (2026-11-06)

Static analysis of the supported European `EBOOT.ELF` (SHA-256 `5bc43ae8…`, load base `0x08804000`, segment 1 at ELF `0x52d548`). Addresses are runtime addresses unless marked ELF. This records facts about the executable's structure; it contains no game code.

## Layering

| Layer | Evidence | Where |
| --- | --- | --- |
| Game (Odin): battle (`Slg…`), objects/visuals (`Vl…`, e.g. `VlCanvas`), menus, scripts | class/state names in strings, `Odin_Main` | spread across `0x08804000–0x08B6xxxx` |
| Miyako Framework (app loop, sequences) | "Miyako Framework", "Framework ready." referenced from `0x08804400` | low text |
| Kurofune Graphics library 3.30 (`Kf…`) | version string, `KfVSyncSema` referenced from `0x08BEBD9C`, `KfInitPrint`, render-texture / camera errors | `0x08B70000–0x08BFFFFF` |
| GU-style GE library (statically linked; "SceGuSignal" name present) | every `sceGe_user` import is called only from here | `0x08C86000–0x08C89300` (135 functions) |
| PSP services (HLE) | imports | — |

## GE/display service callers

- `sceGeListEnQueue` from `0x08C86620`, `0x08C8675C`, `0x08C86B68`; `EnQueueHead` from `0x08C86B68`; `UpdateStallAddr` only from `0x08C89118`; `DrawSync`/`ListSync`/callbacks from `0x08C86620` and neighbours. All inside the GU layer.
- `sceDisplaySetFrameBuf` from `0x08C86C0C`, `0x08C86CE8`, `0x08C8729C` (GU layer). `WaitVblankStart` from Kurofune (`0x08BB876C`, `0x08BB8D54`) and `0x08BFEFC8/…FE4/…F05C`; `WaitVblankStartCB` from `0x08BB8CDC`; `GetFrameBuf` from `0x08BB8B98` (framebuffer capture path).

## GU layer state

- State block: ELF seg1 + `0x29978` → `0x08D5AEC0`. `+0x44` = enabled-state bitmask, `+0x48` = current context pointer. Context `+8` = display-list write pointer.
- 69 functions outside the GU layer call into it, all in `0x08B70000–0x08BFFFFF` (Kurofune). The interface Kurofune uses is about 75 entry points. Most used:

| Entry | Calls | Identified as |
| --- | --- | --- |
| `0x08C874D8` | 52 | enable/disable render state (bit index, on/off; dispatches through a 39-entry switch table at ELF `0x4e576c`) |
| `0x08C87688` | 22 | draw array (prim, vtype, count, indices, vertices) then update stall address — immediate kick |
| `0x08C8714C` | 17 | allocate memory inside the display list (BASE + JUMP over an inline block) |
| `0x08C87C00` | 13 | texture offset (cmds 0x4A/0x4B) |
| `0x08C87C44`, `0x08C87B80` | 8, 3 | texture flush |
| `0x08C87D9C` / `0x08C87DFC` / `0x08C87E20` / `0x08C87E9C` / `0x08C87ED0` | 7/7/5/3/2 | alpha test / depth mask / pixel masks / stencil test / stencil op |
| `0x08C8759C` | 5 | vertex type + vertex/index address setup |
| `0x08C88304` (via `0x08C8675C`) | — | world/view/projection/texture matrix upload |
| `0x08C87380`, `0x08C873D0` | 4, 1 | framebuffer / depth-buffer pointer |

## Direct GE writers outside the libraries (unconfirmed)

A pattern scan for stored GE command words also flags functions in game code (e.g. `0x0898B69C` with 28 callers, `0x0882299C`, `0x089FE984`). The scan has false positives (integer constants), so whether game code writes GE lists itself, or 3D models carry prebuilt display lists that are CALLed, is not established. The draw-provenance trace settles it.

## Timing and readback hints

- `SysGetFrameMilliSec` exists by name, and motion sync logs a fractional motion frame ("…Motion Frame %.3f"): candidate time sources for true HFR.
- "Frame buffer capture failed", render-texture setup errors and `GetFrameBuf` usage: the game captures framebuffers, so the native renderer must provide readback.
- Camera: `SlgActionCamera`, `VlMxSlgCameraInfo`, "Camera creation failed." / camera-motion data — candidates for the camera/projection owner.

## Tooling added

`VC2_FRAME_DUMP` now attributes each primitive to the last observed guest call before the list writer reached it (`| by=<function> ra=<return address>`), and ends each dump with a primitives-per-caller table. Use a Debug build; the optimised build compiles these hooks out.

## Draw provenance results (capture pass 2, 2026-11-06 evening)

Five F9 captures (title, menu, story, battle map, combat), each sampling the list writer over two frames. Each frame is a single list of 29–296 primitives. Every primitive was attributed, and the drawing in all five scenes comes from **five call sites**:

| Draw site (caller of GU) | GU call | Scenes | What it draws |
| --- | --- | --- | --- |
| `0x08BB1ECC` (Kurofune; 6 static callers) | `0x08C8759C` draw-array setup | all | Textured triangles and sprites in **transformed** (non-through) mode: UI, menus, backdrops, story art and in-world billboards all go through the matrix pipeline. CLUT4/CLUT8/8888 textures. 448 of the 695 primitives captured. |
| `0x08BEE048` (Kurofune; no static callers, reached through a function pointer) | `0x08C8759C` | combat | 3D meshes: 142 batches, 35 k vertices in one frame; vertex types include skinning weights (`0x01C5A2`), so this covers skinned characters. |
| `0x08C87688` ← `0x08B7AD54` (22 call sites in one function) | `0x08C8856C` draw-and-kick | combat | Through-mode (screen-space) textured sprites, 2 vertices each: HUD/text-style overlay. |
| `0x08BF9BA4` ← `0x08BF9C74` | `0x08C8759C` | combat | Untextured stencil volumes: the unit shadows. |
| `0x08C87718` (GU layer; called from Kurofune frame start `0x08BB876C` and others) | `0x08C88708` | all | 4–6 untextured through-mode sprites per frame: clears/fills. |

Findings:
- **No CALL or JUMP in any captured list.** Models are not prebuilt display lists; everything is drawn from vertex arrays in RAM. The native renderer can take vertex data directly from these call sites, with no list decoding at all.
- **UI is drawn through the projection pipeline**, not in screen coordinates. Aspect handling for UI is therefore a projection/anchoring question at `0x08BB1ECC`'s callers, not a pixel-coordinate rewrite.
- The replacement surface for drawing is very small: four Kurofune draw sites plus the clear. The state they rely on (textures, blending, tests, matrices) is set through the GU entry points listed above, so the next step is to map which GU state calls precede each draw site.

## Draw sites, camera and screen setup (static analysis, 2026-11-06 evening)

**Mesh draw `0x08BEE048`** (one submesh per call). Arguments: `a1` = submesh descriptor, `a2` = matrix palette (64-byte matrices). Descriptor: `+0` u16 bone-reference count, `+2` u16 skinned-bone count, `+6` u16 vertex count, `+10` u16 flags (bit 0 = face winding), `+12` pointer to bone-reference pairs (u16 palette index, u16 bone slot), `+24` u32 first vertex. The Kurofune context (pointer at ELF seg0 `0x4f8ca8`, relocated) holds the vertex type (`+8`), vertex base (`+12`), vertex stride (`+16`) and a skinning flag (`+1144`). Skinned: each bone matrix is uploaded through GU `0x08C88F5C`; rigid: the model matrix is set through `0x08BB9248` (GU set-matrix slot 2). Then one draw-array of triangles.

**Shadow draw `0x08BF9BA4`**: same shape (model matrix, then draw-array), called only from `0x08BF9C74`.

**2D/UI draw `0x08BB1ECC`**: walks a draw packet (`+4` material slot index into a table, `+28` item list), sets its own projection through `0x08BB922C` (GU set-matrix slot 0), texture offset/scale, render states, then draw-array per item. Callers: the 2D/overlay pass `0x08B7AD54` and five others (`0x08BAB0F0`, `0x08BCBCD4`, `0x08BD0000`, `0x08BF40E4`, `0x08BF8140`).

**GU set-matrix `0x08C87570`** (slot, matrix): slot 0 projection, 1 view, 2 model, 3 texture. Slot 0 is set only by `0x08BB922C`, which is called from the 2D/UI draw and from **`0x08BEA7CC`, Kurofune's camera apply**: it computes the projection, viewport and screen offset from the camera's viewport rectangle. This is where wide-aspect projection belongs. Its callers are `0x08BAC2B0` (from the overlay pass and two others) and `0x08BE9508`.

**Per-frame screen setup `0x08BB8820`**: draw/display/depth buffers, offset (2048−240, 2048−136), viewport (2048, 2048, 480, 272), depth range, scissor (0, 0, 480, 272), default states and matrices 1–3. Called from display setup `0x08BB8D54` (VRAM allocation, 480×272 constants) and `0x08BB97E0`. Every hard-coded PSP screen size lives here and in the camera apply.

**Timing.** Vblank waits: Kurofune `0x08BB876C`, `0x08BB8D54`, `0x08BEBC54` (with callbacks; `KfVSyncSema` at `0x08BEBD9C`) and `0x08BFEFC8` area. `sceKernelGetSystemTimeWide` is used heavily by `0x08B501A0–0x08B52FCC` and `0x08BC3328`. The per-mode update rate is measured at run time with `VC2_TRACE_TIMING`.

## Phase 1 plumbing

- A guest function is replaced with `Runtime::register_function` from the profile's `patch_loaded_image` hook (which runs after the corpus is registered). The registration disables the direct-chaining fast route into that 16 KiB unit; chained calls then fall back to exact-address lookup and reach the replacement. A replacement ends by setting `ctx.pc` (to `$ra`, or to a guest function to tail-call).
- First replacement: GU draw-array `0x08C8759C`, switch `VC2_NATIVE_GU=drawArray` (comma list, or `all`). It writes the same GE words as the original and tail-calls the stall update; a log line every 600 flips reports how many calls it handled. Tested with the optimised build and `VC2_TRACE_TIMING`.
- Implication for the plan: every draw passes through a small set of GU calls with plain arguments (draw-array, set-matrix, bone-matrix, texture and state setters). A native renderer can implement those calls directly and stop writing GE lists altogether, with the camera apply and screen setup above them handling resolution and aspect.

## GU entry-point catalogue (2026-11-06 night)

Kurofune calls 75 GU entry points. The library follows the familiar sceGu design, so most are identified by the GE commands they write. Verified by disassembly and implemented on the host (`host/vc2_native_gu.cpp`, identical GE output plus a host-side state mirror):

| Entry | Name | Writes |
| --- | --- | --- |
| `0x08C87D9C` | alphaFunc(func, ref, mask) | `DB` func \| ref<<8 \| mask<<16 (ref, mask & 0xFF) |
| `0x08C87DD8` | depthFunc(func) | `DE` |
| `0x08C87DFC` | depthMask(disable) | `E7` |
| `0x08C87E20` | pixelMask(mask) | `E8` low 24 bits, `E9` high 8 |
| `0x08C87E9C` | stencilFunc(func, ref, mask) | `DC` |
| `0x08C87ED0` | stencilOp(fail, zfail, zpass) | `DD` |
| `0x08C87F04` | blendFunc(op, src, dst, fixA, fixB) | `DF` op<<8 \| dst<<4 \| src, `E0`, `E1` |
| `0x08C87A40` | texFilter(min, mag) | `C6` |
| `0x08C87AAC` | texWrap(u, v) | `C7` |
| `0x08C87C00` | texOffset(f12, f13) | `4A`, `4B` (float >> 8) |
| `0x08C87074` | texScale(f12, f13) | `48`, `49` |
| `0x08C87838` | offset(x, y) | `4C` x<<4, `4D` y<<4 |
| `0x08C87C44` | texFlush() | `CB` |
| `0x08C87C90` | clutMode(fmt, shift, mask, start) | `C5` |
| `0x08C87CCC` | clutLoad(blocks, addr) | `B1`, `B0`, `C4` |
| `0x08C87874` | ambient(color) | `5C`, `5D` |
| `0x08C87570` | setMatrix(slot, m) | slot 0 `3E` + 16×`3F`; 1 `3C` + 12×`3D`; 2 `3A` + 12×`3B`; 3 `40` + 12×`41` |
| `0x08C87F5C` | boneMatrix(i, m) | `FF`, `2A` i×12, 12×`2B` |
| `0x08C8759C` | drawArray(prim, vtype, count, idx, verts) | `12`, `10`/`02`, `10`/`01`, `04`; tail-calls stall update |

**Batch 2 (all remaining render-state entry points, implemented the same way):** enable `0x08C87424`, disable `0x08C8747C`, setState `0x08C874D8` (through the 22-state switch at `0x08C880F8`: 0 `22` alpha test, 1 `23` depth test, 2 scissor (rectangle from context +172..+184, or the full area from +160/+164 when off; flag at +168), 3 `24`, 4 `21`, 5 `1D`, 6 `20`, 7 `1F`, 8 `1C`, 9 `1E`, 10 `17`, 11–14 `18`–`1B`, 15 `25`, 16 `26`, 17 `27`, 18 `28`, 19 `51`, 20 `38`, 21 fragment 2x (rewrites `C9` from context +228/+232/+236); mask at state block +0x44); texImage `0x08C87B80` (`A8+l`, `A0+l`, `B8+l` log2 via clz, `CB`); texMode `0x08C87A04` (`C2`, `C3`, `CB`); texMapMode `0x08C87B30` (`C0` with projection mode from context +240, `C1`); texFunc `0x08C87A6C` (`C9`); texLevelMode `0x08C87AD8` (`C8`, bias ×16 clamped); texSlope `0x08C87C64` (`D0`); fog `0x08C87D18` (`CF`, `CD`, `CE` = 1/(far−near) or 0); scissor `0x08C87E60`; morphWeight `0x08C88000` (`FF`, `2C+i`); lightColor `0x08C879D0` (`8F/90/91+3l`); light `0x08C8796C` (`63–65+3l`, `5F+l`); lightMode `0x08C879A8` (`5E`); color `0x08C878B4` (`55`, `58`, `56`, `57`); modelColor `0x08C878E0` (`54`, `56`, `55`, `58`, `57`); colorMaterial `0x08C87020` (`53`); frontFace `0x08C8791C` (`9B`, inverted); shadeModel `0x08C87944` (`50`); dither `0x08C86EC8` (`E2`–`E5`); drawBuffer `0x08C87380` (`D2`, `9D`, `9C`); depthBuffer `0x08C873D0` (`9F`, `9E`, state block +52/+56); viewport `0x08C87740` (`42` w×0.5, `43` h×−0.5, `45`, `46`); depthRange `0x08C877E4` / depthOffset `0x08C87810` (`44`, `47`, `D6`, `D7` from context +216/+220/+224); clearColor/Depth/Stencil `0x08C876E4`/`F8`/`0x08C87708` (context +204/+208/+212); HUD blit offsets `0x08C8766C` (context +188..+200).

Left as game code on purpose: clear `0x08C87718` (format-dependent strip clear), Kurofune's HUD blitter `0x08C87688` (inline two-vertex sprites with flip/rotation), list allocation `0x08C8714C`, getters, and the list/frame control family (start, finish, sync, enqueue, callbacks, display swap). Copying these exactly gains nothing: the native path renders clears and sprites directly and replaces list building while keeping the game's synchronisation.

## Next

1. ~~drawArray replacement~~ (verified). Run the 19-entry batch (`VC2_NATIVE_GU=all`): identical output expected.
2. Replace the remaining GU entry points (enable/disable, texture image and mode, buffers, clear, immediate draw, list control) the same way, so the host mirror holds the complete state at every draw.
3. Native submission: draw-array hands vertices, indices, matrices and state to the Vulkan renderer directly; the GE list stays empty for converted draws.
4. Camera apply `0x08BEA7CC` and screen setup `0x08BB8820` for output resolution and aspect.
