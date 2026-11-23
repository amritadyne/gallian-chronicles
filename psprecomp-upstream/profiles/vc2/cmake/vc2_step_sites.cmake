# Patches VC2's generated code at the places that turn the game's step into a
# whole number (trunc.w.s of a value made from the step at EU 0x08D5B934):
# each becomes a call to vc2_step_trunc() (host/vc2_tick.cpp), which keeps a
# running remainder per site when the frame clock runs faster than 59.94 Hz, so
# a frame counter that used to add (int)step = 1 a frame does not add 0 at
# 165 Hz (the Morale meter stalled the start of a battle, 2026-11-08). With the
# normal clock it truncates exactly as the instruction did.
#
# Runs at configure time over profiles/vc2/generated (never committed: this
# edits the generated copy in place, and running it again changes nothing).
# A newly generated corpus is patched at the next configure.
# The sites were found by disassembling every read of the step (213) and
# keeping those followed within 14 instructions by trunc.w.s.
#
# Frame-locked additions (found with the frame census at 165 Hz, 2026-11-08):
# `x += delta` once per frame with a delta that ignores the step. Each becomes
# x += vc2_frame_scale(pc, delta), which scales the delta by the step over the
# frame interval the game asked for (1, or 2 in combat), and leaves it as it is
# at the game's own rate:
#   08893C48  unit timer: unit+2212 += 1.0
#   08C5953C, 08C59550, 08C59584  the animation player (0x08C592E4): track and
#             animation time += the controller's per-frame speed. Still patched
#             (for the call counts) but no longer scaled (vc2_tick.cpp): the
#             speed is already per step since the frame-rate module fix, and
#             scaling it twice made the academy map's water vanish on a cycle
#   08A4C170, 08A4C178  the academy map's cursor (stick read at 0x08A4C0E8): x, y += stick
#             x 8.0 a frame, clamped to +-240 / +-136; the map camera pans
#             after it (it reads the cursor at +192/+196, 0x08A5D240), so it
#             panned 5.5x fast at 165 Hz (found 2026-11-08)
#   088FDAB0  interception fire (2026-11-23): while a unit moves, each enemy
#             that can see it adds the move state's rate to its fire timer
#             (+0x850) once a frame and fires a shot each time the timer
#             passes the weapon's interval (SysStateMove, 0x088FD744). Only in
#             one versus mode does the game multiply the rate by the step
#             (0x088FD80C); otherwise it is per frame, so at 165 Hz in combat
#             enemies fired 5.5 times as often. vc2_patch_intercept_sites marks
#             which way the rate was made; vc2_frame_scale scales only the
#             per-frame one (host/vc2_tick.cpp).
set(_vc2_frame_add_sites 08893C48 08C5953C 08C59550 08C59584 08A4C170 08A4C178 088FDAB0)
#
# Frame-locked counters: `n += k` (whole numbers) once per frame. Each becomes
# n += vc2_frame_count(pc, k): the same scale, with a running remainder per
# site, so it adds k x step / interval on average:
#   088BFCC8  per unit: unit+2096 += 1 (likely the characters' animation frame)
#   08BD2EAC  the main loop's frame counter: +308 += 1 (EU 0x08D0287C)
set(_vc2_frame_count_sites 088BFCC8 08BD2EAC)

set(_vc2_step_sites
    08813600 08886958 0888B8B0 0888F500 0889DEF8 088ACEF0 088AECE4 088BCC0C
    0890D84C 0891B8EC 0892E6AC 0893F04C 08941778 0896EC90 0896ECB4 0897A03C
    0897ACE0 0897C2C0 089A2468 089AC8DC 089ACB04 089ACB40 08A54438 08B09A9C
    08B60068 08B60164)

# Editions. Every address here is the European release's. VC2_EDITION, set by
# the caller (profiles/vc2/CMakeLists.txt), says whose corpus is patched: "eu"
# (generated/), "us" (generated-variants/us/, the US release's corpus
# library) or "jp" (generated-variants/jp/, the Japanese release's: its
# addresses are a table, below). The US executable is the European one shifted in three ranges
# (research/REGIONS.md, host/vc2_region.cpp kUsaShifts), so a site's label,
# its unit and the return addresses in its code are the shifted ones; the
# calls still pass the European address (`pc`), which the profile's tables
# use. In the US library the calls reach the profile through
# host/vc2_corpus_shim.cpp.
if(NOT DEFINED VC2_EDITION)
    set(VC2_EDITION eu)
endif()
# Japan's (NPJH50145) is a separate build: its addresses are a table, not
# ranges (vc2_jp_addresses.cmake; host/vc2_region.cpp kJapan holds the same).
include("${CMAKE_CURRENT_LIST_DIR}/vc2_jp_addresses.cmake")

# The edition's address for a European one (8 hex digits, no 0x, as the
# generated code's labels spell them).
function(vc2_edition_hex eu out)
    math(EXPR a "0x${eu}")
    if(VC2_EDITION STREQUAL "us")
        if(a GREATER_EQUAL 144889572 AND a LESS 144889804)        # 0x08A2D6E4 .. 0x08A2D7CC
            math(EXPR a "${a} + 8")
        elseif(a GREATER_EQUAL 144889804 AND a LESS 147234168)    # .. 0x08C69D78
            math(EXPR a "${a} + 24")
        elseif(a GREATER_EQUAL 147234168 AND a LESS 148072960)    # .. 0x08D36A00
            math(EXPR a "${a} + 16")
        endif()
    elseif(VC2_EDITION STREQUAL "jp")
        string(TOUPPER "${eu}" key)
        set(found "")
        foreach(pair IN LISTS _vc2_jp_addresses)
            if(pair MATCHES "^${key}=([0-9A-F]+)$")
                set(found "${CMAKE_MATCH_1}")
                break()
            endif()
        endforeach()
        if(found STREQUAL "")
            message(WARNING "VC2: no Japanese address for EU ${eu} (cmake/vc2_jp_addresses.cmake)")
            set(${out} "00000000" PARENT_SCOPE)
            return()
        endif()
        math(EXPR a "0x${found}")
    endif()
    math(EXPR a "${a}" OUTPUT_FORMAT HEXADECIMAL)
    string(SUBSTRING "${a}" 2 -1 a)
    string(TOUPPER "${a}" a)
    string(LENGTH "${a}" digits)
    while(digits LESS 8)
        set(a "0${a}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(${out} "${a}" PARENT_SCOPE)
endfunction()

# The generated unit that holds a (European) address in this edition's corpus.
function(vc2_edition_unit generated_dir eu out)
    vc2_edition_hex("${eu}" a)
    math(EXPR unit "(0x${a} - 0x08804000) / 16384")
    string(LENGTH "${unit}" digits)
    while(digits LESS 4)
        set(unit "0${unit}")
        math(EXPR digits "${digits} + 1")
    endwhile()
    set(${out} "${generated_dir}/generated_unit_${unit}.cpp" PARENT_SCOPE)
endfunction()

# Text with European code addresses (L_08xxxxxx labels, 0x08xxxxxxu
# constants) given this edition's.
function(vc2_edition_text text out)
    if(VC2_EDITION STREQUAL "eu")
        set(${out} "${text}" PARENT_SCOPE)
        return()
    endif()
    set(hex "[0-9A-F]")
    string(REGEX MATCHALL "08${hex}${hex}${hex}${hex}${hex}${hex}" found "${text}")
    list(REMOVE_DUPLICATES found)
    set(i 0)
    foreach(eu IN LISTS found)
        string(REPLACE "${eu}" "@vc2:${i}@" text "${text}")
        math(EXPR i "${i} + 1")
    endforeach()
    set(i 0)
    foreach(eu IN LISTS found)
        vc2_edition_hex("${eu}" mapped)
        string(REPLACE "@vc2:${i}@" "${mapped}" text "${text}")
        math(EXPR i "${i} + 1")
    endforeach()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(vc2_patch_step_sites generated_dir)
    # (Separate variables: the lines hold semicolons, which a CMake list splits.)
    set(declaration_1 "extern \"C\" std::uint32_t vc2_step_trunc(std::uint32_t pc, float value) noexcept;  // vc2 step sites")
    set(declaration_2 "extern \"C\" float vc2_frame_scale(std::uint32_t pc, float value) noexcept;")
    set(declaration_3 "extern \"C\" std::uint32_t vc2_frame_count(std::uint32_t pc, std::uint32_t amount) noexcept;")
    set(patched 0)
    set(already 0)
    set(missing "")
    set(by_file "")
    foreach(pc IN LISTS _vc2_step_sites _vc2_frame_add_sites _vc2_frame_count_sites)
        vc2_edition_hex("${pc}" site)
        math(EXPR unit "(0x${site} - 0x08804000) / 16384")
        string(LENGTH "${unit}" digits)
        set(padded "${unit}")
        while(digits LESS 4)
            set(padded "0${padded}")
            math(EXPR digits "${digits} + 1")
        endwhile()
        set(file "${generated_dir}/generated_unit_${padded}.cpp")
        list(APPEND by_file "${file}")
        set_property(GLOBAL APPEND PROPERTY _vc2_sites_${VC2_EDITION}_${padded} "${pc}")
    endforeach()
    list(REMOVE_DUPLICATES by_file)
    foreach(file IN LISTS by_file)
        if(NOT EXISTS "${file}")
            list(APPEND missing "${file}")
            continue()
        endif()
        get_filename_component(name "${file}" NAME_WE)
        string(REGEX REPLACE "^generated_unit_" "" padded "${name}")
        get_property(pcs GLOBAL PROPERTY _vc2_sites_${VC2_EDITION}_${padded})
        file(READ "${file}" text)
        set(original "${text}")
        foreach(pc IN LISTS pcs)
            vc2_edition_hex("${pc}" site)  # its label in this edition; the call passes the European pc
            list(FIND _vc2_frame_count_sites "${pc}" count_index)
            if(NOT count_index EQUAL -1)
                string(FIND "${text}" "vc2_frame_count(0x${pc}u," done)
                if(NOT done EQUAL -1)
                    math(EXPR already "${already} + 1")
                    continue()
                endif()
                string(REGEX MATCH "L_${site}:\n    ctx\\.gpr\\[([0-9]+)\\] = \\(ctx\\.gpr\\[([0-9]+)\\] \\+ static_cast<std::uint32_t>\\(([0-9]+)\\)\\);" line "${text}")
                if(line STREQUAL "")
                    list(APPEND missing "0x${pc}")
                    continue()
                endif()
                set(replacement "L_${site}:\n    ctx.gpr[${CMAKE_MATCH_1}] = (ctx.gpr[${CMAKE_MATCH_2}] + vc2_frame_count(0x${pc}u, ${CMAKE_MATCH_3}u));")
                string(REPLACE "${line}" "${replacement}" text "${text}")
                math(EXPR patched "${patched} + 1")
                continue()
            endif()
            list(FIND _vc2_frame_add_sites "${pc}" add_index)
            if(NOT add_index EQUAL -1)
                string(FIND "${text}" "vc2_frame_scale(0x${pc}u," done)
                if(NOT done EQUAL -1)
                    math(EXPR already "${already} + 1")
                    continue()
                endif()
                string(REGEX MATCH "L_${site}:\n    ctx\\.fpr\\[([0-9]+)\\] = ctx\\.fpr\\[([0-9]+)\\] \\+ ctx\\.fpr\\[([0-9]+)\\];" line "${text}")
                if(line STREQUAL "")
                    list(APPEND missing "0x${pc}")
                    continue()
                endif()
                set(replacement "L_${site}:\n    ctx.fpr[${CMAKE_MATCH_1}] = ctx.fpr[${CMAKE_MATCH_2}] + vc2_frame_scale(0x${pc}u, ctx.fpr[${CMAKE_MATCH_3}]);")
                string(REPLACE "${line}" "${replacement}" text "${text}")
                math(EXPR patched "${patched} + 1")
                continue()
            endif()
            string(FIND "${text}" "vc2_step_trunc(0x${pc}u," done)
            if(NOT done EQUAL -1)
                math(EXPR already "${already} + 1")
                continue()
            endif()
            string(REGEX MATCH "L_${site}:\n    ctx\\.fpr\\[([0-9]+)\\] = std::bit_cast<float>\\(ctx\\.fpu_float_to_word_ct<1u>\\(ctx\\.fpr\\[([0-9]+)\\]\\)\\);" line "${text}")
            if(line STREQUAL "")
                list(APPEND missing "0x${pc}")
                continue()
            endif()
            set(replacement "L_${site}:\n    ctx.fpr[${CMAKE_MATCH_1}] = std::bit_cast<float>(vc2_step_trunc(0x${pc}u, ctx.fpr[${CMAKE_MATCH_2}]));")
            string(REPLACE "${line}" "${replacement}" text "${text}")
            math(EXPR patched "${patched} + 1")
        endforeach()
        # The declarations, each added once, after the corpus includes (a file
        # patched by an earlier version gets the ones it lacks).
        if(NOT text STREQUAL original)
            set(missing_declarations "")
            foreach(name IN ITEMS declaration_1 declaration_2 declaration_3)
                string(FIND "${text}" "${${name}}" found)
                if(found EQUAL -1)
                    string(APPEND missing_declarations "${${name}}\n")
                endif()
            endforeach()
            if(NOT missing_declarations STREQUAL "")
                string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${missing_declarations}" text "${text}")
            endif()
        endif()
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
        endif()
    endforeach()
    list(LENGTH _vc2_step_sites total_trunc)
    list(LENGTH _vc2_frame_add_sites total_add)
    list(LENGTH _vc2_frame_count_sites total_count)
    math(EXPR total "${total_trunc} + ${total_add} + ${total_count}")
    message(STATUS "VC2 step sites: ${patched} patched now, ${already} already patched, of ${total}")
    if(missing)
        message(WARNING "VC2 step sites not found (the corpus may differ): ${missing}")
    endif()
endfunction()

# Level of detail (host/vc2_lod.cpp). The render-parameter loader 0x08B7E99C
# copies a version 2 block whole (memcpy call at EU 0x08B7EA58): the
# destination is noted before the call and the switch distances scaled after
# it returns (0x08B7EA60). A version 1 block goes through 0x08B7E730, whose
# store of each distance (`swc1 f12, 896(t0)` at EU 0x08B7E874) goes through
# vc2_lod_distance(). Each patch is applied once.
function(vc2_patch_lod_site generated_dir)
    vc2_edition_unit("${generated_dir}" 08B7E874 file)
    if(NOT EXISTS "${file}")
        message(WARNING "VC2 LOD sites: ${file} not found")
        return()
    endif()
    file(READ "${file}" text)
    set(original "${text}")
    set(found_all TRUE)
    set(patch_names store begin end)
    set(store_old "L_08B7E874:\n    aot_mem.aot_store32(ctx.gpr[8] + static_cast<std::uint32_t>(896), std::bit_cast<std::uint32_t>(ctx.fpr[12]));")
    set(store_new "L_08B7E874:\n    aot_mem.aot_store32(ctx.gpr[8] + static_cast<std::uint32_t>(896), std::bit_cast<std::uint32_t>(vc2_lod_distance(ctx.gpr[8] + 896u, ctx.fpr[12])));")
    set(begin_old "L_08B7EA58:\n    ctx.gpr[31] = (0x08B7EA60u);")
    set(begin_new "L_08B7EA58:\n    vc2_lod_params_begin(ctx.gpr[4]);\n    ctx.gpr[31] = (0x08B7EA60u);")
    set(end_old "L_08B7EA60:\n    { const bool branch_taken = 0u == 0u;")
    set(end_new "L_08B7EA60:\n    vc2_lod_params_end();\n    { const bool branch_taken = 0u == 0u;")
    foreach(name IN LISTS patch_names)
        vc2_edition_text("${${name}_old}" ${name}_old)
        vc2_edition_text("${${name}_new}" ${name}_new)
        string(FIND "${text}" "${${name}_new}" done)
        if(NOT done EQUAL -1)
            continue()
        endif()
        string(FIND "${text}" "${${name}_old}" at)
        if(at EQUAL -1)
            set(found_all FALSE)
            message(WARNING "VC2 LOD site '${name}' not found (the corpus may differ)")
            continue()
        endif()
        string(REPLACE "${${name}_old}" "${${name}_new}" text "${text}")
    endforeach()
    set(lod_declaration_1 "extern \"C\" float vc2_lod_distance(std::uint32_t address, float value) noexcept;  // vc2 LOD site")
    set(lod_declaration_2 "extern \"C\" void vc2_lod_params_begin(std::uint32_t destination) noexcept;")
    set(lod_declaration_3 "extern \"C\" void vc2_lod_params_end() noexcept;")
    foreach(name IN ITEMS lod_declaration_1 lod_declaration_2 lod_declaration_3)
        string(FIND "${text}" "${${name}}" have)
        if(have EQUAL -1)
            string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${${name}}\n" text "${text}")
        endif()
    endforeach()
    if(NOT text STREQUAL original)
        file(WRITE "${file}" "${text}")
        message(STATUS "VC2 LOD sites: patched")
    else()
        message(STATUS "VC2 LOD sites: already patched")
    endif()
endfunction()

# Action Mode camera (host/vc2_camera.cpp): once the game's own turn of the
# moved unit's camera is done (0x0888FC94, EU L_0888FECC on the usual path in
# unit 0034, L_08890028 on the other in unit 0035), vc2_camera_turn(unit)
# adds what the right stick and the mouse asked for. Each site is patched once.
# One hook site: in generated_unit_<unit>.cpp, `call` goes first after `label`
# (whose first line must read `first_line`), once, and `declaration` after
# the units header.
function(vc2_patch_hook generated_dir unit label first_line call declaration what)
    # `unit` is the European one, kept for reading; the edition's comes from the label.
    string(SUBSTRING "${label}" 2 -1 eu)
    vc2_edition_unit("${generated_dir}" "${eu}" file)
    vc2_edition_text("${label}" label)
    vc2_edition_text("${first_line}" first_line)
    if(NOT EXISTS "${file}")
        message(WARNING "VC2 ${what}: ${file} not found")
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
            message(WARNING "VC2 ${what} ${label} not found (the corpus may differ)")
            return()
        endif()
        string(REPLACE "${label}:\n    ${first_line}" "${label}:\n    ${call}\n    ${first_line}" text "${text}")
    endif()
    string(FIND "${text}" "${declaration}" have)
    if(have EQUAL -1)
        string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${declaration}\n" text "${text}")
    endif()
    if(NOT text STREQUAL original)
        file(WRITE "${file}" "${text}")
        message(STATUS "VC2 ${what} ${label}: patched")
    endif()
endfunction()

# Interception fire (see 088FDAB0 above): the move state makes its rate at
# 0x088FD7DC (per frame) and multiplies it by the step at 0x088FD80C only in a
# versus mode. Each site tells vc2_frame_scale which it was (value 0, result
# unused), so the rate added at 088FDAB0 is scaled once, and only when the
# game did not already scale it.
function(vc2_patch_intercept_sites generated_dir)
    set(declaration "extern \"C\" float vc2_frame_scale(std::uint32_t pc, float value) noexcept;")
    vc2_patch_hook("${generated_dir}" 0062 L_088FD7DC
        "ctx.fpr[22] = ctx.fpr[12] / ctx.fpr[22];"
        "(void)vc2_frame_scale(0x088FD7DCu, 0.0f);  // interception rate made per frame"
        "${declaration}"
        "interception rate site")
    vc2_patch_hook("${generated_dir}" 0062 L_088FD80C
        "{ const float fs = ctx.fpr[22]; const float ft = ctx.fpr[12];"
        "(void)vc2_frame_scale(0x088FD80Cu, 0.0f);  // interception rate times the step (versus)"
        "${declaration}"
        "interception step site")
    # Interception fire's pause (2026-11-23, found in VC3 with the trace-build
    # swap hunt; the same code here): the countdown at +0x1BB4 runs down by the
    # frame's whole steps, at least 1 (trunc(step) at step site 0x0889DEF8,
    # then `if <= 0: 1` at 0x0889DF08). At a high frame rate that made it 1
    # every frame, so enemies fired almost without a pause. At the join
    # (0x0889DF0C) the amount becomes the step site's own whole steps (f12), 0
    # included; at the game's rate the step is 1 or 2 and nothing changes.
    vc2_patch_hook("${generated_dir}" 0038 L_0889DF0C
        "ctx.gpr[5] = (aot_mem.aot_load32(ctx.gpr[16] + static_cast<std::uint32_t>(7092)));"
        "ctx.gpr[4] = std::bit_cast<std::uint32_t>(ctx.fpr[12]);  // interception pause: the whole steps, unclamped"
        "#include <bit>"
        "interception pause site")
    # ... and each unit counts its own whole steps (the site's remainder is
    # shared by every unit that calls it in a frame): vc2_pause_steps.
    vc2_patch_hook("${generated_dir}" 0038 L_0889DF10
        "ctx.gpr[4] = (ctx.gpr[5] - ctx.gpr[4]);"
        "ctx.gpr[4] = vc2_pause_steps(ctx.gpr[16], ctx.gpr[4]);"
        "extern \"C\" std::uint32_t vc2_pause_steps(std::uint32_t object, std::uint32_t whole) noexcept;"
        "interception pause steps site")
    # Burst fire (2026-11-23, VC3-tested): the attack state's phases and shots
    # counted in updates (vc2_tick.cpp vc2_attack_hold / _done / _ready).
    vc2_patch_hook("${generated_dir}" 0066 L_0890D52C
        "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-1712));"
        "if (vc2_attack_hold(ctx.gpr[4])) { ctx.pc = ctx.gpr[31]; return; }"
        "extern \"C\" bool vc2_attack_hold(std::uint32_t state) noexcept;"
        "attack phase site")
    vc2_patch_hook("${generated_dir}" 0066 L_0890FE28
        "ctx.fpr[20] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(1664)));"
        "vc2_attack_done();"
        "extern \"C\" void vc2_attack_done() noexcept;"
        "attack phase end site")
    vc2_patch_hook("${generated_dir}" 0066 L_0890F678
        "{ const bool branch_taken = ctx.gpr[2] == 0u;"
        "ctx.gpr[2] = vc2_attack_ready(ctx.gpr[16], ctx.gpr[2]);"
        "extern \"C\" std::uint32_t vc2_attack_ready(std::uint32_t state, std::uint32_t ready) noexcept;"
        "burst fire site")
    # The battle manager's frame (0x088D1974, one exit 0x088D1AB4): at a high
    # frame rate once per game frame of time (vc2_manager_hold / _done).
    vc2_patch_hook("${generated_dir}" 0051 L_088D1974
        "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-32));"
        "if (vc2_manager_hold()) { ctx.pc = ctx.gpr[31]; return; }"
        "extern \"C\" bool vc2_manager_hold() noexcept;"
        "battle manager site")
    vc2_patch_hook("${generated_dir}" 0051 L_088D1AB4
        "ctx.gpr[16] = (aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(0)));"
        "vc2_manager_done();"
        "extern \"C\" void vc2_manager_done() noexcept;"
        "battle manager end site")
endfunction()

function(vc2_patch_camera_sites generated_dir)
    set(declaration "extern \"C\" void vc2_camera_turn(std::uint32_t unit) noexcept;  // vc2 camera site")
    foreach(site IN ITEMS "0034:0888FECC:0888FED0" "0035:08890028:0889002C")
        string(REPLACE ":" ";" parts "${site}")
        list(GET parts 0 unit)
        list(GET parts 1 label)
        list(GET parts 2 next)
        vc2_edition_unit("${generated_dir}" "${label}" file)
        if(NOT EXISTS "${file}")
            message(WARNING "VC2 camera site: ${file} not found")
            continue()
        endif()
        file(READ "${file}" text)
        set(original "${text}")
        set(old "L_${label}:\n    ctx.gpr[18] = (aot_mem.aot_load32(ctx.gpr[18] + static_cast<std::uint32_t>(72)));\n    goto L_${next};")
        set(new "L_${label}:\n    vc2_camera_turn(ctx.gpr[16]);\n    ctx.gpr[18] = (aot_mem.aot_load32(ctx.gpr[18] + static_cast<std::uint32_t>(72)));\n    goto L_${next};")
        vc2_edition_text("${old}" old)
        vc2_edition_text("${new}" new)
        string(FIND "${text}" "${new}" done)
        if(done EQUAL -1)
            string(FIND "${text}" "${old}" at)
            if(at EQUAL -1)
                message(WARNING "VC2 camera site L_${label} not found (the corpus may differ)")
                continue()
            endif()
            string(REPLACE "${old}" "${new}" text "${text}")
        endif()
        string(FIND "${text}" "${declaration}" have)
        if(have EQUAL -1)
            string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${declaration}\n" text "${text}")
        endif()
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
            message(STATUS "VC2 camera site L_${label}: patched")
        endif()
    endforeach()
    # Before the game reads its camera buttons, on each path (unit 0034):
    # vc2_camera_begin(unit) notes the angles, so Modern controls can undo the
    # game's own button turning (host/vc2_camera.cpp).
    set(begin_declaration "extern \"C\" void vc2_camera_begin(std::uint32_t unit) noexcept;  // vc2 camera begin site")
    foreach(site IN ITEMS "0888FD8C:4:0888FD90" "0888FF00:5:0888FF04")
        string(REPLACE ":" ";" parts "${site}")
        list(GET parts 0 label)
        list(GET parts 1 reg)
        list(GET parts 2 next)
        vc2_edition_unit("${generated_dir}" "${label}" file)
        if(NOT EXISTS "${file}")
            message(WARNING "VC2 camera begin site: ${file} not found")
            continue()
        endif()
        file(READ "${file}" text)
        set(original "${text}")
        set(old "L_${label}:\n    ctx.gpr[${reg}] = (aot_mem.aot_load32(ctx.gpr[18] + static_cast<std::uint32_t>(72)));\n    goto L_${next};")
        set(new "L_${label}:\n    vc2_camera_begin(ctx.gpr[16]);\n    ctx.gpr[${reg}] = (aot_mem.aot_load32(ctx.gpr[18] + static_cast<std::uint32_t>(72)));\n    goto L_${next};")
        vc2_edition_text("${old}" old)
        vc2_edition_text("${new}" new)
        string(FIND "${text}" "${new}" done)
        if(done EQUAL -1)
            string(FIND "${text}" "${old}" at)
            if(at EQUAL -1)
                message(WARNING "VC2 camera begin site L_${label} not found (the corpus may differ)")
                continue()
            endif()
            string(REPLACE "${old}" "${new}" text "${text}")
        endif()
        string(FIND "${text}" "${begin_declaration}" have)
        if(have EQUAL -1)
            string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${begin_declaration}\n" text "${text}")
        endif()
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
            message(STATUS "VC2 camera begin site L_${label}: patched")
        endif()
    endforeach()
    # Target Mode aim: right after the reticle controller (0x08B09910, unit
    # 0193) reads its stick, vc2_aim_stick(sp, reticle) adds the right stick
    # and mouse (host/vc2_camera.cpp).
    set(aim_declaration "extern \"C\" void vc2_aim_stick(std::uint32_t sp, std::uint32_t obj) noexcept;  // vc2 aim site")
    vc2_edition_unit("${generated_dir}" 08B09990 file)
    if(EXISTS "${file}")
        file(READ "${file}" text)
        set(original "${text}")
        set(old "L_08B09990:\n    ctx.gpr[31] = (0x08B09998u);")
        set(new "L_08B09990:\n    vc2_aim_stick(ctx.gpr[29], ctx.gpr[16]);\n    ctx.gpr[31] = (0x08B09998u);")
        vc2_edition_text("${old}" old)
        vc2_edition_text("${new}" new)
        string(FIND "${text}" "${new}" done)
        if(done EQUAL -1)
            string(FIND "${text}" "${old}" at)
            if(at EQUAL -1)
                message(WARNING "VC2 aim site L_08B09990 not found (the corpus may differ)")
            else()
                string(REPLACE "${old}" "${new}" text "${text}")
            endif()
        endif()
        string(FIND "${text}" "${aim_declaration}" have)
        if(have EQUAL -1)
            string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${aim_declaration}\n" text "${text}")
        endif()
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
            message(STATUS "VC2 aim site L_08B09990: patched")
        endif()
    else()
        message(WARNING "VC2 aim site: ${file} not found")
    endif()
    # Target Mode aim: before the aim code scales its nub by the step (EU
    # L_088E1D60, unit 0055), vc2_aim_target(&x, &y, step) adds the right stick
    # and mouse (host/vc2_camera.cpp).
    set(target_declaration "extern \"C\" void vc2_aim_target(float *x, float *y, float step) noexcept;  // vc2 target aim site")
    vc2_edition_unit("${generated_dir}" 088E1D60 file)
    if(EXISTS "${file}")
        file(READ "${file}" text)
        set(original "${text}")
        set(old "L_088E1D60:\n    { const float fs = ctx.fpr[12];")
        set(new "L_088E1D60:\n    vc2_aim_target(&ctx.fpr[12], &ctx.fpr[16], ctx.fpr[13]);\n    { const float fs = ctx.fpr[12];")
        vc2_edition_text("${old}" old)
        vc2_edition_text("${new}" new)
        string(FIND "${text}" "vc2_aim_target(&ctx.fpr[12]" done)
        if(done EQUAL -1)
            string(FIND "${text}" "${old}" at)
            if(at EQUAL -1)
                message(WARNING "VC2 target aim site L_088E1D60 not found (the corpus may differ)")
            else()
                string(REPLACE "${old}" "${new}" text "${text}")
            endif()
        endif()
        string(FIND "${text}" "${target_declaration}" have)
        if(have EQUAL -1)
            string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${target_declaration}\n" text "${text}")
        endif()
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
            message(STATUS "VC2 target aim site L_088E1D60: patched")
        endif()
    else()
        message(WARNING "VC2 target aim site: ${file} not found")
    endif()
    # R&D vehicle preview camera (host/vc2_camera.cpp): the mover's arrival
    # test (0x08A78944, unit 0157; a0 = within 0.001, f12 = distance, s0 = the
    # mover) also accepts the closest approach at high frame rates.
    vc2_patch_hook("${generated_dir}" 0157 L_08A78944
        "{ const bool branch_taken = ctx.gpr[4] == 0u;"
        "ctx.gpr[4] = vc2_preview_arrived(ctx.gpr[4], ctx.fpr[12], ctx.gpr[16]);"
        "extern \"C\" std::uint32_t vc2_preview_arrived(std::uint32_t arrived, float distance, std::uint32_t mover) noexcept;  // vc2 preview camera site"
        "preview camera arrival site")
    # The title menu without Install (host/vc2_native_gu.cpp): the list
    # widget's select (0x08B0C87C, unit 0194; a0 = the list, a1 = the index)
    # steps the cursor over INSTALL while the title menu is shown.
    vc2_patch_hook("${generated_dir}" 0194 L_08B0C87C
        "ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-64));"
        "ctx.gpr[5] = vc2_title_list_select(ctx.gpr[4], ctx.gpr[5]);"
        "extern \"C\" std::uint32_t vc2_title_list_select(std::uint32_t list, std::uint32_t index) noexcept;  // vc2 title menu site"
        "title menu select site")
    # The title menu's SETTINGS (host/vc2_native_gu.cpp): its handler
    # (0x08B81A60, unit 0223; a1 = the entry chosen) runs OPTIONS for the old
    # INSTALL slot, with the in-game Settings screen asked for.
    vc2_patch_hook("${generated_dir}" 0223 L_08B81A60
        "ctx.gpr[6] = (ctx.gpr[5] < static_cast<std::uint32_t>(5) ? 1u : 0u);"
        "ctx.gpr[5] = vc2_title_choice(ctx.gpr[5]);"
        "extern \"C\" std::uint32_t vc2_title_choice(std::uint32_t index) noexcept;  // vc2 title choice site"
        "title menu choice site")
    # Fine aim (host/vc2_camera.cpp): the nub as the aim code read it, before
    # its 0.2 dead zone (L_088E1CEC), and the D-pad it read when the nub was
    # still (L_088E1DBC), which then takes the nub's inner zone.
    vc2_patch_hook("${generated_dir}" 0055 L_088E1CEC
        "ctx.fpr[12] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(0)));"
        "vc2_aim_nub(ctx.gpr[29]);"
        "extern \"C\" void vc2_aim_nub(std::uint32_t sp) noexcept;  // vc2 fine aim site"
        "fine aim nub site")
    vc2_patch_hook("${generated_dir}" 0055 L_088E1DBC
        "ctx.fpr[12] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[29] + static_cast<std::uint32_t>(16)));"
        "vc2_aim_fine(ctx.gpr[29]);"
        "extern \"C\" void vc2_aim_fine(std::uint32_t sp) noexcept;  // vc2 fine aim site"
        "fine aim D-pad site")
    # Movement trace: Action Mode's movement has read the nub (0x088D37C0,
    # L_088D388C, unit 0051; the vector is at s0).
    # (An earlier build put a one-argument call there: replaced first.)
    vc2_edition_unit("${generated_dir}" 088D388C file)
    if(EXISTS "${file}")
        file(READ "${file}" text)
        set(original "${text}")
        string(REPLACE "L_088D388C:\n    vc2_move_stick(ctx.gpr[16]);\n" "L_088D388C:\n" text "${text}")
        string(REPLACE "extern \"C\" void vc2_move_stick(std::uint32_t vector) noexcept;  // vc2 movement trace site\n" "" text "${text}")
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
            message(STATUS "VC2 movement trace site: old one-argument call removed")
        endif()
    endif()
    vc2_patch_hook("${generated_dir}" 0051 L_088D388C
        "ctx.fpr[12] = std::bit_cast<float>(aot_mem.aot_load32(ctx.gpr[16] + static_cast<std::uint32_t>(0)));"
        "vc2_move_stick(ctx.gpr[16], ctx.gpr[29]);"
        "extern \"C\" void vc2_move_stick(std::uint32_t vector, std::uint32_t sp) noexcept;  // vc2 movement trace site 2"
        "movement trace site")
    # Morale gauge at high frame rates (host/vc2_tick.cpp): the battle HUD's
    # gauge (0x0896EB00, unit 0090) moves by (int)(step x 5) or (int)(step x 4)
    # a frame (step sites 0x0896EC90 / 0x0896ECB4) and, while moving, calls a
    # gauge position of exactly 0 a morale drop. Above 165 Hz the whole-step
    # amount is 0 on some frames, so the rise from 0 at the start of a battle
    # could stand still at 0 for a frame and play the drop. vc2_morale_step
    # makes that first amount at least 1 (s6 = the position, s3 = the morale).
    foreach(label IN ITEMS L_0896EC94 L_0896ECB8)
        if(label STREQUAL "L_0896EC94")
            set(first "{ const bool branch_taken = 0u == 0u;")
        else()
            set(first "ctx.gpr[4] = (std::bit_cast<std::uint32_t>(ctx.fpr[12]));")
        endif()
        vc2_patch_hook("${generated_dir}" 0090 ${label}
            "${first}"
            "ctx.fpr[12] = std::bit_cast<float>(vc2_morale_step(std::bit_cast<std::uint32_t>(ctx.fpr[12]), ctx.gpr[22], ctx.gpr[19]));"
            "extern \"C\" std::uint32_t vc2_morale_step(std::uint32_t amount, std::uint32_t position, std::uint32_t morale) noexcept;  // vc2 morale gauge site"
            "morale gauge site")
    endforeach()
    # Button glyphs: the font's glyph staging (0x08BC95A4, unit 0241) calls
    # vc2_glyph_begin(font, flags, code, table) on entry and vc2_glyph_end(font)
    # once its buffer is complete (host/vc2_glyphs.cpp).
    set(glyph_declaration "extern \"C\" void vc2_glyph_begin(std::uint32_t font, std::uint32_t flags, std::uint32_t code, std::uint32_t table) noexcept;\nextern \"C\" void vc2_glyph_end(std::uint32_t font) noexcept;  // vc2 glyph sites")
    # Each site in its own edition's unit: the function may straddle two
    # (in the Japanese corpus the end is in the next unit).
    foreach(site IN ITEMS "begin" "end")
        if(site STREQUAL "begin")
            set(eu 08BC95A4)
            set(old "L_08BC95A4:\n    ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-16));")
            set(new "L_08BC95A4:\n    vc2_glyph_begin(ctx.gpr[4], ctx.gpr[5], ctx.gpr[6], ctx.gpr[7]);\n    ctx.gpr[29] = (ctx.gpr[29] + static_cast<std::uint32_t>(-16));")
        else()
            set(eu 08BC98C8)
            set(old "L_08BC98C8:\n    ctx.gpr[2] = (0u | 1u);")
            set(new "L_08BC98C8:\n    vc2_glyph_end(ctx.gpr[10]);\n    ctx.gpr[2] = (0u | 1u);")
        endif()
        vc2_edition_unit("${generated_dir}" ${eu} file)
        if(NOT EXISTS "${file}")
            message(WARNING "VC2 glyph ${site} site: ${file} not found")
            continue()
        endif()
        file(READ "${file}" text)
        set(original "${text}")
        vc2_edition_text("${old}" old)
        vc2_edition_text("${new}" new)
        string(FIND "${text}" "${new}" done)
        if(done EQUAL -1)
            string(FIND "${text}" "${old}" at)
            if(at EQUAL -1)
                message(WARNING "VC2 glyph ${site} site not found (the corpus may differ)")
                continue()
            endif()
            string(REPLACE "${old}" "${new}" text "${text}")
        endif()
        string(FIND "${text}" "vc2 glyph sites" have)
        if(have EQUAL -1)
            string(REPLACE "#include \"recomp_units.hpp\"\n" "#include \"recomp_units.hpp\"\n${glyph_declaration}\n" text "${text}")
        endif()
        if(NOT text STREQUAL original)
            file(WRITE "${file}" "${text}")
            message(STATUS "VC2 glyph ${site} site (EU L_${eu}): patched")
        endif()
    endforeach()
    vc2_patch_intercept_sites("${generated_dir}")
endfunction()
