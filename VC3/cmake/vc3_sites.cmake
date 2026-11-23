# Patches VC3's generated code (generated/, never committed) at configure
# time, in place and idempotently: running it again changes nothing, and a
# newly generated corpus is patched at the next configure. Every address here
# is VC3's own (the Japanese Extra Edition's; the English ReTranslation has the
# same code), measured in VC3's executable, not translated from VC2's. The
# calls reach host/vc3_tick.cpp and host/vc3_lod.cpp, which hold the same
# addresses. A site whose text is not found is reported and skipped.
#
# Because the patches stay in the generated files, removing a site from these
# lists does not undo it: to switch one off, make its host function leave the
# value as it is.

# Whole steps (as VC2's vc2_step_sites.cmake). The game's step (time block
# 0x08D59D98 + 28) made fractional at high frame rates is truncated to a whole
# number by trunc.w.s in these places; each becomes vc3_step_trunc(pc, value),
# which keeps a running remainder per site (host/vc3_tick.cpp). Found
# 2026-11-15 by scanning every read of the step (215) for a trunc.w.s within 14
# instructions (analysis/claude/step_sites.py), plus 0899F6A4 (VC2's 0897A03C,
# which reaches the step another way). 08813ECC and 088B4E5C truncate
# (step + 0.05), like VC2's frame counters 08813600 and 088ACEF0.
# 08931B40, 0894899C, 0895A0C4, 0895C8A0 (2026-11-16, while chasing the
# vanishing grenade): VC2's 0891B8EC (a unit's turn rate: speed x unit+0x8C8 x
# step) and 0892E6AC / 0893F04C / 08941778 (16 parts' spin, x 65536/2pi), which
# the 14-instruction scan missed (the step is read further back); a scan with
# 40 finds VC2's 26 sites' counterparts and nothing else VC2 patches.
set(_vc3_step_sites
    08813ECC 08887B98 0888D1C8 088915C0 088A1630 088B4E5C 088B6B80 088C6954
    088EE924 0892182C 08991A38 08991A58 0899F6A4 089A0768 089A1F58 089A22C8
    089AD73C 089D1630 089D185C 089D1898 089D7D88 089D7FB4 089D7FF0 08A8E248
    08AE596C 08AE5A70 08B6B128 08931B40 0894899C 0895A0C4 0895C8A0)
# Frame-locked additions: `x += delta` once a frame whatever the step. The
# animation player (VC2 0x08C592E4, VC3 0x08C4CD68): patched for the call
# counts only; host/vc3_tick.cpp leaves them unscaled, as VC2 does.
# The unit timer (VC2 0x08893C48: unit+0x8A4 += 1.0; VC3 0x08895DD4, unit
# +0x8C4), found by hand 2026-11-16 (a grenade vanished at the top of its arc
# at high frame rates only).
# 0890EA38: interception fire (VC2 088FDAB0, 2026-11-23): each enemy that can
# see a moving unit adds the move state's per-frame rate to its fire timer
# (+0x870) once a frame and fires each time it passes the weapon's interval,
# so at 165 Hz enemies fired 5.5 times as often. Scaled unless the game already
# multiplied the rate by the step (a versus mode; vc3_patch_intercept_sites).
set(_vc3_frame_add_sites 08C4CFC0 08C4CFD4 08C4D008 08895DD4 0890EA38)
# Frame-locked counters: `n += 1` once a frame. The main loop's frame counter
# (VC2 0x08BD2EAC); the unit counter (VC2 0x088BFCC8 +0x830, VC3 0x088C9114
# +0x850, in the function the unit timer calls just before it adds).
set(_vc3_frame_count_sites 08BC3B74 088C9114)

function(_vc3_unit generated_dir address out)
    math(EXPR unit "(0x${address} - 0x08804000) / 16384")
    string(LENGTH "${unit}" digits)
    while(digits LESS 4)
        set(unit "0${unit}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(${out} "${generated_dir}/generated_unit_${unit}.cpp" PARENT_SCOPE)
endfunction()

function(_vc3_add_declaration text_var declaration)
    string(FIND "${${text_var}}" "${declaration}" have)
    if(have EQUAL -1)
        string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${declaration}\n" text "${${text_var}}")
        set(${text_var} "${text}" PARENT_SCOPE)
    endif()
endfunction()

function(vc3_patch_step_sites generated_dir)
    set(decl_trunc "extern \"C\" std::uint32_t vc3_step_trunc(std::uint32_t pc, float value) noexcept;  // vc3 step sites")
    set(decl_scale "extern \"C\" float vc3_frame_scale(std::uint32_t pc, float value) noexcept;")
    set(decl_count "extern \"C\" std::uint32_t vc3_frame_count(std::uint32_t pc, std::uint32_t amount) noexcept;")
    set(patched 0)
    set(already 0)
    set(missing "")
    foreach(pc IN LISTS _vc3_step_sites _vc3_frame_add_sites _vc3_frame_count_sites)
        _vc3_unit("${generated_dir}" "${pc}" file)
        if(NOT EXISTS "${file}")
            list(APPEND missing "0x${pc} (no ${file})")
            continue()
        endif()
        file(READ "${file}" text)
        set(original "${text}")
        list(FIND _vc3_frame_count_sites "${pc}" count_index)
        list(FIND _vc3_frame_add_sites "${pc}" add_index)
        if(NOT count_index EQUAL -1)
            set(marker "vc3_frame_count(0x${pc}u,")
            set(decl "${decl_count}")
        elseif(NOT add_index EQUAL -1)
            set(marker "vc3_frame_scale(0x${pc}u,")
            set(decl "${decl_scale}")
        else()
            set(marker "vc3_step_trunc(0x${pc}u,")
            set(decl "${decl_trunc}")
        endif()
        string(FIND "${text}" "${marker}" done)
        if(NOT done EQUAL -1)
            math(EXPR already "${already} + 1")
            continue()
        endif()
        if(NOT count_index EQUAL -1)
            string(REGEX MATCH "L_${pc}:\n    ctx\\.gpr\\[([0-9]+)\\] = \\(ctx\\.gpr\\[([0-9]+)\\] \\+ static_cast<std::uint32_t>\\(([0-9]+)\\)\\);" line "${text}")
            set(replacement "L_${pc}:\n    ctx.gpr[${CMAKE_MATCH_1}] = (ctx.gpr[${CMAKE_MATCH_2}] + vc3_frame_count(0x${pc}u, ${CMAKE_MATCH_3}u));")
        elseif(NOT add_index EQUAL -1)
            string(REGEX MATCH "L_${pc}:\n    ctx\\.fpr\\[([0-9]+)\\] = ctx\\.fpr\\[([0-9]+)\\] \\+ ctx\\.fpr\\[([0-9]+)\\];" line "${text}")
            set(replacement "L_${pc}:\n    ctx.fpr[${CMAKE_MATCH_1}] = ctx.fpr[${CMAKE_MATCH_2}] + vc3_frame_scale(0x${pc}u, ctx.fpr[${CMAKE_MATCH_3}]);")
        else()
            string(REGEX MATCH "L_${pc}:\n    ctx\\.fpr\\[([0-9]+)\\] = std::bit_cast<float>\\(ctx\\.fpu_float_to_word_ct<1u>\\(ctx\\.fpr\\[([0-9]+)\\]\\)\\);" line "${text}")
            set(replacement "L_${pc}:\n    ctx.fpr[${CMAKE_MATCH_1}] = std::bit_cast<float>(vc3_step_trunc(0x${pc}u, ctx.fpr[${CMAKE_MATCH_2}]));")
        endif()
        if(line STREQUAL "")
            list(APPEND missing "0x${pc}")
            continue()
        endif()
        string(REPLACE "${line}" "${replacement}" text "${text}")
        _vc3_add_declaration(text "${decl}")
        file(WRITE "${file}" "${text}")
        math(EXPR patched "${patched} + 1")
    endforeach()
    list(LENGTH _vc3_step_sites n1)
    list(LENGTH _vc3_frame_add_sites n2)
    list(LENGTH _vc3_frame_count_sites n3)
    math(EXPR total "${n1} + ${n2} + ${n3}")
    message(STATUS "VC3 step sites: ${patched} patched now, ${already} already patched, of ${total}")
    if(missing)
        message(WARNING "VC3 step sites not found (the corpus may differ): ${missing}")
    endif()
endfunction()

# One hook: in the unit holding `label`, `call` goes first after the label
# (whose first line must read `first_line`), once; `declaration` after the
# units header.
function(vc3_patch_hook generated_dir label first_line call declaration what)
    string(SUBSTRING "${label}" 2 -1 address)
    _vc3_unit("${generated_dir}" "${address}" file)
    if(NOT EXISTS "${file}")
        message(WARNING "VC3 ${what}: ${file} not found")
        return()
    endif()
    file(READ "${file}" text)
    # CMake on Windows writes the units back with CRLF line ends: match on LF.
    string(REPLACE "\r\n" "\n" text "${text}")
    set(original "${text}")
    string(FIND "${text}" "${label}:\n    ${call}\n" done)
    if(done EQUAL -1)
        string(FIND "${text}" "${label}:\n    ${first_line}" at)
        if(at EQUAL -1)
            message(WARNING "VC3 ${what} ${label} not found (the corpus may differ)")
            return()
        endif()
        string(REPLACE "${label}:\n    ${first_line}" "${label}:\n    ${call}\n    ${first_line}" text "${text}")
    endif()
    _vc3_add_declaration(text "${declaration}")
    if(NOT text STREQUAL original)
        file(WRITE "${file}" "${text}")
        message(STATUS "VC3 ${what} ${label}: patched")
    endif()
endfunction()

# Morale gauge (as VC2's): the battle HUD's gauge moves by the whole-step
# amounts of step sites 08991A38 / 08991A58 (VC2 0896EC90 / 0896ECB4, the same
# code) and calls a moving gauge at exactly 0 a morale drop. vc3_morale_step
# makes the first move from empty at least 1 (s6 = the position, s3 = the
# morale), so a rise from 0 at a high frame rate never plays a false drop.
function(vc3_patch_morale_sites generated_dir)
    foreach(label IN ITEMS L_08991A3C L_08991A5C)
        if(label STREQUAL "L_08991A3C")
            set(first "{ const bool branch_taken = 0u == 0u;")
        else()
            set(first "ctx.gpr[4] = (std::bit_cast<std::uint32_t>(ctx.fpr[12]));")
        endif()
        vc3_patch_hook("${generated_dir}" ${label} "${first}"
            "ctx.fpr[12] = std::bit_cast<float>(vc3_morale_step(std::bit_cast<std::uint32_t>(ctx.fpr[12]), ctx.gpr[22], ctx.gpr[19]));"
            "extern \"C\" std::uint32_t vc3_morale_step(std::uint32_t amount, std::uint32_t position, std::uint32_t morale) noexcept;  // vc3 morale gauge site"
            "morale gauge site")
    endforeach()
    vc3_patch_flight_sites("${generated_dir}")
    vc3_patch_intercept_sites("${generated_dir}")
    # Interception fire's pause (2026-11-23, found with the trace-build swap
    # hunt: owner marked 53 bursts at Game rate). The function at 0x088A1558
    # counts the pause down at +0x6034 by the frame's whole steps, at least 1:
    # trunc(step) (step site 0x088A1630), then `if <= 0: 1` (0x088A1640). At a
    # high frame rate the step site gives 0 on most frames, the clamp made that
    # 1, so the pause ran down by a whole step every frame (2.75x fast at
    # 165 Hz in combat) and enemies fired almost without a pause. At the join
    # (0x088A1644) the amount becomes the step site's own whole steps (f12),
    # 0 included. At the game's rate the step is 1 or 2, so the clamp never
    # mattered and nothing changes. VC2 has the same code (0x0889DF0C).
    vc3_patch_hook("${generated_dir}" L_088A1644 "ctx.gpr[5] = (aot_mem.aot_load32(ctx.gpr[16] + static_cast<std::uint32_t>(24628)));"
        "ctx.gpr[4] = std::bit_cast<std::uint32_t>(ctx.fpr[12]);  // interception pause: the whole steps, unclamped"
        "#include <bit>"
        "interception pause site")
    # ... and each unit counts its own whole steps (the site's remainder is
    # shared by every unit that calls it in a frame): vc3_pause_steps.
    vc3_patch_hook("${generated_dir}" L_088A1648 "ctx.gpr[4] = (ctx.gpr[5] - ctx.gpr[4]);"
        "ctx.gpr[4] = vc3_pause_steps(ctx.gpr[16], ctx.gpr[4]);"
        "extern \"C\" std::uint32_t vc3_pause_steps(std::uint32_t object, std::uint32_t whole) noexcept;  // vc3 pause steps site"
        "interception pause steps site")
    # Burst fire (interception and other bursts without their own timer): the
    # attack state asks 0x0891ED94 "fire the next shot?" once an update and it
    # answers yes every update, so a 10-shot burst took 10 frames: ~310 ms at
    # the game's 30 fps, ~60 ms at 165. v0 goes through vc3_attack_ready, which
    # lets one shot through per game frame of time.
    vc3_patch_hook("${generated_dir}" L_08923DA8 "{ const bool branch_taken = ctx.gpr[2] == 0u;"
        "ctx.gpr[2] = vc3_attack_ready(ctx.gpr[16], ctx.gpr[2]);"
        "extern \"C\" std::uint32_t vc3_attack_ready(std::uint32_t state, std::uint32_t ready) noexcept;  // vc3 burst fire site"
        "burst fire site")
    # The battle manager's frame (0x088DD190, one exit 0x088DD2D0): at a high
    # frame rate once per game frame of time (vc3_manager_hold / _done).
    vc3_patch_hook("${generated_dir}" L_088DD190 "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-32));"
        "if (vc3_manager_hold()) { ctx.pc = ctx.gpr[31]; return; }"
        "extern \"C\" bool vc3_manager_hold() noexcept;  // vc3 battle manager site"
        "battle manager site")
    vc3_patch_hook("${generated_dir}" L_088DD2D0 "ctx.gpr[16] = (aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(0)));"
        "vc3_manager_done();"
        "extern \"C\" void vc3_manager_done() noexcept;  // vc3 battle manager end site"
        "battle manager end site")
    # The attack state's phases (all but aiming, 5) last a set number of
    # updates: at a high frame rate they run once per game frame, with the
    # step set to that frame for the update (vc3_attack_hold at the entry,
    # vc3_attack_done at the one exit 0x089244D4).
    vc3_patch_hook("${generated_dir}" L_089214F8 "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-1984));"
        "if (vc3_attack_hold(ctx.gpr[4])) { ctx.pc = ctx.gpr[31]; return; }"
        "extern \"C\" bool vc3_attack_hold(std::uint32_t state) noexcept;  // vc3 attack phase site"
        "attack phase site")
    vc3_patch_hook("${generated_dir}" L_089244D4 "ctx.fpr[20] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(1936)));"
        "vc3_attack_done();"
        "extern \"C\" void vc3_attack_done() noexcept;  // vc3 attack phase end site"
        "attack phase end site")
    # The game's sound effect function (0x08816500; a0 the sound's id): told
    # to the Settings row, which learns the menus' cursor sound from it
    # (host/vc3_settings_menu.cpp sound_played).
    vc3_patch_hook("${generated_dir}" L_08816500 "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-32));"
        "vc3_sound_played(ctx.gpr[4]);"
        "extern \"C\" void vc3_sound_played(std::uint32_t id) noexcept;  // vc3 sound site"
        "sound site")
endfunction()

# Thrown and bouncing objects (grenades; 2026-11-16, at high frame rates a
# grenade vanished at the top of its arc). Two copies of one routine (VC3
# 0x089D15F8, VC2's 0x089AC8A0; and 0x089D7D50, VC3's own, which grenades
# use) advance a whole-tick flight time (+0x784, 1/60 s) by the step site's
# whole steps, and place the object by that time. One shared remainder per
# site spread the ticks over every object in flight, and at 240 Hz the object
# stood still three frames in four, so its move (new - old position, which the
# routine normalises to face it along its path) was zero. vc3_flight_step gives
# each object its own remainder (s0 = the object, a0 = its time); vc3_flight_time
# places it by its exact time, so it moves every frame.
# Interception fire (0890EA38 above): the move state makes its rate at
# 0x0890E718 (per frame) and multiplies it by the step at 0x0890E748 only in a
# versus mode. Each site tells vc3_frame_scale which it was (value 0, result
# unused), so the rate is scaled once, and only when the game did not.
function(vc3_patch_intercept_sites generated_dir)
    set(declaration "extern \"C\" float vc3_frame_scale(std::uint32_t pc, float value) noexcept;")
    vc3_patch_hook("${generated_dir}" L_0890E718 "ctx.fpr[22] = ctx.fpr[12] / ctx.fpr[22];"
        "(void)vc3_frame_scale(0x0890E718u, 0.0f);  // interception rate made per frame"
        "${declaration}"
        "interception rate site")
    vc3_patch_hook("${generated_dir}" L_0890E748 "{ const float fs = ctx.fpr[22]; const float ft = ctx.fpr[12];"
        "(void)vc3_frame_scale(0x0890E748u, 0.0f);  // interception rate times the step (versus)"
        "${declaration}"
        "interception step site")
endfunction()

function(vc3_patch_flight_sites generated_dir)
    foreach(pair IN ITEMS "L_089D1634:L_089D16D8" "L_089D7D8C:L_089D7E30")
        string(REPLACE ":" ";" labels "${pair}")
        list(GET labels 0 step_label)
        list(GET labels 1 time_label)
        vc3_patch_hook("${generated_dir}" ${step_label} "ctx.gpr[5] = (ctx.gpr[16] + static_cast<std::uint32_t>(1824));"
            "ctx.fpr[12] = std::bit_cast<float>(vc3_flight_step(ctx.gpr[16], ctx.gpr[4], std::bit_cast<std::uint32_t>(ctx.fpr[12])));"
            "extern \"C\" std::uint32_t vc3_flight_step(std::uint32_t object, std::uint32_t time, std::uint32_t whole) noexcept;  // vc3 flight step site"
            "flight step site")
        vc3_patch_hook("${generated_dir}" ${time_label} "{ const float fs = ctx.fpr[20]; const float ft = ctx.fpr[14];"
            "ctx.fpr[14] = vc3_flight_time(ctx.gpr[16], ctx.fpr[14]);"
            "extern \"C\" float vc3_flight_time(std::uint32_t object, float time) noexcept;  // vc3 flight time site"
            "flight time site")
    endforeach()
endfunction()

# Level of detail (host/vc3_lod.cpp; VC2's vc2_patch_lod_site at VC3's
# addresses, measured 2026-11-15). The render-parameter loader (VC3 0x08B09A54)
# copies a version 2 block (0x440 bytes) with the call at 08B09B14: the
# destination is noted before it and the switch distances scaled once it
# returns (08B09B1C). A version 1 block goes through 08B097E0, whose store of
# each distance (`swc1 f12, 896(t0)` at 08B09924) goes through
# vc3_lod_distance().
function(vc3_patch_lod_site generated_dir)
    _vc3_unit("${generated_dir}" 08B09924 file)
    if(NOT EXISTS "${file}")
        message(WARNING "VC3 LOD sites: ${file} not found")
        return()
    endif()
    file(READ "${file}" text)
    set(original "${text}")
    set(store_old "L_08B09924:\n    aot_mem.aot_store32(ctx.gpr[8] + static_cast<std::uint32_t>(896), std::bit_cast<std::uint32_t>(ctx.fpr[12]));")
    set(store_new "L_08B09924:\n    aot_mem.aot_store32(ctx.gpr[8] + static_cast<std::uint32_t>(896), std::bit_cast<std::uint32_t>(vc3_lod_distance(ctx.gpr[8] + 896u, ctx.fpr[12])));")
    set(begin_old "L_08B09B14:\n    ctx.gpr[31] = (0x08B09B1Cu);")
    set(begin_new "L_08B09B14:\n    vc3_lod_params_begin(ctx.gpr[4]);\n    ctx.gpr[31] = (0x08B09B1Cu);")
    set(end_old "L_08B09B1C:\n    ctx.gpr[2] = (0u | 1u);")
    set(end_new "L_08B09B1C:\n    vc3_lod_params_end();\n    ctx.gpr[2] = (0u | 1u);")
    foreach(name IN ITEMS store begin end)
        string(FIND "${text}" "${${name}_new}" done)
        if(NOT done EQUAL -1)
            continue()
        endif()
        string(FIND "${text}" "${${name}_old}" at)
        if(at EQUAL -1)
            message(WARNING "VC3 LOD site '${name}' not found (the corpus may differ)")
            continue()
        endif()
        string(REPLACE "${${name}_old}" "${${name}_new}" text "${text}")
    endforeach()
    _vc3_add_declaration(text "extern \"C\" float vc3_lod_distance(std::uint32_t address, float value) noexcept;  // vc3 LOD site")
    _vc3_add_declaration(text "extern \"C\" void vc3_lod_params_begin(std::uint32_t destination) noexcept;")
    _vc3_add_declaration(text "extern \"C\" void vc3_lod_params_end() noexcept;")
    if(NOT text STREQUAL original)
        file(WRITE "${file}" "${text}")
        message(STATUS "VC3 LOD sites: patched")
    else()
        message(STATUS "VC3 LOD sites: already patched")
    endif()
endfunction()

# Modern controls: the camera, the aim and the screen cursor from the right
# stick and the mouse (host/vc3_camera.cpp; VC2's vc2_patch_camera_sites at
# VC3's own addresses, measured 2026-11-15 in VC3's executable).
#   Action Mode camera, 0x08891D64(unit) (VC2 0x0888FC94): begin sites before
#     the game reads its camera buttons (08891E5C usual path, 08891FD8 the
#     other), turn sites once its own turn is done (08891FA4, 08892108). The
#     unit is s0; yaw +1432, pitch +1428, mode +876, pad s2 +72, as VC2.
#   Screen cursor controller 0x08A8E0B8(obj) (VC2 0x08B09910): right after it
#     reads its virtual stick onto its stack (08A8E138). VC3 moved the fields:
#     stick index +240 (VC2 +224), position +192 / +196 (VC2 +176 / +180).
#   Target Mode aim (VC2 0x088E1C..): the nub as read (088EF790), the step
#     multiply (088EF804; x f12, y f15 in VC3, f16 in VC2; step f13), the D-pad
#     read (088EF860).
#   R&D vehicle preview camera: the mover's arrival test (08A7A9B0, VC2
#     08A78944, the same code).
# VC2's movement trace (L_088D388C) is a diagnostic and is not carried over.
function(vc3_patch_camera_sites generated_dir)
    set(begin "extern \"C\" void vc3_camera_begin(std::uint32_t unit) noexcept;  // vc3 camera begin site")
    set(turn "extern \"C\" void vc3_camera_turn(std::uint32_t unit) noexcept;  // vc3 camera site")
    foreach(label IN ITEMS L_08891E5C L_08891FD8)
        vc3_patch_hook("${generated_dir}" ${label} "ctx.fpr[20] = std::bit_cast<float>(0u);"
            "vc3_camera_begin(ctx.gpr[16]);" "${begin}" "camera begin site")
    endforeach()
    foreach(label IN ITEMS L_08891FA4 L_08892108)
        vc3_patch_hook("${generated_dir}" ${label}
            "ctx.gpr[18] = (aot_mem.aot_load32(ctx.gpr[18] + static_cast<std::uint32_t>(72)));"
            "vc3_camera_turn(ctx.gpr[16]);" "${turn}" "camera site")
    endforeach()
    vc3_patch_hook("${generated_dir}" L_08A8E138 "ctx.gpr[31] = (0x08A8E140u);"
        "vc3_aim_stick(ctx.gpr[29], ctx.gpr[16]);"
        "extern \"C\" void vc3_aim_stick(std::uint32_t sp, std::uint32_t obj) noexcept;  // vc3 cursor site"
        "screen cursor site")
    vc3_patch_hook("${generated_dir}" L_088EF804 "{ const float fs = ctx.fpr[12]; const float ft = ctx.fpr[13];"
        "vc3_aim_target(&ctx.fpr[12], &ctx.fpr[15], ctx.fpr[13]);"
        "extern \"C\" void vc3_aim_target(float *x, float *y, float step) noexcept;  // vc3 target aim site"
        "target aim site")
    vc3_patch_hook("${generated_dir}" L_088EF790
        "ctx.fpr[12] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(0)));"
        "vc3_aim_nub(ctx.gpr[29]);"
        "extern \"C\" void vc3_aim_nub(std::uint32_t sp) noexcept;  // vc3 fine aim site"
        "aim nub site")
    vc3_patch_hook("${generated_dir}" L_088EF860
        "ctx.fpr[12] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(16)));"
        "vc3_aim_fine(ctx.gpr[29]);"
        "extern \"C\" void vc3_aim_fine(std::uint32_t sp) noexcept;  // vc3 fine aim site"
        "fine aim site")
    vc3_patch_hook("${generated_dir}" L_08A7A9B0 "{ const bool branch_taken = ctx.gpr[4] == 0u;"
        "ctx.gpr[4] = vc3_preview_arrived(ctx.gpr[4], ctx.fpr[12], ctx.gpr[16]);"
        "extern \"C\" std::uint32_t vc3_preview_arrived(std::uint32_t arrived, float distance, std::uint32_t mover) noexcept;  // vc3 preview camera site"
        "preview camera arrival site")
endfunction()

# Sound volumes (host/vc3_audio.cpp): OPTIONS closing applies its volumes
# (L_08AC2A60, VC2 0x08B3E244): they are the player's and go to settings.ini.
# OPTIONS' Default resets the options block itself (L_08AC3B18): from then
# until closing, the launcher's volumes do not override the game's.
function(vc3_patch_audio_sites generated_dir)
    vc3_patch_hook("${generated_dir}" L_08AC2A60 "ctx.gpr[4] = (2255u << 16u);"
        "vc3_audio_options_closed();"
        "extern \"C\" void vc3_audio_options_closed() noexcept;  // vc3 audio site"
        "OPTIONS closing (volumes) site")
    vc3_patch_hook("${generated_dir}" L_08AC3B18 "ctx.gpr[4] = (2255u << 16u);"
        "vc3_audio_options_default();"
        "extern \"C\" void vc3_audio_options_default() noexcept;  // vc3 audio site"
        "OPTIONS Default (volumes) site")
endfunction()

# The title menu's SETTINGS (host/vc3_native_gu.cpp): its handler (VC3
# 0x08B84E9C, VC2 0x08B81A60, the same code; a1 = the entry chosen) runs
# OPTIONS for the old INSTALL slot, with the in-game Settings screen asked for.
function(vc3_patch_title_sites generated_dir)
    vc3_patch_hook("${generated_dir}" L_08B84E9C
        "ctx.gpr[6] = (ctx.gpr[5] < static_cast<std::uint32_t>(5) ? 1u : 0u);"
        "ctx.gpr[5] = vc3_title_choice(ctx.gpr[5]);"
        "extern \"C\" std::uint32_t vc3_title_choice(std::uint32_t index) noexcept;  // vc3 title choice site"
        "title menu choice site")
endfunction()

# OPTIONS without Installed Data (host/vc3_native_gu.cpp): the tab's builder
# (VC3 0x08AC5340.., VC2 0x08B40A18..) makes one row for each of the option
# table's entries 0..5 (fp the entry); entry 5, the last, is Installed Data
# (the PSP's data install, of no use here). Its two loop tests (L_08AC53B4,
# L_08AC56C0: fp < 6) ask vc3_options_rows(6), which answers 5 unless
# VC3_KEEP_INSTALL=1: the row is never made, the others are as they were.
function(vc3_patch_options_rows generated_dir)
    _vc3_unit("${generated_dir}" 08AC53B4 file)
    if(NOT EXISTS "${file}")
        message(WARNING "VC3 OPTIONS rows: ${file} not found")
        return()
    endif()
    file(READ "${file}" text)
    set(original "${text}")
    foreach(label IN ITEMS L_08AC53B4 L_08AC56C0)
        set(old "${label}:\n    ctx.gpr[4] = (static_cast<std::int32_t>(ctx.gpr[30]) < 6 ? 1u : 0u);")
        set(new "${label}:\n    ctx.gpr[4] = (static_cast<std::int32_t>(ctx.gpr[30]) < static_cast<std::int32_t>(vc3_options_rows(6u)) ? 1u : 0u);")
        string(FIND "${text}" "${new}" done)
        if(NOT done EQUAL -1)
            continue()
        endif()
        string(FIND "${text}" "${old}" at)
        if(at EQUAL -1)
            message(WARNING "VC3 OPTIONS rows site ${label} not found (the corpus may differ)")
            continue()
        endif()
        string(REPLACE "${old}" "${new}" text "${text}")
    endforeach()
    _vc3_add_declaration(text "extern \"C\" std::uint32_t vc3_options_rows(std::uint32_t rows) noexcept;  // vc3 OPTIONS rows site")
    if(NOT text STREQUAL original)
        file(WRITE "${file}" "${text}")
        message(STATUS "VC3 OPTIONS rows (no Installed Data): patched")
    endif()
endfunction()

# Button glyphs in rewritten text (host/vc3_text.cpp, as VC2's): the font's
# glyph staging (VC3 0x08BBA26C, VC2 0x08BC95A4; a0 font, a1 flags, a2 code,
# a3 table) calls vc3_glyph_begin on entry and vc3_glyph_end where it returns
# 1 with the cell complete (VC3 L_08BBA590, VC2 L_08BC98C8; t2 = font).
function(vc3_patch_glyph_sites generated_dir)
    set(declaration "extern \"C\" void vc3_glyph_begin(std::uint32_t font, std::uint32_t flags, std::uint32_t code, std::uint32_t table) noexcept;\nextern \"C\" void vc3_glyph_end(std::uint32_t font) noexcept;  // vc3 glyph sites")
    vc3_patch_hook("${generated_dir}" L_08BBA26C "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-16));"
        "vc3_glyph_begin(ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]);" "${declaration}" "glyph staging begin site")
    vc3_patch_hook("${generated_dir}" L_08BBA590 "ctx.gpr[2] = (0u | 1u);"
        "vc3_glyph_end(ctx.gpr[10]);" "${declaration}" "glyph staging end site")
endfunction()
