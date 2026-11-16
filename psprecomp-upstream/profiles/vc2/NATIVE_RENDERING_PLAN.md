# VC2 native rendering plan (draft, 2026-11-06)

Status: draft, revised 2026-11-06 after the user set the purpose (true high frame rate with no interpolation, any resolution, any aspect; fix the game where needed). Nothing in this plan is implemented yet.

## Goal

At present VC2 draws by building PSP GE display lists, and PortableKit emulates the GE to replay those lists through Vulkan. The goal is to remove that emulation. VC2's own drawing code should go straight to a native Vulkan renderer, and the release build should contain no GE command decoding or replay.

**Purpose:** high frame rates, arbitrary resolutions and arbitrary aspect ratios. Matching the PSP output pixel for pixel is not a goal. Where the game breaks under these improvements (frame-locked timing, UI that assumes 480×272, culling that assumes the PSP view), **the game gets fixed**, with patches in the VC2 profile, and the improvement stays.

**Done means:**
- A full set of play-route checkpoints (title, menus, story scenes, movies, battle map, 3D combat, results, saves) runs with **zero GE primitives submitted**.
- This holds with `PSPRECOMP_NO_INTERPRETER=1` and with the GE decoder compiled out.
- These run at any window size and aspect ratio (16:9, 16:10, 21:9, 32:9, 4:3) with correct HUD placement and no stretching.
- **True high frame rate:** the game simulates at the display rate (120/144/240 Hz and uncapped). Every displayed frame is a real game update: no interpolated or generated frames. Gameplay speed, timings and audio sync stay the same as at the original rate.
- The art style survives scaling: outlines, hatching and paper overlays stay crisp at high resolution, not blurred up from 480×272.

## Where we start

```
VC2 game code ─► engine graphics library (Kurofune 3.30) ─► GE display lists in guest RAM
   ─► sceGeListEnQueue / UpdateStallAddr (HLE) ─► PortableKit GE decoder ─► Vulkan replay ─► present
```

Facts already established:
- The recompiler can replace a guest function with host code. It uses `Runtime::register_function()` for full replacement and `register_native_fast_path()` for leaf functions.
- **Limitation:** a call to a function in the same generated unit compiles to a direct jump. Such calls skip the replacement, so the generator must be told which addresses may be replaced (see Phase 1).
- The F9 frame dump records the GE state of every primitive in a frame. VC2 uses through-mode 2D sprites for its UI, UV-mapped 3D, and two-pass stencil shadow volumes. It also blends 16-px line textures, whose look is unconfirmed.
- Frames are paced by `sceDisplaySetFrameBuf` once per vblank. The Kurofune version string is in the executable. The app's Render step is at vtable +72 (ELF `0x52bb10`).

## Where to cut over

| Option | What gets replaced | Verdict |
| --- | --- | --- |
| A. GE-command level | Nothing structural. This is today's emulator with a better backend. | Rejected: it is still emulation. |
| **B. Engine API level** | Kurofune's draw, material, texture, matrix and render-target entry points become host functions that call a native renderer. | **Recommended.** The interface is small and stable, and game logic stays as it is. |
| C. Scene level | Whole renderers (battle map, combat view, menus) rewritten from game data structures. | Needed in places. Wide aspect ratios need the camera and culling, which sit above the draw calls. |

Option B is the usual route for engine-library games. It also lets the switch happen **one category at a time**, with the GE path still drawing everything not yet converted. The PC goals mean B alone isn't enough: the native renderer must also own the **camera and projection**. So Phase 0 also maps the engine's camera and culling, not just its draw calls.

True high frame rate is not a renderer feature. It is a change to the game's simulation, and it runs as its own workstream (see below). The renderer's part is to cost little per frame and never assume a fixed frame rate.

## Architecture

1. **Replaceable-function list in the profile config.** The generator emits a dispatch call, instead of a direct jump, for every address on this list. A runtime switch can set each replacement to native or GE. The switch is the main debugging tool: any draw category can be A/B-compared in the same build.
2. **A native render API in the VC2 profile** (`host/gfx/`). This holds VC2/Kurofune-specific translation, which reads engine structures from guest memory.
3. **Generic PSP services in PortableKit.** Texture decoding (swizzle, CLUT 4/8, DXT), vertex-format decoding, and the texture cache keyed by guest address and content hash already exist for the GE path. They should be exposed as a library, not rewritten.
4. **One ordered command stream per frame.** Native draws and GE replay draws go into the same frame in submission order, sharing render targets and depth/stencil. Without this, a half-converted frame layers incorrectly.
5. **Keep the GE contract the game sees.** The game still waits for list completion, signal and finish callbacks, and `sceGeDrawSync`. When a list carries no primitives, or all of its primitives were drawn natively, the HLE must still complete it on time. In the end state, GE HLE is acknowledgement only.
6. **The game's coordinates are not the screen's.** The game keeps working in its 480×272 logical space. The native renderer maps that to the real output: 3D gets a widened projection (Hor+: the same vertical view, more on the sides); 2D gets an anchor policy (left, right, centre or stretch-to-fill) for each UI element or element group.
7. **The game loop is driven by real time.** The PSP vblank pacing (`sceDisplayWaitVblank*`, `sceDisplaySetFrameBuf`) becomes a host-controlled tick that runs once per displayed frame and passes the elapsed time into the game.

## The PC improvements in detail

**True high frame rate.** The game runs one real update per displayed frame. Interpolation and frame generation are excluded. VC2's movement, animation, camera, AI, BLiTZ command-point and action-point drain, interception fire, scripts, UI tweens and audio cues are almost certainly counted in frames. Every one of them has to become time-based, or it would run 2–4× too fast. This is the largest single workstream in the plan.

Measured 2026-11-06: menus, story scenes and the battle map run at 60 frames/s; 3D combat runs at 30 frames/s (two vblanks per frame); loading screens run uncapped. Each displayed frame is a newly rendered one.

How it gets done:
1. **Find the game's clock.** Locate the frame/tick counters and the main-loop step (Phase 0). Many PSP games already pass a "frames elapsed" step (1 at 60 fps, 2 at 30 fps) to their update functions. If VC2 does, that step becomes a fractional time value. This one change would cover every system that already respects the step.
2. **Find what's frame-locked, by experiment.** Run the same scripted input route twice: at the original rate, and with the tick doubled. Log game state per unit of game time: unit positions, AP gauge, camera, animation frames, timers, script state and RNG calls. Every quantity that diverges is a frame-locked system. Repeat at 3× and 4× to separate per-frame counts from per-frame rates.
3. **Fix each system in the game.** Profile patches convert per-frame constants (speeds, accelerations, damping, timers, countdown frames, per-frame probabilities) to per-second values scaled by the step. The recompiled C++ makes these direct, reviewable changes. Each patch is recorded with the trace that proved it was needed.
4. **Keep outcomes fair.** A per-frame chance (e.g. one roll per frame for enemy interception fire) is converted to the same chance per second, not left to roll 4× as often. RNG call order will change at a higher rate, so routes are compared by statistics and timings, not by identical rolls.
5. **Scripts and cutscenes** move from frame-counted waits to time-based ones, keeping lip-sync and audio alignment.
6. **Movies** play at their own frame rate on a native path. The UI around them still runs at display rate.

Verification: the 2× and 4× runs of a timed route must match the 1× run in game-time outcomes, such as how far a unit moves on a full AP gauge, how long a scripted scene lasts, and how fast menus and fades complete. They must also play visibly smoothly. Variable rate (uncapped, VRR) comes after fixed multiples work, since it needs every patched system to accept any step size.

Fallback: if a particular system genuinely cannot be converted (deep fixed-step physics, say), it would run at a fixed internal rate and be flagged for discussion. That would be by decision, not by default.

**Arbitrary resolution.**
- Render targets at output resolution, or at a chosen internal scale.
- Native fonts (already rendered from a system font).
- Upscaling or replacement for 2D art, with the texture-pack hook PortableKit already has.
- Screen-space effects (outlines, blur, the Canvas overlays) re-expressed in output pixels or logical units, so they don't shrink or blur at 4K.
- Fixes for the game wherever it rasterises to a 480×272 buffer and reads it back.

**Arbitrary aspect ratio.**
- Hor+ projection for 3D.
- Culling and level-of-detail decisions widened to the new view. The game's own frustum culling will remove objects that are now on screen, so it gets patched.
- HUD and menus anchored to the screen edges.
- Full-screen 2D (story backgrounds, map overview, movies) shown pillarboxed, or extended where an asset allows.
- Touch-up for anything that assumes it can't be seen beyond 480 px: edge-of-screen spawns, transitions, letterbox masks.

**Fixing the game.** Each break gets a profile-level patch: a replaced guest function or a fixed constant, recorded with the evidence for it. It is never a renderer special case. Patches that change behaviour sit behind settings until verified.

## Phases

| Phase | Work | Exit evidence |
| --- | --- | --- |
| **0. Map the engine** | Find Kurofune's entry points and its low-level "write GE command" helpers. Extend the frame dump to tag each primitive with the guest function that emitted it. Build a catalogue of draw categories with their share of primitives per scene: UI sprites, fonts, fades, movies, static meshes, terrain, skinned characters, shadows, particles, framebuffer effects. Also locate the camera and projection setup, the frustum-culling routine, the per-object scene structures and the game's tick and frame counters. | A table mapping each draw category to the guest functions that emit it, across title, menu, story, battle-map and combat frames. The camera, culling and tick-rate facts recorded with their addresses. |
| **1. Replacement plumbing** | Generator support for the replaceable-function list. Helpers for the guest ABI (arguments, stack, guest pointers, return values). Per-function native/GE switches. | One Kurofune function replaced natively, with screenshots identical to the GE path. |
| **2. Native renderer core** | Native submission alongside GE replay in one ordered frame. Shared render targets. Texture cache through PortableKit's decoders. Internal resolution scaling. | A mixed frame (one native category, the rest GE) diffs clean against the GE-only frame. |
| **3. 2D and UI** | Sprites, menus, fonts (already native via MS Gothic), fades, movie surfaces. Anchor policy for HUD and menus; pillarboxing for full-screen 2D; any resolution. | Title, menus and story scenes submit no GE primitives for these categories, and they lay out correctly at 16:9, 21:9 and 4:3 at 4K. |
| **4. 3D world** | Static meshes, terrain, skinned characters (GPU skinning from engine bone data), lighting, fog, materials, transparency ordering, shadows. Native camera with Hor+ projection; patch the game's culling for wide views. | Battle map and combat with no GE primitives outside effects, at any aspect, with no objects popping at the screen edges. |
| **5. Engine effects** | Render-to-texture, framebuffer copies, the Canvas look (outlines, hatching, paper overlays), blur and depth of field, map overview. All rebuilt to work at output resolution. Evidence comes from the frame dumps. | Effect-heavy scenes look right at 4K and ultrawide, not just at 480×272. |
| **6. True high frame rate** | Host-driven tick with elapsed time; differential 1×/2×/4× tracing; per-system time-base patches; script timing; then variable rate. This can start in parallel with Phase 3 on the GE path, because it changes the game, not the renderer. | 2×/4× routes match 1× in game-time outcomes and audio sync; real updates every frame at 144 Hz and uncapped. |
| **7. Remove the GE path** | A `VC2_NATIVE_ONLY` build option compiles out GE decoding and replay. Any list containing a primitive is logged as an error. | The full checkpoint route runs with zero GE primitives. |

Phases 0–2 are groundwork and produce nothing visible. Phases 3–6 each give a visible result and can ship separately. Roadmap milestone 6 (PC presentation) is now delivered through Phases 3–6, not as a separate step at the end.

## How each step is verified

- **Fixed routes:** `VC2_INPUT_SCRIPT` replays the same input and screenshots the same frames twice, once per path, using the per-function switch.
- **Image diff** against the GE path, with the native renderer forced to 480×272, 16:9 and the original tick rate. This checks that each converted category is correct before the improvements switch on. While GE rendering remains, it is the reference, so its known faults (shadows, possibly stripes) have to be fixed or noted first. Where in doubt, compare against PPSSPP.
- **Improvement checks** on the same routes at several sizes and aspects (1080p 16:9, 1440p 21:9, 4K 32:9, 4:3). Pass criteria are visual: edges, anchoring, no stretching, no popping.
- **HFR checks** use the differential method in the HFR section: the same game-time outcomes at 1×, 2× and 4× tick rates, then at variable rate.
- **Frame dump** before and after each category switches: the native draws should disappear from the GE list.
- **Gameplay checks** on every phase, with `PSPRECOMP_NO_INTERPRETER=1` and the optimised build.

## Risks

- **Engine code inlined into the game.** If Kurofune calls were inlined, there is no single entry point to replace. The fallback is lower-level hooks, at the helpers that write GE commands. Phase 0 will show how much of this exists.
- **The game reading back what it drew:** save thumbnails, screen-capture transitions, CPU reads of a framebuffer. The native path must place equivalent pixels in guest memory where the game expects them.
- **Timing and synchronisation:** the game waits on GE callbacks and signals. Completing lists too early or too late changes pacing or deadlocks the game.
- **Effects that rely on PSP quirks:** 5551 one-bit stencil, colour doubling, dithering, clamped blending. Some looks may depend on them and need deliberate reproduction.
- **Frame-locked logic is everywhere.** True HFR touches far more of the game than the renderer does. Differential tracing finds what diverges on the tested routes. Systems only reached elsewhere (rare missions, specific orders and potentials) need their own routes, so coverage is the main risk.
- **CPU budget.** At 240 Hz the game logic runs 4–8× as often as on PSP. Recompiled code is fast, but the system-call layer and any remaining GE emulation must be cheap per frame. The optimised build's results are the first measure.
- **Gameplay tied to what's on screen.** Wider views can expose things the game expects to be hidden: spawn points, enemy behaviour toggled by visibility, map-edge geometry. Each needs a game fix, or a decision to limit the aspect ratio in that scene.
- **Scope:** this is a new renderer for the engine. Going category by category keeps the game playable throughout, but the total is large, and Phase 0 is needed before any honest time estimate.

## Relationship to current work

- The emulated GE path stays the **reference and fallback** until Phase 7. It gets fixes only where they unblock testing or are needed as a correct reference (e.g. patch 0004, stencil).
- Gameplay validation (audio, movies, the first mission, saves) continues on the GE path in parallel. It does not depend on this plan.
- Removing the data-install feature is independent.
- Licensing guardrails are unchanged. PortableKit (MIT) code may be reused and extended. GPL renderers such as PPSSPP and psp-recomp are behaviour references only.

## Decisions

**Settled (2026-11-06):** the native renderer exists to deliver high frame rates, arbitrary resolutions and arbitrary aspect ratios. It is not a faithful reproduction of the PSP output. Breakages caused by these improvements are fixed in the game. High frame rate means **true HFR**: the game simulates every displayed frame. Interpolation and frame generation are not used.

**Still open:**
1. **Effects at the PSP's quality or upgraded?** For example, real shadow maps instead of stencil blobs. The default is to keep the original technique at higher resolution, then upgrade case by case.
2. **Fallback in development builds.** Keep the GE path selectable until Phase 7 (recommended), or drop it earlier to save maintenance?
3. **Order of work.** Phase 0 is analysis only and can start now alongside gameplay validation.

## Immediate next steps (Phase 0)

1. Locate Kurofune in the executable: references to its version string, its initialisation, and the code reached from the Render step at vtable +72.
2. Find the low-level helpers that append words to the display list buffer. These are the functions that store values with GE command bytes in the top 8 bits into the list's write pointer.
3. Add caller tagging to the F9 frame dump so each primitive line names its emitting guest function.
4. Find the camera/projection setup, the frustum-culling test, the game's tick counter and any "frames elapsed" step passed to update functions. Then measure the actual update rate in 2D, battle-map and combat modes.
5. Build the state logger and the doubled-tick switch needed for differential HFR tracing.
6. Capture one title, one menu, one story, one battle-map and one combat frame, and write the category catalogue to `research/`.
