// VC2 native GU layer (native-rendering plan, phase 1).
// See vc3_native_gu.cpp and research/ENGINE_MAP_2026-11-06.md.

#pragma once

#include "hle/hle_common.hpp"

#include <array>
#include <cstdint>
#include <cstddef>

namespace portablekit::vc3 {

// The GU state most recently requested by the game through the replaced entry
// points, kept on the host. This is what a native draw will be built from.
// Fields hold the game's arguments as given (the GE encodings are derived from
// them), so they read the way the game's code asked for them.
struct GuMirror {
    struct AlphaTest { std::uint32_t function = 0u, reference = 0u, mask = 0u; } alpha_test;
    std::uint32_t depth_function = 0u;
    std::uint32_t depth_mask = 0u;     // 1 = depth writes disabled
    std::uint32_t pixel_mask = 0u;     // set bits are NOT written (RGB low 24, alpha high 8)
    struct StencilTest { std::uint32_t function = 0u, reference = 0u, mask = 0u; } stencil_test;
    struct StencilOp { std::uint32_t fail = 0u, depth_fail = 0u, pass = 0u; } stencil_op;
    struct Blend { std::uint32_t op = 0u, source = 0u, destination = 0u, fixed_a = 0u, fixed_b = 0u; } blend;
    struct TextureFilter { std::uint32_t minify = 0u, magnify = 0u; } texture_filter;
    struct TextureWrap { std::uint32_t u = 0u, v = 0u; } texture_wrap;
    float texture_scale_u = 1.0f, texture_scale_v = 1.0f;
    float texture_offset_u = 0.0f, texture_offset_v = 0.0f;
    std::uint32_t screen_offset_x = 0u, screen_offset_y = 0u;  // in pixels
    struct ClutMode { std::uint32_t format = 0u, shift = 0u, mask = 0u, start = 0u; } clut_mode;
    struct ClutLoad { std::uint32_t blocks = 0u, address = 0u; } clut_load;
    std::uint32_t ambient = 0u;  // ABGR
    std::uint64_t texture_flushes = 0u;
    // Matrix slots as the game sets them: 0 projection (4x4), 1 view, 2 model,
    // 3 texture (3x4 each, stored as 4x4 with the implicit last column).
    std::array<std::array<float, 16>, 4> matrices{};
    std::array<std::array<float, 12>, 8> bones{};
    std::uint32_t last_bone = 0u;
    std::uint64_t draws = 0u;

    // Enabled states, bit n = GU state n (0 alpha test, 1 depth test, 2 scissor,
    // 3 stencil, 4 blend, 5 cull, 6 dither, 7 fog, 8 clip planes, 9 texture,
    // 10 lighting, 11-14 lights, 15 line smooth, 16 patch cull, 17 colour test,
    // 18 logic op, 19 normal reverse, 20 patch face, 21 fragment 2x).
    std::uint32_t enabled = 0u;
    struct TextureLevel { std::uint32_t width = 0u, height = 0u, buffer_width = 0u, address = 0u; };
    std::array<TextureLevel, 8> texture_levels{};
    struct TextureMode { std::uint32_t format = 0u, max_mips = 0u, multi_clut = 0u, swizzle = 0u; } texture_mode;
    struct TextureMapMode { std::uint32_t mode = 0u, light_u = 0u, light_v = 0u; } texture_map_mode;
    struct TextureFunction { std::uint32_t function = 0u, alpha = 0u; } texture_function;
    struct TextureLevelMode { std::uint32_t mode = 0u; float bias = 0.0f; } texture_level_mode;
    float texture_slope = 0.0f;
    struct Fog { float near_z = 0.0f, far_z = 0.0f; std::uint32_t color = 0u; } fog;
    struct Rect { std::int32_t x = 0, y = 0, width = 0, height = 0; } scissor;
    std::array<float, 8> morph_weights{};
    struct LightColors { std::uint32_t ambient = 0u, diffuse = 0u, specular = 0u; };
    std::array<LightColors, 4> light_colors{};
    struct Light { std::uint32_t type = 0u, components = 0u; std::array<float, 3> position{}; };
    std::array<Light, 4> lights{};
    std::uint32_t light_mode = 0u;
    struct Material { std::uint32_t emissive = 0u, ambient = 0u, diffuse = 0u, specular = 0u; } material;
    std::uint32_t color_material = 0u;
    std::uint32_t front_face = 0u;
    std::uint32_t shade_model = 0u;
    std::array<std::int32_t, 16> dither{};
    struct DrawBuffer { std::uint32_t format = 0u, address = 0u, width = 0u; } draw_buffer;
    struct DepthBuffer { std::uint32_t address = 0u, width = 0u; } depth_buffer;
    struct Viewport { std::int32_t center_x = 0, center_y = 0, width = 0, height = 0; } viewport;
    struct DepthRange { std::int32_t near_z = 0, far_z = 0, offset = 0; } depth_range;
    struct Clear { std::uint32_t color = 0u, depth = 0u, stencil = 0u; } clear;
    std::array<std::uint32_t, 4> blit_offsets{};  // HUD blitter extents: u, v, x, y spans (context +188..+200)

    // Values as last written to the hardware registers, where the library
    // derives them (or the GE keeps advancing them) rather than taking them
    // from one call's arguments.
    std::uint32_t vertex_type = 0u;
    std::uint32_t vertex_address = 0u;  // advances past each non-indexed draw
    std::uint32_t index_address = 0u;   // advances past each indexed draw
    std::uint32_t scissor_start = 0u;   // D4 data: y1 << 10 | x1
    std::uint32_t scissor_end = 0u;     // D5 data: y2 << 10 | x2
    std::uint32_t depth_min = 0u, depth_max = 0xFFFFu;  // D6, D7
    float depth_scale = 0.0f, depth_translate = 0.0f;   // 44, 47 (full precision)
    float fog_inverse = 0.0f;                           // CE
    std::uint32_t clut_load_bytes = 0u, clut_max_bytes = 0u;
    // drawBufferList sets the target only for the rest of the list it is
    // called in: each list start re-sends the default from the GU state block.
    // Where and in which list the last explicit call was made.
    DrawBuffer explicit_draw_buffer;
    std::uint32_t explicit_draw_buffer_list = 0u, explicit_draw_buffer_write = 0u;
};

const GuMirror &gu_mirror();

// Registers the selected host replacements (VC3_NATIVE_GU=all or a comma list
// of names). Called from the profile's patch_loaded_image hook, which runs
// after the generated corpus is registered.
void install_native_gu(Runtime &rt);

// Logs replacement call counts every 600 flips while VC3_NATIVE_GU is set.
void register_native_gu_report(HleRegistrar &hle);

// GameProfile::view_aspect_frame: the shape of the picture the game is drawn
// into (video.aspect=fill gives it the window's), once per flip. Native draws
// into a shown framebuffer are fitted to it (Hor+ for the 3D view).
void view_aspect_frame(Runtime &rt, float aspect);

// The title menu's SETTINGS (the old INSTALL slot, repainted from the menu's
// own letters through the texture override): installed after the prompts,
// whose override it passes everything else on to.
void install_title_settings();
// The title menu was on screen within the last two seconds (Discord status).
[[nodiscard]] bool title_menu_shown();
// The game began writing a save (sceUtilitySavedataInitStart, a write mode):
// the saving indicator shows for a moment (the PSP lit its Memory Stick LED).
void note_save_write();

}  // namespace portablekit::vc3
