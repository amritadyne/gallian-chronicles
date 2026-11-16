// VC2 native GU layer (native-rendering plan, phase 1).
//
// Every draw in Valkyria Chronicles II reaches the GPU through a statically
// linked GU-style library at 0x08C86000-0x08C89300 (research/ENGINE_MAP). Its
// entry points take plain arguments (a blend mode, a matrix, a vertex array),
// so the native renderer can be built by implementing them on the host instead
// of decoding the GE command lists they produce.
//
// This file replaces those entry points one at a time, behind VC2_NATIVE_GU,
// with host code that (for now) writes exactly the GE words the game's own code
// writes, and also keeps the requested state in a host-side mirror
// (GuMirror). Identical output keeps every step testable against the game's own
// code in the same build; the mirror is what native draws will be built from
// once draw-array stops writing GE words.
//
// The behaviour of each entry point was established by disassembling it; the
// code below is an independent host implementation of that behaviour. A
// replacement is registered after the generated corpus (patch_loaded_image),
// which makes the runtime route calls into that 16 KiB unit through exact-
// address lookup (Runtime::register_function). A replacement returns by setting
// ctx.pc to $ra, or to a guest function to continue there as a tail call.

#include "vc2_native_gu.hpp"
#include "vc2_census.hpp"
#include "vc2_region.hpp"
#include "vc2_settings_menu.hpp"
#include "vc2_glyphs.hpp"
#include "vc2_controls.hpp"
#include "vc2_settings.hpp"

#include "profile.hpp"
#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"
#include "gpu/ge_state.hpp"
#include "settings/settings.hpp"
#if defined(PORTABLEKIT_HAS_SDL)
#include <SDL3/SDL.h>
#endif
#if defined(PORTABLEKIT_HAS_RENDERER)
#include "gpu/vulkan_renderer.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <deque>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <cstdio>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <cstdlib>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

namespace portablekit::vc2 {
namespace {

// GU state block (ELF segment 1 + 0x29978): +0x48 holds the current GU context,
// and the context's +8 is the display-list write pointer.
// European addresses (vc2_region.hpp); at() gives the running edition's.
constexpr std::uint32_t kEuGuStateBlock = 0x08D5AEC0u;  // +0x44 enabled mask, +0x48 context, +52/+56 depth buffer
constexpr std::uint32_t kEuGuContextPointer = 0x08D5AF08u;
constexpr std::uint32_t kListWriteField = 0x08u;
// The game's own stall-address update, which draw-array tail-calls.
constexpr std::uint32_t kEuGuUpdateStall = 0x08C89118u;
// The running edition's, fixed once the executable is known.
std::uint32_t gu_state_block() { static const std::uint32_t address = at(kEuGuStateBlock); return address; }
std::uint32_t gu_context_pointer() { static const std::uint32_t address = at(kEuGuContextPointer); return address; }
std::uint32_t gu_update_stall() { static const std::uint32_t address = at(kEuGuUpdateStall); return address; }

GuMirror &mirror() {
    static GuMirror state;
    return state;
}

Runtime &host(psprecomp::CorpusRuntime &corpus) { return static_cast<Runtime &>(corpus); }

void return_to_caller(AllegrexContext &ctx) { ctx.pc = ctx.gpr[31]; }

std::uint32_t context_address(Runtime &rt) { return rt.memory().load32(gu_context_pointer()); }
std::uint32_t context_word(Runtime &rt, std::uint32_t offset) { return rt.memory().load32(context_address(rt) + offset); }
void set_context_word(Runtime &rt, std::uint32_t offset, std::uint32_t value) {
    rt.memory().store32(context_address(rt) + offset, value);
}


bool native_draw_enabled();
void restore_camera_projection(std::uint32_t matrix, std::array<float, 16> &stored);

// Native-renderer phase 2 (VC2_NATIVE_LISTS, with VC2_NATIVE_DRAW): the GU
// state functions stop writing GE words. Every draw is built from the host
// mirror and reaches the renderer through its marker, so the GE has no use for
// the state; the list keeps only markers, its own framing (start, finish,
// signals, written by the game's code) and inline clear/blit vertex blocks.
// The mirror and the GU context fields the game reads back are updated as
// before. Any primitive the GE still draws itself would now lack its state, so
// each one is counted and the first few are logged: the expected count is 0.
bool native_lists_enabled() {
#if defined(VC2_NATIVE_ONLY)
    return true;
#else
    static const bool enabled = native_draw_enabled() && env("NATIVE_LISTS") != nullptr;
    return enabled;
#endif
}

struct ListStats {
    std::uint64_t words_not_written = 0u;  // state words the display-list path would have written
    std::uint64_t ge_draws = 0u;           // primitives the GE drew from its own state
};
ListStats &list_stats() {
    static ListStats stats;
    return stats;
}

// Appends GE words at the current GU context's list write pointer. With
// native lists, state words are not written (see native_lists_enabled);
// markers always are.
class ListWriter {
public:
    explicit ListWriter(Runtime &rt)
        : memory_(rt.memory()), context_(memory_.load32(gu_context_pointer())),
          write_(memory_.load32(context_ + kListWriteField)) {}
    ListWriter(const ListWriter &) = delete;
    ListWriter &operator=(const ListWriter &) = delete;
    ~ListWriter() { memory_.store32(context_ + kListWriteField, write_); }

    void write(std::uint32_t word) {
        memory_.store32(write_, word);
        write_ += 4u;
    }
    void emit(std::uint32_t word) {
        if (native_lists_enabled()) {
            ++list_stats().words_not_written;
            return;
        }
        write(word);
    }
    void emit(std::uint32_t command, std::uint32_t argument) { emit((command << 24u) | (argument & 0x00FFFFFFu)); }
    // A native draw's marker: a NOP whose data names its slot.
    void marker(std::uint32_t word) { write(word); }
    [[nodiscard]] std::uint32_t address() const noexcept { return write_; }

private:
    psprecomp::GuestMemory &memory_;
    std::uint32_t context_;
    std::uint32_t write_;
};

// Integer to float as the FPU's cvt.s.w does it.
float to_float(std::uint32_t value) { return static_cast<float>(static_cast<std::int32_t>(value)); }
std::uint32_t float_bits(float value) { return std::bit_cast<std::uint32_t>(value); }

// A float as the GE takes it: the top 24 bits of its IEEE-754 encoding.
std::uint32_t ge_float(std::uint32_t bits) { return bits >> 8u; }

std::uint32_t load_bits(Runtime &rt, std::uint32_t address) {
    auto &memory = rt.memory();
    if ((address & 3u) == 0u) return memory.load32(address);
    std::uint32_t value = 0u;  // matrices may be unaligned; the original reads them bytewise
    for (std::uint32_t i = 0; i < 4u; ++i) value |= static_cast<std::uint32_t>(memory.load8(address + i)) << (8u * i);
    return value;
}

// --- Entry points -------------------------------------------------------------

// alphaFunc(function, reference, mask)
void gu_alpha_func(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t function = ctx.gpr[4], reference = ctx.gpr[5] & 0xFFu, mask = ctx.gpr[6] & 0xFFu;
    ListWriter(host(corpus)).emit(0xDB000000u | function | (reference << 8u) | (mask << 16u));
    mirror().alpha_test = {function, reference, mask};
    return_to_caller(ctx);
}

// depthFunc(function)
void gu_depth_func(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0xDE000000u | ctx.gpr[4]);
    mirror().depth_function = ctx.gpr[4];
    return_to_caller(ctx);
}

// depthMask(disable_writes)
void gu_depth_mask(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0xE7000000u | ctx.gpr[4]);
    mirror().depth_mask = ctx.gpr[4];
    return_to_caller(ctx);
}

// pixelMask(mask): low 24 bits for RGB, high 8 for alpha.
void gu_pixel_mask(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t mask = ctx.gpr[4];
    {
        ListWriter list(host(corpus));
        list.emit(0xE8u, mask);
        list.emit(0xE9000000u | (mask >> 24u));
    }
    mirror().pixel_mask = mask;
    return_to_caller(ctx);
}

// stencilFunc(function, reference, mask)
void gu_stencil_func(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t function = ctx.gpr[4], reference = ctx.gpr[5], mask = ctx.gpr[6];
    ListWriter(host(corpus)).emit(0xDC000000u | function | (reference << 8u) | (mask << 16u));
    mirror().stencil_test = {function, reference, mask};
    return_to_caller(ctx);
}

// stencilOp(fail, depth_fail, pass)
void gu_stencil_op(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t fail = ctx.gpr[4], depth_fail = ctx.gpr[5], pass = ctx.gpr[6];
    ListWriter(host(corpus)).emit(0xDD000000u | fail | (depth_fail << 8u) | (pass << 16u));
    mirror().stencil_op = {fail, depth_fail, pass};
    return_to_caller(ctx);
}

// blendFunc(op, source, destination, fixed_a, fixed_b)
void gu_blend_func(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t op = ctx.gpr[4], source = ctx.gpr[5], destination = ctx.gpr[6];
    const std::uint32_t fixed_a = ctx.gpr[7] & 0x00FFFFFFu, fixed_b = ctx.gpr[8] & 0x00FFFFFFu;
    {
        ListWriter list(host(corpus));
        list.emit(0xDF000000u | (op << 8u) | (destination << 4u) | source);
        list.emit(0xE0000000u | fixed_a);
        list.emit(0xE1000000u | fixed_b);
    }
    mirror().blend = {op, source, destination, fixed_a, fixed_b};
    return_to_caller(ctx);
}

// texFilter(minify, magnify)
void gu_tex_filter(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0xC6000000u | (ctx.gpr[5] << 8u) | ctx.gpr[4]);
    mirror().texture_filter = {ctx.gpr[4], ctx.gpr[5]};
    return_to_caller(ctx);
}

// texWrap(u, v)
void gu_tex_wrap(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0xC7000000u | (ctx.gpr[5] << 8u) | ctx.gpr[4]);
    mirror().texture_wrap = {ctx.gpr[4], ctx.gpr[5]};
    return_to_caller(ctx);
}

// texOffset(float u, float v) -- arguments in $f12, $f13.
void gu_tex_offset(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t u = ctx.fpr_bits(12), v = ctx.fpr_bits(13);
    {
        ListWriter list(host(corpus));
        list.emit(0x4A000000u | ge_float(u));
        list.emit(0x4B000000u | ge_float(v));
    }
    mirror().texture_offset_u = std::bit_cast<float>(u);
    mirror().texture_offset_v = std::bit_cast<float>(v);
    return_to_caller(ctx);
}

// texScale(float u, float v) -- arguments in $f12, $f13.
void gu_tex_scale(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t u = ctx.fpr_bits(12), v = ctx.fpr_bits(13);
    {
        ListWriter list(host(corpus));
        list.emit(0x48000000u | ge_float(u));
        list.emit(0x49000000u | ge_float(v));
    }
    mirror().texture_scale_u = std::bit_cast<float>(u);
    mirror().texture_scale_v = std::bit_cast<float>(v);
    return_to_caller(ctx);
}

// offset(x, y): screen offset in pixels (the GE takes 1/16 pixels).
void gu_offset(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    {
        ListWriter list(host(corpus));
        list.emit(0x4C000000u | (ctx.gpr[4] << 4u));
        list.emit(0x4D000000u | (ctx.gpr[5] << 4u));
    }
    mirror().screen_offset_x = ctx.gpr[4];
    mirror().screen_offset_y = ctx.gpr[5];
    return_to_caller(ctx);
}

// texFlush()
void gu_tex_flush(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0xCB000000u);
    ++mirror().texture_flushes;
    return_to_caller(ctx);
}

// clutMode(format, shift, mask, start)
void gu_clut_mode(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t format = ctx.gpr[4], shift = ctx.gpr[5], mask = ctx.gpr[6], start = ctx.gpr[7];
    ListWriter(host(corpus)).emit(0xC5000000u | (start << 16u) | (mask << 8u) | (shift << 2u) | format);
    mirror().clut_mode = {format, shift, mask, start};
    return_to_caller(ctx);
}

// clutLoad(blocks, address)
void gu_clut_load(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t blocks = ctx.gpr[4], address = ctx.gpr[5];
    {
        ListWriter list(host(corpus));
        list.emit(0xB1000000u | (((address >> 24u) & 0xFu) << 16u));
        list.emit(0xB0u, address);
        list.emit(0xC4000000u | blocks);
    }
    mirror().clut_load = {blocks, address};
    if (const std::uint32_t loaded = (blocks & 0x7Fu) == 0x40u ? 0x40u : (blocks & 0x3Fu); loaded != 0u) {
        mirror().clut_load_bytes = loaded * 32u;
        mirror().clut_max_bytes = std::max(mirror().clut_max_bytes, mirror().clut_load_bytes);
    }
    return_to_caller(ctx);
}

// ambient(color ABGR)
void gu_ambient(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t color = ctx.gpr[4];
    {
        ListWriter list(host(corpus));
        list.emit(0x5Cu, color);
        list.emit(0x5D000000u | (color >> 24u));
    }
    mirror().ambient = color;
    return_to_caller(ctx);
}

// setMatrix(slot, matrix): 0 projection (16 values), 1 view, 2 model, 3 texture
// (3x4 each, taken from a 4x4 row-major block). Other slots write nothing.
void gu_set_matrix(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t slot = ctx.gpr[4], matrix = ctx.gpr[5];
    constexpr std::uint32_t kStart[4] = {0x3Eu, 0x3Cu, 0x3Au, 0x40u};
    if (slot < 4u) {
        auto &stored = mirror().matrices[slot];
        ListWriter list(rt);
        list.emit(kStart[slot] << 24u);
        const std::uint32_t data = kStart[slot] + 1u;
        for (std::uint32_t row = 0; row < 4u; ++row) {
            const std::uint32_t columns = slot == 0u ? 4u : 3u;
            for (std::uint32_t column = 0; column < columns; ++column) {
                const std::uint32_t bits = load_bits(rt, matrix + (row * 4u + column) * 4u);
                list.emit((data << 24u) | ge_float(bits));
                stored[row * 4u + column] = std::bit_cast<float>(bits);
            }
            if (slot != 0u) stored[row * 4u + 3u] = row == 3u ? 1.0f : 0.0f;
        }
        if (slot == 0u) restore_camera_projection(matrix, stored);
    }
    return_to_caller(ctx);
}

// boneMatrix(index, matrix): a filler word, the bone's register offset, then a
// 3x4 block.
void gu_bone_matrix(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t index = ctx.gpr[4], matrix = ctx.gpr[5];
    {
        ListWriter list(rt);
        list.emit(0xFF000000u);
        list.emit(0x2A000000u | (index * 12u));
        for (std::uint32_t row = 0; row < 4u; ++row)
            for (std::uint32_t column = 0; column < 3u; ++column) {
                const std::uint32_t bits = load_bits(rt, matrix + (row * 4u + column) * 4u);
                list.emit(0x2B000000u | ge_float(bits));
                if (index < mirror().bones.size()) mirror().bones[index][row * 3u + column] = std::bit_cast<float>(bits);
            }
    }
    mirror().last_bone = index;
    return_to_caller(ctx);
}

// --- Native draw check (VC2_NATIVE_CHECK=1) ------------------------------------
// Every draw-array call also builds the native DrawCall from the host mirror and
// keeps it under the list address of its PRIM word. When the display-list path
// reaches that PRIM, the observer (PortableKit patch 0006) compares the two
// field by field; the differences show exactly what the mirror is missing
// before rendering switches to the native draws.

bool native_check_enabled() {
#if defined(VC2_NATIVE_ONLY)
    return false;  // nothing to compare with: the GE emulation is not built
#else
    static const bool enabled = env("NATIVE_CHECK") != nullptr;
    return enabled;
#endif
}

struct CheckStats {
    std::uint64_t compared = 0u, matched = 0u, unmatched_ge = 0u, dropped = 0u;
    std::map<std::string, std::uint64_t> mismatches;
    std::map<std::string, int> examples_printed;
};
CheckStats &check_stats() {
    static CheckStats stats;
    return stats;
}

// A float as the GE keeps it: the top 24 bits.
std::string hex8(std::uint32_t value) {
    char text[12];
    std::snprintf(text, sizeof(text), "%08x", value);
    return text;
}

float ge_precision(float value) { return std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) & 0xFFFFFF00u); }
std::uint32_t resolve(std::uint32_t address) {
    address &= 0x0FFFFFFFu;
    return (address & 0xFF000000u) == 0u ? (address | 0x04000000u) : address;
}
bool on(const GuMirror &m, std::uint32_t state) { return (m.enabled & (1u << state)) != 0u; }

// What a draw reads from memory, captured with its state when the game makes
// the call. The bytes themselves are read when the list reaches the draw, as
// the GE reads them: the game builds each frame's list a frame ahead and may
// rewrite a vertex buffer before the list runs.
struct Geometry {
    std::uint32_t vertex_type = 0u, vertex_address = 0u, index_address = 0u, count = 0u;
    std::array<float, 96> bones{};  // 8 bone matrices, 3x4 each, at GE precision
    // The draw's vertices were built on the host (clears and blits, phase 2):
    // they are already in the DrawCall and nothing is read from guest memory.
    bool host_built = false;
};

Geometry geometry_of(const GuMirror &m, std::uint32_t count) {
    Geometry g{m.vertex_type, m.vertex_address, m.index_address, count, {}};
    for (std::size_t b = 0; b < 8u; ++b)
        for (std::size_t k = 0; k < 12u; ++k) g.bones[b * 12u + k] = ge_precision(m.bones[b][k]);
    return g;
}

bool decode_geometry(const psprecomp::GuestMemory &memory, const Geometry &m, gpu::DrawCall &call);

const psprecomp::GuestMemory *&guest_memory() {
    static const psprecomp::GuestMemory *memory = nullptr;
    return memory;
}

struct PendingDraw {
    gpu::DrawCall call;  // state built from the mirror when the game made the call
    Geometry geometry;   // decoded when the list reaches the draw
};

std::unordered_map<std::uint32_t, PendingDraw> &pending_draws() {
    static std::unordered_map<std::uint32_t, PendingDraw> draws;
    return draws;
}


gpu::DrawCall build_native_draw(const GuMirror &m, std::uint32_t primitive, std::uint32_t count) {
    gpu::DrawCall call;
    call.primitive = static_cast<gpu::PrimitiveType>(primitive & 7u);
    call.through = (m.vertex_type & (1u << 23u)) != 0u;
    const auto &level = m.texture_levels[0];
    const auto log2 = [](std::uint32_t value) { return 31u - static_cast<std::uint32_t>(std::countl_zero(value)); };
    auto &t = call.texture;
    t.enabled = on(m, 9u);
    t.address = resolve(level.address);
    t.buffer_width = level.buffer_width & 0xFFFFu;
    t.width = static_cast<std::uint16_t>(1u << (log2(level.width) & 0xFu));
    t.height = static_cast<std::uint16_t>(1u << (log2(level.height) & 0xFu));
    t.format = static_cast<gpu::TextureFormat>(m.texture_mode.format & 0xFu);
    t.swizzled = (m.texture_mode.swizzle & 1u) != 0u;
    t.clut_address = resolve(m.clut_load.address);
    const std::uint32_t clut_word = ((m.clut_mode.start << 16u) | (m.clut_mode.mask << 8u) | (m.clut_mode.shift << 2u) |
                                     m.clut_mode.format) & 0x00FFFFFFu;
    t.clut_format_word = 0xC5000000u | clut_word;
    t.clut_format = clut_word & 3u;
    t.clut_shift = (clut_word >> 2u) & 0x1Fu;
    t.clut_mask = (clut_word >> 8u) & 0xFFu;
    t.clut_offset = (clut_word >> 16u) & 0x1Fu;
    t.clut_load_bytes = m.clut_load_bytes;
    t.clut_max_bytes = m.clut_max_bytes;
    t.function = m.texture_function.function & 7u;
    t.alpha_from_texture = (m.texture_function.alpha & 1u) != 0u;
    t.min_filter = m.texture_filter.minify & 7u;
    t.mag_filter = m.texture_filter.magnify & 1u;
    t.wrap_s = m.texture_wrap.u & 1u;
    t.wrap_t = m.texture_wrap.v & 1u;
    t.scale_u = ge_precision(m.texture_scale_u);
    t.scale_v = ge_precision(m.texture_scale_v);
    t.offset_u = ge_precision(m.texture_offset_u);
    t.offset_v = ge_precision(m.texture_offset_v);
    call.target.color_address = resolve(m.draw_buffer.address);
    call.target.color_stride = m.draw_buffer.width & 0xFFFFu;
    call.target.color_format = m.draw_buffer.format & 3u;
    call.target.depth_address = resolve(m.depth_buffer.address);
    call.target.depth_stride = m.depth_buffer.width & 0xFFFFu;
    call.blend = {on(m, 4u), m.blend.source & 0xFu, m.blend.destination & 0xFu, m.blend.op & 0xFu, m.blend.fixed_a,
                  m.blend.fixed_b};
    call.depth = {on(m, 1u), (m.depth_mask & 1u) == 0u, m.depth_function & 7u, static_cast<std::uint16_t>(m.depth_min),
                  static_cast<std::uint16_t>(m.depth_max)};
    call.alpha_test = {on(m, 0u), m.alpha_test.function & 7u, m.alpha_test.reference & 0xFFu, m.alpha_test.mask & 0xFFu};
    call.stencil = {on(m, 3u),          m.stencil_test.function & 7u, m.stencil_test.reference & 0xFFu,
                    m.stencil_test.mask & 0xFFu, m.stencil_op.fail & 7u,  m.stencil_op.depth_fail & 7u,
                    m.stencil_op.pass & 7u};
    call.color_write_mask = m.pixel_mask & 0x00FFFFFFu;
    call.alpha_write_mask = (m.pixel_mask >> 24u) & 0xFFu;
    auto &v = call.viewport;
    v.x_scale = ge_precision(to_float(static_cast<std::uint32_t>(m.viewport.width)) * 0.5f);
    v.y_scale = ge_precision(to_float(static_cast<std::uint32_t>(m.viewport.height)) * -0.5f);
    v.z_scale = ge_precision(m.depth_scale);
    v.x_offset = ge_precision(to_float(static_cast<std::uint32_t>(m.viewport.center_x)));
    v.y_offset = ge_precision(to_float(static_cast<std::uint32_t>(m.viewport.center_y)));
    v.z_offset = ge_precision(m.depth_translate);
    v.scissor_x1 = m.scissor_start & 0x3FFu;
    v.scissor_y1 = (m.scissor_start >> 10u) & 0x3FFu;
    v.scissor_x2 = m.scissor_end & 0x3FFu;
    v.scissor_y2 = (m.scissor_end >> 10u) & 0x3FFu;
    v.offset_x = static_cast<float>((m.screen_offset_x << 4u) & 0xFFFFu) / 16.0f;
    v.offset_y = static_cast<float>((m.screen_offset_y << 4u) & 0xFFFFu) / 16.0f;
    call.culling_enabled = on(m, 5u);
    call.cull_clockwise = m.front_face == 0u;  // frontFace(0) writes 1
    call.vertex_type = m.vertex_type;
    call.vertex_address = m.vertex_address;
    const std::uint32_t index_type = (m.vertex_type >> 11u) & 3u;
    call.index_address = index_type != 0u ? m.index_address : 0u;
    call.primitive_count = count;
    call.material_color = m.material.ambient;
    call.lighting_enabled = on(m, 10u);
    call.has_vertex_color = ((m.vertex_type >> 2u) & 7u) != 0u;
    auto &l = call.lighting;
    l.material_update = m.color_material & 7u;
    l.material_emissive = m.material.emissive & 0x00FFFFFFu;
    l.material_diffuse = m.material.diffuse & 0x00FFFFFFu;
    l.material_specular = m.material.specular & 0x00FFFFFFu;
    l.ambient_color = m.ambient & 0x00FFFFFFu;
    l.ambient_alpha = (m.ambient >> 24u) & 0xFFu;
    l.mode = m.light_mode & 1u;
    l.reverse_normals = on(m, 19u);
    for (std::size_t i = 0; i < 4u; ++i) {
        auto &light = l.lights[i];
        light.enabled = on(m, 11u + static_cast<std::uint32_t>(i));
        const std::uint32_t kind = m.lights[i].components == 8u ? 2u : (m.lights[i].components == 6u ? 1u : 0u);
        light.kind = kind;
        light.type = m.lights[i].type & 3u;
        for (std::size_t k = 0; k < 3u; ++k) light.position[k] = ge_precision(m.lights[i].position[k]);
        // The library's initialisation leaves the attenuation registers at 0 and
        // the game never sets them (its lights are directional, which ignore
        // attenuation); match that rather than PortableKit's {1, 0, 0} default.
        light.attenuation = {0.0f, 0.0f, 0.0f};
        light.ambient = m.light_colors[i].ambient;
        light.diffuse = m.light_colors[i].diffuse;
        light.specular = m.light_colors[i].specular;
    }
    call.fog = {on(m, 7u), ge_precision(m.fog.far_z), ge_precision(m.fog_inverse), m.fog.color};
    const auto matrix = [](const std::array<float, 16> &in) {
        std::array<float, 16> out{};
        for (std::size_t i = 0; i < 16u; ++i) out[i] = ge_precision(in[i]);
        return out;
    };
    call.projection = matrix(m.matrices[0]);
    call.view = matrix(m.matrices[1]);
    call.world = matrix(m.matrices[2]);
    call.texture_matrix = matrix(m.matrices[3]);
    for (auto *mat : {&call.view, &call.world, &call.texture_matrix}) {
        (*mat)[3] = (*mat)[7] = (*mat)[11] = 0.0f;
        (*mat)[15] = 1.0f;
    }
    return call;
}

template <typename T>
std::string text(const T &value) {
    std::ostringstream out;
    if constexpr (std::is_same_v<T, float>) out << value << " (0x" << std::hex << std::bit_cast<std::uint32_t>(value) << ")";
    else if constexpr (std::is_same_v<T, bool>) out << (value ? "true" : "false");
    else if constexpr (std::is_enum_v<T>) out << static_cast<unsigned>(value);
    else out << "0x" << std::hex << static_cast<std::uint64_t>(value);
    return out.str();
}

void note_mismatch(const std::string &field, const std::string &ge, const std::string &native) {
    auto &stats = check_stats();
    ++stats.mismatches[field];
    if (stats.examples_printed[field]++ < 3)
        std::cerr << "[vc2-native-check] " << field << ": GE " << ge << " native " << native << "\n";
}

template <typename T>
bool same(const T &a, const T &b) {
    if constexpr (std::is_same_v<T, float>) return std::bit_cast<std::uint32_t>(a) == std::bit_cast<std::uint32_t>(b);
    else return a == b;
}

void compare_draws(const gpu::DrawCall &ge, const gpu::DrawCall &native) {
    bool all = true;
    const auto field = [&](const char *name, const auto &a, const auto &b) {
        if (same(a, b)) return;
        all = false;
        note_mismatch(name, text(a), text(b));
    };
    field("primitive", ge.primitive, native.primitive);
    field("primitive_count", ge.primitive_count, native.primitive_count);
    field("through", ge.through, native.through);
    field("vertex_type", ge.vertex_type, native.vertex_type);
    field("vertex_address", ge.vertex_address, native.vertex_address);
    field("index_address", ge.index_address, native.index_address);
    field("texture.enabled", ge.texture.enabled, native.texture.enabled);
    if (ge.texture.enabled) {
        field("texture.address", ge.texture.address, native.texture.address);
        field("texture.buffer_width", ge.texture.buffer_width, native.texture.buffer_width);
        field("texture.width", ge.texture.width, native.texture.width);
        field("texture.height", ge.texture.height, native.texture.height);
        field("texture.format", ge.texture.format, native.texture.format);
        field("texture.swizzled", ge.texture.swizzled, native.texture.swizzled);
        field("texture.function", ge.texture.function, native.texture.function);
        field("texture.alpha_from_texture", ge.texture.alpha_from_texture, native.texture.alpha_from_texture);
        field("texture.min_filter", ge.texture.min_filter, native.texture.min_filter);
        field("texture.mag_filter", ge.texture.mag_filter, native.texture.mag_filter);
        field("texture.wrap_s", ge.texture.wrap_s, native.texture.wrap_s);
        field("texture.wrap_t", ge.texture.wrap_t, native.texture.wrap_t);
        field("texture.scale_u", ge.texture.scale_u, native.texture.scale_u);
        field("texture.scale_v", ge.texture.scale_v, native.texture.scale_v);
        field("texture.offset_u", ge.texture.offset_u, native.texture.offset_u);
        field("texture.offset_v", ge.texture.offset_v, native.texture.offset_v);
        const auto format = static_cast<unsigned>(ge.texture.format);
        if (format >= 4u && format <= 7u) {
            field("texture.clut_address", ge.texture.clut_address, native.texture.clut_address);
            field("texture.clut_format_word", ge.texture.clut_format_word, native.texture.clut_format_word);
            field("texture.clut_load_bytes", ge.texture.clut_load_bytes, native.texture.clut_load_bytes);
        }
    }
    field("target.color_address", ge.target.color_address, native.target.color_address);
    field("target.color_stride", ge.target.color_stride, native.target.color_stride);
    field("target.color_format", ge.target.color_format, native.target.color_format);
    field("target.depth_address", ge.target.depth_address, native.target.depth_address);
    field("target.depth_stride", ge.target.depth_stride, native.target.depth_stride);
    field("blend.enabled", ge.blend.enabled, native.blend.enabled);
    if (ge.blend.enabled) {
        field("blend.source", ge.blend.source_factor, native.blend.source_factor);
        field("blend.destination", ge.blend.destination_factor, native.blend.destination_factor);
        field("blend.equation", ge.blend.equation, native.blend.equation);
        field("blend.fixed_source", ge.blend.fixed_source, native.blend.fixed_source);
        field("blend.fixed_destination", ge.blend.fixed_destination, native.blend.fixed_destination);
    }
    field("depth.test_enabled", ge.depth.test_enabled, native.depth.test_enabled);
    field("depth.write_enabled", ge.depth.write_enabled, native.depth.write_enabled);
    field("depth.function", ge.depth.function, native.depth.function);
    field("depth.range_near", ge.depth.range_near, native.depth.range_near);
    field("depth.range_far", ge.depth.range_far, native.depth.range_far);
    field("alpha_test.enabled", ge.alpha_test.enabled, native.alpha_test.enabled);
    if (ge.alpha_test.enabled) {
        field("alpha_test.function", ge.alpha_test.function, native.alpha_test.function);
        field("alpha_test.reference", ge.alpha_test.reference, native.alpha_test.reference);
        field("alpha_test.mask", ge.alpha_test.mask, native.alpha_test.mask);
    }
    field("stencil.enabled", ge.stencil.enabled, native.stencil.enabled);
    if (ge.stencil.enabled) {
        field("stencil.function", ge.stencil.function, native.stencil.function);
        field("stencil.reference", ge.stencil.reference, native.stencil.reference);
        field("stencil.mask", ge.stencil.mask, native.stencil.mask);
        field("stencil.fail", ge.stencil.fail, native.stencil.fail);
        field("stencil.depth_fail", ge.stencil.depth_fail, native.stencil.depth_fail);
        field("stencil.pass", ge.stencil.pass, native.stencil.pass);
    }
    field("color_write_mask", ge.color_write_mask, native.color_write_mask);
    field("alpha_write_mask", ge.alpha_write_mask, native.alpha_write_mask);
    field("viewport.x_scale", ge.viewport.x_scale, native.viewport.x_scale);
    field("viewport.y_scale", ge.viewport.y_scale, native.viewport.y_scale);
    field("viewport.z_scale", ge.viewport.z_scale, native.viewport.z_scale);
    field("viewport.x_offset", ge.viewport.x_offset, native.viewport.x_offset);
    field("viewport.y_offset", ge.viewport.y_offset, native.viewport.y_offset);
    field("viewport.z_offset", ge.viewport.z_offset, native.viewport.z_offset);
    field("viewport.scissor_x1", ge.viewport.scissor_x1, native.viewport.scissor_x1);
    field("viewport.scissor_y1", ge.viewport.scissor_y1, native.viewport.scissor_y1);
    field("viewport.scissor_x2", ge.viewport.scissor_x2, native.viewport.scissor_x2);
    field("viewport.scissor_y2", ge.viewport.scissor_y2, native.viewport.scissor_y2);
    field("viewport.offset_x", ge.viewport.offset_x, native.viewport.offset_x);
    field("viewport.offset_y", ge.viewport.offset_y, native.viewport.offset_y);
    field("culling_enabled", ge.culling_enabled, native.culling_enabled);
    if (ge.culling_enabled) field("cull_clockwise", ge.cull_clockwise, native.cull_clockwise);
    field("clear_mode", ge.clear_mode, native.clear_mode);
    field("material_color", ge.material_color, native.material_color);
    field("lighting_enabled", ge.lighting_enabled, native.lighting_enabled);
    if (ge.lighting_enabled) {
        const auto &a = ge.lighting, &b = native.lighting;
        field("lighting.material_update", a.material_update, b.material_update);
        field("lighting.material_emissive", a.material_emissive, b.material_emissive);
        field("lighting.material_diffuse", a.material_diffuse, b.material_diffuse);
        field("lighting.material_specular", a.material_specular, b.material_specular);
        field("lighting.specular_power", a.specular_power, b.specular_power);
        field("lighting.ambient_color", a.ambient_color, b.ambient_color);
        field("lighting.ambient_alpha", a.ambient_alpha, b.ambient_alpha);
        field("lighting.mode", a.mode, b.mode);
        field("lighting.reverse_normals", a.reverse_normals, b.reverse_normals);
        for (std::size_t i = 0; i < 4u; ++i) {
            const std::string n = "lighting.light" + std::to_string(i) + ".";
            field((n + "enabled").c_str(), a.lights[i].enabled, b.lights[i].enabled);
            if (!a.lights[i].enabled) continue;
            field((n + "kind").c_str(), a.lights[i].kind, b.lights[i].kind);
            field((n + "type").c_str(), a.lights[i].type, b.lights[i].type);
            for (std::size_t k = 0; k < 3u; ++k) {
                field((n + "position" + std::to_string(k)).c_str(), a.lights[i].position[k], b.lights[i].position[k]);
                field((n + "direction" + std::to_string(k)).c_str(), a.lights[i].direction[k], b.lights[i].direction[k]);
                field((n + "attenuation" + std::to_string(k)).c_str(), a.lights[i].attenuation[k],
                      b.lights[i].attenuation[k]);
            }
            field((n + "spot_exponent").c_str(), a.lights[i].spot_exponent, b.lights[i].spot_exponent);
            field((n + "spot_cutoff").c_str(), a.lights[i].spot_cutoff, b.lights[i].spot_cutoff);
            field((n + "ambient").c_str(), a.lights[i].ambient, b.lights[i].ambient);
            field((n + "diffuse").c_str(), a.lights[i].diffuse, b.lights[i].diffuse);
            field((n + "specular").c_str(), a.lights[i].specular, b.lights[i].specular);
        }
    }
    field("fog.enabled", ge.fog.enabled, native.fog.enabled);
    if (ge.fog.enabled) {
        field("fog.end", ge.fog.end, native.fog.end);
        field("fog.scale", ge.fog.scale, native.fog.scale);
        field("fog.color", ge.fog.color, native.fog.color);
    }
    // With GPU vertex decode on, the display-list path passes raw vertex bytes
    // instead of decoded vertices; compare the count with those.
    field("vertex_count", ge.raw_vertices != nullptr ? static_cast<std::size_t>(ge.raw_count) : ge.vertices.size(),
          native.vertices.size());
    field("index_count", ge.indices.size(), native.indices.size());
    if (ge.raw_vertices == nullptr && ge.vertices.size() == native.vertices.size()) {
        for (std::size_t i = 0; i < ge.vertices.size(); ++i) {
            const auto &a = ge.vertices[i], &b = native.vertices[i];
            bool equal = a.color == b.color;
            for (std::size_t k = 0; k < 4u; ++k) equal = equal && same(a.position[k], b.position[k]);
            for (std::size_t k = 0; k < 3u; ++k) equal = equal && same(a.normal[k], b.normal[k]);
            for (std::size_t k = 0; k < 2u; ++k) equal = equal && same(a.texcoord[k], b.texcoord[k]);
            if (!equal) {
                all = false;
                note_mismatch("vertices", "#" + std::to_string(i) + " pos " + text(a.position[0]) + "," +
                                              text(a.position[1]) + " color " + text(a.color),
                              "pos " + text(b.position[0]) + "," + text(b.position[1]) + " color " + text(b.color));
                break;
            }
        }
    }
    if (ge.indices.size() == native.indices.size() && ge.indices != native.indices) {
        all = false;
        note_mismatch("indices", "differ", "differ");
    }
    field("clear_flags", ge.clear_flags, native.clear_flags);
    const auto matrix = [&](const char *name, const std::array<float, 16> &a, const std::array<float, 16> &b) {
        for (std::size_t i = 0; i < 16u; ++i)
            if (!same(a[i], b[i])) {
                all = false;
                note_mismatch(name, "[" + std::to_string(i) + "]=" + text(a[i]), text(b[i]));
                return;
            }
    };
    if (!ge.through) {
        matrix("matrix.world", ge.world, native.world);
        matrix("matrix.view", ge.view, native.view);
        matrix("matrix.projection", ge.projection, native.projection);
        if (ge.texture.enabled) matrix("matrix.texture", ge.texture_matrix, native.texture_matrix);
    }
    if (all) ++check_stats().matched;
}

// With native lists: a primitive the GE drew itself (not through a marker).
void observe_ge_draw_without_state(const gpu::DrawCall &call, std::uint32_t primitive_address) {
    const std::uint64_t seen = ++list_stats().ge_draws;
    if (seen <= 10u)
        std::cerr << "[vc2-native-lists] the GE drew a primitive itself (its state was not written): list 0x"
                  << std::hex << primitive_address << " primitive " << static_cast<int>(call.primitive)
                  << " vertices " << std::dec << call.vertices.size() << (call.through ? " through" : "")
                  << (call.clear_mode ? " clear" : "") << " target 0x" << std::hex << call.target.color_address
                  << std::dec << "\n";
}

void observe_ge_draw(const gpu::DrawCall &call, std::uint32_t primitive_address) {
    auto &pending = pending_draws();
    const auto found = pending.find(primitive_address & 0x1FFFFFFFu);
    if (found == pending.end()) {
        ++check_stats().unmatched_ge;  // a draw not converted, or a list the game wrote itself
        return;
    }
    ++check_stats().compared;
    if (guest_memory() == nullptr || !decode_geometry(*guest_memory(), found->second.geometry, found->second.call))
        note_mismatch("geometry", "drawn", "not decodable");
    else
        compare_draws(call, found->second.call);
    pending.erase(found);
}

// --- Movie trace (VC2_TRACE_MOVIE=1) -------------------------------------------
// For the 1-pixel vertical jitter seen during videos: while pictures are being
// decoded, every displayed frame logs the framebuffer shown, where each decoded
// picture went, and each draw with its texture, screen position and UVs (the
// first and last vertex, decoded when the call is made). Up to 600 frames.

struct MovieTrace {
    std::uint32_t active_flips = 0u;  // > 0 while pictures are being decoded
    std::uint64_t frames_logged = 0u;
    std::uint32_t last_framebuffer = 0u;
    std::vector<std::string> lines;   // this frame's
    // Raw memory dump (VC2_MOVIE_DUMP=<dir>): the picture buffers and the
    // textures drawn, at four consecutive frames from the 120th movie frame.
    std::vector<std::uint32_t> pictures, textures;  // seen so far
};
MovieTrace &movie_trace() {
    static MovieTrace trace;
    return trace;
}
bool movie_trace_enabled() {
    static const bool enabled = env("TRACE_MOVIE") != nullptr;
    return enabled;
}

void movie_note(std::string line) {
    auto &trace = movie_trace();
    if (!movie_trace_enabled() || trace.active_flips == 0u || trace.frames_logged >= 600u) return;
    if (trace.lines.size() < 64u) trace.lines.push_back(std::move(line));
}

void remember(std::vector<std::uint32_t> &list, std::uint32_t address) {
    if (address != 0u && std::find(list.begin(), list.end(), address) == list.end() && list.size() < 8u)
        list.push_back(address);
}

void movie_note_draw(Runtime &rt, const GuMirror &m, const char *kind, std::uint32_t count) {
    if (!movie_trace_enabled() || movie_trace().active_flips == 0u) return;
    if (std::string_view(kind) == "draw" && ((m.enabled >> 9u) & 1u) != 0u)
        remember(movie_trace().textures, m.texture_levels[0].address);
    std::vector<gpu::Vertex> vertices;
    const std::uint32_t index_type = (m.vertex_type >> 11u) & 3u;
    std::ostringstream out;
    out << "  " << kind << " n=" << count << " vtype=" << hex8(m.vertex_type) << " tex=" << hex8(m.texture_levels[0].address)
        << " " << m.texture_levels[0].width << "x" << m.texture_levels[0].height << " bw=" << m.texture_levels[0].buffer_width
        << " fmt=" << m.texture_mode.format << " texon=" << ((m.enabled >> 9u) & 1u)
        << " ofs=" << m.screen_offset_x << "," << m.screen_offset_y << " vp=" << m.viewport.center_x << ","
        << m.viewport.center_y << " " << m.viewport.width << "x" << m.viewport.height;
    // Every vertex the draw uses (up to 8), through its indices when it has them.
    if (count != 0u && m.vertex_address != 0u) {
        const auto &memory = rt.memory();
        const std::uint32_t size = index_type == 1u ? 1u : index_type == 2u ? 2u : 4u;
        std::uint32_t highest = 0u;
        std::vector<std::uint32_t> order;
        for (std::uint32_t i = 0; i < std::min(count, 8u); ++i) {
            std::uint32_t index = i;
            if (index_type != 0u && m.index_address != 0u) {
                const std::uint32_t at = m.index_address + i * size;
                index = size == 1u ? memory.load8(at) : size == 2u ? memory.load16(at) : memory.load32(at);
            }
            order.push_back(index);
            highest = std::max(highest, index);
        }
        if (highest < 4096u &&
            gpu::decode_vertices(memory, m.vertex_address, m.vertex_type, highest + 1u, vertices) != 0u &&
            vertices.size() > highest) {
            out << std::fixed << std::setprecision(3);
            for (const std::uint32_t index : order) {
                const auto &v = vertices[index];
                out << " [" << index << "] " << v.position[0] << "," << v.position[1] << "," << v.position[2] << " uv "
                    << v.texcoord[0] << "," << v.texcoord[1];
            }
        }
    }
    movie_note(out.str());
}

// The target a draw renders into: the last drawBufferList of the current list,
// or else the default every list start re-sends from the GU state block
// (+36 format, +40 width, +44 address).
void resolve_target(Runtime &rt, GuMirror &m) {
    const std::uint32_t list = context_word(rt, 4u), write = context_word(rt, kListWriteField);
    if (m.explicit_draw_buffer_list == list && write >= m.explicit_draw_buffer_write &&
        m.explicit_draw_buffer_write != 0u) {
        m.draw_buffer = m.explicit_draw_buffer;
        return;
    }
    auto &memory = rt.memory();
    m.draw_buffer = {memory.load32(gu_state_block() + 36u), memory.load32(gu_state_block() + 44u),
                     memory.load32(gu_state_block() + 40u)};
}

// --- Native draws (VC2_NATIVE_DRAW=1) ------------------------------------------
// Draw-array builds the complete DrawCall itself -- state from the mirror,
// indices and vertices decoded here -- and leaves one marker word (a NOP with
// data 0x800000 | slot) in the display list instead of the vertex-type,
// address and PRIM commands. PortableKit (patch 0007) hands the prepared draw
// to the renderer when the list reaches the marker, so it keeps its order with
// the clears and HUD blits that still go through the list.

bool native_draw_enabled() {
#if defined(VC2_NATIVE_ONLY)
    return true;
#else
    static const bool enabled = env("NATIVE_DRAW") != nullptr;
    return enabled;
#endif
}

constexpr std::uint32_t kMarkerTag = 0x800000u;
constexpr std::uint32_t kNativeSlots = 4096u;

// Phase 2: clears and blits carry vertices built on the host instead of
// vertex bytes placed in the display list. VC2_LIST_VERTICES=1 goes back to the
// list bytes (decoded when the list reaches the draw), for comparison.
bool host_vertices_enabled() {
#if defined(VC2_NATIVE_ONLY)
    return true;  // the list's own bytes would need the GE to draw them
#else
    static const bool enabled = env("LIST_VERTICES") == nullptr;
    return enabled;
#endif
}

// The vertex pointer was last left by a clear or blit with host-built
// vertices, so a draw continuing from it has no bytes to read.
bool &vertex_pointer_from_host_draw() {
    static bool flag = false;
    return flag;
}

struct NativeSlot {
    gpu::DrawCall call;
    Geometry geometry;
    bool ready = false;
};

std::array<NativeSlot, kNativeSlots> &native_slots() {
    static std::array<NativeSlot, kNativeSlots> slots;
    return slots;
}

struct NativeStats {
    std::uint64_t built = 0u, submitted = 0u, skipped = 0u, stale = 0u;
    std::uint64_t host_vertices = 0u;      // clears and blits whose vertices were built on the host
    std::uint64_t continued_pointer = 0u;  // draws that took their vertex pointer from one of those
};
NativeStats &native_stats() {
    static NativeStats stats;
    return stats;
}

// A picture drawn from the top rows of a larger texture (the movie player's
// 480x272 frames sit in 512x512 textures; v runs 0 .. 272/512). At 480x272
// every pixel samples a texel centre inside the picture. At a higher internal
// resolution the top and bottom rows of pixels sample between texels, and
// bilinear filtering blends in row 511 (the top edge wraps) and row 272 (past
// the picture): whatever memory follows the texture. VC2 alternates two
// movie textures, only one of which has black memory there, so a grey line
// appeared at the top and bottom of every other frame (the "1-pixel jitter").
// The texture is described as only the rows the draw uses, with v rescaled,
// and clamped at its edges, so every sample stays inside the picture. A draw
// that already uses the whole texture height or repeats it is left exactly as
// it was; at 480x272 the result is unchanged. (Lighting does not matter: it
// does not change which texels are sampled, and VC2 leaves it on for these
// draws although their vertices have no normals.) VC2_NO_PICTURE_CROP turns
// it off.
const char *crop_picture_texture(gpu::DrawCall &call) {
    static const bool off = env("NO_PICTURE_CROP") != nullptr;
    if (off) return "off";
    if (call.through) return "through";
    if (!call.texture.enabled) return "untextured";
    if (call.texture.swizzled) return "swizzled";
    if (call.vertices.empty()) return "no decoded vertices";
    if (call.vertices.size() > 6u) return "more than 6 vertices";
    if (call.texture.height == 0u) return "no height";
    gpu::TextureState &texture = call.texture;
    float u_min = 1e30f, u_max = -1e30f, v_min = 1e30f, v_max = -1e30f;
    for (const gpu::Vertex &vertex : call.vertices) {
        const float u = vertex.texcoord[0] * texture.scale_u + texture.offset_u;
        const float v = vertex.texcoord[1] * texture.scale_v + texture.offset_v;
        u_min = std::min(u_min, u); u_max = std::max(u_max, u);
        v_min = std::min(v_min, v); v_max = std::max(v_max, v);
    }
    // Normalised coordinates inside one copy of the texture, starting at its top.
    if (v_min != 0.0f || v_max <= 0.0f || v_max >= 1.0f || u_min < 0.0f || u_max > 1.0f) return "uv range";
    const float used_rows = v_max * static_cast<float>(texture.height);
    const auto rows = static_cast<std::uint32_t>(std::lround(used_rows));
    // Only a whole number of rows, as a picture's height is.
    if (rows == 0u || rows >= texture.height || std::fabs(used_rows - static_cast<float>(rows)) > 1e-3f) return "rows";
    const float stretch = static_cast<float>(texture.height) / static_cast<float>(rows);
    texture.height = static_cast<std::uint16_t>(rows);
    texture.scale_v *= stretch;
    texture.offset_v *= stretch;
    texture.wrap_t = 1u;  // clamp
    texture.wrap_s = 1u;  // u stays inside [0, 1], so clamping changes nothing but the left and right edges
    return "cropped";
}

// --- Texture trace (VC2_TRACE_TEXTURE=<hex address>) ---------------------------
// For the missing button icons: every native draw that samples the texture at
// that address is logged when it is built, when the list reaches it (or finds
// it gone), with its texture-space rectangle and how many texels of that
// rectangle are not fully transparent at that moment (32-bit textures). Up to
// 400 lines.
std::uint32_t traced_texture() {
    static const std::uint32_t address = [] {
        const char *text = env("TRACE_TEXTURE");
        return text != nullptr ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 16)) & 0x0FFFFFFFu : 0u;
    }();
    return address;
}
bool traces_texture(const gpu::DrawCall &call) {
    return traced_texture() != 0u && call.texture.enabled && (call.texture.address & 0x0FFFFFFFu) == traced_texture();
}
void texture_note(const char *event, std::uint32_t slot, const gpu::DrawCall &call) {
    static std::uint32_t lines = 0u;
    if (++lines > 400u) return;
    std::ostringstream line;
    line << "[vc2-texture] " << event << " slot " << slot << " at draw " << native_stats().built << " verts "
         << call.vertices.size() << " through " << call.through << " tex " << hex8(call.texture.address) << " "
         << call.texture.width << "x" << call.texture.height << " bw " << call.texture.buffer_width << " fmt "
         << static_cast<unsigned>(call.texture.format) << " target " << hex8(call.target.color_address);
    if (!call.vertices.empty()) {
        const gpu::TextureState &t = call.texture;
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
        for (const gpu::Vertex &v : call.vertices) {
            const float u = call.through ? v.texcoord[0] : (v.texcoord[0] * t.scale_u + t.offset_u) * t.width;
            const float w = call.through ? v.texcoord[1] : (v.texcoord[1] * t.scale_v + t.offset_v) * t.height;
            u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, w); v1 = std::max(v1, w);
        }
        line << " texels u " << u0 << ".." << u1 << " v " << v0 << ".." << v1;
        if (t.format == gpu::TextureFormat::Rgba8888 && guest_memory() != nullptr) {
            const std::uint32_t bw = t.buffer_width != 0u ? t.buffer_width : t.width;
            std::uint32_t opaque = 0u, total = 0u;
            const auto x0 = static_cast<std::uint32_t>(std::max(0.0f, u0)), y0 = static_cast<std::uint32_t>(std::max(0.0f, v0));
            const auto x1 = static_cast<std::uint32_t>(std::max(0.0f, u1)), y1 = static_cast<std::uint32_t>(std::max(0.0f, v1));
            for (std::uint32_t y = y0; y < y1 && y < t.height; ++y)
                for (std::uint32_t x = x0; x < x1 && x < bw; ++x) {
                    const std::uint32_t address = t.address + (y * bw + x) * 4u;
                    if (!guest_memory()->contains(address, 4u)) continue;
                    ++total;
                    if ((guest_memory()->load32(address) >> 24u) != 0u) ++opaque;
                }
            line << " visible texels " << opaque << "/" << total;
        }
    }
    std::cerr << line.str() << "\n";
}

// --- Widescreen and any aspect (with native draws) ----------------------------------
// Under video.aspect=fill the renderer's targets take the window's shape and
// the game's 480x272 screen is spread over them, so each PSP pixel is r times
// as wide as it is tall (r = window shape / 480:272; below 1 it is taller).
// Draws into a shown framebuffer are brought back to square pixels by scaling
// x by sx = min(1, 1/r) and y by sy = min(1, r) about an anchor:
//  - The 3D view (a perspective projection over the full viewport) about the
//    screen's centre, through the projection, with the viewport left full so
//    nothing is clipped: Hor+ (the same vertical view, more at the sides; a
//    taller window shows more above and below).
//  - Interface and other 2D (flat projections and screen-space draws) is
//    placed as whole elements: each frame's 2D draws are grouped (draws that
//    overlap, or share their world matrix's translation, the origin the game
//    lays an element out from), and each group keeps its distance from the
//    left edge when it lies wholly left of the centre, from the right edge
//    when wholly right of it, and stays centred when it crosses the centre
//    (the same above and below for a taller window). Placing single draws
//    split elements drawn in pieces (2026-11-07 trace: movies are 64-pixel
//    strips, the title 120-pixel strips, the crosshair four corners).
//  - A frame with no 3D view is a 2D screen and is kept whole in the middle
//    (see place_frame). In 3D scenes:
//  - Pictures (draws over the full height, or the whole screen when opaque:
//    movies, the title, story art) stay whole in the middle at the PSP's
//    shape. See-through or untextured draws over the full width are stretched
//    along it (the vignette, text bars, fades, the shadow pass).
// Left as they are: draws that sample VRAM (the screen itself, or pictures
// rendered into buffers: glow, blur), clears, and draws into buffers that are
// not shown (the renderer maps those back when they are sampled). The renderer
// is told not to pull screen-space draws in itself (patch 0013).
// VC2_NO_WIDE=1 turns all of this off.

struct Extent {
    float left = 1e30f, right = -1e30f, top = 1e30f, bottom = -1e30f;
    void add(float x, float y) {
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
};

struct WideState {
    float sx = 1.0f, sy = 1.0f;
    // Whether this frame (or the previous one) is a 3D scene: its 3D draws
    // together cover most of the screen (the academy map, combat). A 2D screen
    // can show a small 3D model in a panel (Mod Vehicle's tank) and stays a
    // 2D screen. A frame can run more than one list (the academy map draws its
    // 3D view and its interface apart), so this is judged per frame.
    bool view_this_frame = false, view_last_frame = false;
    // Whether this frame (or the previous one) draws an opaque backdrop over
    // the whole screen (see place_frame): a 2D screen, whatever 3D it shows
    // in front (the homeroom news).
    bool backdrop_this_frame = false, backdrop_last_frame = false;
    Extent view_extent;  // the union of this frame's 3D draws on the PSP screen
    bool trace_frame = false;  // VC2_TRACE_WIDE: this frame's 2D draws are logged
    std::uint64_t frame = 0u;
    std::uint32_t traced = 0u;
    std::array<std::uint32_t, 4> shown{};  // framebuffers passed to sceDisplaySetFrameBuf, most recent first
    std::uint64_t view = 0u, left = 0u, right = 0u, centred = 0u, panels = 0u, stretched = 0u, pillarboxed = 0u,
                  sampled_vram = 0u, unknown = 0u;
    // The game's camera, widened (see build_perspective): counts.
    std::uint64_t camera_widened = 0u, camera_restored = 0u, screen_tests_widened = 0u;
};
WideState &wide() {
    static WideState w;
    return w;
}

bool wide_trace_enabled() {
    static const bool enabled = env("TRACE_WIDE") != nullptr;
    return enabled;
}

// VC2_TRACE_SCENE: one line per frame around every change between a 3D scene
// and a 2D screen (the 20 frames before, from a ring, and the 40 after): the
// decision, the 3D draws' combined extent, the frame's placements by kind and
// the game camera's widenings, to see what each frame of a transition does.
bool scene_trace_enabled() {
    static const bool enabled = env("TRACE_SCENE") != nullptr;
    return enabled;
}

// Under VC2_TRACE_SCENE the frame's 2D draws (and draws left as they are) are
// also kept, one line each. Around a change only the frame lines are shown;
// F9 marks a moment (a fly-in gone wrong, a screen laid out wrongly): the 165
// frames before it and the 330 after (1 s and 2 s at 165 FPS) are shown with every draw.
std::vector<std::string> &scene_draw_lines() {
    static std::vector<std::string> lines;
    return lines;
}

struct SceneTrace {
    std::array<std::uint64_t, 11> last{};  // the WideState counters at the previous flip
    std::deque<std::vector<std::string>> ring;  // per frame: its line, then its draws
    bool last_2d = true;
    std::uint32_t after = 0u, marked_after = 0u;
    std::uint64_t changes = 0u, marks = 0u;
    bool key_down = false;
};

void trace_scene_frame();

bool wide_enabled() {
    static const bool enabled = env("NO_WIDE") == nullptr;
    return enabled;
}

void note_shown_framebuffer(std::uint32_t address) {
    if (address == 0u) return;
    auto &shown = wide().shown;
    const std::uint32_t resolved = resolve(address);
    if (shown[0] == resolved) return;
    std::rotate(shown.begin(), shown.end() - 1, shown.end());
    shown[0] = resolved;
}

bool is_shown(std::uint32_t address) {
    const auto &shown = wide().shown;
    return address != 0u && std::find(shown.begin(), shown.end(), address) != shown.end();
}


void trace_scene_frame() {
    static SceneTrace t;
    auto &w = wide();
    const std::array<std::uint64_t, 11> now{w.view,    w.left,      w.right,          w.centred,
                                            w.panels,  w.stretched, w.pillarboxed,    w.sampled_vram,
                                            w.unknown, w.camera_widened, w.camera_restored};
    std::array<std::uint64_t, 11> d{};
    for (std::size_t i = 0; i < now.size(); ++i) d[i] = now[i] - t.last[i];
    t.last = now;
    // The decision place_frame made for this frame's lists.
    const bool is_2d = (!w.view_this_frame && !w.view_last_frame) || w.backdrop_this_frame || w.backdrop_last_frame;
    std::ostringstream line;
    line << std::fixed << std::setprecision(0) << "[vc2-scene] f" << w.frame << (is_2d ? " 2D" : " 3D")
         << " (3D now " << (w.view_this_frame ? "yes" : "no") << ", last " << (w.view_last_frame ? "yes" : "no") << ")";
    if (w.view_extent.left <= w.view_extent.right)
        line << " 3D x " << w.view_extent.left << ".." << w.view_extent.right << " y " << w.view_extent.top << ".."
             << w.view_extent.bottom;
    line << " | view " << d[0] << " left " << d[1] << " right " << d[2] << " centred " << d[3] << " panel " << d[4]
         << " stretched " << d[5] << " picture " << d[6] << " vram " << d[7] << " unknown " << d[8]
         << " | camera widened " << d[9] << " restored " << d[10] << " | shown " << hex8(w.shown[0]);
    std::vector<std::string> record{line.str()};
    for (std::string &draw : scene_draw_lines()) record.push_back("    " + std::move(draw));
    scene_draw_lines().clear();
    const auto print = [](const std::vector<std::string> &lines, bool draws) {
        for (std::size_t i = 0; i < (draws ? lines.size() : std::min<std::size_t>(lines.size(), 1u)); ++i)
            std::cerr << lines[i] << "\n";
    };
#if defined(PORTABLEKIT_HAS_SDL)
    int count = 0;
    const bool *keys = SDL_GetKeyboardState(&count);
    const bool down = keys != nullptr && SDL_SCANCODE_F9 < count && keys[SDL_SCANCODE_F9];
#else
    const bool down = false;
#endif
    const bool marked = down && !t.key_down;
    t.key_down = down;
    if (marked) {
        ++t.marks;
        std::cerr << "[vc2-scene] mark " << t.marks << " (F9) at frame " << w.frame << "; the frames before, with draws:\n";
        for (const auto &old : t.ring) print(old, true);
        t.marked_after = 330u;
    }
    if (is_2d != t.last_2d && w.frame > 1u) {
        ++t.changes;
        std::cerr << "[vc2-scene] change " << t.changes << " to " << (is_2d ? "2D" : "3D") << " at frame " << w.frame
                  << "; the frames before:\n";
        if (t.marked_after == 0u)
            for (const auto &old : t.ring) print(old, false);
        t.after = 40u;
    }
    t.last_2d = is_2d;
    if (t.marked_after != 0u || t.after != 0u) {
        print(record, t.marked_after != 0u);
        if (t.marked_after != 0u) --t.marked_after;
        if (t.after != 0u) --t.after;
        t.ring.clear();
    } else {
        t.ring.push_back(std::move(record));
        if (t.ring.size() > 165u) t.ring.pop_front();
    }
}

// Where the draw lands on the PSP screen: screen-space vertices as they are,
// others put through the world, view and projection matrices and the
// viewport. False when it cannot tell (a vertex behind the eye, or too many
// vertices to look at).
bool screen_extent(const gpu::DrawCall &call, Extent &extent) {
    if (call.vertices.empty() || call.vertices.size() > 8192u) return false;
    if (call.through) {
        for (const gpu::Vertex &vertex : call.vertices) extent.add(vertex.position[0], vertex.position[1]);
        return true;
    }
    const auto apply = [](const std::array<float, 4> &p, const std::array<float, 16> &m) {
        std::array<float, 4> out{};
        for (std::size_t j = 0; j < 4u; ++j)
            out[j] = p[0] * m[j] + p[1] * m[4u + j] + p[2] * m[8u + j] + p[3] * m[12u + j];
        return out;
    };
    const auto &vp = call.viewport;
    for (const gpu::Vertex &vertex : call.vertices) {
        std::array<float, 4> p{vertex.position[0], vertex.position[1], vertex.position[2], 1.0f};
        p = apply(apply(apply(p, call.world), call.view), call.projection);
        if (p[3] <= 1e-6f) return false;
        extent.add(vp.x_offset - vp.offset_x + p[0] / p[3] * vp.x_scale,
                   vp.y_offset - vp.offset_y + p[1] / p[3] * vp.y_scale);
    }
    return true;
}

// The edge a span is kept by: 0 (left or top), `size` (right or bottom), or
// the middle when it crosses it.
float anchor_of(float low, float high, float size) {
    const float middle = 0.5f * size;
    if (high <= middle + 0.5f) return 0.0f;
    if (low >= middle - 0.5f) return size;
    return middle;
}

// The edge a whole element is kept by: it must lie on one side of the centre
// and reach into that side's outer quarter. Small pieces near the middle (aim
// markers, decorations beside a centred box) stay with the centre.
float element_anchor(float low, float high, float size) {
    const float middle = 0.5f * size;
    if (high <= middle + 0.5f && low < 0.25f * size) return 0.0f;
    if (low >= middle - 0.5f && high > 0.75f * size) return size;
    return middle;
}

std::uint32_t move_edge(std::uint32_t edge, float anchor, float scale) {
    return static_cast<std::uint32_t>(std::max(0.0f, std::round(anchor + (static_cast<float>(edge) - anchor) * scale)));
}

// Scissor edges are pixel boundaries: first is the first pixel in, last + 1
// the first one out.
void move_span(std::uint32_t &first, std::uint32_t &last, float anchor, float scale) {
    const std::uint32_t begin = move_edge(first, anchor, scale);
    const std::uint32_t end = move_edge(last + 1u, anchor, scale);
    first = begin;
    last = end > begin ? end - 1u : begin;
}

enum class Place { Untouched, View, Panel, Stretch, Picture, Bar, Element };

struct Placement {
    Place place = Place::Untouched;
    bool clip_only = false;  // left as it is, but clipped to the PSP's area (a 2D screen's screen effects)
    Extent extent;
    bool known = false;
    bool keep_x = false, keep_y = false;  // stretched over the window along that axis
    float ax = 240.0f, ay = 136.0f;       // anchor on the PSP screen
    bool has_origin = false;              // a world translation other than (0, 0)
    float origin_x = 0.0f, origin_y = 0.0f;
    int group = -1;
    float magnified = 0.0f;               // screen pixels per texel (magnification())
    bool faded = false;                   // vertex alpha below opaque (a translucent overlay)
    const char *why = "";                 // trace: the rule that decided
    float scale_x = -1.0f;                // across: this scale about ax instead of the window's (strip layers)
    bool zoom = false;                    // zoomed to fill the window (vc2.backgrounds), about the centre
};

constexpr float kScreenWidth = 480.0f, kScreenHeight = 272.0f;
// How close to a screen edge counts as reaching it: transformed coordinates
// come out a little short (the academy map's vignette ends a hair short of 479).
constexpr float kEdge = 2.0f;

bool full_viewport_of(const gpu::DrawCall &call) {
    const auto &vp = call.viewport;
    const float centre_x = vp.x_offset - vp.offset_x, centre_y = vp.y_offset - vp.offset_y;
    return std::fabs(std::fabs(vp.x_scale) - 240.0f) < 1.0f && std::fabs(std::fabs(vp.y_scale) - 136.0f) < 1.0f &&
           std::fabs(centre_x - 240.0f) < 1.0f && std::fabs(centre_y - 136.0f) < 1.0f;
}

bool perspective_of(const gpu::DrawCall &call) {
    const auto &m = call.projection;
    return !call.through && (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f);
}

bool placing_active() {
    const auto &w = wide();
    return wide_enabled() && (w.sx < 0.9999f || w.sy < 0.9999f);
}

// How many screen pixels each texel covers, across and down (the larger of
// the two is returned). Decorations stretch a small texture over a large area
// (frames, vignettes, bars: tens of times); pictures show theirs at about its
// own size. 0 when untextured or unknown.
float magnification(const gpu::DrawCall &call, const Extent &e) {
    if (!call.texture.enabled || call.vertices.empty()) return 0.0f;
    const auto &t = call.texture;
    float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
    for (const gpu::Vertex &vertex : call.vertices) {
        const float u = call.through ? vertex.texcoord[0] : (vertex.texcoord[0] * t.scale_u + t.offset_u) * t.width;
        const float v = call.through ? vertex.texcoord[1] : (vertex.texcoord[1] * t.scale_v + t.offset_v) * t.height;
        u0 = std::min(u0, u); u1 = std::max(u1, u); v0 = std::min(v0, v); v1 = std::max(v1, v);
    }
    const float across = (e.right - e.left) / std::max(u1 - u0, 0.5f);
    const float down = (e.bottom - e.top) / std::max(v1 - v0, 0.5f);
    return std::max(across, down);
}

// What kind of draw this is for placement, before any grouping.
Placement classify(const gpu::DrawCall &call) {
    Placement p;
    auto &w = wide();
    if (call.clear_mode || !is_shown(call.target.color_address)) return p;
    if (call.texture.enabled && (call.texture.address & 0x0F000000u) == 0x04000000u) {
        ++w.sampled_vram;
        return p;
    }
    if (!call.through) {
        p.has_origin = std::fabs(call.world[12]) >= 0.5f || std::fabs(call.world[13]) >= 0.5f;
        p.origin_x = call.world[12];
        p.origin_y = call.world[13];
    }
    if (!call.through && !full_viewport_of(call)) {
        const auto &vp = call.viewport;
        const float centre_x = vp.x_offset - vp.offset_x, centre_y = vp.y_offset - vp.offset_y;
        p.place = Place::Panel;
        p.ax = anchor_of(centre_x - std::fabs(vp.x_scale), centre_x + std::fabs(vp.x_scale), kScreenWidth);
        p.ay = anchor_of(centre_y - std::fabs(vp.y_scale), centre_y + std::fabs(vp.y_scale), kScreenHeight);
        return p;
    }
    if (perspective_of(call)) {
        // A few vertices in 3D spanning the PSP screen's width (or height) are
        // a screen effect placed in the world, sized to the PSP's view: the
        // academy map's haze over the top 43% of the screen (texture
        // 09a811c0, x -38..518), whose edges showed as a lighter box once
        // Hor+ widened the view. Stretched over the window instead.
        // Only see-through cards facing the camera (every vertex at the same
        // depth): a ground plane seen at an angle stays 3D.
        const auto facing_camera = [&]() {
            float low = 1e30f, high = -1e30f;
            for (const gpu::Vertex &vertex : call.vertices) {
                std::array<float, 4> q{vertex.position[0], vertex.position[1], vertex.position[2], 1.0f};
                for (const auto *mat : {&call.world, &call.view, &call.projection}) {
                    std::array<float, 4> r{};
                    for (std::size_t j = 0; j < 4u; ++j)
                        r[j] = q[0] * (*mat)[j] + q[1] * (*mat)[4u + j] + q[2] * (*mat)[8u + j] + q[3] * (*mat)[12u + j];
                    q = r;
                }
                low = std::min(low, q[3]);
                high = std::max(high, q[3]);
            }
            return low > 1e-6f && high - low <= 0.01f * high;
        };
        if (call.vertices.size() <= 8u && call.blend.enabled && facing_camera()) {
            Extent e;
            const bool known = screen_extent(call, e);
            const bool across = known && e.left <= kEdge && e.right >= kScreenWidth - kEdge;
            const bool down = known && e.top <= kEdge && e.bottom >= kScreenHeight - kEdge;
            if ((w.sx < 0.9999f && across) || (w.sy < 0.9999f && down)) {
                p.known = true;
                p.extent = e;
                p.place = Place::Stretch;
                p.why = "screen card in 3D";
                ++w.view;  // still part of a 3D frame
                return p;
            }
        }
        if (w.trace_frame && call.vertices.size() <= 8u && call.blend.enabled && w.traced < 800u) {
            Extent e;
            ++w.traced;
            std::ostringstream line;
            line << std::fixed << std::setprecision(1) << "[vc2-wide-trace] f" << w.frame << " #" << w.traced
                 << " small 3D v" << call.vertices.size() << " tex "
                 << (call.texture.enabled ? hex8(call.texture.address) : std::string("-"));
            if (screen_extent(call, e)) line << " x " << e.left << ".." << e.right << " y " << e.top << ".." << e.bottom;
            else line << " (partly behind the eye)";
            line << (facing_camera() ? " facing camera" : " at an angle") << " -> 3D view";
            std::cerr << line.str() << "\n";
        }
        p.place = Place::View;
        if (!w.view_this_frame) {
            // 3D that cannot be measured (partly behind the eye, or a large
            // mesh) is world geometry: it fills the screen.
            Extent e;
            if (call.vertices.size() > 8192u || !screen_extent(call, e)) {
                w.view_this_frame = true;
            } else {
                w.view_extent.add(e.left, e.top);
                w.view_extent.add(e.right, e.bottom);
                const float width = std::min(w.view_extent.right, kScreenWidth) - std::max(w.view_extent.left, 0.0f);
                const float height = std::min(w.view_extent.bottom, kScreenHeight) - std::max(w.view_extent.top, 0.0f);
                if (width >= 0.6f * kScreenWidth && height >= 0.6f * kScreenHeight) w.view_this_frame = true;
            }
        }
        return p;
    }
    p.known = screen_extent(call, p.extent);
    if (!p.known) {
        ++w.unknown;
        p.place = Place::Picture;  // centred
        return p;
    }
    const Extent &e = p.extent;
    p.magnified = magnification(call, e);
    p.faded = !call.vertices.empty() && (call.vertices[0].color >> 24u) < 0xF0u;
    const bool full_w = e.left <= kEdge && e.right >= kScreenWidth - kEdge;
    const bool full_h = e.top <= kEdge && e.bottom >= kScreenHeight - kEdge;
    const bool see_through = call.blend.enabled || !call.texture.enabled;
    // A see-through piece of a screen-sized decoration (a 9-slice frame, a
    // vignette, a bar) drawn by magnifying a small texture: stretched over the
    // window whole, so its pieces stay together. A textured piece shown at
    // about its texture's size is part of a picture instead.
    const bool decoration = see_through && (!call.texture.enabled || magnification(call, e) >= 3.0f);
    if ((full_w || full_h) && decoration && call.texture.enabled) {
        p.place = Place::Stretch;
    } else if (full_w && full_h) {
        p.place = see_through ? Place::Stretch : Place::Picture;  // overlays (vignette) stretch, opaque pictures stay whole
    } else if (full_w && see_through) {
        p.place = Place::Bar;  // stretched across, kept by its own edge up and down
        p.keep_x = true;
        p.ay = anchor_of(e.top, e.bottom, kScreenHeight);
    } else if (full_h && call.texture.enabled) {
        p.place = Place::Picture;  // a strip of a full-screen picture
    } else if (full_h) {
        p.place = Place::Bar;  // stretched up and down, kept by its own edge across
        p.keep_y = true;
        p.ax = anchor_of(e.left, e.right, kScreenWidth);
    } else {
        p.place = Place::Element;
    }
    return p;
}

void apply_placement(gpu::DrawCall &call, const Placement &p) {
    auto &w = wide();
    switch (p.place) {
    case Place::Untouched:
        if (p.clip_only) {
            move_span(call.viewport.scissor_x1, call.viewport.scissor_x2, 240.0f, w.sx);
            move_span(call.viewport.scissor_y1, call.viewport.scissor_y2, 136.0f, w.sy);
        }
        return;
    case Place::Stretch: ++w.stretched; return;
    case Place::View: ++w.view; break;
    case Place::Panel: ++w.panels; break;
    case Place::Picture: ++w.pillarboxed; break;
    case Place::Bar: ++w.stretched; break;
    case Place::Element:
        if (p.ax == 0.0f) ++w.left;
        else if (p.ax == kScreenWidth) ++w.right;
        else ++w.centred;
        break;
    }
    auto &vp = call.viewport;
    float sx = p.scale_x >= 0.0f ? p.scale_x : p.keep_x ? 1.0f : w.sx, sy = p.keep_y ? 1.0f : w.sy;
    if (p.zoom) {
        // Square pixels, scaled up until the window is covered: the excess
        // (above and below in a wide window, at the sides in a tall one) is cut.
        const float k = std::max(1.0f / w.sx, 1.0f / w.sy);
        sx = w.sx * k;
        sy = w.sy * k;
    }
    const float ax = p.ax, ay = p.ay;
    const float centre_x = vp.x_offset - vp.offset_x, centre_y = vp.y_offset - vp.offset_y;
    // The scissor moves with the draw. The PSP never shows what lies off its
    // screen (clouds drifting in, tooltips sliding in, the rest of a map), so
    // a full-screen scissor becomes the PSP screen's area at its new place,
    // which keeps that hidden in the wider window. The 3D view alone keeps the
    // whole window: showing more of it is the point.
    const bool full_scissor =
        vp.scissor_x1 == 0u && vp.scissor_y1 == 0u && vp.scissor_x2 >= 479u && vp.scissor_y2 >= 271u;
    if (!full_scissor || p.place != Place::View) {
        move_span(vp.scissor_x1, vp.scissor_x2, ax, sx);
        move_span(vp.scissor_y1, vp.scissor_y2, ay, sy);
    }
    if (call.through) {
        for (gpu::Vertex &vertex : call.vertices) {
            vertex.position[0] = ax + (vertex.position[0] - ax) * sx;
            vertex.position[1] = ay + (vertex.position[1] - ay) * sy;
        }
    } else if (p.place != Place::Panel) {
        // Screen x = centre + clip x * scale; moving it to a + (x - a) * s is
        // clip x * s plus a shift of (a - centre)(1 - s) / scale in clip units,
        // which the projection gives as that many times w. The viewport stays
        // full, so a wider 3D view is not clipped.
        const float tx = (ax - centre_x) * (1.0f - sx) / vp.x_scale;
        const float ty = (ay - centre_y) * (1.0f - sy) / vp.y_scale;
        auto &m = call.projection;
        for (std::size_t row = 0; row < 4u; ++row) {
            m[row * 4u + 0u] = m[row * 4u + 0u] * sx + m[row * 4u + 3u] * tx;
            m[row * 4u + 1u] = m[row * 4u + 1u] * sy + m[row * 4u + 3u] * ty;
        }
    } else {
        vp.x_scale *= sx;
        vp.y_scale *= sy;
        vp.x_offset = vp.offset_x + ax + (centre_x - ax) * sx;
        vp.y_offset = vp.offset_y + ay + (centre_y - ay) * sy;
    }
}

const char *place_name(const Placement &p) {
    switch (p.place) {
    case Place::Untouched: return "untouched";
    case Place::View: return "3D view";
    case Place::Panel: return "panel";
    case Place::Stretch: return "stretch";
    case Place::Picture: return "picture";
    case Place::Bar: return "bar";
    case Place::Element: return "element";
    }
    return "?";
}

// VC2_WATCH_TEXTURE=<address>: logs each time draws with that texture appear
// in or vanish from the frames for more than 3 frames ([vc2-watch], with how
// long the previous state lasted), and marks the moment for a running swap
// hunt (census F11), so a hunt needs no F12 by hand (the academy map's water,
// tex 0x09A86760, going black on a timer at high frame rates, 2026-11-08).
struct TextureWatch {
    std::uint32_t texture = 0u;
    bool seen = false;       // this frame
    bool shown = false;      // the settled state
    bool settled = false;
    std::uint32_t streak = 0u;
    std::uint64_t since_frame = 0u;
    std::chrono::steady_clock::time_point since{};
    std::uint64_t changes = 0u;
};

TextureWatch &texture_watch() {
    static TextureWatch t = [] {
        TextureWatch w;
        if (const char *text = env("WATCH_TEXTURE")) w.texture = static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0));
        if (w.texture != 0u) std::cerr << "[vc2-watch] watching draws with texture " << hex8(w.texture) << "\n";
        return w;
    }();
    return t;
}

void watch_draw(const gpu::DrawCall &call) {
    TextureWatch &t = texture_watch();
    if (t.texture != 0u && call.texture.enabled && call.texture.address == t.texture) t.seen = true;
}

void watch_frame(std::uint64_t frame) {
    TextureWatch &t = texture_watch();
    if (t.texture == 0u) return;
    const bool seen = t.seen;
    t.seen = false;
    const auto now = std::chrono::steady_clock::now();
    if (!t.settled) {
        t.settled = true;
        t.shown = seen;
        t.since_frame = frame;
        t.since = now;
        return;
    }
    if (seen == t.shown) {
        t.streak = 0u;
        return;
    }
    if (++t.streak < 3u) return;
    t.streak = 0u;
    ++t.changes;
    const double seconds = std::chrono::duration<double>(now - t.since).count();
    std::cerr << "[vc2-watch] texture " << hex8(t.texture) << (seen ? " back" : " gone") << " at frame " << frame
              << " (change " << t.changes << "; " << (t.shown ? "shown" : "gone") << " for " << frame - t.since_frame
              << " frames, " << std::fixed << std::setprecision(2) << seconds << std::defaultfloat << " s)\n";
    t.shown = seen;
    t.since_frame = frame;
    t.since = now;
    census_mark();
}

void trace_placement(const gpu::DrawCall &call, const Placement &p) {
    watch_draw(call);
    auto &w = wide();
    const bool scene = scene_trace_enabled();
    // The scene trace also lists the 3D view's draws (by texture and world
    // position), to see which ones a frame leaves out (the academy map's water
    // going black at high frame rates, 2026-11-08).
    if (p.place == Place::View && !scene) return;
    if (!scene && (!w.trace_frame || w.traced >= 800u || p.place == Place::Untouched)) return;
    if (scene && scene_draw_lines().size() >= 600u) return;
    ++w.traced;
    std::ostringstream line;
    line << std::fixed << std::setprecision(1) << "[vc2-wide-trace] f" << w.frame << " #" << w.traced << " "
         << place_name(p) << (p.group >= 0 ? " g" + std::to_string(p.group) : std::string())
         << (call.through ? " through" : "") << " v" << call.vertices.size() << " tex "
         << (call.texture.enabled ? hex8(call.texture.address) : std::string("-")) << " blend " << call.blend.enabled;
    if (p.known) line << " x " << p.extent.left << ".." << p.extent.right << " y " << p.extent.top << ".." << p.extent.bottom;
    if (p.has_origin) line << " origin " << p.origin_x << "," << p.origin_y;
    if (!call.through) line << " z " << call.world[14];
    if (p.place == Place::View) line << " world " << call.world[12] << "," << call.world[13] << "," << call.world[14];
    if (call.texture.enabled) line << " tex " << call.texture.width << "x" << call.texture.height << " mag " << p.magnified;
    if (p.faded) line << " faded";
    if (*p.why) line << " [" << p.why << "]";
    line << " -> x " << (p.keep_x ? std::string("stretch") : p.ax == 0.0f ? "left" : p.ax == kScreenWidth ? "right" : "centre")
         << ", y " << (p.keep_y ? std::string("stretch") : p.ay == 0.0f ? "top" : p.ay == kScreenHeight ? "bottom" : "centre");
    if (p.scale_x >= 0.0f) line << " scale_x " << p.scale_x;
    line << " fb " << hex8(call.target.color_address);
    // VC2_TRACE_SCENE_DETAIL: primitive, texture coordinates, colours and the
    // world origin, and the first vertices (the in-game settings screen copies
    // the Options screen's own draws, 2026-11-09).
    static const bool detail = env("TRACE_SCENE_DETAIL") != nullptr;
    if (detail && !call.vertices.empty()) {
        float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
        for (const gpu::Vertex &v : call.vertices) {
            u0 = std::min(u0, v.texcoord[0]), u1 = std::max(u1, v.texcoord[0]);
            v0 = std::min(v0, v.texcoord[1]), v1 = std::max(v1, v.texcoord[1]);
        }
        line << std::setprecision(4) << " | prim " << static_cast<int>(call.primitive) << " uv " << u0 << ".." << u1
             << " " << v0 << ".." << v1 << " col " << hex8(call.vertices.front().color) << ".."
             << hex8(call.vertices.back().color) << " mat " << hex8(call.material_color) << " vtype "
             << hex8(call.vertex_type) << " uvrect " << call.uv_rects << " world " << call.world[12] << ","
             << call.world[13] << " sc " << call.world[0] << "," << call.world[5] << " ts "
             << call.texture_matrix[0] << "," << call.texture_matrix[5] << "," << call.texture_matrix[12] << ","
             << call.texture_matrix[13] << " fmt " << static_cast<int>(call.texture.format);
        const std::size_t shown = std::min<std::size_t>(call.vertices.size(), 12u);
        for (std::size_t i = 0; i < shown; ++i) {
            const gpu::Vertex &v = call.vertices[i];
            line << " [" << v.position[0] << "," << v.position[1] << "," << v.position[2] << " uv " << v.texcoord[0]
                 << "," << v.texcoord[1] << " c " << hex8(v.color) << "]";
        }
    }
    if (scene) scene_draw_lines().push_back(line.str());
    else std::cerr << line.str() << "\n";
}

// One draw on its own (the display-list path): elements stay centred.
void place_single(gpu::DrawCall &call) {
    if (!placing_active()) return;
    const Placement p = classify(call);
    trace_placement(call, p);
    apply_placement(call, p);
}

// A whole frame's draws, in order (the native list executor): elements are
// grouped first and each group is placed by the edge it lies by.

// --- Backgrounds zoomed to fill the window (vc2.backgrounds) ---------------------------
// Some players never want the PSP's shape with black beside it in a wide (or
// tall) window. With vc2.backgrounds=zoom, a full-screen picture (a 2D
// screen's backdrop, story art, the title's backdrop; drawn in one or in
// strips of one texture, opaque, at about its texture's size) is zoomed about
// the centre until it covers the window, square pixels kept, the excess
// above and below cut, as Valkyria Chronicles 4 does. What is drawn over it
// (text boxes, menus, portraits) keeps its place at the PSP's shape; and a
// see-through layer over the whole screen (a fade, a vignette) is zoomed too
// while a backdrop is, so a fade still darkens the whole window. Movies stay
// whole unless the choice is zoom-movies. Screens whose interface points at
// places in the picture (a map with markers) would come apart, so a picture
// with interface drawn into it at its own scale is not zoomed... that can only
// be found in play: VC2_TRACE_WIDE shows "zoomed" on each draw that was.

std::uint64_t &movie_until_frame() {
    static std::uint64_t until = 0u;
    return until;
}

void zoom_backgrounds(std::vector<Placement> &placements, const std::vector<gpu::DrawCall *> &calls,
                      bool screen_2d) {
    auto &w = wide();
    if (!(w.sx < 0.9999f || w.sy < 0.9999f)) return;
    const std::string &choice = options::get(options::Key::Backgrounds);
    if (choice != "zoom" && choice != "zoom-movies") return;
    // 2D screens only: in a 3D scene (the academy map) the interface sheets
    // reaching every edge were taken for a frame and stretched (EXP and DCT
    // came apart from their numbers, 2026-11-13).
    if (!screen_2d) return;
    const bool movie_time = w.frame <= movie_until_frame();
    const bool movies_too = choice == "zoom-movies";
    // A movie frame: a 32-bit picture 512 texels wide, at least 272 tall (the
    // decoder's output, VC2's own art is paletted). Kept whole unless the
    // choice is zoom-movies, from its very first frame (the decode call alone
    // came a moment late: the movie was zoomed for its first second).
    const auto movie_frame = [](const gpu::DrawCall &call) {
        return call.texture.enabled && call.texture.format == gpu::TextureFormat::Rgba8888 &&
               call.texture.width == 512u && call.texture.height >= 272u;
    };
    // Textured draws at about their texture's size, by texture: a texture
    // whose draws together cover the whole screen, in large pieces (strips or
    // the whole picture, not an interface sheet's small parts spread over the
    // screen), is a backdrop. Blended or not: VC2 draws most of its 2D with
    // blending on (only opaque ones counted, nothing was zoomed: 2026-11-13).
    constexpr float kScreenArea = kScreenWidth * kScreenHeight;
    std::map<std::uint32_t, Extent> boxes;
    std::map<std::uint32_t, bool> usable;
    std::map<std::uint32_t, float> areas;
    struct TextureStats {
        int draws = 0;
        float min_area = 1e9f, min_mag = 1e9f, max_mag = 0.0f;
        bool blended = false, movie = false;
        bool through = false, tiled = false;
        std::uint32_t width = 0u, height = 0u;
        int format = 0;
    };
    std::map<std::uint32_t, TextureStats> stats;
    std::set<std::size_t> pieces;  // draws that are pieces of a picture
    std::map<std::uint32_t, Extent> all_boxes;
    std::map<std::uint32_t, float> all_areas;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        const Placement &p = placements[i];
        const gpu::DrawCall &call = *calls[i];
        if (!p.known || !call.texture.enabled || call.clear_mode) continue;
        if (p.place == Place::Untouched || p.place == Place::View || p.place == Place::Panel) continue;
        const std::uint32_t texture = call.texture.address;
        const float area = std::max(0.0f, p.extent.right - p.extent.left) * std::max(0.0f, p.extent.bottom - p.extent.top);
        const bool movie = movie_frame(call);
        // A piece of a picture: shown at up to 2.5x its texture (VC2 keeps
        // some backdrops at half size: the title's, at 1.9) and not tiny. A
        // texture's other draws (a strip of it stretched as a bar) do not
        // count, and are not zoomed with it.
        const bool piece = p.magnified > 0.0f && p.magnified < 2.5f && area >= kScreenArea / 32.0f &&
                           (!movie || movies_too) && (movies_too || !movie_time || !movie);
        auto [it, fresh] = usable.emplace(texture, true);
        it->second = it->second && (!movie || movies_too) && (movies_too || !movie_time || !movie);
        if (piece) {
            pieces.insert(i);
            areas[texture] += area;
            boxes[texture].add(p.extent.left, p.extent.top);
            boxes[texture].add(p.extent.right, p.extent.bottom);
        }
        all_boxes[texture].add(p.extent.left, p.extent.top);
        all_boxes[texture].add(p.extent.right, p.extent.bottom);
        all_areas[texture] += area;
        TextureStats &st = stats[texture];
        ++st.draws;
        st.min_area = std::min(st.min_area, area);
        st.min_mag = std::min(st.min_mag, p.magnified);
        st.max_mag = std::max(st.max_mag, p.magnified);
        st.blended = st.blended || call.blend.enabled;
        st.movie = st.movie || movie;
        st.through = st.through || call.through;
        // Repeated across the draw (a 32x32 paper texture laid over the
        // battle map): a surface, never a character.
        if (piece && p.magnified > 0.0f && call.texture.width > 0u && call.texture.height > 0u &&
            ((p.extent.right - p.extent.left) / p.magnified > 1.5f * static_cast<float>(call.texture.width) ||
             (p.extent.bottom - p.extent.top) / p.magnified > 1.5f * static_cast<float>(call.texture.height)))
            st.tiled = true;
        st.width = call.texture.width, st.height = call.texture.height;
        st.format = static_cast<int>(call.texture.format);
    }
    std::set<std::uint32_t> backdrops;
    for (const auto &[texture, box] : boxes)
        if (usable[texture] && areas[texture] >= kScreenArea * 0.9f && box.left <= kEdge &&
            box.right >= kScreenWidth - kEdge && box.top <= kEdge && box.bottom >= kScreenHeight - kEdge)
            backdrops.insert(texture);
    // One backdrop is zoomed: the scene. A full-screen picture in front of it
    // is a character (story art: Hubert at z -146 in front of the town at
    // -150, his eyes and mouth drawn over him as small pieces) and keeps the
    // PSP's shape. So the deepest one (most negative z); level ones (the
    // title: a dark plate and smoke at 1.9x, then the title art at 1.1x, all
    // at -80) go to the sharpest, the one drawn nearest its texture's size.
    // It is kept for a moment (kStickyFrames): a backdrop zoomed just before
    // stays zoomed while it shakes or slides (Avan's "What?!" snapped the
    // scene to 16:9 and back), and nothing shallower takes its place while
    // the scene changes (Hubert fading in, alone for a few frames, was
    // zoomed and left his face behind) (2026-11-13).
    struct Sticky {
        std::uint32_t texture = 0u;
        std::uint64_t frame = 0u;
        float z = 0.0f;
    };
    static Sticky sticky;
    constexpr std::uint64_t kStickyFrames = 30u;
    const bool recent = sticky.texture != 0u && w.frame <= sticky.frame + kStickyFrames;
    // z of each texture's first piece. A through-mode draw has no depth of
    // its own (world[14] is whatever the last 3D draw left: -1, -2801 for the
    // same battle picture), so it counts as the shallowest for the choice and
    // is never left out for being in front (2026-11-13).
    constexpr float kNoDepth = 1e9f;
    std::map<std::uint32_t, float> depth;
    std::map<std::uint32_t, std::size_t> first_piece;
    for (std::size_t i = 0; i < calls.size(); ++i)
        if (pieces.count(i) != 0u && depth.count(calls[i]->texture.address) == 0u) {
            depth[calls[i]->texture.address] = calls[i]->through ? kNoDepth : calls[i]->world[14];
            first_piece[calls[i]->texture.address] = i;
        }
    if (recent && backdrops.count(sticky.texture) == 0u && areas[sticky.texture] >= kScreenArea * 0.6f &&
        usable[sticky.texture])
        backdrops.insert(sticky.texture);  // still the scene, moved off an edge
    // Nothing in front of the scene zoomed a moment ago takes its place (the
    // scene itself may move in depth: the battle map's paper did).
    if (recent)
        std::erase_if(backdrops, [&](std::uint32_t t) {
            return t != sticky.texture && depth[t] != kNoDepth && depth[t] > sticky.z + 0.5f;
        });
    std::uint32_t chosen = backdrops.size() == 1u ? *backdrops.begin() : 0u;
    if (backdrops.size() > 1u) {
        // The deepest, then the sharpest.
        std::uint32_t best = 0u;
        for (const std::uint32_t t : backdrops) {
            if (best == 0u) { best = t; continue; }
            const float dz = depth[t] - depth[best];
            if (dz < -0.5f || (dz <= 0.5f && stats[t].min_mag < stats[best].min_mag - 0.05f)) best = t;
        }
        // The one zoomed now stays while it is drawn, until another has been
        // the better choice for kStickyFrames running: switching at once
        // flickered on the battle map (two pictures trading places in
        // depth), holding on for good kept the title's dark plate (zoomed
        // while alone during the storage check) in front of the art.
        static std::uint32_t challenger = 0u;
        static std::uint64_t challenger_since = 0u;
        if (recent && backdrops.count(sticky.texture) != 0u && best != sticky.texture) {
            if (challenger != best) challenger = best, challenger_since = w.frame;
            if (w.frame < challenger_since + kStickyFrames) best = sticky.texture;
        } else {
            challenger = 0u;
        }
        chosen = best;
        // Zoomed with it: every backdrop beneath it (the title's plate and
        // smoke under its see-through sketch art, which alone left the plate
        // at the PSP's shape: pillarboxes with the art's lines over black),
        // and every surface (the battle map's paper and its border, tiled:
        // either one alone left the other at the PSP's shape for a frame,
        // the flashes on the battle map). A picture over it (Hubert over the
        // town) is not (2026-11-13).
        const std::size_t chosen_first = first_piece[best];
        std::erase_if(backdrops, [&](std::uint32_t t) {
            return t != best && !(first_piece[t] < chosen_first) && !stats[t].tiled;
        });
    }
    if (!backdrops.empty()) {
        if (chosen != sticky.texture && !movie_time)
            std::cerr << "[vc2-backgrounds] f" << w.frame << ": zooming 0x" << std::hex << chosen << std::dec << " (z "
                      << depth[chosen] << ", " << stats[chosen].min_mag << "x)\n";
        sticky = {chosen, w.frame, depth[chosen]};
    }
    // A screen with nothing to zoom just after one that had (a fade alone
    // between the battle map's command point animation and the cursor, and
    // after an action turn): its see-through layers are still zoomed, or the
    // fade covered the PSP's shape only, a flash of pillarboxes (2026-11-13).
    const bool layers_only = backdrops.empty() && recent && !movie_time;
    if (backdrops.empty() && !layers_only) return;
    // A frame drawn over the scene in pieces at its edges (the story scenes'
    // white mist: see-through, one texture, reaching every edge of the screen
    // but covering little of it) goes to the window's edges with the zoomed
    // scene, stretched (it stayed at the PSP's edges, 2026-11-13).
    const float chosen_z = chosen != 0u && depth.count(chosen) != 0u ? depth[chosen] : kNoDepth;
    const std::size_t chosen_first = chosen != 0u && first_piece.count(chosen) != 0u ? first_piece[chosen] : 0u;
    std::set<std::uint32_t> frames;
    for (const auto &[texture, box] : all_boxes) {
        if (layers_only) break;
        const TextureStats &st = stats[texture];
        if (backdrops.count(texture) == 0u && st.blended && !st.movie && all_areas[texture] < kScreenArea * 0.6f &&
            box.left <= kEdge && box.right >= kScreenWidth - kEdge && box.top <= kEdge && box.bottom >= kScreenHeight - kEdge)
            frames.insert(texture);
    }
    for (std::size_t i = 0; i < placements.size(); ++i) {
        Placement &p = placements[i];
        const gpu::DrawCall &call = *calls[i];
        if (p.place == Place::Untouched || p.place == Place::View || p.place == Place::Panel || !p.known) continue;
        if (call.texture.enabled && frames.count(call.texture.address) != 0u) {
            p.place = Place::Stretch;
            p.why = "frame stretched with the zoomed scene";
            continue;
        }
        const bool backdrop =
            call.texture.enabled && backdrops.count(call.texture.address) != 0u && pieces.count(i) != 0u;
        // Drawn beneath the chosen backdrop, at its depth: part of the same
        // scene, seen through it (the title's spire, a small picture under
        // the see-through art, stayed at the PSP's place, 2026-11-13).
        const bool beneath = chosen_z != kNoDepth && i < chosen_first && !call.through &&
                             std::fabs(call.world[14] - chosen_z) <= 0.5f;
        // A see-through layer over the whole screen: a fade (no texture), a
        // vignette (a small texture magnified). A see-through picture at
        // about its texture's size is a character drawn over the scene (story
        // art: Hubert, whose eyes and mouth are pieces drawn over him) and
        // keeps the PSP's shape (2026-11-13).
        const bool layer = !backdrop && (!call.texture.enabled || (call.blend.enabled && p.magnified >= 1.5f)) &&
                           p.extent.left <= kEdge && p.extent.right >= kScreenWidth - kEdge &&
                           p.extent.top <= kEdge && p.extent.bottom >= kScreenHeight - kEdge;
        if (!backdrop && !layer && !beneath) continue;
        p.place = Place::Picture;
        p.zoom = true;
        p.keep_x = p.keep_y = false;
        p.scale_x = -1.0f;
        p.ax = 240.0f;
        p.ay = 136.0f;
        p.why = backdrop ? "zoomed backdrop" : beneath ? "zoomed beneath the backdrop" : "zoomed layer";
    }
    (void)screen_2d;
}

void place_frame(const std::vector<gpu::DrawCall *> &calls) {
    if (!placing_active() || calls.empty()) return;
    std::vector<Placement> placements;
    placements.reserve(calls.size());
    for (const gpu::DrawCall *call : calls) placements.push_back(classify(*call));
    // Markers on the 3D view: 2D sprites the game places at a projected 3D
    // point (the academy map's facility icons, the combat crosshair) are laid
    // at the far end of the depth range (world z about -4960; the interface
    // uses -90 to -789). They take exactly the 3D view's Hor+ mapping, so
    // they move with the world (anchored like the interface, they jumped
    // between edges while the map was panned), and are not clipped to the
    // PSP's area, so they also appear in the widened sides.
    constexpr float kMarkerDepth = -1000.0f;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        Placement &p = placements[i];
        if (p.place == Place::Element && !calls[i]->through && calls[i]->world[14] < kMarkerDepth) {
            p.place = Place::View;
            p.ax = 240.0f;
            p.ay = 136.0f;
            p.why = "marker on the 3D view";
        }
    }
    // A backdrop over the whole screen, drawn in the scene at about
    // its texture's size in strips of one texture (the Briefing Room's and
    // the homeroom's classrooms: four strips at z -1960, 1.1 screen pixels
    // per texel), makes the frame a 2D screen even with 3D drawn in front:
    // the homeroom news has a 3D view over most of the screen and was laid
    // out like a 3D scene (background spread, title at the window's edge).
    // A backdrop fading in or out counts too: the crossfade from the academy
    // map into the homeroom news (or the Briefing Room) then plays inside the
    // PSP's area, as on the PSP, with one switch at its start; judged a 3D
    // scene, the map stayed in the widened sides for the second the news took
    // to fade in (2026-11-08). The academy map and combat draw no backdrop.
    {
        std::map<std::uint32_t, Extent> backdrops;
        std::map<std::uint32_t, int> counts;
        std::map<std::uint32_t, bool> usable;
        for (std::size_t i = 0; i < placements.size(); ++i) {
            const Placement &p = placements[i];
            const gpu::DrawCall &call = *calls[i];
            if (!p.known || !call.texture.enabled || call.through) continue;
            if (p.place != Place::Picture && p.place != Place::Element) continue;
            const std::uint32_t texture = call.texture.address;
            auto [it, fresh] = usable.emplace(texture, true);
            it->second = it->second && p.magnified > 0.0f && p.magnified < 1.5f;
            backdrops[texture].add(p.extent.left, p.extent.top);
            backdrops[texture].add(p.extent.right, p.extent.bottom);
            ++counts[texture];
        }
        for (const auto &[texture, box] : backdrops)
            if (usable[texture] && counts[texture] >= 2 && box.left <= kEdge && box.right >= kScreenWidth - kEdge &&
                box.top <= kEdge && box.bottom >= kScreenHeight - kEdge)
                wide().backdrop_this_frame = true;
    }
    // Overlays drawn in pieces (the vignette as a 9-slice of strips and
    // bands, a dark frame): see-through draws of one texture that together
    // cover the whole screen, magnified or translucent, are one layer and are
    // stretched over the window. A picture drawn in strips (the title's
    // backdrop, at about its texture's size and opaque colour) is not.
    {
        std::map<std::uint32_t, std::vector<std::size_t>> by_texture;
        for (std::size_t i = 0; i < placements.size(); ++i) {
            const Placement &p = placements[i];
            if (!calls[i]->texture.enabled || !calls[i]->blend.enabled || !p.known) continue;
            if (p.place != Place::Picture && p.place != Place::Element && p.place != Place::Bar) continue;
            by_texture[calls[i]->texture.address].push_back(i);
        }
        // In a 3D scene a see-through layer over the whole screen can only be
        // an overlay (the academy map's vignette: four strips at about their
        // texture's size, translucent only while it fades): full-screen
        // pictures belong to 2D screens, which are placed whole below.
        auto &state = wide();
        const bool has_view = state.view_this_frame || state.view_last_frame;
        for (const auto &[texture, members] : by_texture) {
            if (members.size() < 2u) continue;
            Extent box;
            bool overlay = false;
            // A backdrop drawn in strips at about its texture's size, laid out
            // in the scene rather than on the screen (the Briefing Room's
            // classroom: four strips at z -1960, 1.1 screen pixels per texel)
            // is a picture even while it fades in or out over a 3D view: as
            // an overlay it was spread over the window during the crossfade
            // from the academy map, then cut into strips of different scales
            // until the fade ended (2026-11-08).
            bool backdrop = true;
            for (const std::size_t i : members) {
                const Placement &p = placements[i];
                box.add(p.extent.left, p.extent.top);
                box.add(p.extent.right, p.extent.bottom);
                overlay = overlay || has_view || p.magnified >= 2.0f || p.faded;
                backdrop = backdrop && !calls[i]->through && p.magnified > 0.0f && p.magnified < 1.5f;
            }
            if (backdrop || !overlay || box.left > kEdge || box.right < kScreenWidth - kEdge || box.top > kEdge ||
                box.bottom < kScreenHeight - kEdge)
                continue;
            // A layer of full-height strips side by side (the academy map's
            // vignette: four) is laid out like a 9-slice: the outer strips
            // keep their proportions at the window's edges, so the rim keeps
            // its thickness, and the strips between are stretched to meet
            // them. Shared edges map to the same place, so no seam opens.
            std::vector<std::size_t> strips(members.begin(), members.end());
            std::sort(strips.begin(), strips.end(), [&](std::size_t a, std::size_t b) {
                return placements[a].extent.left < placements[b].extent.left;
            });
            bool layout = strips.size() >= 3u && wide().sx < 0.9999f;
            for (std::size_t k = 0; layout && k < strips.size(); ++k) {
                const Extent &e = placements[strips[k]].extent;
                layout = e.top <= kEdge && e.bottom >= kScreenHeight - kEdge &&
                         (k == 0u || std::fabs(e.left - placements[strips[k - 1u]].extent.right) <= 1.0f);
            }
            if (!layout) {
                for (const std::size_t i : members) {
                    placements[i].place = Place::Stretch;
                    placements[i].why = "overlay layer";
                }
                continue;
            }
            const float sx = wide().sx;
            const float inner_left = placements[strips.front()].extent.right;
            const float inner_right = placements[strips.back()].extent.left;
            // Where the inner span lands: right after the left strip, kept by
            // the left edge, up to the right strip, kept by the right edge.
            const float new_left = inner_left * sx;
            const float new_right = kScreenWidth - (kScreenWidth - inner_right) * sx;
            const float inner_scale = (new_right - new_left) / std::max(inner_right - inner_left, 1.0f);
            const float inner_anchor = std::fabs(1.0f - inner_scale) < 1e-4f
                                           ? inner_left
                                           : (new_left - inner_left * inner_scale) / (1.0f - inner_scale);
            for (std::size_t k = 0; k < strips.size(); ++k) {
                Placement &p = placements[strips[k]];
                p.place = Place::Bar;
                p.keep_y = true;
                p.keep_x = false;
                p.ay = 136.0f;
                p.why = "overlay strips";
                if (k == 0u) {
                    p.ax = 0.0f;
                } else if (k + 1u == strips.size()) {
                    p.ax = kScreenWidth;
                } else {
                    p.ax = inner_anchor;
                    p.scale_x = inner_scale;
                }
            }
        }
    }
    // Bars drawn in pieces (letterbox bands in two halves): see-through
    // pieces at the same height that together cover the full width are
    // stretched across like a single full-width draw.
    {
        std::map<std::pair<long, long>, std::vector<std::size_t>> rows;
        for (std::size_t i = 0; i < placements.size(); ++i) {
            const Placement &p = placements[i];
            if (p.place != Place::Element || !(calls[i]->blend.enabled || !calls[i]->texture.enabled)) continue;
            // Bands only: taller rows of tiles (the title's line art, two
            // rows of eight) are pictures, grouped like any other element.
            if (p.extent.bottom - p.extent.top > 64.0f) continue;
            // ... at the screen's top or bottom edge (letterbox bands); a
            // gauge in segments across the top (the CP bar) is an element.
            if (p.extent.top > kEdge && p.extent.bottom < kScreenHeight - kEdge) continue;
            if (calls[i]->texture.enabled && magnification(*calls[i], p.extent) < 3.0f) continue;
            rows[{std::lround(p.extent.top), std::lround(p.extent.bottom)}].push_back(i);
        }
        for (const auto &[row, members] : rows) {
            if (members.size() < 2u) continue;
            std::vector<std::pair<float, float>> spans;
            for (const std::size_t i : members) spans.emplace_back(placements[i].extent.left, placements[i].extent.right);
            std::sort(spans.begin(), spans.end());
            float covered = spans.front().first > kEdge ? -1.0f : spans.front().second;
            for (std::size_t k = 1; k < spans.size() && covered >= 0.0f; ++k)
                if (spans[k].first <= covered + 1.0f) covered = std::max(covered, spans[k].second);
            if (covered < kScreenWidth - kEdge) continue;
            for (const std::size_t i : members) {
                Placement &p = placements[i];
                p.place = Place::Bar;
                p.keep_x = true;
                p.ay = anchor_of(p.extent.top, p.extent.bottom, kScreenHeight);
            }
        }
    }
    std::vector<std::size_t> elements;
    for (std::size_t i = 0; i < placements.size(); ++i)
        if (placements[i].place == Place::Element) elements.push_back(i);
    std::vector<std::size_t> parent(placements.size());
    for (std::size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    const auto find = [&](std::size_t i) {
        while (parent[i] != i) i = parent[i] = parent[parent[i]];
        return i;
    };
    const auto join = [&](std::size_t a, std::size_t b) { parent[find(a)] = find(b); };
    const auto overlaps = [](const Extent &a, const Extent &b) {
        constexpr float kTouch = 1.0f;
        return a.left <= b.right + kTouch && b.left <= a.right + kTouch && a.top <= b.bottom + kTouch &&
               b.top <= a.bottom + kTouch;
    };
    const auto inside = [](const Extent &a, const Extent &b) {
        return a.left >= b.left - 1.0f && a.right <= b.right + 1.0f && a.top >= b.top - 1.0f && a.bottom <= b.bottom + 1.0f;
    };
    const auto large = [](const Extent &e) {
        const float width = e.right - e.left, height = e.bottom - e.top;
        return width >= 0.5f * kScreenWidth || width * height >= kScreenWidth * kScreenHeight / 6.0f;
    };
    // 1. Pieces laid out from the same origin are one element.
    for (std::size_t m = 0; m < elements.size(); ++m)
        for (std::size_t n = m + 1u; n < elements.size(); ++n) {
            const Placement &a = placements[elements[m]], &b = placements[elements[n]];
            if (a.has_origin && b.has_origin && std::fabs(a.origin_x - b.origin_x) < 0.5f &&
                std::fabs(a.origin_y - b.origin_y) < 0.5f)
                join(elements[m], elements[n]);
        }
    struct OriginGroup {
        std::vector<std::size_t> members;
        Extent box;
        bool large = false;
        long container = -1;  // the root of the large group it lies inside
        std::size_t first = static_cast<std::size_t>(-1);  // its first draw
    };
    std::map<std::size_t, OriginGroup> origin_groups;
    for (const std::size_t i : elements) {
        OriginGroup &g = origin_groups[find(i)];
        g.members.push_back(i);
        g.box.add(placements[i].extent.left, placements[i].extent.top);
        g.box.add(placements[i].extent.right, placements[i].extent.bottom);
        g.large = g.large || large(placements[i].extent);
        g.first = std::min(g.first, i);
    }
    // 2. A small element lying wholly inside a large one (an icon in a menu
    // row, a marker on a map) belongs to the smallest such. Containment is
    // judged for the whole origin group, so a panel with one piece over a map
    // keeps its own place. Only a large element drawn before it can hold it
    // (a menu's panel, then its icons): a tutorial's text box, drawn over the
    // HUD, took the minimap's markers and the unit panel's portrait under it,
    // and with them the minimap and the panel, to the centre (2026-11-12).
    for (auto &[root, g] : origin_groups) {
        if (g.large) continue;
        float best = 1e30f;
        for (const auto &[other, h] : origin_groups) {
            if (!h.large || h.first > g.first || !inside(g.box, h.box)) continue;
            const float area = (h.box.right - h.box.left) * (h.box.bottom - h.box.top);
            if (area < best) {
                best = area;
                g.container = static_cast<long>(other);
            }
        }
    }
    std::vector<std::pair<std::size_t, long>> roots;
    std::vector<long> container_of(placements.size(), -1);
    for (const auto &[root, g] : origin_groups) {
        roots.emplace_back(root, g.container);
        for (const std::size_t i : g.members) container_of[i] = g.container;
    }
    for (const auto &[root, container] : roots)
        if (container >= 0) join(root, static_cast<std::size_t>(container));
    // 3. Small elements that overlap, within the same container and drawn
    // close together, are one. The game draws an element's pieces in a run;
    // without the draw-order limit the sniper scope's zoom pointer, when low
    // on its ladder, bridged the scope to the unit panel beside it and the
    // meter jumped between the centre and the right edge.
    constexpr std::size_t kNearDraws = 16u;
    for (std::size_t m = 0; m < roots.size(); ++m) {
        const OriginGroup &a = origin_groups[roots[m].first];
        if (a.large) continue;
        for (std::size_t n = m + 1u; n < roots.size(); ++n) {
            const OriginGroup &b = origin_groups[roots[n].first];
            if (b.large || a.container != b.container) continue;
            bool touching = false;
            for (const std::size_t i : a.members) {
                for (const std::size_t j : b.members)
                    if ((i > j ? i - j : j - i) <= kNearDraws && overlaps(placements[i].extent, placements[j].extent)) {
                        touching = true;
                        break;
                    }
                if (touching) break;
            }
            if (touching) join(roots[m].first, roots[n].first);
        }
    }
    // 4. Words of one line of text are one element: pieces of one texture
    // drawn close together, on the same line (overlapping by most of their
    // height) with a gap of at most about a letter's height between them. A
    // phase banner flying in letter by letter split at its word gap: the
    // first word was pinned to the left edge and the rest stayed centred, so
    // the line came apart as it moved (2026-11-08). Only within one container
    // and only lines of about the same height: a tutorial's message (several
    // lines, in its box) took the unit panel's name beside it, and the panel
    // with it, to the centre; the bottom bar's thin strip took the panel too
    // (2026-11-12).
    for (std::size_t m = 0; m < elements.size(); ++m) {
        const std::size_t i = elements[m];
        const gpu::DrawCall &ci = *calls[i];
        if (!ci.texture.enabled) continue;
        const Extent &a = placements[i].extent;
        for (std::size_t n = m + 1u; n < elements.size() && elements[n] - i <= 2u * kNearDraws; ++n) {
            const std::size_t j = elements[n];
            const gpu::DrawCall &cj = *calls[j];
            if (!cj.texture.enabled || cj.texture.address != ci.texture.address || ci.through != cj.through) continue;
            if (container_of[i] != container_of[j]) continue;
            const Extent &b = placements[j].extent;
            const float ha = a.bottom - a.top, hb = b.bottom - b.top;
            const float shared = std::min(a.bottom, b.bottom) - std::max(a.top, b.top);
            if (ha <= 0.0f || hb <= 0.0f || shared < 0.6f * std::max(ha, hb)) continue;
            const float gap = std::max(a.left, b.left) - std::min(a.right, b.right);
            if (gap > std::max(ha, hb)) continue;
            join(i, j);
        }
    }
    std::map<std::size_t, Extent> boxes;
    for (const std::size_t i : elements) {
        Extent &box = boxes[find(i)];
        box.add(placements[i].extent.left, placements[i].extent.top);
        box.add(placements[i].extent.right, placements[i].extent.bottom);
    }
    std::map<std::size_t, int> ids;
    for (const std::size_t i : elements) {
        const std::size_t root = find(i);
        const Extent &box = boxes[root];
        Placement &p = placements[i];
        p.ax = element_anchor(box.left, box.right, kScreenWidth);
        p.ay = element_anchor(box.top, box.bottom, kScreenHeight);
        p.group = ids.emplace(root, static_cast<int>(ids.size())).first->second;
    }
    // A see-through or plain panel over the full height at one side (the sniper
    // scope's black mask beside its ring) is stretched to meet the centred
    // content next to it, not just to its own PSP edge, which left a gap of
    // 3D view between the two. Its inner edge goes where the centred mapping
    // puts that edge; its outer edge stays at the window's edge.
    if (wide().sx < 0.9999f) {
        const float sx = wide().sx;
        for (Placement &p : placements) {
            if (p.place != Place::Stretch || !p.known) continue;
            const Extent &e = p.extent;
            if (e.top > kEdge || e.bottom < kScreenHeight - kEdge) continue;
            if (e.left <= kEdge && e.right < 240.0f - 1.0f && e.right > 1.0f) {
                const float inner = 240.0f + (e.right - 240.0f) * sx;
                p.place = Place::Bar;
                p.keep_y = true;
                p.ax = 0.0f;
                p.scale_x = inner / e.right;
                p.why = "side mask meeting the centre";
            } else if (e.right >= kScreenWidth - kEdge && e.left > 240.0f + 1.0f && e.left < kScreenWidth - 1.0f) {
                const float inner = 240.0f + (e.left - 240.0f) * sx;
                p.place = Place::Bar;
                p.keep_y = true;
                p.ax = kScreenWidth;
                p.scale_x = (kScreenWidth - inner) / (kScreenWidth - e.left);
                p.why = "side mask meeting the centre";
            }
        }
    }
    // A 2D screen (no 3D view in the frame: the title, menus, the Briefing
    // Room, data select, the battle overview's paper map, movies) is kept
    // whole: everything in it, overlays and bars included, is centred at the
    // PSP's shape and clipped to it, with black beside it. Anchoring to the
    // window's edges is for interface drawn over a 3D view (the academy map,
    // combat), where Hor+ fills the sides. Anchoring parts of 2D screens split
    // them (2026-11-07: Briefing and Load menus at the left edge only, the
    // battle overview's CP bar and unit panel in pieces, the title's cloud
    // vignette and sunset layer stretched).
    const bool screen_2d = (!wide().view_this_frame && !wide().view_last_frame) || wide().backdrop_this_frame ||
                           wide().backdrop_last_frame;
    if (screen_2d) {
        for (std::size_t i = 0; i < placements.size(); ++i) {
            Placement &p = placements[i];
            if (p.place == Place::Untouched) {
                // A screen effect that draws a copy of the screen back over
                // it (the academy map's glow, still running under the homeroom
                // news) covers the whole window; in a 2D screen it is kept to
                // the PSP's area, or the map's glow showed beside the screen.
                const gpu::DrawCall &call = *calls[i];
                if (!call.clear_mode && is_shown(call.target.color_address) && call.texture.enabled &&
                    (call.texture.address & 0x0F000000u) == 0x04000000u)
                    p.clip_only = true;
                continue;
            }
            // 3D in front of a backdrop goes with the screen, as the PSP drew
            // it (its camera is restored to the PSP's for drawing).
            // A small 3D model in the screen (a panel's tank) goes with it too.
            if (p.place != Place::Panel) p.place = Place::Picture;
            p.keep_x = p.keep_y = false;
            // Whole, at the window's scale: not a strip layer's own scale.
            p.scale_x = -1.0f;
            p.ax = 240.0f;
            p.ay = 136.0f;
            p.why = "2D screen";
        }
    }
    zoom_backgrounds(placements, calls, screen_2d);
    const bool picture_scene = screen_2d;
    auto &w = wide();
    if (w.trace_frame && w.traced == 0u)
        std::cerr << "[vc2-wide-trace] f" << w.frame << (picture_scene ? " 2D screen" : " 3D scene") << "\n";
    for (std::size_t i = 0; i < calls.size(); ++i) {
        trace_placement(*calls[i], placements[i]);
        apply_placement(*calls[i], placements[i]);
    }
}

}  // namespace

void view_aspect_frame(Runtime &, float aspect) {
    constexpr float kPsp = 480.0f / 272.0f;
    const float r = aspect > 0.0f ? aspect / kPsp : 1.0f;
    auto &w = wide();
    w.sx = r > 1.0f ? 1.0f / r : 1.0f;
    w.sy = r < 1.0f ? r : 1.0f;
#if defined(PORTABLEKIT_HAS_RENDERER)
    // Screen-space interface draws are placed here, not by the renderer.
    if (gpu::VulkanRenderer *renderer = ensure_renderer())
        renderer->set_profile_places_interface(wide_enabled() && native_draw_enabled());
#endif
}

namespace {

// --- The game's camera at the window's shape ------------------------------------------
// The renderer's Hor+ (above) widens the picture, but the game still decides
// what to draw with the PSP's 480:272 view, so models near the left and right
// edges of a wider window vanish as the camera turns (2026-11-07). The game's
// camera is widened instead, where Kurofune builds its perspective matrix
// (European 0x08BB4FF4, called each frame by the camera apply at 0x08BEA65C,
// which passes the global camera block). That function is replaced by a host
// one: the same matrix, built from the camera's field of view, x and y scale
// (camera +0x18, +0x1C), near and far planes, the viewport's scale and the
// game's screen size, with the x scale taken times sx (the y scale times sy
// for a taller window). The matrix in game memory, which the game's own
// culling reads, has the window's shape. (Replacing only the small getter the
// builder reads the scale through did nothing: the two are compiled into one
// unit and call each other directly.)
// The picture itself is still placed by the renderer as before: when the
// game hands that matrix to setMatrix, the mirror the native draws are built
// from gets the unwidened matrix, so nothing on screen moves. Needs the
// native GU layer with setMatrix and native draws. VC2_NO_GAME_CAMERA=1 turns
// it off.
constexpr std::uint32_t kEuPerspectiveBuilder = 0x08BB4FF4u;  // (matrix out, camera, viewport)

// Whether the game's camera is widened now: only while the screen is a 3D
// scene (this frame so far or the last one had a 3D view, and no backdrop made
// it a 2D screen). A 2D screen shows the PSP's area anyway, and widening there
// broke R&D's Mod Vehicle: the tank's own off-centre camera, widened, made the
// game judge the tank off screen and not draw it at all (2026-11-09; the 2D
// screen layout, with only this off, drew it).
bool widen_camera_now() {
    const auto &w = wide();
    if (!(w.sx < 0.9999f || w.sy < 0.9999f)) return false;
    const bool view = w.view_this_frame || w.view_last_frame;
    const bool backdrop = w.backdrop_this_frame || w.backdrop_last_frame;
    return view && !backdrop;
}
constexpr std::uint32_t kEuScreenSizeGetter = 0x08BEAAD8u;    // lui/lwc1 of the screen size (480, 272)

struct GameCamera {
    std::uint32_t screen_size = 0u;  // the game's screen size floats (width, height)
    // Every matrix written widened, by where it was written, with the matrix
    // as the game would have built it. A screen can build several cameras a
    // frame (R&D's Mod Vehicle: the tank's own, off-centre, besides another);
    // keeping only the last one left the tank's widened and drew it off its
    // window, 2026-11-09. Up to 16, the oldest dropped.
    // A matrix is only put back while it is still exactly the widened one
    // written there: the game also copies other matrices to those places
    // (R&D's Mod Vehicle, changing a part, 2026-11-09: the tank jumped about
    // when stale entries were put back over them).
    struct Built {
        std::uint32_t address;
        std::array<float, 16> plain;
        std::array<float, 16> widened;
    };
    std::vector<Built> built;
    void remember(std::uint32_t address, const std::array<float, 16> &plain, const std::array<float, 16> &widened) {
        for (Built &entry : built)
            if (entry.address == address) {
                entry.plain = plain;
                entry.widened = widened;
                return;
            }
        if (built.size() >= 16u) built.erase(built.begin());
        built.push_back({address, plain, widened});
    }
    void forget(std::uint32_t address) {
        built.erase(std::remove_if(built.begin(), built.end(), [&](const Built &entry) { return entry.address == address; }),
                    built.end());
    }
    const std::array<float, 16> *plain_for(std::uint32_t address, const std::array<float, 16> &given) const {
        for (const Built &entry : built)
            if (entry.address == address) return entry.widened == given ? &entry.plain : nullptr;
        return nullptr;
    }
};
GameCamera &game_camera() {
    static GameCamera camera;
    return camera;
}

bool game_camera_enabled() {
    static const bool enabled = env("NO_GAME_CAMERA") == nullptr;
    return enabled;
}

// Kurofune's perspective matrix, step for step as the game's code computes it
// (single precision, same order), for a camera x and y scale times wx, wy.
std::array<float, 16> perspective_matrix(psprecomp::GuestMemory &memory, std::uint32_t camera, std::uint32_t viewport,
                                         float width, float height, float wx, float wy) {
    const auto f = [&](std::uint32_t address) { return std::bit_cast<float>(memory.load32(address)); };
    // Focal length in pixels: half the screen height over the tangent of half
    // the field of view, the angle taken through 16-bit angle units.
    const float fov = f(camera + 0x04u);
    const std::int32_t units = static_cast<std::int32_t>(fov * std::bit_cast<float>(0x4622F983u));
    const float half = static_cast<float>(units / 2) * std::bit_cast<float>(0x38C90FDBu);
    const float focal = height / (std::tan(half) * 2.0f);
    float scale_x = f(camera + 0x18u) * wx, scale_y = f(camera + 0x1Cu) * wy;
    float centre_x = f(camera + 0x10u), centre_y = f(camera + 0x14u);
    const float near_z = -f(camera + 0x08u), far_z = -f(camera + 0x0Cu);
    const float view_x = f(viewport + 0x10u), view_y = f(viewport + 0x14u);
    if (!(view_x <= 1.0f)) {
        scale_x = view_x * scale_x;
        centre_x = centre_x * view_x;
    }
    if (!(view_y <= 1.0f)) {
        scale_y = view_y * scale_y;
        centre_y = centre_y * view_y;
    }
    const float across = (height / scale_y) / (width / scale_x);
    float y_scale = 1.0f / ((1.0f / focal) * height / scale_y);
    const float depth = 1.0f / (far_z - near_z);
    std::array<float, 16> m{};
    y_scale = y_scale * 2.0f;
    m[0] = y_scale * across;
    m[5] = y_scale;
    m[10] = -((far_z + near_z) * depth);
    m[11] = -1.0f;
    m[14] = (far_z * near_z * -2.0f) * depth;
    m[15] = 0.0f;
    // The off-centre shift, as the game computes it (EU 0x08BB517C..0x08BB51C4:
    // -1 x offset / size x 2, no focal term). It was copied wrong (y_scale x
    // offset / size x 2, sign flipped), which only shows for a camera whose
    // centre is not the screen's middle: R&D's Mod Vehicle draws the tank with
    // its centre on its window at the right, and the tank was thrown off the
    // left of the screen and culled (2026-11-09). Centred cameras: 0 either way.
    const float offset_x = width * -0.5f + centre_x;
    const float offset_y = height * 0.5f - centre_y;
    m[8] = (-1.0f * offset_x) / width * 2.0f;
    m[9] = (-1.0f * offset_y) / height * 2.0f;
    return m;
}

void build_perspective(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    auto &memory = rt.memory();
    auto &camera = game_camera();
    const std::uint32_t out = ctx.gpr[4], cam = ctx.gpr[5], viewport = ctx.gpr[6];
    const float width = std::bit_cast<float>(memory.load32(camera.screen_size));
    const float height = std::bit_cast<float>(memory.load32(camera.screen_size + 4u));
    auto &w = wide();
    const std::array<float, 16> plain = perspective_matrix(memory, cam, viewport, width, height, 1.0f, 1.0f);
    std::array<float, 16> written = plain;
    if (widen_camera_now()) {
        written = perspective_matrix(memory, cam, viewport, width, height, w.sx, w.sy);
        camera.remember(out, plain, written);
        ++w.camera_widened;
    } else {
        camera.forget(out);
    }
    for (std::uint32_t i = 0; i < 16u; ++i) memory.store32(out + i * 4u, std::bit_cast<std::uint32_t>(written[i]));
    return_to_caller(ctx);
}

void install_camera_trace(Runtime &rt);

void install_game_camera(Runtime &rt) {
    if (!game_camera_enabled() || !wide_enabled() || !native_draw_enabled()) return;
    if (!code_matches(rt, kEuPerspectiveBuilder) || !code_matches(rt, kEuScreenSizeGetter)) {
        std::cerr << "[vc2-camera] the camera code is not the expected functions in the " << region_name()
                  << " executable; the game keeps the PSP's view for culling\n";
        return;
    }
    // lui $6, hi; lwc1 $f12, lo($6): the loaded (relocated) address.
    const std::uint32_t getter = at(kEuScreenSizeGetter);
    const std::uint32_t hi = rt.memory().load32(getter) & 0xFFFFu;
    const std::uint32_t lo = rt.memory().load32(getter + 4u) & 0xFFFFu;
    const std::uint32_t address = (hi << 16u) + static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(lo)));
    if (!rt.memory().contains(address, 8u)) {
        std::cerr << "[vc2-camera] the screen size was not found; the game keeps the PSP's view\n";
        return;
    }
    game_camera().screen_size = address;
    rt.register_function(at(kEuPerspectiveBuilder), &build_perspective, "vc2_perspective");
    std::cerr << "[vc2-camera] the game's perspective is built on the host at the window's shape (screen size at 0x"
              << std::hex << address << std::dec << ")\n";
    install_camera_trace(rt);
}

// The small camera getters below are replaced by host copies (the same reads)
// that, under VC2_TRACE_WIDE, count calls by return address (calls from inside
// the getters' own compiled unit go straight to the game's code and are not
// seen). That trace found the test that hid characters at the PSP's screen
// edges (2026-11-07): Kurofune's sphere-on-screen test (European 0x08BD19F4,
// called about once per object per frame in combat) projects an object's
// bounding sphere with the camera's x scale and compares it with the 480-pixel
// viewport rectangle. Its point-on-screen sibling (0x08BD1558) does the same
// for a point. For those two callers the x scale getter returns the scale
// times sx (y times sy), so they test against the window's width, as the
// widened perspective matrix does for the scenery. Without the trace only that
// getter is replaced.
struct CameraReader {
    const char *name;
    std::uint32_t address;  // European
    enum Kind { Float, Pair, Quad, Screen } kind;
    std::uint32_t offset;  // Float: the field; Pair: the first of two; Quad: four from 0
};
constexpr CameraReader kCameraReaders[] = {
    {"near", 0x08BB4CC4u, CameraReader::Float, 0x08u},      {"far", 0x08BB4CCCu, CameraReader::Float, 0x0Cu},
    {"fov", 0x08BB4CFCu, CameraReader::Float, 0x04u},       {"centre", 0x08BB4CD4u, CameraReader::Pair, 0x10u},
    {"scale", 0x08BB4CE8u, CameraReader::Pair, 0x18u},      {"viewport rect", 0x08BB4F20u, CameraReader::Quad, 0u},
    {"viewport scale", 0x08BB4F44u, CameraReader::Pair, 0x10u}, {"viewport size", 0x08BB4F58u, CameraReader::Pair, 0x18u},
    {"screen size", 0x08BEAAD8u, CameraReader::Screen, 0u},
};
constexpr std::size_t kCameraReaderCount = std::size(kCameraReaders);

std::array<std::map<std::uint32_t, std::uint64_t>, kCameraReaderCount> &camera_reads() {
    static std::array<std::map<std::uint32_t, std::uint64_t>, kCameraReaderCount> reads;
    return reads;
}

constexpr std::uint32_t kEuCameraScaleGetter = 0x08BB4CE8u;
// Return addresses of the x/y scale reads in the two on-screen tests.
constexpr std::uint32_t kEuScreenTestScaleReturns[] = {0x08BD1AE4u /* sphere */, 0x08BD16A4u /* point */};

void widen_screen_test_scale(psprecomp::GuestMemory &memory, AllegrexContext &ctx) {
    auto &w = wide();
    if (!widen_camera_now()) return;
    static const std::array<std::uint32_t, 2> returns = {at(kEuScreenTestScaleReturns[0]), at(kEuScreenTestScaleReturns[1])};
    if (ctx.gpr[31] != returns[0] && ctx.gpr[31] != returns[1]) return;
    const float x = std::bit_cast<float>(memory.load32(ctx.gpr[4])) * w.sx;
    const float y = std::bit_cast<float>(memory.load32(ctx.gpr[5])) * w.sy;
    memory.store32(ctx.gpr[4], std::bit_cast<std::uint32_t>(x));
    memory.store32(ctx.gpr[5], std::bit_cast<std::uint32_t>(y));
    ++w.screen_tests_widened;
}

template <std::size_t I>
void camera_reader(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    constexpr CameraReader reader = kCameraReaders[I];
    auto &memory = host(corpus).memory();
    if (wide_trace_enabled()) ++camera_reads()[I][ctx.gpr[31]];
    const auto copy = [&](std::uint32_t from, std::uint32_t to) {
        const std::uint32_t bits = memory.load32(from);
        memory.store32(to, bits);
        ctx.set_fpr_bits(12u, bits);
    };
    switch (reader.kind) {
    case CameraReader::Float: ctx.set_fpr_bits(0u, memory.load32(ctx.gpr[4] + reader.offset)); break;
    case CameraReader::Pair:
        copy(ctx.gpr[6] + reader.offset, ctx.gpr[4]);
        copy(ctx.gpr[6] + reader.offset + 4u, ctx.gpr[5]);
        if constexpr (reader.address == kEuCameraScaleGetter) widen_screen_test_scale(memory, ctx);
        break;
    case CameraReader::Quad:
        for (std::uint32_t i = 0; i < 4u; ++i) copy(ctx.gpr[8] + i * 4u, ctx.gpr[4u + i]);
        break;
    case CameraReader::Screen:
        copy(game_camera().screen_size, ctx.gpr[4]);
        copy(game_camera().screen_size + 4u, ctx.gpr[5]);
        ctx.gpr[6] = game_camera().screen_size;
        break;
    }
    return_to_caller(ctx);
}

template <std::size_t... I>
constexpr std::array<void (*)(psprecomp::CorpusRuntime &, AllegrexContext &), kCameraReaderCount> make_camera_readers(std::index_sequence<I...>) {
    return {&camera_reader<I>...};
}

void install_camera_trace(Runtime &rt) {
    if (game_camera().screen_size == 0u) return;
    static constexpr auto readers = make_camera_readers(std::make_index_sequence<kCameraReaderCount>{});
    std::size_t installed = 0u;
    for (std::size_t i = 0; i < kCameraReaderCount; ++i) {
        if (!wide_trace_enabled() && kCameraReaders[i].address != kEuCameraScaleGetter) continue;
        if (!code_matches(rt, kCameraReaders[i].address)) continue;
        rt.register_function(at(kCameraReaders[i].address), readers[i],
                             std::string("vc2_camera_trace_") + kCameraReaders[i].name);
        ++installed;
    }
    if (wide_trace_enabled())
        std::cerr << "[vc2-camera-trace] " << installed << " of " << kCameraReaderCount
                  << " camera and screen readers counted by caller\n";
}

void report_camera_trace(std::uint64_t flips) {
    if (!wide_trace_enabled() || flips % 600u != 0u) return;
    for (std::size_t i = 0; i < kCameraReaderCount; ++i) {
        auto &reads = camera_reads()[i];
        if (reads.empty()) continue;
        std::vector<std::pair<std::uint32_t, std::uint64_t>> sorted(reads.begin(), reads.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::cerr << "[vc2-camera-trace] by flip " << flips << " " << kCameraReaders[i].name << ":";
        for (std::size_t k = 0; k < sorted.size() && k < 16u; ++k)
            std::cerr << " 0x" << std::hex << sorted[k].first << std::dec << "=" << sorted[k].second;
        std::cerr << "\n";
        reads.clear();
    }
}

void restore_camera_projection(std::uint32_t matrix, std::array<float, 16> &stored) {
    auto &w = wide();
    const std::array<float, 16> *plain = game_camera().plain_for(matrix, stored);
    if (plain == nullptr) return;
    stored = *plain;
    ++w.camera_restored;
}

// --- Sprite edges at high resolution (patch 0014) -----------------------------------
// At 480x272 a sprite's pixels sample its texture only between the centres of
// its edge pixels. At a higher resolution the outermost pixels sample past
// those, and bilinear filtering pulls in the neighbouring texels of the
// game's texture sheet: a faint line where a stretched menu-row body meets
// its end cap (the mission list, 2026-11-07). PortableKit clamps screen-space
// tiles to the texels a 480x272 raster samples; VC2 draws its interface
// through flat matrices instead, so each flat sprite here gets the same range
// (DrawCall::uv_rects): its texture coordinates, pulled in by half a PSP
// pixel. At 480x272 nothing changes. VC2_NO_SPRITE_EDGES=1 turns it off.

struct EdgeStats {
    std::uint64_t clamped = 0u;  // sprites given their texel range
    std::uint64_t nearest = 0u;  // of those, drawn by the game with nearest-texel magnification
    std::map<std::string, std::uint64_t> skipped;  // draws left alone, by reason
};
EdgeStats &edge_stats() {
    static EdgeStats stats;
    return stats;
}

bool sprite_edges_enabled() {
    static const bool enabled = env("NO_SPRITE_EDGES") == nullptr;
    return enabled;
}

// A vertex's position on the PSP screen, through the draw's matrices and
// viewport. False behind the eye.
bool screen_point(const gpu::DrawCall &call, const gpu::Vertex &vertex, float &x, float &y) {
    const auto apply = [](const std::array<float, 4> &p, const std::array<float, 16> &m) {
        std::array<float, 4> out{};
        for (std::size_t j = 0; j < 4u; ++j)
            out[j] = p[0] * m[j] + p[1] * m[4u + j] + p[2] * m[8u + j] + p[3] * m[12u + j];
        return out;
    };
    std::array<float, 4> p{vertex.position[0], vertex.position[1], vertex.position[2], 1.0f};
    p = apply(apply(apply(p, call.world), call.view), call.projection);
    if (p[3] <= 1e-6f) return false;
    const auto &vp = call.viewport;
    x = vp.x_offset - vp.offset_x + p[0] / p[3] * vp.x_scale;
    y = vp.y_offset - vp.offset_y + p[1] / p[3] * vp.y_scale;
    return true;
}

void clamp_flat_sprites(gpu::DrawCall &call) {
    if (!sprite_edges_enabled() || call.through || call.clear_mode || !call.texture.enabled || call.vertices.empty())
        return;
    auto &stats = edge_stats();
    const auto skip = [&](const char *why) { ++stats.skipped[why]; };
    const auto &m = call.projection;
    if (m[3] != 0.0f || m[7] != 0.0f || m[11] != 0.0f || m[15] != 1.0f) return;  // perspective: 3D, not a sprite
    // VC2 leaves lighting on for its interface, whose vertices have no
    // normals; the shader lights those with a zero normal, as the GE does, so
    // the normal's slot can carry the range. Lit draws with normals are 3D.
    if (call.lighting_enabled && ((call.vertex_type >> 5u) & 3u) != 0u) return skip("lit with normals");
    if (!call.indices.empty()) {
        // Shared vertices cannot carry one range per sprite: the draw's
        // vertices are laid out in index order instead (interface draws are
        // small; the Load prompt's buttons are drawn this way).
        if (call.indices.size() > 4096u) return skip("indexed, large");
        std::vector<gpu::Vertex> unpacked;
        unpacked.reserve(call.indices.size());
        for (const auto index : call.indices) {
            if (static_cast<std::size_t>(index) >= call.vertices.size()) return skip("indexed, out of range");
            unpacked.push_back(call.vertices[index]);
        }
        call.vertices = std::move(unpacked);
        call.indices.clear();
    }
    const auto &t = call.texture;
    // The range one sprite (2 corners) or quad (4 vertices) may sample, or
    // the reason it gets none (then it samples freely, as before).
    const auto range_of = [&](const gpu::Vertex *corners, std::size_t count, std::array<float, 4> &range) -> const char * {
        float u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f, x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
        for (std::size_t i = 0; i < count; ++i) {
            float x = 0.0f, y = 0.0f;
            if (!screen_point(call, corners[i], x, y)) return "behind the eye";
            u0 = std::min(u0, corners[i].texcoord[0]); u1 = std::max(u1, corners[i].texcoord[0]);
            v0 = std::min(v0, corners[i].texcoord[1]); v1 = std::max(v1, corners[i].texcoord[1]);
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
        }
        // Zero-width glyphs (spaces) and the like.
        if (x1 - x0 < 1.0f || y1 - y0 < 1.0f || u1 <= u0 || v1 <= v0) return "degenerate sprite";
        // A range beyond one copy of the texture repeats it on purpose.
        const float su0 = u0 * t.scale_u + t.offset_u, su1 = u1 * t.scale_u + t.offset_u;
        const float sv0 = v0 * t.scale_v + t.offset_v, sv1 = v1 * t.scale_v + t.offset_v;
        if (std::min(su0, su1) < -1e-4f || std::max(su0, su1) > 1.0001f || std::min(sv0, sv1) < -1e-4f ||
            std::max(sv0, sv1) > 1.0001f)
            return "sprite repeats its texture";
        // Half a PSP pixel: what a 480x272 raster samples. When the game asks
        // for nearest texels (the renderer filters every texture bilinearly),
        // at least half a texel: a stretched piece then shows its own edge
        // texel at its edge, as on the PSP, instead of blending in the texel
        // beyond it (the light column where a menu row's body meets its end
        // cap). For pieces drawn 1:1 the two are the same.
        float inset_u = 0.5f * (u1 - u0) / (x1 - x0), inset_v = 0.5f * (v1 - v0) / (y1 - y0);
        if (t.mag_filter == 0u) {
            const float texel_u = 1.0f / std::max(static_cast<float>(t.width) * std::fabs(t.scale_u), 1e-6f);
            const float texel_v = 1.0f / std::max(static_cast<float>(t.height) * std::fabs(t.scale_v), 1e-6f);
            inset_u = std::min(std::max(inset_u, 0.5f * texel_u), 0.5f * (u1 - u0));
            inset_v = std::min(std::max(inset_v, 0.5f * texel_v), 0.5f * (v1 - v0));
        }
        range = {u0 + inset_u, v0 + inset_v, u1 - inset_u, v1 - inset_v};
        return nullptr;
    };
    const auto give = [&](gpu::Vertex &vertex, const std::array<float, 4> &range) {
        vertex.normal = {range[0], range[1], range[2]};
        vertex.position[3] = range[3];
    };
    const auto range_or_free = [&](const gpu::Vertex *corners, std::size_t count) {
        std::array<float, 4> range{};
        if (const char *why = range_of(corners, count, range)) {
            skip(why);
            return std::array<float, 4>{-1e30f, -1e30f, 1e30f, 1e30f};
        }
        ++stats.clamped;
        if (t.mag_filter == 0u) ++stats.nearest;
        return range;
    };
    const std::size_t n = call.vertices.size();
    if (call.primitive == gpu::PrimitiveType::Sprites && n % 2u == 0u) {
        for (std::size_t first = 0; first < n; first += 2u) {
            const auto range = range_or_free(&call.vertices[first], 2u);
            give(call.vertices[first], range);
            give(call.vertices[first + 1u], range);
        }
    } else if ((call.primitive == gpu::PrimitiveType::TriangleStrip || call.primitive == gpu::PrimitiveType::TriangleFan) &&
               n == 4u) {
        const auto range = range_or_free(call.vertices.data(), 4u);
        for (gpu::Vertex &vertex : call.vertices) give(vertex, range);
    } else if (call.primitive == gpu::PrimitiveType::TriangleStrip && n >= 6u && n % 2u == 0u) {
        // A strip of quads side by side (a row highlight: end cap, body, end
        // cap) shares the vertices where the quads join, and the seam shows
        // exactly there. Each quad is drawn as its own two triangles instead,
        // with its own range.
        std::vector<gpu::Vertex> triangles;
        triangles.reserve((n / 2u - 1u) * 6u);
        for (std::size_t first = 0; first + 3u < n; first += 2u) {
            std::array<gpu::Vertex, 4> quad{call.vertices[first], call.vertices[first + 1u], call.vertices[first + 2u],
                                            call.vertices[first + 3u]};
            const auto range = range_or_free(quad.data(), 4u);
            for (gpu::Vertex &vertex : quad) give(vertex, range);
            for (const std::size_t corner : {0u, 1u, 2u, 2u, 1u, 3u}) triangles.push_back(quad[corner]);
        }
        call.vertices = std::move(triangles);
        call.primitive = gpu::PrimitiveType::Triangles;
    } else {
        return skip(("primitive " + std::to_string(static_cast<int>(call.primitive)) + " with " +
                     std::to_string(n) + " vertices").c_str());
    }
    call.uv_rects = true;
}

gpu::DrawCall *take_native_draw(std::uint32_t marker) {
    if ((marker & kMarkerTag) == 0u) return nullptr;
    const std::uint32_t slot = marker & ~kMarkerTag;
    if (slot >= kNativeSlots) return nullptr;
    NativeSlot &entry = native_slots()[slot];
    if (!entry.ready) {
        ++native_stats().stale;
        if (traces_texture(entry.call)) texture_note("REACHED BUT GONE", slot, entry.call);
        return nullptr;
    }
    entry.ready = false;
    // The vertex and index bytes are read now, when the list reaches the draw.
    if (guest_memory() == nullptr || !decode_geometry(*guest_memory(), entry.geometry, entry.call)) {
        ++native_stats().skipped;
        return nullptr;
    }
    const char *crop = crop_picture_texture(entry.call);
    clamp_flat_sprites(entry.call);
    if (traces_texture(entry.call)) texture_note(crop == std::string_view("cropped") ? "drawn CROPPED" : "drawn", slot, entry.call);
    if (movie_trace_enabled() && movie_trace().frames_logged >= 118u && movie_trace().frames_logged < 126u) {
        const gpu::DrawCall &c = entry.call;
        float y0 = 1e30f, y1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
        for (const gpu::Vertex &v : c.vertices) {
            y0 = std::min(y0, v.position[1]); y1 = std::max(y1, v.position[1]);
            z0 = std::min(z0, v.position[2]); z1 = std::max(z1, v.position[2]);
        }
        std::ostringstream line;
        line << "  submit " << (c.clear_mode ? "clear" : "draw") << " prim=" << static_cast<int>(c.primitive)
             << " verts=" << c.vertices.size() << " idx=" << c.indices.size() << " through=" << c.through
             << " lit=" << c.lighting_enabled << " blend=" << c.blend.enabled << "(" << c.blend.source_factor << ","
             << c.blend.destination_factor << "," << c.blend.equation << ")" << " color0=" << std::hex
             << (c.vertices.empty() ? 0u : c.vertices[0].color) << std::dec << " y=" << y0 << ".." << y1
             << " z=" << z0 << ".." << z1 << " target=" << hex8(c.target.color_address)
             << " tex=" << hex8(c.texture.address) << " " << c.texture.width << "x" << c.texture.height
             << " sv=" << c.texture.scale_v << " ov=" << c.texture.offset_v << " wrap=" << c.texture.wrap_s << "/"
             << c.texture.wrap_t << " filter=" << c.texture.min_filter << "/" << c.texture.mag_filter
             << " crop=" << crop;
        movie_note(line.str());
    }
    ++native_stats().submitted;
    return &entry.call;
}

// The display-list path's source of native draws (GE patch 0007): one at a
// time, so interface elements stay centred under widescreen.
const gpu::DrawCall *native_source(std::uint32_t marker) {
    gpu::DrawCall *call = take_native_draw(marker);
    if (call != nullptr) place_single(*call);
    return call;
}

// Equal state gives an equal version, so the renderer's cached lighting and
// material work stays valid exactly as long as the state does.
std::uint64_t hash_words(std::initializer_list<std::uint64_t> values, std::uint64_t seed = 1469598103934665603ull) {
    std::uint64_t h = seed;
    for (const std::uint64_t value : values) {
        h ^= value;
        h *= 1099511628211ull;
    }
    return h;
}
std::uint64_t fbits(float value) { return std::bit_cast<std::uint32_t>(value); }

void stamp_versions(gpu::DrawCall &call) {
    const auto &l = call.lighting;
    std::uint64_t environment = hash_words({l.ambient_color, l.ambient_alpha, call.fog.enabled, fbits(call.fog.end),
                                            fbits(call.fog.scale), call.fog.color});
    for (const auto &light : l.lights)
        environment = hash_words({light.enabled, light.kind, light.type, fbits(light.position[0]),
                                  fbits(light.position[1]), fbits(light.position[2]), fbits(light.direction[0]),
                                  fbits(light.direction[1]), fbits(light.direction[2]), fbits(light.attenuation[0]),
                                  fbits(light.attenuation[1]), fbits(light.attenuation[2]), fbits(light.spot_exponent),
                                  fbits(light.spot_cutoff), light.ambient, light.diffuse, light.specular},
                                 environment);
    call.environment_version = environment;
    call.material_version = hash_words({call.material_color, l.material_update, l.material_emissive, l.material_diffuse,
                                        l.material_specular, fbits(l.specular_power), l.mode, l.reverse_normals});
}

// Indices and vertices exactly as the display-list path reads them: only the
// vertices the indices reference are decoded, and the indices are rebased onto
// them. Returns false when the GE would not draw.
bool decode_geometry(const psprecomp::GuestMemory &memory, const Geometry &m, gpu::DrawCall &call) {
    if (m.host_built) return !call.vertices.empty();
    const std::uint32_t count = m.count;
    if (count == 0u || m.vertex_address == 0u) return false;
    const std::uint32_t index_type = (m.vertex_type >> 11u) & 3u;
    std::uint32_t vertex_count = count, first_vertex = 0u;
    call.indices.clear();
    call.vertices.clear();
    if (index_type != 0u && m.index_address != 0u) {
        const std::uint32_t size = index_type == 1u ? 1u : index_type == 2u ? 2u : 4u;
        if (!memory.contains(m.index_address, count * size)) return false;
        call.indices.resize(count);
        std::uint32_t lowest = 0xFFFFFFFFu, highest = 0u;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t at = m.index_address + i * size;
            const std::uint32_t index = size == 1u ? memory.load8(at) : size == 2u ? memory.load16(at) : memory.load32(at);
            lowest = std::min(lowest, index);
            highest = std::max(highest, index);
            call.indices[i] = static_cast<std::uint16_t>(index);
        }
        first_vertex = lowest;
        for (std::uint16_t &index : call.indices) index = static_cast<std::uint16_t>(index - lowest);
        vertex_count = highest - lowest + 1u;
    }
    std::vector<gpu::Vertex> probe_out;
    const std::uint32_t probe = gpu::decode_vertices(memory, m.vertex_address, m.vertex_type, 0u, probe_out);
    if (probe == 0u) return false;
    const std::uint32_t first_address = m.vertex_address + first_vertex * probe;
    if (!memory.contains(first_address, static_cast<std::size_t>(probe) * vertex_count)) return false;
    const std::uint32_t stride =
        gpu::decode_vertices(memory, first_address, m.vertex_type, vertex_count, call.vertices, m.bones.data());
    return stride != 0u && !call.vertices.empty();
}

// drawArray(prim, vertex_type, count, indices, vertices) -- fifth argument in $t0.
// Ends by continuing into the game's stall-address update with $ra unchanged,
// exactly as the original's tail call does.
void gu_draw_array(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t prim = ctx.gpr[4], vtype = ctx.gpr[5], count = ctx.gpr[6];
    const std::uint32_t indices = ctx.gpr[7], vertices = ctx.gpr[8];
    auto &m = mirror();
    std::uint32_t primitive_address = 0u;
    if (movie_trace_enabled()) {
        GuMirror probe = m;
        if (vtype != 0u) probe.vertex_type = vtype & 0x00FFFFFFu;
        if (indices != 0u) probe.index_address = indices & 0x0FFFFFFFu;
        if (vertices != 0u) probe.vertex_address = vertices & 0x0FFFFFFFu;
        movie_note_draw(rt, probe, "draw", count & 0xFFFFu);
    }
    if (native_draw_enabled()) {
        if (vtype != 0u) m.vertex_type = vtype & 0x00FFFFFFu;
        if (indices != 0u) m.index_address = indices & 0x0FFFFFFFu;
        if (vertices != 0u) {
            m.vertex_address = vertices & 0x0FFFFFFFu;
            vertex_pointer_from_host_draw() = false;
        } else if (vertex_pointer_from_host_draw()) {
            // The GE would read on from where a clear's or blit's vertices
            // ended: list data, never a real mesh. Not expected from VC2.
            if (++native_stats().continued_pointer <= 5u)
                std::cerr << "[vc2-native-draw] a draw continued from a host-built clear's or blit's vertex pointer\n";
        }
        const std::uint32_t draw_count = count & 0xFFFFu;
        static std::uint32_t next_slot = 0u;
        NativeSlot &slot = native_slots()[next_slot];
        if (slot.ready) {
            ++native_stats().stale;  // never reached by the list; overwritten
            if (traces_texture(slot.call)) texture_note("OVERWRITTEN BEFORE DRAWN", next_slot, slot.call);
        }
        resolve_target(rt, m);
        slot.call = build_native_draw(m, prim, draw_count);
        if (traces_texture(slot.call)) texture_note("built", next_slot, slot.call);
        slot.geometry = geometry_of(m, draw_count);
        slot.ready = draw_count != 0u && m.vertex_address != 0u;  // the GE draws nothing otherwise
        if (slot.ready) {
            stamp_versions(slot.call);
            ++native_stats().built;
            ListWriter(rt).marker(kMarkerTag | next_slot);
            next_slot = (next_slot + 1u) % 2048u;  // first half of the ring; clears and blits use the second
        } else {
            ++native_stats().skipped;
        }
        // Pointers advance exactly as after a GE draw.
        if (draw_count != 0u && m.vertex_address != 0u) {
            const std::uint32_t index_type = (m.vertex_type >> 11u) & 3u;
            if (index_type != 0u && m.index_address != 0u) {
                m.index_address += draw_count * (index_type == 1u ? 1u : index_type == 2u ? 2u : 4u);
            } else {
                std::vector<gpu::Vertex> probe;
                const std::uint32_t stride = gpu::decode_vertices(rt.memory(), m.vertex_address, m.vertex_type, 0u, probe);
                m.vertex_address += stride * draw_count;
            }
        }
        ++m.draws;
        ctx.pc = gu_update_stall();
        return;
    }
    {
        ListWriter list(rt);
        if (vtype != 0u) list.emit(0x12u, vtype);
        if (indices != 0u) {
            list.emit(0x10000000u | (((indices >> 24u) & 0xFu) << 16u));
            list.emit(0x02u, indices);
        }
        if (vertices != 0u) {
            list.emit(0x10000000u | (((vertices >> 24u) & 0xFu) << 16u));
            list.emit(0x01u, vertices);
        }
        primitive_address = list.address();
        list.emit(0x04000000u | ((prim & 7u) << 16u) | count);
    }
    if (vtype != 0u) m.vertex_type = vtype & 0x00FFFFFFu;
    if (indices != 0u) m.index_address = indices & 0x0FFFFFFFu;
    if (vertices != 0u) m.vertex_address = vertices & 0x0FFFFFFFu;
    const std::uint32_t draw_count = count & 0xFFFFu;
    if (native_check_enabled() && draw_count != 0u) {
        auto &pending = pending_draws();
        if (pending.size() > 20000u) {  // draws the GE never reached (e.g. a list abandoned)
            check_stats().dropped += pending.size();
            pending.clear();
        }
        resolve_target(rt, m);
        pending[primitive_address & 0x1FFFFFFFu] = {build_native_draw(m, prim, draw_count), geometry_of(m, draw_count)};
    }
    // The GE advances the pointer a draw consumed, so the next draw that passes
    // none continues from there.
    if (draw_count != 0u && m.vertex_address != 0u) {
        const std::uint32_t index_type = (m.vertex_type >> 11u) & 3u;
        if (index_type != 0u && m.index_address != 0u) {
            m.index_address += draw_count * (index_type == 1u ? 1u : index_type == 2u ? 2u : 4u);
        } else {
            std::vector<gpu::Vertex> probe;
            const std::uint32_t stride = gpu::decode_vertices(rt.memory(), m.vertex_address, m.vertex_type, 0u, probe);
            m.vertex_address += stride * draw_count;
        }
    }
    ++m.draws;
    ctx.pc = gu_update_stall();
}

// --- Batch 2: every remaining entry point that sets render state ----------------

std::uint32_t enabled_mask_address() { return gu_state_block() + 0x44u; }



// The library's internal state switch (0x08C880F8): one GE word per state,
// except scissor (rectangle from the context, or the full area when off) and
// fragment 2x (rewrites the texture function word).
void write_state(Runtime &rt, std::uint32_t state, std::uint32_t on) {
    constexpr std::uint8_t kCommand[22] = {0x22, 0x23, 0x00, 0x24, 0x21, 0x1D, 0x20, 0x1F, 0x1C, 0x1E, 0x17,
                                           0x18, 0x19, 0x1A, 0x1B, 0x25, 0x26, 0x27, 0x28, 0x51, 0x38, 0x00};
    if (state >= 22u) return;
    ListWriter list(rt);
    if (state == 2u) {  // scissor test
        std::uint32_t region;
        if (on != 0u) {
            set_context_word(rt, 168u, 1u);
            list.emit(0xD4000000u | (context_word(rt, 176u) << 10u) | context_word(rt, 172u));
            region = (context_word(rt, 184u) << 10u) | context_word(rt, 180u);
        } else {
            set_context_word(rt, 168u, 0u);
            list.emit(0xD4000000u);
            region = ((context_word(rt, 164u) - 1u) << 10u) | (context_word(rt, 160u) - 1u);
        }
        list.emit(0xD5000000u | region);
        list.emit(0x15000000u);
        list.emit(0x16000000u | region);
        mirror().scissor_start = on != 0u ? ((context_word(rt, 176u) << 10u) | context_word(rt, 172u)) & 0x00FFFFFFu : 0u;
        mirror().scissor_end = region & 0x00FFFFFFu;
        return;
    }
    if (state == 21u) {  // fragment 2x
        set_context_word(rt, 228u, on != 0u ? 1u : 0u);
        list.emit(0xC9000000u | (context_word(rt, 228u) << 16u) | (context_word(rt, 236u) << 8u) | context_word(rt, 232u));
        return;
    }
    list.emit((static_cast<std::uint32_t>(kCommand[state]) << 24u) | on);
}

void update_enabled(Runtime &rt, std::uint32_t state, bool on) {
    auto &memory = rt.memory();
    const std::uint32_t bit = 1u << (state & 31u);
    const std::uint32_t mask = memory.load32(enabled_mask_address());
    memory.store32(enabled_mask_address(), on ? (mask | bit) : (mask & ~bit));
    mirror().enabled = on ? (mirror().enabled | bit) : (mirror().enabled & ~bit);
}

// enable(state)
void gu_enable(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    write_state(rt, ctx.gpr[4], 1u);
    update_enabled(rt, ctx.gpr[4], true);
    return_to_caller(ctx);
}

// disable(state)
void gu_disable(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    write_state(rt, ctx.gpr[4], 0u);
    update_enabled(rt, ctx.gpr[4], false);
    return_to_caller(ctx);
}

// setState(state, on)
void gu_set_state(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const bool on = ctx.gpr[5] != 0u;
    write_state(rt, ctx.gpr[4], on ? 1u : 0u);
    update_enabled(rt, ctx.gpr[4], on);
    return_to_caller(ctx);
}

// texImage(level, width, height, buffer_width, address); sizes as log2 via a
// leading-zero count, as the original computes them (0 gives an all-ones value).
void gu_tex_image(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t level = ctx.gpr[4], width = ctx.gpr[5], height = ctx.gpr[6];
    const std::uint32_t buffer_width = ctx.gpr[7], address = ctx.gpr[8];
    const auto log2 = [](std::uint32_t value) { return 31u - static_cast<std::uint32_t>(std::countl_zero(value)); };
    {
        ListWriter list(host(corpus));
        list.emit(((0xA8u + level) << 24u) | (((address >> 24u) & 0xFu) << 16u) | buffer_width);
        list.emit(((0xA0u + level) << 24u) | (address & 0x00FFFFFFu));
        list.emit(((0xB8u + level) << 24u) | (log2(height) << 8u) | log2(width));
        list.emit(0xCB000000u);
    }
    if (level < mirror().texture_levels.size()) mirror().texture_levels[level] = {width, height, buffer_width, address};
    return_to_caller(ctx);
}

// texMode(format, max_mips, multi_clut, swizzle)
void gu_tex_mode(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t format = ctx.gpr[4], max_mips = ctx.gpr[5], multi_clut = ctx.gpr[6], swizzle = ctx.gpr[7];
    {
        ListWriter list(host(corpus));
        list.emit(0xC2000000u | (max_mips << 16u) | (multi_clut << 8u) | swizzle);
        list.emit(0xC3000000u | format);
        list.emit(0xCB000000u);
    }
    mirror().texture_mode = {format, max_mips, multi_clut, swizzle};
    return_to_caller(ctx);
}

// texMapMode(mode, light_u, light_v); keeps the projection mode at context +240.
void gu_tex_map_mode(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t mode = ctx.gpr[4] & 3u, light_u = ctx.gpr[5], light_v = ctx.gpr[6];
    const std::uint32_t projection = context_word(rt, 240u);
    set_context_word(rt, 244u, mode);
    {
        ListWriter list(rt);
        list.emit(0xC0000000u | (projection << 8u) | mode);
        list.emit(0xC1000000u | (light_v << 8u) | light_u);
    }
    mirror().texture_map_mode = {mode, light_u, light_v};
    return_to_caller(ctx);
}

// texFunc(function, alpha); fragment 2x comes from context +228.
void gu_tex_func(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t function = ctx.gpr[4], alpha = ctx.gpr[5];
    const std::uint32_t doubled = context_word(rt, 228u);
    set_context_word(rt, 236u, alpha);
    set_context_word(rt, 232u, function);
    ListWriter(rt).emit(0xC9000000u | (doubled << 16u) | (alpha << 8u) | function);
    mirror().texture_function = {function, alpha};
    return_to_caller(ctx);
}

// texLevelMode(mode, float bias in $f12): bias x16, truncated, clamped to -128..127.
void gu_tex_level_mode(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t mode = ctx.gpr[4];
    const float bias = std::bit_cast<float>(ctx.fpr_bits(12));
    const float scaled = bias * 16.0f;
    std::int32_t value;
    if (std::isnan(scaled) || scaled >= 2147483648.0f) value = 0x7FFFFFFF;  // FPU invalid result
    else if (scaled < -2147483648.0f) value = static_cast<std::int32_t>(0x80000000u);
    else value = static_cast<std::int32_t>(scaled);
    std::int32_t clamped = std::max(value, -128);
    if (value >= 128) clamped = 127;
    ListWriter(host(corpus)).emit(0xC8000000u | ((static_cast<std::uint32_t>(clamped) & 0xFFu) << 16u) | mode);
    mirror().texture_level_mode = {mode, bias};
    return_to_caller(ctx);
}

// texSlope(float in $f12)
void gu_tex_slope(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t bits = ctx.fpr_bits(12);
    ListWriter(host(corpus)).emit(0xD0000000u | ge_float(bits));
    mirror().texture_slope = std::bit_cast<float>(bits);
    return_to_caller(ctx);
}

// fog(float near $f12, float far $f13, color)
void gu_fog(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const float near_z = std::bit_cast<float>(ctx.fpr_bits(12)), far_z = std::bit_cast<float>(ctx.fpr_bits(13));
    const std::uint32_t color = ctx.gpr[4];
    const float distance = far_z - near_z;
    const float inverse = (0.0f < distance || distance < 0.0f) ? 1.0f / distance : 0.0f;
    {
        ListWriter list(host(corpus));
        list.emit(0xCFu, color);
        list.emit(0xCD000000u | ge_float(ctx.fpr_bits(13)));
        list.emit(0xCE000000u | ge_float(float_bits(inverse)));
    }
    mirror().fog = {near_z, far_z, color & 0x00FFFFFFu};
    mirror().fog_inverse = inverse;
    return_to_caller(ctx);
}

// scissor(x, y, width, height): stored in the context; written only while the
// scissor test is enabled (context +168).
void gu_scissor(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t x = ctx.gpr[4], y = ctx.gpr[5], width = ctx.gpr[6], height = ctx.gpr[7];
    const std::uint32_t right = x + width - 1u, bottom = y + height - 1u;
    set_context_word(rt, 172u, x);
    set_context_word(rt, 176u, y);
    set_context_word(rt, 180u, right);
    set_context_word(rt, 184u, bottom);
    if (context_word(rt, 168u) != 0u) {
        const std::uint32_t region = (bottom << 10u) | right;
        ListWriter list(rt);
        list.emit(0xD4000000u | (y << 10u) | x);
        list.emit(0xD5000000u | region);
        list.emit(0x15000000u);
        list.emit(0x16000000u | region);
        mirror().scissor_start = ((y << 10u) | x) & 0x00FFFFFFu;
        mirror().scissor_end = region & 0x00FFFFFFu;
    }
    mirror().scissor = {static_cast<std::int32_t>(x), static_cast<std::int32_t>(y), static_cast<std::int32_t>(width),
                        static_cast<std::int32_t>(height)};
    return_to_caller(ctx);
}

// morphWeight(index, float weight in $f12)
void gu_morph_weight(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t index = ctx.gpr[4], bits = ctx.fpr_bits(12);
    {
        ListWriter list(host(corpus));
        list.emit(0xFF000000u);
        list.emit(((index + 0x2Cu) << 24u) | ge_float(bits));
    }
    if (index < mirror().morph_weights.size()) mirror().morph_weights[index] = std::bit_cast<float>(bits);
    return_to_caller(ctx);
}

// lightColor(light, component, color): 1 ambient, 2 diffuse, 3 both, 4 specular,
// 6 diffuse and specular; 0, 5 and 7+ write nothing.
void gu_light_color(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t light = ctx.gpr[4], component = ctx.gpr[5], color = ctx.gpr[6] & 0x00FFFFFFu;
    const std::uint32_t ambient = (0x8Fu + light * 3u) << 24u, diffuse = (0x90u + light * 3u) << 24u,
                        specular = (0x91u + light * 3u) << 24u;
    {
        ListWriter list(host(corpus));
        switch (component) {
            case 1u: list.emit(ambient | color); break;
            case 2u: list.emit(diffuse | color); break;
            case 3u: list.emit(ambient | color); list.emit(diffuse | color); break;
            case 4u: list.emit(specular | color); break;
            case 6u: list.emit(diffuse | color); list.emit(specular | color); break;
            default: break;
        }
    }
    if (light < mirror().light_colors.size()) {
        auto &colors = mirror().light_colors[light];
        if (component == 1u || component == 3u) colors.ambient = color;
        if (component == 2u || component == 3u || component == 6u) colors.diffuse = color;
        if (component == 4u || component == 6u) colors.specular = color;
    }
    return_to_caller(ctx);
}

// light(light, type, components, position*): position X, Y, Z, then the type word.
void gu_light(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t light = ctx.gpr[4], type = ctx.gpr[5], components = ctx.gpr[6], position = ctx.gpr[7];
    const std::uint32_t kind = components == 8u ? 2u : (components == 6u ? 1u : 0u);
    const std::uint32_t x = rt.memory().load32(position), y = rt.memory().load32(position + 4u),
                        z = rt.memory().load32(position + 8u);
    {
        ListWriter list(rt);
        list.emit(((0x63u + light * 3u) << 24u) | ge_float(x));
        list.emit(((0x64u + light * 3u) << 24u) | ge_float(y));
        list.emit(((0x65u + light * 3u) << 24u) | ge_float(z));
        list.emit(((0x5Fu + light) << 24u) | (type << 8u) | kind);
    }
    if (light < mirror().lights.size())
        mirror().lights[light] = {type, components,
                                  {std::bit_cast<float>(x), std::bit_cast<float>(y), std::bit_cast<float>(z)}};
    return_to_caller(ctx);
}

// lightMode(mode)
void gu_light_mode(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0x5E000000u | (ctx.gpr[4] & 1u));
    mirror().light_mode = ctx.gpr[4] & 1u;
    return_to_caller(ctx);
}

// color(color): material ambient (with alpha), diffuse and specular.
void gu_color(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t color = ctx.gpr[4], rgb = color & 0x00FFFFFFu;
    {
        ListWriter list(host(corpus));
        list.emit(0x55000000u | rgb);
        list.emit(0x58000000u | (color >> 24u));
        list.emit(0x56000000u | rgb);
        list.emit(0x57000000u | rgb);
    }
    mirror().material.ambient = color;
    mirror().material.diffuse = rgb;
    mirror().material.specular = rgb;
    return_to_caller(ctx);
}

// modelColor(emissive, ambient, diffuse, specular)
void gu_model_color(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t emissive = ctx.gpr[4], ambient = ctx.gpr[5], diffuse = ctx.gpr[6], specular = ctx.gpr[7];
    {
        ListWriter list(host(corpus));
        list.emit(0x54u, emissive);
        list.emit(0x56u, diffuse);
        list.emit(0x55u, ambient);
        list.emit(0x58000000u | (ambient >> 24u));
        list.emit(0x57u, specular);
    }
    mirror().material = {emissive & 0x00FFFFFFu, ambient, diffuse & 0x00FFFFFFu, specular & 0x00FFFFFFu};
    return_to_caller(ctx);
}

// colorMaterial(components)
void gu_color_material(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(0x53000000u | (ctx.gpr[4] & 7u));
    mirror().color_material = ctx.gpr[4] & 7u;
    return_to_caller(ctx);
}

// frontFace(order): 0 writes 1, anything else writes 0.
void gu_front_face(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(ctx.gpr[4] != 0u ? 0x9B000000u : 0x9B000001u);
    mirror().front_face = ctx.gpr[4];
    return_to_caller(ctx);
}

// shadeModel(mode): non-zero is smooth.
void gu_shade_model(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ListWriter(host(corpus)).emit(ctx.gpr[4] != 0u ? 0x50000001u : 0x50000000u);
    mirror().shade_model = ctx.gpr[4];
    return_to_caller(ctx);
}

// setDither(matrix*): 4x4 ints, one GE word per row.
void gu_dither(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t matrix = ctx.gpr[4];
    std::array<std::uint32_t, 16> values{};
    for (std::uint32_t i = 0; i < 16u; ++i) values[i] = rt.memory().load32(matrix + i * 4u);
    {
        ListWriter list(rt);
        for (std::uint32_t row = 0; row < 4u; ++row) {
            const std::uint32_t *v = &values[row * 4u];
            list.emit(((0xE2u + row) << 24u) | ((v[3] & 0xFu) << 12u) | ((v[2] & 0xFu) << 8u) | ((v[1] & 0xFu) << 4u) |
                      (v[0] & 0xFu));
        }
    }
    for (std::size_t i = 0; i < 16u; ++i) mirror().dither[i] = static_cast<std::int32_t>(values[i]);
    return_to_caller(ctx);
}

// drawBuffer(format, address, width)
void gu_draw_buffer(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t format = ctx.gpr[4], address = ctx.gpr[5], width = ctx.gpr[6];
    {
        ListWriter list(host(corpus));
        list.emit(0xD2000000u | format);
        list.emit(0x9D000000u | (((address >> 24u) & 0xFu) << 16u) | width);
        list.emit(0x9Cu, address);
    }
    auto &rt = host(corpus);
    mirror().draw_buffer = mirror().explicit_draw_buffer = {format, address, width};
    mirror().explicit_draw_buffer_list = context_word(rt, 4u);
    mirror().explicit_draw_buffer_write = context_word(rt, kListWriteField);
    return_to_caller(ctx);
}

// depthBuffer(address, width); also kept in the GU state block (+52, +56).
void gu_depth_buffer(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    const std::uint32_t address = ctx.gpr[4], width = ctx.gpr[5];
    {
        ListWriter list(rt);
        list.emit(0x9F000000u | (((address >> 24u) & 0xFu) << 16u) | width);
        list.emit(0x9Eu, address);
    }
    rt.memory().store32(gu_state_block() + 56u, width);
    rt.memory().store32(gu_state_block() + 52u, address);
    mirror().depth_buffer = {address, width};
    return_to_caller(ctx);
}

// viewport(center_x, center_y, width, height): scale +-half size, then centre.
void gu_viewport(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    const std::uint32_t cx = ctx.gpr[4], cy = ctx.gpr[5], width = ctx.gpr[6], height = ctx.gpr[7];
    {
        ListWriter list(host(corpus));
        list.emit(0x42000000u | ge_float(float_bits(to_float(width) * 0.5f)));
        list.emit(0x43000000u | ge_float(float_bits(to_float(height) * -0.5f)));
        list.emit(0x45000000u | ge_float(float_bits(to_float(cx))));
        list.emit(0x46000000u | ge_float(float_bits(to_float(cy))));
    }
    mirror().viewport = {static_cast<std::int32_t>(cx), static_cast<std::int32_t>(cy), static_cast<std::int32_t>(width),
                         static_cast<std::int32_t>(height)};
    return_to_caller(ctx);
}

// Depth scale, depth translate and the min/max range, from near and far in
// context +216/+220 and the depth offset in +224.
void write_depth_range(Runtime &rt) {
    const std::uint32_t near_z = context_word(rt, 216u), far_z = context_word(rt, 220u), offset = context_word(rt, 224u);
    const float center = to_float(near_z + far_z) * 0.5f;
    const float scale = center - to_float(near_z);
    const float translate = center + to_float(offset);
    const bool ordered = static_cast<std::int32_t>(near_z) < static_cast<std::int32_t>(far_z);
    ListWriter list(rt);
    list.emit(0x44000000u | ge_float(float_bits(scale)));
    list.emit(0x47000000u | ge_float(float_bits(translate)));
    list.emit(0xD6000000u | (ordered ? near_z : far_z));
    list.emit(0xD7000000u | (ordered ? far_z : near_z));
    mirror().depth_min = (ordered ? near_z : far_z) & 0x00FFFFFFu;
    mirror().depth_max = (ordered ? far_z : near_z) & 0x00FFFFFFu;
    mirror().depth_scale = scale;
    mirror().depth_translate = translate;
    mirror().depth_range = {static_cast<std::int32_t>(near_z), static_cast<std::int32_t>(far_z),
                            static_cast<std::int32_t>(offset)};
}

// depthRange(near, far)
void gu_depth_range(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    set_context_word(rt, 216u, ctx.gpr[4]);
    set_context_word(rt, 220u, ctx.gpr[5]);
    write_depth_range(rt);
    return_to_caller(ctx);
}

// depthOffset(offset)
void gu_depth_offset(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    set_context_word(rt, 224u, ctx.gpr[4]);
    write_depth_range(rt);
    return_to_caller(ctx);
}

// clearColor (+204), clearStencil (+208), clearDepth (+212): context fields only; the clear reads them.
void gu_clear_color(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    set_context_word(host(corpus), 204u, ctx.gpr[4] & 0x00FFFFFFu);
    mirror().clear.color = ctx.gpr[4] & 0x00FFFFFFu;
    return_to_caller(ctx);
}
void gu_clear_stencil(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    set_context_word(host(corpus), 208u, ctx.gpr[4]);
    mirror().clear.stencil = ctx.gpr[4];
    return_to_caller(ctx);
}
void gu_clear_depth(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    set_context_word(host(corpus), 212u, ctx.gpr[4]);
    mirror().clear.depth = ctx.gpr[4];
    return_to_caller(ctx);
}

// The HUD blitter's sprite extents (context +188 u span, +192 v span, +196 x span, +200 y span).
void gu_blit_offsets(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    for (std::uint32_t i = 0; i < 4u; ++i) {
        set_context_word(rt, 188u + i * 4u, ctx.gpr[4u + i]);
        mirror().blit_offsets[i] = ctx.gpr[4u + i];
    }
    return_to_caller(ctx);
}

// --- Batch 3: the clear and Kurofune's HUD blitter --------------------------------
// Both put their vertices in the display list itself, jumped over, as the GU
// library does with transient data. The host versions place exactly the same
// vertex bytes there (the list memory is the library's scratch space), so the
// vertices decode identically by construction; only the draw commands after
// them are replaced by a marker in native mode.

// After a converted call has made its vertices: native mode prepares the draw
// and leaves a marker; otherwise the game's own commands are written (and, in
// check mode, the native draw is recorded for comparison). `vertices` is the
// address of the vertex bytes placed in the list, or 0 when none were (native
// mode, phase 2: the list then carries only the marker). `host` holds the same
// vertices built on the host, used whenever host vertices are on; check mode
// compares them with what the GE decodes from the list bytes. `clear_word` is
// the CLEARMODE start word for a clear, 0 otherwise.
void finish_converted_draw(Runtime &rt, std::uint32_t vertex_type, std::uint32_t vertices,
                           const std::vector<gpu::Vertex> &host, std::uint32_t primitive, std::uint32_t count,
                           std::uint32_t clear_word) {
    auto &m = mirror();
    resolve_target(rt, m);
    GuMirror state = m;
    state.vertex_type = vertex_type;
    state.vertex_address = vertices & 0x0FFFFFFFu;
    movie_note_draw(rt, state, clear_word != 0u ? "clear" : "blit", count);
    const auto build = [&](gpu::DrawCall &call, Geometry &geometry) {
        call = build_native_draw(state, primitive, count);
        if (clear_word != 0u) {
            call.clear_mode = true;
            call.clear_flags = (clear_word >> 8u) & 7u;
        }
        geometry = geometry_of(state, count);
        if (host_vertices_enabled()) {
            call.vertices = host;
            geometry.host_built = true;
            return count != 0u && !host.empty();
        }
        return count != 0u && state.vertex_address != 0u;
    };
    if (native_draw_enabled()) {
        static std::uint32_t next_slot = 2048u;  // second half of the ring, apart from draw-array's
        NativeSlot &slot = native_slots()[next_slot];
        if (slot.ready) ++native_stats().stale;
        slot.ready = build(slot.call, slot.geometry);
        if (slot.ready) {
            stamp_versions(slot.call);
            ++native_stats().built;
            if (slot.geometry.host_built) ++native_stats().host_vertices;
            ListWriter(rt).marker(kMarkerTag | next_slot);
            next_slot = next_slot + 1u < kNativeSlots ? next_slot + 1u : 2048u;
        } else {
            ++native_stats().skipped;
        }
    } else {
        std::uint32_t primitive_address = 0u;
        {
            ListWriter list(rt);
            if (clear_word != 0u) list.emit(clear_word);
            list.emit(0x12u, vertex_type);
            if (vertices != 0u) {
                list.emit(0x10000000u | (((vertices >> 24u) & 0xFu) << 16u));
                list.emit(0x01u, vertices);
            }
            primitive_address = list.address();
            list.emit(0x04000000u | (primitive << 16u) | count);
            if (clear_word != 0u) list.emit(0xD3000000u);
        }
        if (native_check_enabled()) {
            PendingDraw draw;
            if (build(draw.call, draw.geometry)) pending_draws()[primitive_address & 0x1FFFFFFFu] = std::move(draw);
        }
    }
    // The vertex type and pointer are left as the GE would leave them; with no
    // vertex bytes in the list there is nothing for the pointer to point at.
    m.vertex_type = vertex_type;
    if (vertices == 0u) {
        m.vertex_address = 0u;
        vertex_pointer_from_host_draw() = true;
        return;
    }
    vertex_pointer_from_host_draw() = false;
    std::vector<gpu::Vertex> probe;
    m.vertex_address = (vertices & 0x0FFFFFFFu) +
                       gpu::decode_vertices(rt.memory(), vertices, vertex_type, 0u, probe) * count;
}

// A screen-space (through-mode) vertex exactly as the vertex decoder reads one
// with 16-bit positions: x and y signed, z unsigned, all in integer units.
gpu::Vertex screen_vertex(std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    gpu::Vertex vertex;
    vertex.position[0] = static_cast<float>(static_cast<std::int16_t>(x & 0xFFFFu));
    vertex.position[1] = static_cast<float>(static_cast<std::int16_t>(y & 0xFFFFu));
    vertex.position[2] = static_cast<float>(z & 0xFFFFu);
    return vertex;
}

// Whether the list needs the vertex bytes: always on the display-list path
// (the GE reads them), and in native mode only when host vertices are off.
bool list_needs_vertex_bytes() { return !native_draw_enabled() || !host_vertices_enabled(); }

// Reserves `bytes` of inline data in the list behind a BASE + JUMP pair and
// returns the address of the data; the list continues after it.
std::uint32_t inline_block(Runtime &rt, std::uint32_t bytes) {
    auto &memory = rt.memory();
    const std::uint32_t context = context_address(rt);
    const std::uint32_t start = memory.load32(context + kListWriteField);
    const std::uint32_t end = start + 8u + bytes;
    memory.store32(start, 0x10000000u | (((end >> 24u) & 0xFu) << 16u));
    memory.store32(start + 4u, 0x08000000u | (end & 0x00FFFFFFu));
    memory.store32(context + kListWriteField, end);
    return start + 8u;
}

// clear(flags): 1 colour, 2 stencil, 4 depth, 16 fast (full-width strips);
// otherwise one sprite over the scissor rectangle. The clear stencil goes into
// the colour's alpha bits for the framebuffer format.
void gu_clear(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    auto &memory = rt.memory();
    const std::uint32_t flags = ctx.gpr[4];
    const std::uint32_t format = context_word(rt, 156u), stencil = context_word(rt, 208u), depth = context_word(rt, 212u);
    std::uint32_t color = context_word(rt, 204u);
    if (format == 3u) color |= stencil << 24u;
    else if (format == 1u) color |= stencil << 31u;
    else if (format == 2u) color |= stencil << 28u;
    const std::uint32_t clear_word =
        0xD3000001u | (((flags >> 2u) & 1u) << 10u) | (((flags >> 1u) & 1u) << 9u) | ((flags & 1u) << 8u);
    // The vertices: colour (8888) and 16-bit x, y, z, 12 bytes each.
    std::vector<std::array<std::uint32_t, 2>> corners;
    if ((flags & 0x10u) != 0u) {
        const std::uint32_t strips = format == 3u ? 32u : 16u, width = format == 3u ? 32u : 64u;
        for (std::uint32_t i = 0; i < strips; ++i) corners.push_back({(i / 2u + i % 2u) * width, (i % 2u) * 272u});
    } else {
        const std::uint32_t x1 = context_word(rt, 172u), y1 = context_word(rt, 176u);
        const std::uint32_t width = context_word(rt, 180u) - x1 + 1u, height = context_word(rt, 184u) - y1 + 1u;
        corners.push_back({x1, y1});
        corners.push_back({x1 + width, y1 + height});
    }
    std::vector<gpu::Vertex> host;
    for (const auto &[x, y] : corners) {
        host.push_back(screen_vertex(x, y, depth));
        host.back().color = color;
    }
    const auto count = static_cast<std::uint32_t>(host.size());
    std::uint32_t data = 0u;
    if (list_needs_vertex_bytes()) {
        data = inline_block(rt, count * 12u);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t at = data + i * 12u;
            memory.store32(at, color);
            memory.store16(at + 4u, static_cast<std::uint16_t>(corners[i][0]));
            memory.store16(at + 6u, static_cast<std::uint16_t>(corners[i][1]));
            memory.store16(at + 8u, static_cast<std::uint16_t>(depth));
        }
    }
    finish_converted_draw(rt, 0x0080011Cu, data, host, 6u, count, clear_word);
    return_to_caller(ctx);
}

// Kurofune's HUD/text blitter: blit(x, y, z, u, v, flip, mirror) draws one
// screen-space sprite of the extents set by blitExtents. flip bit 0 swaps u,
// bit 1 swaps v; mirror 1 swaps y, 2 swaps x and y, 3 swaps x. Ends in the
// stall-address update, as the original does.
void gu_blit(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    auto &rt = host(corpus);
    auto &memory = rt.memory();
    const std::uint32_t x = ctx.gpr[4], y = ctx.gpr[5], z = ctx.gpr[6], u = ctx.gpr[7], v = ctx.gpr[8];
    const std::uint32_t flip = ctx.gpr[9];
    const std::int32_t mirror_mode = static_cast<std::int32_t>(ctx.gpr[10]);
    std::uint32_t u0 = u, v0 = v, x0 = x, y0 = y;
    std::uint32_t u1 = u + context_word(rt, 188u), v1 = v + context_word(rt, 192u);
    std::uint32_t x1 = x + context_word(rt, 196u), y1 = y + context_word(rt, 200u);
    if ((flip & 1u) != 0u) std::swap(u0, u1);
    if (((flip >> 1u) & 1u) != 0u) std::swap(v0, v1);
    if (mirror_mode == 1) std::swap(y0, y1);
    else if (mirror_mode == 2) { std::swap(x0, x1); std::swap(y0, y1); }
    else if (mirror_mode == 3) std::swap(x0, x1);
    // The vertices: 16-bit u, v and x, y, z, 10 bytes each.
    std::vector<gpu::Vertex> host{screen_vertex(x0, y0, z), screen_vertex(x1, y1, z)};
    host[0].texcoord = {static_cast<float>(u0 & 0xFFFFu), static_cast<float>(v0 & 0xFFFFu)};
    host[1].texcoord = {static_cast<float>(u1 & 0xFFFFu), static_cast<float>(v1 & 0xFFFFu)};
    std::uint32_t data = 0u;
    if (list_needs_vertex_bytes()) {
        data = inline_block(rt, 20u);
        const std::uint32_t values[10] = {u0, v0, x0, y0, z, u1, v1, x1, y1, z};
        for (std::uint32_t i = 0; i < 10u; ++i) memory.store16(data + i * 2u, static_cast<std::uint16_t>(values[i]));
    }
    finish_converted_draw(rt, 0x00800102u, data, host, 6u, 2u, 0u);
    ctx.pc = gu_update_stall();
}

// --- Registration ---------------------------------------------------------------

using Replacement = void (*)(psprecomp::CorpusRuntime &, AllegrexContext &);

struct Entry {
    const char *name;
    std::uint32_t address;
    Replacement function;
};

constexpr Entry kEntries[] = {
    {"alphaFunc", 0x08C87D9Cu, &gu_alpha_func},     {"depthFunc", 0x08C87DD8u, &gu_depth_func},
    {"depthMask", 0x08C87DFCu, &gu_depth_mask},     {"pixelMask", 0x08C87E20u, &gu_pixel_mask},
    {"stencilFunc", 0x08C87E9Cu, &gu_stencil_func}, {"stencilOp", 0x08C87ED0u, &gu_stencil_op},
    {"blendFunc", 0x08C87F04u, &gu_blend_func},     {"texFilter", 0x08C87A40u, &gu_tex_filter},
    {"texWrap", 0x08C87AACu, &gu_tex_wrap},         {"texOffset", 0x08C87C00u, &gu_tex_offset},
    {"texScale", 0x08C87074u, &gu_tex_scale},       {"offset", 0x08C87838u, &gu_offset},
    {"texFlush", 0x08C87C44u, &gu_tex_flush},       {"clutMode", 0x08C87C90u, &gu_clut_mode},
    {"clutLoad", 0x08C87CCCu, &gu_clut_load},       {"ambient", 0x08C87874u, &gu_ambient},
    {"setMatrix", 0x08C87570u, &gu_set_matrix},     {"boneMatrix", 0x08C87F5Cu, &gu_bone_matrix},
    {"drawArray", 0x08C8759Cu, &gu_draw_array},
    // batch 2
    {"enable", 0x08C87424u, &gu_enable},            {"disable", 0x08C8747Cu, &gu_disable},
    {"setState", 0x08C874D8u, &gu_set_state},       {"texImage", 0x08C87B80u, &gu_tex_image},
    {"texMode", 0x08C87A04u, &gu_tex_mode},         {"texMapMode", 0x08C87B30u, &gu_tex_map_mode},
    {"texFunc", 0x08C87A6Cu, &gu_tex_func},         {"texLevelMode", 0x08C87AD8u, &gu_tex_level_mode},
    {"texSlope", 0x08C87C64u, &gu_tex_slope},       {"fog", 0x08C87D18u, &gu_fog},
    {"scissor", 0x08C87E60u, &gu_scissor},          {"morphWeight", 0x08C88000u, &gu_morph_weight},
    {"lightColor", 0x08C879D0u, &gu_light_color},   {"light", 0x08C8796Cu, &gu_light},
    {"lightMode", 0x08C879A8u, &gu_light_mode},     {"color", 0x08C878B4u, &gu_color},
    {"modelColor", 0x08C878E0u, &gu_model_color},   {"colorMaterial", 0x08C87020u, &gu_color_material},
    {"frontFace", 0x08C8791Cu, &gu_front_face},     {"shadeModel", 0x08C87944u, &gu_shade_model},
    {"dither", 0x08C86EC8u, &gu_dither},            {"drawBuffer", 0x08C87380u, &gu_draw_buffer},
    {"depthBuffer", 0x08C873D0u, &gu_depth_buffer}, {"viewport", 0x08C87740u, &gu_viewport},
    {"depthRange", 0x08C877E4u, &gu_depth_range},   {"depthOffset", 0x08C87810u, &gu_depth_offset},
    {"clearColor", 0x08C876E4u, &gu_clear_color},   {"clearStencil", 0x08C876F8u, &gu_clear_stencil},
    {"clearDepth", 0x08C87708u, &gu_clear_depth}, {"blitExtents", 0x08C8766Cu, &gu_blit_offsets},
    // batch 3
    {"clear", 0x08C87718u, &gu_clear},              {"blit", 0x08C87688u, &gu_blit},
};
constexpr std::size_t kEntryCount = std::size(kEntries);

std::array<std::uint64_t, kEntryCount> &call_counts() {
    static std::array<std::uint64_t, kEntryCount> counts{};
    return counts;
}

// Wraps entry I so each replacement's calls are counted.
template <std::size_t I>
void counted(psprecomp::CorpusRuntime &corpus, AllegrexContext &ctx) {
    ++call_counts()[I];
    kEntries[I].function(corpus, ctx);
}

template <std::size_t... I>
constexpr std::array<Replacement, kEntryCount> make_counted(std::index_sequence<I...>) {
    return {&counted<I>...};
}
constexpr auto kCounted = make_counted(std::make_index_sequence<kEntryCount>{});

bool selected(std::string_view name) {
#if defined(VC2_NATIVE_ONLY)
    (void)name;
    return true;  // every entry point: the GE emulation is not built
#endif
    const char *list = env("NATIVE_GU");
    if (list == nullptr) return false;
    const std::string_view all(list);
    if (all == "all") return true;
    std::size_t start = 0u;
    while (start <= all.size()) {
        const std::size_t end = std::min(all.find(',', start), all.size());
        if (all.substr(start, end - start) == name) return true;
        start = end + 1u;
    }
    return false;
}

}  // namespace

const GuMirror &gu_mirror() { return mirror(); }

void install_native_gu(Runtime &rt) {
    guest_memory() = &rt.memory();
    std::size_t installed = 0u;
    for (std::size_t i = 0; i < kEntryCount; ++i) {
        if (!selected(kEntries[i].name)) continue;
        // The table holds the European entry points; each edition's is checked
        // to be the same function before the game's code is replaced.
        if (!code_matches(rt, kEntries[i].address)) {
            std::cerr << "[vc2-native-gu] " << kEntries[i].name << ": the code at 0x" << std::hex
                      << at(kEntries[i].address) << std::dec << " is not the expected function in the "
                      << region_name() << " executable; the game's own code stays\n";
            continue;
        }
        rt.register_function(at(kEntries[i].address), kCounted[i], std::string("vc2_native_gu_") + kEntries[i].name);
        ++installed;
    }
    if (native_draw_enabled() && selected("drawArray") && selected("setMatrix")) install_game_camera(rt);
    if (native_draw_enabled()) {
        if (selected("drawArray")) {
            gpu::set_native_draw_source(&native_source);
            std::cerr << "[vc2-native-draw] draw-array draws are built natively and drawn from markers\n";
            if (native_lists_enabled()) {
                gpu::set_draw_observer(&observe_ge_draw_without_state);
                std::cerr << "[vc2-native-lists] GU state is not written to the display lists; they carry markers only\n";
            }
        } else {
            std::cerr << "[vc2-native-draw] needs VC2_NATIVE_GU to include drawArray; off\n";
        }
    } else if (native_check_enabled()) {
        if (selected("drawArray")) {
            gpu::set_draw_observer(&observe_ge_draw);
            std::cerr << "[vc2-native-check] comparing native draws with the display-list path\n";
        } else {
            std::cerr << "[vc2-native-check] needs VC2_NATIVE_GU to include drawArray; check off\n";
        }
    }
    if (installed != 0u)
        std::cerr << "[vc2-native-gu] " << installed << " of " << kEntryCount << " GU entry points replaced by host code\n";
}

void register_movie_trace(HleRegistrar &hle) {
    if (!movie_trace_enabled()) return;
    // sceMpegAvcDecode(mpeg, au, frameWidth, bufferPointer, outInit)
    if (HleFunction decode = hle.bound_function("sceMpeg", 0x0E3C2E9Du)) {
        hle.bind("sceMpeg", 0x0E3C2E9Du, [inner = std::move(decode)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t width = ctx.gpr[6], pointer = ctx.gpr[7], out_init = ctx.gpr[8];
            const std::uint32_t destination = pointer != 0u ? rt.memory().load32(pointer) : 0u;
            if (movie_trace().active_flips == 0u) std::cerr << "[vc2-movie] pictures started\n";
            movie_trace().active_flips = 10u;
            inner(rt, ctx);
            const std::uint32_t produced = out_init != 0u ? rt.memory().load32(out_init) : 0u;
            remember(movie_trace().pictures, destination);
            movie_note("  picture -> " + hex8(destination) + " width " + std::to_string(width) +
                       ((destination & 0x1F000000u) == 0x04000000u ? " (VRAM: uploaded to the framebuffer)" : " (RAM)") +
                       (produced != 0u ? "" : " (none produced)"));
        });
    }
    if (HleFunction flip = hle.bound_function("sceDisplay", 0x289D82FEu)) {
        hle.bind("sceDisplay", 0x289D82FEu, [inner = std::move(flip), flips = std::uint64_t{0}](
                                                Runtime &rt, AllegrexContext &ctx) mutable {
            auto &trace = movie_trace();
            ++flips;
#if defined(PORTABLEKIT_HAS_RENDERER)
            // The frame drawn for the previous flip as the renderer shows it
            // (internal resolution, before the window blit), for movie frames
            // 121-128: finds a shift that appears only after drawing.
            if (const char *directory = env("MOVIE_DUMP");
                directory != nullptr && trace.active_flips != 0u && trace.frames_logged >= 121u &&
                trace.frames_logged < 129u) {
                if (gpu::VulkanRenderer *renderer = ensure_renderer()) {
                    std::filesystem::create_directories(directory);
                    const std::string path = std::string(directory) + "/shown" +
                                             std::to_string(trace.frames_logged) + ".bmp";
                    std::cerr << "[vc2-movie] shown frame " << trace.frames_logged << " (last flip fb="
                              << hex8(trace.last_framebuffer) << ")" << (renderer->capture_frame(path) ? "" : " NOT")
                              << " captured\n";
                }
            }
#endif
            if (const char *directory = env("MOVIE_DUMP");
                directory != nullptr && trace.active_flips != 0u && trace.frames_logged >= 120u &&
                trace.frames_logged < 124u) {
                std::filesystem::create_directories(directory);
                const auto dump = [&](const char *what, std::uint32_t address) {
                    constexpr std::uint32_t kBytes = 512u * 272u * 4u;
                    if (!rt.memory().contains(address, kBytes)) return;
                    std::vector<std::uint8_t> bytes(kBytes);
                    rt.memory().copy_out(address, bytes);
                    const std::string path = std::string(directory) + "/flip" + std::to_string(trace.frames_logged) +
                                             "-" + what + "-" + hex8(address) + ".bin";
                    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()),
                                                                static_cast<std::streamsize>(bytes.size()));
                };
                for (const std::uint32_t address : trace.pictures) dump("picture", address);
                for (const std::uint32_t address : trace.textures) dump("texture", address);
                std::cerr << "[vc2-movie] dumped " << trace.pictures.size() << " pictures and "
                          << trace.textures.size() << " textures at movie frame " << trace.frames_logged << "\n";
            }
            if (trace.active_flips != 0u && trace.frames_logged < 600u) {
                trace.last_framebuffer = ctx.gpr[4];
                std::cerr << "[vc2-movie] flip " << flips << " fb=" << hex8(ctx.gpr[4]) << " stride=" << ctx.gpr[5]
                          << " psm=" << ctx.gpr[6] << "\n";
                for (const auto &line : trace.lines) std::cerr << line << "\n";
                ++trace.frames_logged;
                if (--trace.active_flips == 0u) std::cerr << "[vc2-movie] pictures stopped\n";
            }
            trace.lines.clear();
            inner(rt, ctx);
        });
    }
    std::cerr << "[vc2-movie] tracing video frames\n";
}

// --- Native list executor (with VC2_NATIVE_LISTS) --------------------------------
// The GE is out of the loop: the game's display lists are run here. With
// markers-only lists a frame's list holds only (list census, 2026-11-07):
// SIGNAL+END pairs, the native draw markers, BASE+JUMP over data the game keeps
// inside the list, FINISH+END. The executor submits each marker's native draw
// to the renderer and raises the game's signal and finish callbacks exactly as
// PortableKit's GE did (interrupt context, low 16 bits of the signal, the
// address after the command). Register writes (the boot list, the framebuffer
// setup at screen changes) are ignored: every native draw carries its full
// state. A command that would make the GE draw or move memory (PRIM, BEZIER,
// SPLINE, TRANSFER START) cannot happen with every draw native; if one appears
// it is counted and reported, never emulated.
//
// All six list services are replaced: sceGeSetCallback, sceGeUnsetCallback,
// sceGeListEnQueue, sceGeListEnQueueHead, sceGeListUpdateStallAddr,
// sceGeListSync and sceGeDrawSync. Lists run as soon as they are queued or
// their stall address moves, so the game never waits; the sync calls report
// whether a list has reached its end.

struct ExecCallback {
    std::uint32_t signal_function = 0u, signal_argument = 0u, finish_function = 0u, finish_argument = 0u;
};
struct ExecList {
    std::uint32_t pc = 0u, stall = 0u, base = 0u, offset = 0u;
    std::int32_t callback = -1;
    std::vector<std::uint32_t> returns;
    bool done = false;
};
struct ExecStats {
    std::uint64_t lists = 0u, draws = 0u, signals = 0u, finishes = 0u, register_words = 0u;
    std::map<std::string, std::uint64_t> unsupported;  // commands the GE would have had to run
};
struct Executor {
    std::map<std::int32_t, ExecCallback> callbacks;
    std::int32_t next_callback = 0;
    std::map<std::uint32_t, ExecList> lists;
    std::uint32_t next_list = 1u;
    ExecStats stats;
};
Executor &executor() {
    static Executor e;
    return e;
}

bool native_executor_enabled() { return native_lists_enabled(); }

void raise_list_interrupt(const ExecList &list, bool finish, std::uint32_t value, std::uint32_t pc) {
    auto &e = executor();
    const auto found = e.callbacks.find(list.callback);
    if (found == e.callbacks.end()) return;
    const std::uint32_t function = finish ? found->second.finish_function : found->second.signal_function;
    if (function == 0u) return;
    InterruptCall call{};
    call.function = function;
    call.arguments = {value & 0xFFFFu, finish ? found->second.finish_argument : found->second.signal_argument, pc, 0u};
    kernel().queue_interrupt(std::move(call));
}

void note_unsupported(const std::string &what, std::uint32_t at) {
    auto &stats = executor().stats;
    if (stats.unsupported[what]++ < 3u)
        std::cerr << "[vc2-native-exec] UNSUPPORTED " << what << " at " << hex8(at)
                  << " (a draw or transfer not converted to native; not run)\n";
}

// --- SETTINGS on the title menu (2026-11-09) ----------------------------------
// The PSP's data install (a copy of the disc on the memory stick) has no use on
// a PC: its title menu entry is SETTINGS instead, which opens the game's own
// Options screen as the in-game Settings screen (vc2_settings_menu.cpp).
// The menu is a generic list widget laid out by if_mlx_adv_title.mlx: five
// entries, one 18 px row each (origins x 240, y 165 + 18k), every word a band
// of one 512x256 sheet (176x24 texels at u 0.656.., one band under another).
// The INSTALL band is repainted as SETTINGS from the menu's own letters (the
// texture override below: S T I N from OPTIONS, E from EXTRAS, G from NEW
// GAME), so the entry keeps every effect the game gives it. Choosing it runs
// the game's own OPTIONS (vc2_title_choice, the title's handler 0x08B81A60,
// unit 0223) with the Settings screen asked for. Europe and the US.
// VC2_KEEP_INSTALL=1 leaves the original menu.
namespace {
std::atomic<std::int64_t> title_menu_seen_ms{-1000000};

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool keep_install() {
    static const bool keep = [] {
        const char *value = std::getenv("VC2_KEEP_INSTALL");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return keep;
}

// The title menu row (0..4) a draw belongs to, or -1.
int title_menu_row(const gpu::DrawCall &call) {
    if (call.through || !call.texture.enabled || std::fabs(call.world[12] - 240.0f) > 0.5f) return -1;
    const float k = (call.world[13] - 165.0f) / 18.0f;
    const long row = std::lround(k);
    if (row < 0 || row > 4 || std::fabs(k - static_cast<float>(row)) > 0.03f) return -1;
    return static_cast<int>(row);
}

// The title sheet: its address and the INSTALL band (texture coordinates).
struct TitleSheet {
    std::atomic<std::uint32_t> address{0u};
    std::atomic<std::uint32_t> width{0u}, height{0u};
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    std::uint64_t band_hash = 0u;  // the original's INSTALL band (alpha), once seen
    std::uint32_t painted = 0u;
};
TitleSheet &title_sheet() {
    static TitleSheet t;
    return t;
}

void title_menu_settings(const std::vector<gpu::DrawCall *> &draws) {
    if (keep_install() || region() == Region::Japan) return;
    std::uint32_t rows = 0u, texture = 0u;
    float u0 = 1e9f, v0 = 1e9f, u1 = -1e9f, v1 = -1e9f;
    const gpu::DrawCall *sample = nullptr;
    for (const gpu::DrawCall *call : draws) {
        const int row = title_menu_row(*call);
        if (row < 0) continue;
        if (rows == 0u) texture = call->texture.address, sample = call;
        else if (call->texture.address != texture) return;
        rows |= 1u << row;
        if (row == 3)
            for (const gpu::Vertex &v : call->vertices) {
                u0 = std::min(u0, v.texcoord[0]), u1 = std::max(u1, v.texcoord[0]);
                v0 = std::min(v0, v.texcoord[1]), v1 = std::max(v1, v.texcoord[1]);
            }
    }
    if (rows != 0x1Fu || sample == nullptr || u1 <= u0 || v1 <= v0) return;
    title_menu_seen_ms.store(steady_ms(), std::memory_order_relaxed);
    TitleSheet &t = title_sheet();
    if (t.address.load() != texture) {
        t.u0 = u0, t.v0 = v0, t.u1 = u1, t.v1 = v1;
        t.width = sample->texture.width;
        t.height = sample->texture.height;
        t.band_hash = 0u;
        t.address = texture;
        gpu::bump_texture_override_generation();  // painted the next time it is drawn
        std::cerr << "[vc2-title] title sheet at " << hex8(texture) << ": INSTALL becomes SETTINGS\n";
    }
}

// Paints SETTINGS over the INSTALL band of `pixels` (w x h RGBA, R low), from
// the menu's own letters. Letters are found by their gold fill in each band
// (EXTRAS 6, NEW GAME 7, OPTIONS 7 letters, the bands 25/256 apart above
// INSTALL's) and copied with their outline. False when the sheet is not laid
// out as expected (then it stays as it is).
bool paint_settings(std::vector<std::uint32_t> &pixels, std::uint32_t w, std::uint32_t h, const TitleSheet &t) {
    const auto band_rows = [&](float v0, float v1) {
        return std::pair<int, int>{static_cast<int>(std::lround(v0 * h)), static_cast<int>(std::lround(v1 * h))};
    };
    const int x0 = static_cast<int>(std::lround(t.u0 * w)), x1 = std::min<int>(static_cast<int>(std::lround(t.u1 * w)), w);
    const float pitch = 25.0f / 256.0f;  // band to band (the sheet's layout)
    struct Letter { int l, r, y0, y1; };
    const int need = w >= 1024u ? 2 : 1;
    const auto letters = [&](float v0, std::size_t want) {
        const auto [y0, y1] = band_rows(v0, v0 + (t.v1 - t.v0));
        std::vector<std::pair<int, int>> runs;
        std::vector<int> fill(static_cast<std::size_t>(std::max(0, x1 - x0)), 0);
        int start = -1;
        for (int x = x0; x <= x1; ++x) {
            int count = 0;
            if (x < x1)
                for (int y = std::max(0, y0); y < std::min<int>(y1, h); ++y) {
                    const std::uint32_t p = pixels[static_cast<std::size_t>(y) * w + x];
                    const int r = p & 0xFF, g = (p >> 8) & 0xFF, b = (p >> 16) & 0xFF, a = p >> 24;
                    if (a > 128 && (r * 3 + g * 6 + b) / 10 > 60) ++count;
                }
            if (x < x1) fill[static_cast<std::size_t>(x - x0)] = count;
            const bool on = count >= need;
            if (on && start < 0) start = x;
            if (!on && start >= 0) runs.push_back({start, x}), start = -1;
        }
        // At the game's own size neighbouring letters can touch (EXTRAS's R
        // and A): a run about twice a letter's width is split where it is
        // thinnest, in its middle half.
        while (!runs.empty() && runs.size() < want) {
            std::vector<int> widths;
            for (const auto &[a, b] : runs) widths.push_back(b - a);
            std::vector<int> sorted = widths;
            std::sort(sorted.begin(), sorted.end());
            const std::size_t widest = static_cast<std::size_t>(std::max_element(widths.begin(), widths.end()) - widths.begin());
            const auto [a, b] = runs[widest];
            if (static_cast<float>(b - a) < 1.6f * static_cast<float>(sorted[sorted.size() / 2])) break;
            int cut = a + (b - a) / 4;
            for (int x = cut; x < b - (b - a) / 4; ++x)
                if (fill[static_cast<std::size_t>(x - x0)] < fill[static_cast<std::size_t>(cut - x0)]) cut = x;
            runs[widest] = {a, cut};
            runs.insert(runs.begin() + static_cast<std::ptrdiff_t>(widest) + 1, {cut + 1, b});
        }
        const int margin = std::max(2, static_cast<int>(std::lround(6.0 * w / 2048.0)));
        std::vector<Letter> out;
        for (std::size_t i = 0; i < runs.size(); ++i) {
            const int l = i > 0 ? std::max((runs[i - 1].second + runs[i].first) / 2, runs[i].first - margin)
                                : std::max(x0, runs[i].first - margin);
            const int r = i + 1 < runs.size() ? std::min((runs[i].second + runs[i + 1].first) / 2, runs[i].second + margin)
                                              : std::min(x1, runs[i].second + margin);
            out.push_back({l, r, y0, y1});
        }
        return out;
    };
    const auto extras = letters(t.v0 - 4.0f * pitch, 6u);
    const auto new_game = letters(t.v0 - 3.0f * pitch, 7u);
    const auto options = letters(t.v0 - 1.0f * pitch, 7u);
    if (extras.size() != 6u || new_game.size() != 7u || options.size() != 7u) {
        std::cerr << "[vc2-title] the title sheet's letters were not found (" << extras.size() << "/" << new_game.size()
                  << "/" << options.size() << "); INSTALL kept\n";
        return false;
    }
    // S E T T I N G S
    const Letter pick[8] = {options[6], extras[0], options[2], options[2], options[3], options[5], new_game[3], options[6]};
    const std::vector<std::uint32_t> source = pixels;
    const auto [ty0, ty1] = band_rows(t.v0, t.v1);
    for (int y = std::max(0, ty0); y < std::min<int>(ty1, h); ++y)
        for (int x = x0; x < x1; ++x) pixels[static_cast<std::size_t>(y) * w + x] = 0u;
    int total = 0;
    for (const Letter &l : pick) total += l.r - l.l;
    if (total > x1 - x0) return false;
    int x = x0 + ((x1 - x0) - total) / 2;
    for (const Letter &l : pick) {
        for (int dy = 0; dy < l.y1 - l.y0; ++dy) {
            const int sy = l.y0 + dy, dy_ = ty0 + dy;
            if (sy < 0 || sy >= static_cast<int>(h) || dy_ < 0 || dy_ >= static_cast<int>(h)) continue;
            for (int dx = 0; dx < l.r - l.l; ++dx) {
                const std::uint32_t p = source[static_cast<std::size_t>(sy) * w + l.l + dx];
                std::uint32_t &q = pixels[static_cast<std::size_t>(dy_) * w + x + dx];
                // Over (the neighbours' outlines meet at the seams).
                const std::uint32_t pa = p >> 24, qa = q >> 24;
                if (pa == 0u) continue;
                if (pa == 255u || qa == 0u) {
                    q = p;
                    continue;
                }
                const std::uint32_t oa = pa + qa * (255u - pa) / 255u;
                std::uint32_t out = oa << 24;
                for (int c = 0; c < 3; ++c) {
                    const std::uint32_t pc = (p >> (c * 8)) & 0xFF, qc = (q >> (c * 8)) & 0xFF;
                    const std::uint32_t v = (pc * pa + qc * qa * (255u - pa) / 255u) / std::max(oa, 1u);
                    out |= std::min(v, 255u) << (c * 8);
                }
                q = out;
            }
        }
        x += l.r - l.l;
    }
    return true;
}

gpu::TextureOverride g_prompt_override = nullptr;

gpu::TextureOverrideResult title_override(const gpu::TextureState &texture, const std::uint32_t *texels,
                                          const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                          gpu::TextureOverrideImage &out) {
    using Result = gpu::TextureOverrideResult;
    TitleSheet &t = title_sheet();
    const std::uint32_t address = t.address.load();
    // The renderer asks again, after a change of generation, only about the
    // textures claimed when they were made, and the title sheet is made
    // before its address is known: every texture of its size is claimed
    // (kept as it is) until then, so it is made again once it is known.
    const bool sheet_size = texture.width == 512u && texture.height == 256u;
    if (address == 0u && sheet_size) return Result::Keep;
    if (address == 0u || texture.address != address || texture.width != t.width.load() ||
        texture.height != t.height.load())
        return g_prompt_override != nullptr ? g_prompt_override(texture, texels, pack_image, wanted_scale, out)
                                            : Result::Ignore;
    if (texels == nullptr) return Result::Keep;
    const std::uint32_t w = texture.width, h = texture.height;
    // The same sheet as when it was found (the address may be reused later):
    // the INSTALL band's coverage.
    std::uint64_t hash = 1469598103934665603ull;
    const int y0 = static_cast<int>(t.v0 * h), y1 = static_cast<int>(t.v1 * h), x0 = static_cast<int>(t.u0 * w);
    for (int y = y0; y < y1 && y < static_cast<int>(h); ++y)
        for (std::uint32_t x = x0; x < w; ++x) hash = (hash ^ ((texels[y * w + x] >> 24) > 128u)) * 1099511628211ull;
    if (t.band_hash == 0u) t.band_hash = hash;
    else if (hash != t.band_hash)
        return g_prompt_override != nullptr ? g_prompt_override(texture, texels, pack_image, wanted_scale, out)
                                            : Result::Ignore;
    if (pack_image == nullptr) return Result::WantsPackImage;
    std::uint32_t pw = w, ph = h;
    std::vector<std::uint32_t> pixels;
    if (pack_image->width >= w && pack_image->width % w == 0u && pack_image->height * w == pack_image->width * h &&
        pack_image->pixels.size() == std::size_t{pack_image->width} * pack_image->height) {
        pw = pack_image->width, ph = pack_image->height;
        pixels = pack_image->pixels;
    } else {
        pixels.assign(texels, texels + std::size_t{w} * h);
    }
    if (!paint_settings(pixels, pw, ph, t)) return Result::Keep;
    out.width = pw;
    out.height = ph;
    out.pixels = std::move(pixels);
    if (++t.painted <= 10u)
        std::cerr << "[vc2-title] SETTINGS painted at " << pw << "x" << ph
                  << (pw != w ? " over the texture pack's image" : " over the original") << "\n";
    return Result::Replace;
}

}  // namespace

bool title_menu_shown() { return steady_ms() - title_menu_seen_ms.load(std::memory_order_relaxed) <= 2000; }

// Installed after the prompts (vc2_profile.cpp), whose override it passes on to.
void install_title_settings() {
    if (keep_install() || region() == Region::Japan) return;
    g_prompt_override = gpu::texture_override();
    gpu::set_texture_override(&title_override);
}

// --- The Settings screen (2026-11-09) ------------------------------------------
// The game's Options screen, opened from the title's SETTINGS, with our rows:
// the screen's frame, panel, tab boxes, description box and button hints stay
// the game's; its rows, tab names, title and description are replaced by
// copies of its own pieces (label box, value boxes, focus frame, arrows, tab
// boxes) and text drawn from the game's font sheet with the game's metrics
// (each character 0.75 of its 16-texel cell, a 1-pixel dark outline). The
// model and the pad: vc2_settings_menu.cpp. Layout (PSP pixels): rows 19 high
// from y 56, label boxes x 33..231, value boxes 66 wide from x 250.
namespace {

struct Rect {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
};
Rect extent(const gpu::DrawCall &call) {
    Rect r;
    for (const gpu::Vertex &v : call.vertices) {
        const float x = v.position[0] * call.world[0] + call.world[12];
        const float y = v.position[1] * call.world[5] + call.world[13];
        r.x0 = std::min(r.x0, x), r.x1 = std::max(r.x1, x), r.y0 = std::min(r.y0, y), r.y1 = std::max(r.y1, y);
    }
    return r;
}
bool near(float a, float b, float tolerance = 1.5f) { return std::fabs(a - b) <= tolerance; }

bool is_font(const gpu::DrawCall &call) {
    return !call.through && call.texture.enabled && call.texture.width == 256u && call.texture.height == 128u &&
           call.primitive == gpu::PrimitiveType::Sprites;
}

// The font object's character rectangles (+0xDB8: 96 of u0 v0 u1 v1), found
// by the class pointer at +0x10 (EU 0x08D2F5F0), as vc2_text_hd did. Asked
// for only once the game draws text; until the font object is made (boot),
// looked for again at most once a second.
const std::array<std::array<float, 4>, 96> *font_rects() {
    static std::array<std::array<float, 4>, 96> rects{};
    static bool found = false;
    static std::chrono::steady_clock::time_point next{};
    static std::uint32_t misses = 0u;
    if (found) return &rects;
    const auto now = std::chrono::steady_clock::now();
    if (now < next) return nullptr;
    next = now + std::chrono::seconds(1);
    auto &memory = kernel().runtime().memory();
    const std::uint32_t font_class = at(0x08D2F5F0u);
    if (font_class == 0u) return nullptr;
    for (std::uint32_t address = 0x08800010u; address + 0xE30u < 0x0A000000u; address += 4u) {
        if (memory.load32(address) != font_class) continue;
        const std::uint32_t object = address - 0x10u;
        if (memory.load16(object + 0xDE4u) != 16u || memory.load16(object + 0xDE6u) != 16u) continue;
        const std::uint32_t table = memory.load32(object + 0xDB8u);
        if (table == 0u || !memory.contains(table, 96u * 16u)) continue;
        for (std::uint32_t i = 0; i < 96u; ++i)
            for (std::uint32_t k = 0; k < 4u; ++k)
                rects[i][k] = std::bit_cast<float>(memory.load32(table + i * 16u + k * 4u));
        found = true;
        std::cerr << "[vc2-settings-screen] font rectangles at " << hex8(table) << "\n";
        // For the text editor's preview (vc2-script): every character's cell
        // in the 256 x 128 sheet, in texels (its width is its advance at 1x).
        std::cerr << "[vc2-font-metrics]";
        for (std::uint32_t i = 0; i < 96u; ++i)
            std::cerr << " " << i + 32u << ":" << std::lround(rects[i][0] * 256.0f) << ","
                      << std::lround(rects[i][1] * 128.0f) << "," << std::lround(rects[i][2] * 256.0f) << ","
                      << std::lround(rects[i][3] * 128.0f);
        std::cerr << "\n";
        break;
    }
    if (!found && ++misses <= 3u)
        std::cerr << "[vc2-settings-screen] the font object was not found (yet); looked for again each second\n";
    return found ? &rects : nullptr;
}

std::deque<gpu::DrawCall> &extra_draws() {
    static std::deque<gpu::DrawCall> d;
    return d;
}

// A copy of `from` whose screen rectangle `a` lands on `b` (positions made
// absolute: the world's scale and translation folded in).
gpu::DrawCall *moved(const gpu::DrawCall &from, const Rect &a, const Rect &b, std::uint32_t colour = 0u,
                     bool recolour = false) {
    gpu::DrawCall &call = extra_draws().emplace_back(from);
    call.raw_vertices = nullptr, call.raw_count = 0u, call.bone_matrices = nullptr;
    const float sx = (a.x1 - a.x0) > 0.0f ? (b.x1 - b.x0) / (a.x1 - a.x0) : 1.0f;
    const float sy = (a.y1 - a.y0) > 0.0f ? (b.y1 - b.y0) / (a.y1 - a.y0) : 1.0f;
    for (gpu::Vertex &v : call.vertices) {
        const float x = v.position[0] * from.world[0] + from.world[12];
        const float y = v.position[1] * from.world[5] + from.world[13];
        v.position[0] = b.x0 + (x - a.x0) * sx;
        v.position[1] = b.y0 + (y - a.y0) * sy;
        if (recolour) v.color = colour;
    }
    call.world[0] = 1.0f, call.world[5] = 1.0f, call.world[12] = 0.0f, call.world[13] = 0.0f;
    return &call;
}

struct TextStyle {
    const gpu::DrawCall *font = nullptr;
    std::uint32_t main = 0xFFFFFFFFu, outline = 0xFF202020u;
    float scale = 0.75f;
};

float text_width(const std::string &text, float scale) {
    const auto *rects = font_rects();
    if (rects == nullptr) return 0.0f;
    float w = 0.0f;
    for (const char c : text) {
        const int i = static_cast<unsigned char>(c) - 32;
        if (i < 0 || i >= 96) continue;
        w += ((*rects)[i][2] - (*rects)[i][0]) * 256.0f * scale;
    }
    return w;
}

// Text as the game draws it: per character four outline sprites (one pixel
// off each diagonal) and the character, from the font sheet's rectangles.
gpu::DrawCall *text(const TextStyle &style, const std::string &s, float x, float y) {
    const auto *rects = font_rects();
    if (rects == nullptr || style.font == nullptr || s.empty()) return nullptr;
    gpu::DrawCall &call = extra_draws().emplace_back(*style.font);
    call.raw_vertices = nullptr, call.raw_count = 0u, call.bone_matrices = nullptr;
    call.vertices.clear();
    call.indices.clear();
    call.world[0] = 1.0f, call.world[5] = 1.0f, call.world[12] = 0.0f, call.world[13] = 0.0f;
    const auto sprite = [&](float x0, float y0, float x1, float y1, const std::array<float, 4> &uv, std::uint32_t colour) {
        const float iu = 0.5f * (uv[2] - uv[0]) / std::max(x1 - x0, 1e-3f);
        const float iv = 0.5f * (uv[3] - uv[1]) / std::max(y1 - y0, 1e-3f);
        gpu::Vertex a{}, b{};
        a.position = {x0, y0, 0.0f, uv[3] - iv};
        b.position = {x1, y1, 0.0f, uv[3] - iv};
        a.texcoord = {uv[0], uv[1]};
        b.texcoord = {uv[2], uv[3]};
        a.normal = b.normal = {uv[0] + iu, uv[1] + iv, uv[2] - iu};
        a.color = b.color = colour;
        call.vertices.push_back(a);
        call.vertices.push_back(b);
    };
    float pen = x;
    for (const char c : s) {
        const int i = static_cast<unsigned char>(c) - 32;
        if (i < 0 || i >= 96) continue;
        const auto &uv = (*rects)[i];
        const float w = (uv[2] - uv[0]) * 256.0f * style.scale, h = (uv[3] - uv[1]) * 128.0f * style.scale;
        if (w > 0.0f && h > 0.0f && c != ' ') {
            for (const float dx : {-1.0f, 1.0f})
                for (const float dy : {-1.0f, 1.0f}) sprite(pen + dx, y + dy, pen + dx + w, y + dy + h, uv, style.outline);
            sprite(pen, y, pen + w, y + h, uv, style.main);
        }
        pen += w;
    }
    call.primitive_count = static_cast<std::uint32_t>(call.vertices.size() / 2u);
    return call.vertices.empty() ? nullptr : &call;
}

// Splits `s` into lines no wider than `width`.
std::vector<std::string> wrap(const std::string &s, float width, float scale) {
    std::vector<std::string> lines;
    std::string line, word;
    std::istringstream in(s);
    while (in >> word) {
        const std::string tried = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_width(tried, scale) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = tried;
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

// The Options screen's panel (x 10..470, y 30..198, z -170): the frame is
// the Options screen.
bool is_options_panel(const gpu::DrawCall &call) {
    if (call.through || !call.texture.enabled || call.vertices.size() != 4u || !near(call.world[14], -170.0f, 0.6f))
        return false;
    const Rect r = extent(call);
    return near(r.x0, 10.0f) && near(r.x1, 470.0f) && near(r.y0, 30.0f) && near(r.y1, 198.0f);
}

void settings_screen(std::vector<gpu::DrawCall *> &draws) {
    if (region() == Region::Japan || keep_install()) return;
    bool shown = false;
    for (const gpu::DrawCall *call : draws)
        if (is_options_panel(*call)) shown = true;
    settings_menu::options_frame(shown);
    if (!shown || !settings_menu::active()) return;
    // The pieces, from the game's own rows of this frame.
    const gpu::DrawCall *font = nullptr, *title_font = nullptr, *tab_font_on = nullptr, *tab_font_off = nullptr;
    std::vector<const gpu::DrawCall *> label_box, value_box, focus, tab_on, tab_off, tab_left, tab_right;
    const gpu::DrawCall *arrow_left = nullptr, *arrow_right = nullptr;
    // A volume slider (Preferences): its frame halves (x 271..349, 349..426,
    // 11 high), its bar (278..405 at the game's volume, 5 high) and its min /
    // max marks (249..269, 428..448, 8 high), from one row.
    std::vector<const gpu::DrawCall *> slider_frame;
    const gpu::DrawCall *slider_bar = nullptr, *slider_min = nullptr, *slider_max = nullptr;
    float slider_y = -1.0f;
    float label_y = -1.0f, value_y = -1.0f, focus_y = -1.0f;
    Rect tab_on_rect, tab_off_rect;
    for (const gpu::DrawCall *call : draws) {
        if (call->through || !call->texture.enabled || call->vertices.empty()) continue;
        const Rect r = extent(*call);
        if (is_font(*call)) {
            if (near(r.y0, 6.0f, 2.0f) && r.x1 < 200.0f) title_font = call;
            else if (near(r.y0, 34.0f, 2.0f)) {
                // Tab names: the active one's ink is lighter.
                const std::uint32_t ink = call->vertices.size() > 8u ? call->vertices[8].color : 0u;
                const int light = static_cast<int>(ink & 0xFF) + ((ink >> 8) & 0xFF) + ((ink >> 16) & 0xFF);
                (light > 600 ? tab_font_on : tab_font_off) = call;
            } else if (r.y0 > 50.0f && r.y1 < 197.0f && font == nullptr) font = call;
            continue;
        }
        const float h = r.y1 - r.y0;
        if (r.y0 > 50.0f && r.y1 < 197.0f && (near(h, 11.0f) || near(h, 5.0f) || near(h, 8.0f))) {
            const bool frame = near(h, 11.0f) && ((near(r.x0, 271.0f) && near(r.x1, 349.0f)) ||
                                                  (near(r.x0, 349.0f) && near(r.x1, 426.0f)));
            const bool bar = near(h, 5.0f) && near(r.x0, 278.0f);
            const bool mark = near(h, 8.0f) && ((near(r.x0, 249.0f) && near(r.x1, 269.0f)) ||
                                                (near(r.x0, 428.0f) && near(r.x1, 448.0f)));
            if (frame || bar || mark) {
                const float row_y = r.y0 - (frame ? 4.0f : bar ? 7.0f : 5.0f);
                if (slider_y < 0.0f) slider_y = row_y;
                if (near(row_y, slider_y)) {
                    if (frame) slider_frame.push_back(call);
                    else if (bar) slider_bar = call;
                    else (near(r.x0, 249.0f) ? slider_min : slider_max) = call;
                }
                continue;
            }
        }
        if (near(r.y0, 32.0f) && near(r.y1, 48.0f) && r.x0 >= 22.0f && r.x1 <= 458.0f) {
            // Tab boxes: two halves and their faded copies per tab, sorted out
            // below (left tab: x below 240).
            (r.x0 < 239.0f ? tab_left : tab_right).push_back(call);
            continue;
        }
        if (!near(h, 19.0f) && !(near(h, 13.0f) && near(r.x1 - r.x0, 7.0f))) continue;
        if (r.y0 < 50.0f || r.y1 > 197.0f) continue;
        if (near(r.x1 - r.x0, 7.0f)) {
            if (arrow_left == nullptr || r.x0 < extent(*arrow_left).x0) arrow_left = call;
            if (arrow_right == nullptr || r.x0 > extent(*arrow_right).x0) arrow_right = call;
            continue;
        }
        if ((near(r.x0, 33.0f) && near(r.x1, 132.0f)) || (near(r.x0, 132.0f) && near(r.x1, 231.0f))) {
            if (label_y < 0.0f) label_y = r.y0;
            if (near(r.y0, label_y)) label_box.push_back(call);
            continue;
        }
        if ((near(r.x0, 33.0f) && near(r.x1, 49.0f)) || (near(r.x0, 49.0f) && near(r.x1, 215.0f)) ||
            (near(r.x0, 215.0f) && near(r.x1, 231.0f))) {
            if (focus_y < 0.0f) focus_y = r.y0;
            if (near(r.y0, focus_y)) focus.push_back(call);
            continue;
        }
        if (near(r.x0, 250.0f) && near(r.x1, 316.0f)) {
            if (value_y < 0.0f) value_y = r.y0;
            if (near(r.y0, value_y)) value_box.push_back(call);
        }
    }
    // The active tab: the one whose opaque body is the brighter.
    const auto body_light = [&](const std::vector<const gpu::DrawCall *> &tab) {
        int best = -1;
        for (const gpu::DrawCall *call : tab) {
            const std::uint32_t c = call->vertices.front().color;
            if ((c >> 24) != 0xFFu) continue;
            best = std::max(best, static_cast<int>(c & 0xFF) + static_cast<int>((c >> 8) & 0xFF) +
                                      static_cast<int>((c >> 16) & 0xFF));
        }
        return best;
    };
    {
        const bool left_on = body_light(tab_left) >= body_light(tab_right);
        tab_on = left_on ? tab_left : tab_right;
        tab_off = left_on ? tab_right : tab_left;
        for (const gpu::DrawCall *call : tab_on) {
            const Rect r = extent(*call);
            tab_on_rect.x0 = std::min(tab_on_rect.x0, r.x0), tab_on_rect.x1 = std::max(tab_on_rect.x1, r.x1);
        }
        for (const gpu::DrawCall *call : tab_off) {
            const Rect r = extent(*call);
            tab_off_rect.x0 = std::min(tab_off_rect.x0, r.x0), tab_off_rect.x1 = std::max(tab_off_rect.x1, r.x1);
        }
    }
    if (font == nullptr) font = title_font;
    if (font == nullptr || title_font == nullptr || label_box.size() != 2u || value_box.empty() || focus.empty() ||
        tab_on.empty() || tab_off.empty() || font_rects() == nullptr) {
        static std::uint32_t logged = 0u;
        if (++logged <= 5u)
            std::cerr << "[vc2-settings-screen] the Options screen's pieces were not all found (font " << (font != nullptr)
                      << ", label " << label_box.size() << ", value " << value_box.size() << ", focus " << focus.size()
                      << ", tabs " << tab_on.size() << "/" << tab_off.size() << "); the game's rows are left\n";
        return;
    }
    if (tab_font_on == nullptr) tab_font_on = font;
    if (tab_font_off == nullptr) tab_font_off = tab_font_on;
    // The game's rows, tab names, title and description go.
    draws.erase(std::remove_if(draws.begin(), draws.end(),
                               [&](const gpu::DrawCall *call) {
                                   if (call->through || call->vertices.empty()) return false;
                                   const Rect r = extent(*call);
                                   if (call->world[14] < -165.0f) return false;  // panel, frame, boxes behind
                                   if (r.x0 >= 20.0f && r.x1 <= 460.0f && r.y0 >= 50.0f && r.y1 <= 197.0f) return true;
                                   if (r.x0 >= 22.0f && r.x1 <= 458.0f && r.y0 >= 31.0f && r.y1 <= 49.0f) return true;
                                   if (is_font(*call) && r.y0 >= 4.0f && r.y1 <= 26.0f && r.x1 < 200.0f) return true;
                                   if (is_font(*call) && r.y0 >= 214.0f && r.x1 <= 312.0f) return true;
                                   return false;
                               }),
                draws.end());
    const settings_menu::View view = settings_menu::view();
    std::vector<gpu::DrawCall *> boxes, texts;
    const auto add = [](std::vector<gpu::DrawCall *> &list, gpu::DrawCall *call) {
        if (call != nullptr) list.push_back(call);
    };
    const auto ink = [](const gpu::DrawCall *call, std::uint32_t &main, std::uint32_t &outline) {
        if (call->vertices.size() > 8u) main = call->vertices[8].color, outline = call->vertices[0].color;
    };
    // Tabs: the panel's tab strip (x 23..457) shared out.
    const int tab_count = static_cast<int>(view.tabs.size());
    const float strip0 = 23.0f, strip1 = 457.0f, tab_w = (strip1 - strip0) / std::max(tab_count, 1);
    for (int i = 0; i < tab_count; ++i) {
        const bool on = i == view.tab;
        const Rect to{strip0 + tab_w * i, 32.0f, strip0 + tab_w * (i + 1), 48.0f};
        for (const gpu::DrawCall *piece : on ? tab_on : tab_off) {
            const Rect from = on ? tab_on_rect : tab_off_rect;
            const Rect r = extent(*piece);
            // Each half keeps its share of the tab.
            const float f0 = (r.x0 - from.x0) / (from.x1 - from.x0), f1 = (r.x1 - from.x0) / (from.x1 - from.x0);
            add(boxes, moved(*piece, r, Rect{to.x0 + f0 * (to.x1 - to.x0), r.y0, to.x0 + f1 * (to.x1 - to.x0), r.y1}));
        }
        TextStyle style{on ? tab_font_on : tab_font_off};
        ink(style.font, style.main, style.outline);
        const float w = text_width(view.tabs[i], style.scale);
        add(texts, text(style, view.tabs[i], std::round((to.x0 + to.x1 - w) * 0.5f), 35.0f));
    }
    // Rows.
    TextStyle row_style{font};
    ink(font, row_style.main, row_style.outline);
    const Rect label_from = {33.0f, label_y, 231.0f, label_y + 19.0f};
    const Rect value_from = {250.0f, value_y, 316.0f, value_y + 19.0f};
    for (std::size_t i = 0; i < view.rows.size(); ++i) {
        const settings_menu::RowView &row = view.rows[i];
        const float y = 56.0f + 19.0f * static_cast<float>(i);
        for (const gpu::DrawCall *piece : label_box) {
            const Rect r = extent(*piece);
            add(boxes, moved(*piece, r, Rect{r.x0, y, r.x1, y + 19.0f}));
        }
        if (static_cast<int>(i) == view.focus)
            for (const gpu::DrawCall *piece : focus) {
                const Rect r = extent(*piece);
                add(boxes, moved(*piece, r, Rect{r.x0, y + (r.y0 - focus_y), r.x1, y + (r.y1 - focus_y)}));
            }
        const float lw = text_width(row.label, row_style.scale);
        add(texts, text(row_style, row.label, std::round(132.0f - lw * 0.5f), y + 4.0f));
        if (row.slider && slider_frame.size() == 2u && slider_bar != nullptr && slider_min != nullptr &&
            slider_max != nullptr) {
            // The game's slider, moved to this row; the bar's length is the
            // value (278 at min .. 419 at max: the frame's inside).
            const float dy = y - slider_y;
            for (const gpu::DrawCall *piece : {slider_frame[0], slider_frame[1], slider_min, slider_max}) {
                const Rect r = extent(*piece);
                add(boxes, moved(*piece, r, Rect{r.x0, r.y0 + dy, r.x1, r.y1 + dy}));
            }
            if (row.fraction > 0.0f) {
                const Rect r = extent(*slider_bar);
                add(boxes, moved(*slider_bar, r, Rect{278.0f, r.y0 + dy, 278.0f + 141.0f * row.fraction, r.y1 + dy}));
            }
            if (static_cast<int>(i) == view.focus && arrow_left != nullptr && arrow_right != nullptr) {
                add(boxes, moved(*arrow_left, extent(*arrow_left), Rect{241.0f, y + 3.0f, 248.0f, y + 16.0f}));
                add(boxes, moved(*arrow_right, extent(*arrow_right), Rect{450.0f, y + 3.0f, 457.0f, y + 16.0f}));
            }
            continue;
        }
        const int boxes_n = row.stepper ? 1 : static_cast<int>(row.values.size());
        for (int k = 0; k < boxes_n; ++k) {
            const float bx0 = 250.0f + 66.0f * k, bx1 = row.stepper ? 448.0f : bx0 + 66.0f;
            const bool chosen = row.stepper || k == row.selected;
            for (const gpu::DrawCall *piece : value_box) {
                gpu::DrawCall *copy = moved(*piece, value_from, Rect{bx0, y, bx1, y + 19.0f});
                // The chosen box's blue body is filled (alpha 0xDC), the
                // others' clear; the frame (0x826E50) stays.
                for (gpu::Vertex &v : copy->vertices)
                    if ((v.color & 0x00FFFFFFu) != 0x00826E50u)
                        v.color = (v.color & 0x00FFFFFFu) | (chosen ? 0xDC000000u : 0u);
                add(boxes, copy);
            }
            const std::string &label = row.values[static_cast<std::size_t>(k)];
            const float w = text_width(label, row_style.scale);
            add(texts, text(row_style, label, std::round((bx0 + bx1 - w) * 0.5f), y + 4.0f));
        }
        if (static_cast<int>(i) == view.focus && arrow_left != nullptr && arrow_right != nullptr) {
            const float right_x = row.stepper ? 450.0f : 250.0f + 66.0f * boxes_n + 2.0f;
            if (!(row.stepper && row.first))
                add(boxes, moved(*arrow_left, extent(*arrow_left), Rect{241.0f, y + 3.0f, 248.0f, y + 16.0f}));
            if (!(row.stepper && row.last))
                add(boxes, moved(*arrow_right, extent(*arrow_right), Rect{right_x, y + 3.0f, right_x + 7.0f, y + 16.0f}));
        }
    }
    // The title and the description.
    {
        TextStyle style{title_font, 0xFFFFFFFFu, 0xFF202020u, 1.0f};
        ink(title_font, style.main, style.outline);
        add(texts, text(style, view.title, extent(*title_font).x0 + 1.0f, extent(*title_font).y0 + 1.0f));
    }
    const std::vector<std::string> lines = wrap(view.help, 286.0f, row_style.scale);
    for (std::size_t i = 0; i < lines.size() && i < 3u; ++i)
        add(texts, text(row_style, lines[i], 16.0f, 221.0f + 13.0f * static_cast<float>(i)));
    for (gpu::DrawCall *call : boxes) draws.push_back(call);
    for (gpu::DrawCall *call : texts) draws.push_back(call);
}

// --- "Settings" under "Options" in the game's menus (2026-11-09) ---------------
// Wherever a menu has an Options row (the academy menu, the battle's command
// menu, ...), a Settings row is drawn under it: the row's text is the game's
// font in the Options row's style, the rows below and the panel's bottom move
// down a row, the panel grows by a row (its pieces cut at the Options row's
// bottom, a row of its middle copied in). The cursor on Settings is
// vc2_settings_menu.cpp's (entry_*): the game's cursor stays on Options, so
// the highlight is moved down a row and the description replaced, and Cross
// there is the game's own Options with the Settings screen asked for.
// Layout found (PSP pixels): rows 20 apart, a row's band from 4 above its
// text; the highlight's frame is colour 0xFF6000.

// The text of one of the game's text draws (per character four outline
// sprites and the character), from the font's rectangles; "" when not one.
std::string decode_text(const gpu::DrawCall &call) {
    const auto *rects = font_rects();
    if (rects == nullptr || !is_font(call) || call.vertices.size() % 10u != 0u) return {};
    std::string s;
    for (std::size_t i = 8; i + 1 < call.vertices.size(); i += 10) {
        const gpu::Vertex &a = call.vertices[i], &b = call.vertices[i + 1];
        char c = '?';
        for (int k = 0; k < 96; ++k) {
            const auto &r = (*rects)[k];
            if (std::fabs(a.texcoord[0] - r[0]) < 1e-3f && std::fabs(a.texcoord[1] - r[1]) < 1e-3f &&
                std::fabs(b.texcoord[0] - r[2]) < 1e-3f && std::fabs(b.texcoord[1] - r[3]) < 1e-3f) {
                c = static_cast<char>(32 + k);
                break;
            }
        }
        s.push_back(c);
    }
    return s;
}

// A text draw's style: its ink and outline, the size of its characters, and
// where its first character is (screen).
TextStyle style_of(const gpu::DrawCall &call, float &x, float &y) {
    TextStyle style{&call};
    x = call.world[12], y = call.world[13];
    if (call.vertices.size() >= 10u) {
        const gpu::Vertex &a = call.vertices[8], &b = call.vertices[9];
        style.main = a.color, style.outline = call.vertices[0].color;
        const float texels = (b.texcoord[1] - a.texcoord[1]) * 128.0f;
        const float height = (b.position[1] - a.position[1]) * call.world[5];
        if (texels > 1.0f && height > 1.0f) style.scale = height / texels;
        x = a.position[0] * call.world[0] + call.world[12];
        y = a.position[1] * call.world[5] + call.world[13];
    }
    return style;
}

// A copy of the quad `from` (four vertices, its sides on the axes) cut to the
// screen rows c0..c1, its texture with it, and moved down by `dy`.
gpu::DrawCall *cut_quad(const gpu::DrawCall &from, float c0, float c1, float dy) {
    const Rect r = extent(from);
    if (from.vertices.size() != 4u || r.y1 - r.y0 < 1.0f) return nullptr;
    float v_top = 0.0f, v_bottom = 0.0f;
    bool top = false, bottom = false;
    for (const gpu::Vertex &v : from.vertices) {
        const float y = v.position[1] * from.world[5] + from.world[13];
        if (std::fabs(y - r.y0) < 0.01f) v_top = v.texcoord[1], top = true;
        else if (std::fabs(y - r.y1) < 0.01f) v_bottom = v.texcoord[1], bottom = true;
    }
    if (!top || !bottom) return nullptr;
    gpu::DrawCall &call = extra_draws().emplace_back(from);
    call.raw_vertices = nullptr, call.raw_count = 0u, call.bone_matrices = nullptr;
    for (gpu::Vertex &v : call.vertices) {
        const float x = v.position[0] * from.world[0] + from.world[12];
        const float y = v.position[1] * from.world[5] + from.world[13];
        const float to = y < (r.y0 + r.y1) * 0.5f ? c0 : c1;
        v.position[0] = x;
        v.position[1] = to + dy;
        v.texcoord[1] = v_top + (to - r.y0) / (r.y1 - r.y0) * (v_bottom - v_top);
    }
    call.world[0] = 1.0f, call.world[5] = 1.0f, call.world[12] = 0.0f, call.world[13] = 0.0f;
    return &call;
}

constexpr const char *kEntryLabel = "Settings";
constexpr const char *kEntryHelp = "Display, graphics and control settings.";

void settings_entry(std::vector<gpu::DrawCall *> &draws) {
    if (region() == Region::Japan || keep_install() || settings_menu::active()) return;
    // The Options row: a text draw reading "Options" (the font's table is
    // looked at only when there is such a draw).
    const gpu::DrawCall *options = nullptr;
    for (const gpu::DrawCall *call : draws)
        if (is_font(*call) && call->vertices.size() == 70u && call->world[14] > -1000.0f &&
            font_rects() != nullptr && decode_text(*call) == "Options") {
            options = call;
            break;
        }
    if (options == nullptr) return;
    // Its menu's rows: text draws in its column (origin x, depth), whole rows
    // apart, next to one another.
    const float ox = options->world[12], oy = options->world[13], oz = options->world[14];
    std::vector<const gpu::DrawCall *> column;
    for (const gpu::DrawCall *call : draws)
        if (call != options && is_font(*call) && near(call->world[12], ox, 0.6f) && near(call->world[14], oz, 1.5f))
            column.push_back(call);
    float spacing = 1e9f;
    for (const gpu::DrawCall *call : column) {
        const float d = std::fabs(call->world[13] - oy);
        if (d > 8.0f) spacing = std::min(spacing, d);
    }
    if (spacing < 14.0f || spacing > 30.0f) return;
    std::map<long, const gpu::DrawCall *> by_row{{0L, options}};
    for (const gpu::DrawCall *call : column) {
        const float k = (call->world[13] - oy) / spacing;
        const long row = std::lround(k);
        if (std::fabs(k - static_cast<float>(row)) < 0.05f) by_row.emplace(row, call);
    }
    long first = 0, last = 0;
    while (by_row.count(first - 1) != 0u) --first;
    while (by_row.count(last + 1) != 0u) ++last;
    const int rows = static_cast<int>(last - first + 1);
    if (rows < 2) return;
    // The panel: quads behind the rows that cover them all, and not much more.
    const float text_x = extent(*options).x0, top = oy + first * spacing, bottom = oy + last * spacing;
    std::vector<std::size_t> panel;
    Rect pr;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const gpu::DrawCall *call = draws[i];
        if (call->through || !call->texture.enabled || is_font(*call) || call->world[14] > oz - 5.0f) continue;
        const Rect r = extent(*call);
        if (r.x0 > text_x + 1.0f || r.x1 < text_x + 40.0f || r.x1 - r.x0 > 320.0f) continue;
        if (r.y0 > top - 1.0f || r.y1 < bottom + spacing - 6.0f || r.y1 - r.y0 > (rows + 1) * spacing) continue;
        if (call->vertices.size() != 4u) return;  // not a plain quad: the menu is left as it is
        panel.push_back(i);
        pr.x0 = std::min(pr.x0, r.x0), pr.x1 = std::max(pr.x1, r.x1);
        pr.y0 = std::min(pr.y0, r.y0), pr.y1 = std::max(pr.y1, r.y1);
    }
    if (panel.empty()) return;
    // A row's band: from `lead` above its text, `spacing` high.
    const float lead = std::clamp(top - pr.y0, 0.0f, spacing * 0.5f);
    const float cut = oy - lead + spacing;  // the bottom of the Options row
    const auto in_panel = [&](const Rect &r) {
        return r.x0 >= pr.x0 - 1.0f && r.x1 <= pr.x1 + 1.0f && r.y0 >= pr.y0 - 1.0f && r.y1 <= pr.y1 + 1.0f;
    };
    const auto is_panel = [&](std::size_t i) { return std::find(panel.begin(), panel.end(), i) != panel.end(); };
    // The game's cursor: the row of the highlight's frame.
    int focus = -1;
    for (std::size_t i = 0; i < draws.size() && focus < 0; ++i) {
        const gpu::DrawCall *call = draws[i];
        if (is_panel(i) || is_font(*call) || call->vertices.empty()) continue;
        if ((call->vertices.front().color & 0x00FFFFFFu) != 0x00FF6000u) continue;
        const Rect r = extent(*call);
        if (!in_panel(r) || !near(r.y1 - r.y0, spacing)) continue;
        const float k = (r.y0 + lead - oy) / spacing;
        const long row = std::lround(k);
        if (std::fabs(k - static_cast<float>(row)) < 0.1f && row >= first && row <= last)
            focus = static_cast<int>(row - first);
    }
    settings_menu::entry_frame(rows, static_cast<int>(-first), focus);
    const bool on = settings_menu::entry_on();
    {
        static std::uint32_t logged = 0u;
        static int last_rows = -1, last_focus = -2;
        if ((rows != last_rows || focus != last_focus) && ++logged <= 100u)
            std::cerr << "[vc2-settings-entry] menu of " << rows << " rows, Options row " << -first << ", cursor "
                      << focus << ", rows " << spacing << " apart, panel " << pr.x0 << ".." << pr.x1 << " x "
                      << pr.y0 << ".." << pr.y1 << "\n";
        last_rows = rows, last_focus = focus;
    }
    // The rows under Options (and whatever else in the panel) down a row; on
    // Settings the Options row's highlight too.
    const gpu::DrawCall *description = nullptr;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        gpu::DrawCall *call = draws[i];
        if (is_panel(i) || call->vertices.empty()) continue;
        const Rect r = extent(*call);
        if (in_panel(r)) {
            const bool below = r.y0 >= cut - 1.0f;
            const bool highlight = on && !is_font(*call) && near(r.y0, cut - spacing) && near(r.y1, cut);
            if (below || highlight) call->world[13] += spacing;
            continue;
        }
        // The description: the longest other text of the screen (not the 3D
        // scene's), outside the panel.
        if (on && is_font(*call) && call->world[14] > -1000.0f && call->vertices.size() >= 80u &&
            (description == nullptr || call->vertices.size() > description->vertices.size()))
            description = call;
    }
    // The panel, a row longer.
    std::vector<gpu::DrawCall *> out;
    out.reserve(draws.size() + panel.size() * 2u + 2u);
    for (std::size_t i = 0; i < draws.size(); ++i) {
        gpu::DrawCall *call = draws[i];
        if (is_panel(i)) {
            const Rect r = extent(*call);
            const float s0 = cut - spacing >= r.y0 + 6.0f ? cut - spacing : cut;
            gpu::DrawCall *upper = cut_quad(*call, r.y0, cut, 0.0f);
            gpu::DrawCall *filler = cut_quad(*call, s0, s0 + spacing, cut - s0);
            gpu::DrawCall *lower = cut_quad(*call, cut, r.y1, spacing);
            if (upper == nullptr || filler == nullptr || lower == nullptr) {
                out.push_back(call);
                continue;
            }
            out.push_back(upper);
            out.push_back(filler);
            out.push_back(lower);
            continue;
        }
        if (call == description) {
            float x = 0.0f, y = 0.0f;
            const TextStyle style = style_of(*call, x, y);
            if (gpu::DrawCall *help = text(style, kEntryHelp, x, y)) {
                out.push_back(help);
                continue;
            }
        }
        out.push_back(call);
        if (call == options) {
            float x = 0.0f, y = 0.0f;
            const TextStyle style = style_of(*call, x, y);
            if (gpu::DrawCall *label = text(style, kEntryLabel, x, y + spacing)) out.push_back(label);
        }
    }
    draws = std::move(out);
}

// --- Tab badges: the keys (2026-11-09) ------------------------------------------
// Screens with tabs (Options, Settings, the briefing) show L and R badges
// beside them: a dark plate and a letter, 12 x 12 each, the letters' cells
// under the plates' in their sheet, L's cell left of R's. With the keyboard's
// prompts the letters become the keys that switch tabs (kbm_prompt: 1 and 2),
// in the game's font.
void tab_key_badges(std::vector<gpu::DrawCall *> &draws) {
    if (!keyboard_prompts()) return;
    KbmPrompt left_key, right_key;
    if (!kbm_prompt(0x0100u, left_key) || !kbm_prompt(0x0200u, right_key) || left_key.cap == nullptr ||
        right_key.cap == nullptr)
        return;
    struct Cell {
        std::size_t index;
        Rect r;
        float u0, v0, u1, v1;
    };
    const gpu::DrawCall *font = nullptr;
    std::vector<Cell> cells;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const gpu::DrawCall *call = draws[i];
        if (font == nullptr && is_font(*call)) font = call;
        if (call->through || !call->texture.enabled || call->vertices.size() != 4u || is_font(*call)) continue;
        const Rect r = extent(*call);
        if (!near(r.x1 - r.x0, 12.0f, 0.6f) || !near(r.y1 - r.y0, 12.0f, 0.6f)) continue;
        Cell c{i, r, 1e9f, 1e9f, -1e9f, -1e9f};
        for (const gpu::Vertex &v : call->vertices) {
            c.u0 = std::min(c.u0, v.texcoord[0]), c.u1 = std::max(c.u1, v.texcoord[0]);
            c.v0 = std::min(c.v0, v.texcoord[1]), c.v1 = std::max(c.v1, v.texcoord[1]);
        }
        if (!near((c.u1 - c.u0) * call->texture.width, 12.0f, 0.5f) ||
            !near((c.v1 - c.v0) * call->texture.height, 12.0f, 0.5f))
            continue;
        cells.push_back(c);
    }
    if (font == nullptr || cells.size() < 4u || font_rects() == nullptr) return;
    const auto same_place = [](const Cell &a, const Cell &b) {
        return near(a.r.x0, b.r.x0, 0.3f) && near(a.r.y0, b.r.y0, 0.3f);
    };
    const auto tex = [&](const Cell &c) { return draws[c.index]->texture.address; };
    // A letter: a cell with a plate at its place, the plate's cell just above
    // its own in the sheet.
    const auto plate_of = [&](const Cell &letter) -> const Cell * {
        for (const Cell &c : cells)
            if (&c != &letter && tex(c) == tex(letter) && same_place(c, letter) && near(c.u0, letter.u0, 1e-3f) &&
                near(c.v1, letter.v0, 1e-3f))
                return &c;
        return nullptr;
    };
    std::vector<std::pair<std::size_t, gpu::DrawCall *>> replace;
    for (const Cell &l : cells) {
        const Cell *l_plate = plate_of(l);
        if (l_plate == nullptr) continue;
        for (const Cell &r : cells) {
            if (&r == &l || tex(r) != tex(l) || !near(r.r.y0, l.r.y0, 0.3f) || r.r.x0 <= l.r.x0 + 12.0f) continue;
            if (!near(r.u0, l.u1, 1e-3f) || !near(r.v0, l.v0, 1e-3f)) continue;
            const Cell *r_plate = plate_of(r);
            if (r_plate == nullptr) continue;
            for (const auto &[cell, plate, key] : {std::tuple{&l, l_plate, left_key.cap}, std::tuple{&r, r_plate, right_key.cap}}) {
                const gpu::DrawCall *letter = draws[cell->index];
                TextStyle style{font, letter->vertices.front().color, draws[plate->index]->vertices.front().color,
                                0.6875f};
                const float w = text_width(key, style.scale);
                gpu::DrawCall *drawn = text(style, key, std::round((cell->r.x0 + cell->r.x1 - w) * 0.5f),
                                            cell->r.y0 + 0.5f);
                if (drawn == nullptr) continue;
                drawn->world[14] = letter->world[14];
                replace.emplace_back(cell->index, drawn);
            }
        }
    }
    for (const auto &[index, drawn] : replace) draws[index] = drawn;
    static std::uint32_t logged = 0u;
    if (!replace.empty() && ++logged <= 20u)
        std::cerr << "[vc2-tab-keys] tab badges show " << left_key.cap << " and " << right_key.cap << "\n";
}

// --- The saving indicator (2026-11-11) -----------------------------------------
// The PSP lit its Memory Stick access LED while saving, and the game's
// messages tell the player to watch it. Here, while the game writes a save
// (note_save_write, from the save call) and for a moment after, "Saving" and a
// turning ring of dots show at the lower right of the screen, in the game's
// font, as part of its interface (place_frame keeps them by that edge on a
// wide screen). The dots are the font sheet's own solid pixel (the inside of
// the broken bar's stroke), stretched; no texture of ours.
std::atomic<std::int64_t> save_write_ms{-1000000};
constexpr std::int64_t kSaveShownMs = 2000, kSaveFadeMs = 400;

void save_indicator(std::vector<gpu::DrawCall *> &draws) {
    const std::int64_t age = steady_ms() - save_write_ms.load(std::memory_order_relaxed);
    if (age < 0 || age > kSaveShownMs) return;
    const gpu::DrawCall *font = nullptr;
    for (const gpu::DrawCall *call : draws)
        if (is_font(*call) && call->world[14] > -1000.0f) font = call;  // the last: drawn on top
    const auto *rects = font_rects();
    if (font == nullptr || rects == nullptr) return;
    const float fade = age > kSaveShownMs - kSaveFadeMs
                           ? static_cast<float>(kSaveShownMs - age) / static_cast<float>(kSaveFadeMs)
                           : std::min(1.0f, static_cast<float>(age) / 150.0f);
    const auto alpha = [&](float a) { return static_cast<std::uint32_t>(std::lround(std::clamp(a * fade, 0.0f, 1.0f) * 255.0f)) << 24; };
    // "Saving", right-aligned against the ring.
    const float cx = 461.0f, cy = 255.0f, ring = 6.5f;
    TextStyle style{font, alpha(1.0f) | 0x00FFFFFFu, alpha(1.0f) | 0x00202020u, 0.75f};
    const std::string label = "Saving";
    if (gpu::DrawCall *drawn = text(style, label, std::round(cx - ring - 6.0f - text_width(label, 0.75f)), cy - 6.0f))
        draws.push_back(drawn);
    // The ring: eight dots, the brightest going round once a second.
    const auto &bar = (*rects)['|' - 32];
    const float u = bar[0] + 3.0f / 256.0f, v = bar[1] + 4.0f / 128.0f;  // inside the bar's top stroke
    const std::array<float, 4> solid{u - 0.4f / 256.0f, v - 0.4f / 128.0f, u + 0.4f / 256.0f, v + 0.4f / 128.0f};
    gpu::DrawCall &call = extra_draws().emplace_back(*font);
    call.raw_vertices = nullptr, call.raw_count = 0u, call.bone_matrices = nullptr;
    call.vertices.clear(), call.indices.clear();
    call.world[0] = 1.0f, call.world[5] = 1.0f, call.world[12] = 0.0f, call.world[13] = 0.0f;
    const float lead = static_cast<float>(age % 1000) / 125.0f;  // 0..8
    for (int i = 0; i < 8; ++i) {
        const float angle = static_cast<float>(i) * 0.7853982f - 1.5707963f;
        const float x = cx + std::cos(angle) * ring, y = cy + std::sin(angle) * ring;
        const float behind = std::fmod(lead - static_cast<float>(i) + 8.0f, 8.0f);  // 0: the lead dot
        const float bright = 0.25f + 0.75f * std::max(0.0f, 1.0f - behind / 5.0f);
        for (int pass = 0; pass < 2; ++pass) {  // a dark edge, then the dot
            const float r = pass == 0 ? 2.0f : 1.4f;
            const std::uint32_t colour = pass == 0 ? alpha(bright) | 0x00202020u : alpha(bright) | 0x00FFFFFFu;
            gpu::Vertex a{}, b{};
            a.position = {x - r, y - r, 0.0f, solid[3]};
            b.position = {x + r, y + r, 0.0f, solid[3]};
            a.texcoord = {solid[0], solid[1]};
            b.texcoord = {solid[2], solid[3]};
            a.normal = b.normal = {solid[0], solid[1], solid[2]};
            a.color = b.color = colour;
            call.vertices.push_back(a);
            call.vertices.push_back(b);
        }
    }
    call.primitive_count = static_cast<std::uint32_t>(call.vertices.size() / 2u);
    draws.push_back(&call);
    static std::uint32_t logged = 0u;
    if (age < 100 && ++logged <= 20u) std::cerr << "[vc2-save] saving indicator shown\n";
}

}  // namespace

void note_save_write() { save_write_ms.store(steady_ms(), std::memory_order_relaxed); }

// The video.* settings, applied now (the in-game Settings screen).
#if defined(PORTABLEKIT_HAS_RENDERER)
#define VC2_RENDERER(call)                                                  \
    do {                                                                    \
        if (gpu::VulkanRenderer *renderer = ensure_renderer()) renderer->call; \
    } while (0)
#else
#define VC2_RENDERER(call) \
    do {                   \
    } while (0)
#endif
bool apply_video_setting(const std::string &key, const std::string &value) {
    settings::Settings &s = settings::current();
    if (key == "video.display_mode") {
        const settings::DisplayMode mode = value == "borderless"   ? settings::DisplayMode::Borderless
                                           : value == "fullscreen" ? settings::DisplayMode::Fullscreen
                                                                   : settings::DisplayMode::Window;
        settings::set_display_mode(s, mode);
        VC2_RENDERER(set_display_mode(mode));
        return true;
    }
    if (key == "video.resolution") {
        settings::Resolution resolution{};
        if (!settings::parse_resolution(value, resolution)) return false;
        s.resolution = resolution;
        VC2_RENDERER(set_resolution(resolution));
        return true;
    }
    if (key == "video.aspect") {
        // "original" (the PSP's), "stretch", "fill" (the window's shape) or a
        // shape such as "16:10" (settings::parse_aspect).
        if (!settings::parse_aspect(value, s.aspect, s.shape_width, s.shape_height)) return false;
        VC2_RENDERER(set_aspect(s.aspect));
        return true;
    }
    if (key == "video.present_mode") {
        s.present_mode = value == "mailbox"     ? settings::PresentMode::Mailbox
                         : value == "immediate" ? settings::PresentMode::Immediate
                                                : settings::PresentMode::Fifo;
        VC2_RENDERER(set_present_mode(s.present_mode));
        return true;
    }
    if (key == "video.sharp_textures") {
        s.sharp_textures = value == "1";
        VC2_RENDERER(set_sharp_textures(s.sharp_textures));
        return true;
    }
    if (key == "video.sharp_screen") {
        s.sharp_screen = value == "1";
        VC2_RENDERER(set_sharp_screen(s.sharp_screen));
        return true;
    }
    if (key == "video.texture_pack") {
        s.texture_pack = value == "1";
        VC2_RENDERER(set_texture_pack(s.texture_pack));
        return true;
    }
    if (key == "video.performance") {
        s.perf = value == "overlay" ? settings::PerfDisplay::Overlay
                 : value == "fps"   ? settings::PerfDisplay::Fps
                                    : settings::PerfDisplay::Off;
        VC2_RENDERER(set_perf_overlay(s.perf != settings::PerfDisplay::Off));
        return true;
    }
    return false;
}
#undef VC2_RENDERER

void run_native_list(Runtime &rt, ExecList &list) {
    const psprecomp::GuestMemory &memory = rt.memory();
#if defined(PORTABLEKIT_HAS_RENDERER)
    gpu::VulkanRenderer *renderer = ensure_renderer();
    if (renderer != nullptr && !renderer->available()) renderer = nullptr;
    if (renderer != nullptr) renderer->begin_display_list();
#endif
    auto &stats = executor().stats;
    // The run's draws are gathered and placed together (widescreen grouping,
    // place_frame), then handed to the renderer in their order. Nothing runs
    // in between, so they read the same vertices and textures they would one
    // at a time.
    std::vector<gpu::DrawCall *> draws;
    std::uint32_t previous = 0u;
    for (std::uint32_t steps = 0; steps < (1u << 22u) && !list.done; ++steps) {
        if (list.stall != 0u && list.pc == list.stall) break;
        if (!memory.contains(list.pc, 4u)) {
            note_unsupported("list outside memory", list.pc);
            list.done = true;
            break;
        }
        const std::uint32_t at = list.pc;
        const std::uint32_t word = memory.load32(at);
        list.pc += 4u;
        const std::uint32_t command = word >> 24u, data = word & 0x00FFFFFFu;
        const std::uint32_t before = previous;
        previous = word;
        const auto target = [&] { return (list.offset + (list.base | data)) & 0x0FFFFFFCu; };
        switch (command) {
        case 0x00u:  // NOP, or a native draw's marker
            if (data == 0u) break;
            if (gpu::DrawCall *call = take_native_draw(data)) {
                ++stats.draws;
                draws.push_back(call);
            }
            break;
        case 0x10u: list.base = (data & 0x000F0000u) << 8u; break;
        case 0x13u: list.offset = data << 8u; break;
        case 0x08u: list.pc = target(); break;
        case 0x0Au:
            list.returns.push_back(list.pc);
            list.pc = target();
            break;
        case 0x0Bu:
            if (!list.returns.empty()) {
                list.pc = list.returns.back();
                list.returns.pop_back();
            }
            break;
        case 0x0Eu: {  // SIGNAL: 0x01-0x03 interrupt the CPU; its END follows
            const std::uint32_t behaviour = data >> 16u;
            if (behaviour >= 0x10u) {
                note_unsupported("SIGNAL with list flow " + hex8(word), at);
                break;
            }
            ++stats.signals;
            raise_list_interrupt(list, false, data, list.pc);
            break;
        }
        case 0x0Fu:  // FINISH: the finish callback; its END ends the list
            ++stats.finishes;
            raise_list_interrupt(list, true, data, list.pc);
            break;
        case 0x0Cu: {  // END: closes an interrupting SIGNAL, otherwise ends the list
            const std::uint32_t behaviour = (before >> 16u) & 0xFFu;
            if ((before >> 24u) == 0x0Eu && (behaviour == 0x01u || behaviour == 0x02u)) break;
            list.done = true;
            break;
        }
        case 0x04u: note_unsupported("PRIM", at); break;
        case 0x05u: note_unsupported("BEZIER", at); break;
        case 0x06u: note_unsupported("SPLINE", at); break;
        case 0xEAu: note_unsupported("TRANSFER START", at); break;
        default: ++stats.register_words; break;  // state the native draws already carry
        }
    }
    extra_draws().clear();  // the last list's copies, submitted by now
    title_menu_settings(draws);
    settings_screen(draws);
    settings_entry(draws);
    tab_key_badges(draws);
    save_indicator(draws);
    place_frame(draws);
#if defined(PORTABLEKIT_HAS_RENDERER)
    if (renderer != nullptr)
        for (const gpu::DrawCall *call : draws) renderer->submit(*call, memory);
#endif
}

void register_native_executor(HleRegistrar &hle) {
    if (!native_executor_enabled()) return;
    hle.bind("sceGe_user", 0xA4FC06A4u, [](Runtime &rt, AllegrexContext &ctx) {  // sceGeSetCallback
        auto &e = executor();
        const std::uint32_t data = ctx.gpr[4];
        auto &memory = rt.memory();
        const std::int32_t id = e.next_callback++;
        if (memory.contains(data, 16u))
            e.callbacks[id] = ExecCallback{memory.load32(data), memory.load32(data + 4u), memory.load32(data + 8u),
                                           memory.load32(data + 12u)};
        kernel().finish(ctx, static_cast<std::uint32_t>(id));
    });
    hle.bind("sceGe_user", 0x05DB22CEu, [](Runtime &, AllegrexContext &ctx) {  // sceGeUnsetCallback
        executor().callbacks.erase(static_cast<std::int32_t>(ctx.gpr[4]));
        kernel().finish(ctx, 0u);
    });
    const auto enqueue = [](Runtime &rt, AllegrexContext &ctx) {  // sceGeListEnQueue(Head)
        auto &e = executor();
        const std::uint32_t id = e.next_list++;
        ExecList &list = e.lists[id];
        list.pc = ctx.gpr[4] & 0x0FFFFFFFu;
        list.stall = ctx.gpr[5] & 0x0FFFFFFFu;
        list.callback = static_cast<std::int32_t>(ctx.gpr[6]);
        ++e.stats.lists;
        run_native_list(rt, list);
        kernel().finish(ctx, id);
    };
    hle.bind("sceGe_user", 0xAB49E76Au, enqueue);
    hle.bind("sceGe_user", 0x1C0D95A6u, enqueue);
    hle.bind("sceGe_user", 0xE0D68148u, [](Runtime &rt, AllegrexContext &ctx) {  // sceGeListUpdateStallAddr
        auto &e = executor();
        const auto found = e.lists.find(ctx.gpr[4]);
        if (found == e.lists.end()) {
            kernel().finish(ctx, error::kIllegalArgument);
            return;
        }
        found->second.stall = ctx.gpr[5] & 0x0FFFFFFFu;
        run_native_list(rt, found->second);
        kernel().finish(ctx, 0u);
    });
    hle.bind("sceGe_user", 0x03444EB4u, [](Runtime &, AllegrexContext &ctx) {  // sceGeListSync: 0 done, 2 drawing
        const auto &lists = executor().lists;
        const auto found = lists.find(ctx.gpr[4]);
        kernel().finish(ctx, found == lists.end() || found->second.done ? 0u : 2u);
    });
    hle.bind("sceGe_user", 0xB287BD61u, [](Runtime &, AllegrexContext &ctx) {  // sceGeDrawSync
        std::erase_if(executor().lists, [](const auto &item) { return item.second.done; });
        kernel().finish(ctx, 0u);
    });
    std::cerr << "[vc2-native-exec] display lists run by the VC2 profile; the GE is not used\n";
}

void report_native_executor(std::uint64_t flips) {
    if (!native_executor_enabled() || flips % 600u != 0u) return;
    const auto &stats = executor().stats;
    std::cerr << "[vc2-native-exec] by flip " << flips << ": lists " << stats.lists << ", native draws "
              << stats.draws << ", signals " << stats.signals << ", finishes " << stats.finishes
              << ", register words ignored " << stats.register_words << ", unsupported commands ";
    std::uint64_t total = 0u;
    for (const auto &[name, count] : stats.unsupported) total += count;
    std::cerr << total;
    for (const auto &[name, count] : stats.unsupported) std::cerr << " [" << name << "]=" << count;
    std::cerr << "\n";
}

// --- List census (VC2_LIST_CENSUS=1) ----------------------------------------------
// Phase 2 step 3 groundwork: what the game's display lists still carry, and how
// it drives the GE. Every list is walked as the GE walks it (BASE, JUMP, CALL,
// RET, END after FINISH), from where it is queued up to each new stall address,
// and every command is counted: markers, signals by value, framing, anything
// else. The GE services the game calls are counted by caller. Reported every
// 600 flips; the first three lists after flip 1200 are printed whole (runs of
// the same command collapsed).

bool list_census_enabled() {
    static const bool enabled = env("LIST_CENSUS") != nullptr;
    return enabled;
}

struct CensusList {
    std::uint32_t pc = 0u, base = 0u;
    std::vector<std::uint32_t> returns;
    bool ended = false, after_signal = false, sample = false;
    std::vector<std::pair<std::string, std::uint32_t>> sequence;  // sampled lists only
};

struct Census {
    std::unordered_map<std::uint32_t, CensusList> lists;
    std::map<std::string, std::uint64_t> commands;
    std::map<std::string, std::uint64_t> calls;
    std::uint64_t flips = 0u, lists_seen = 0u;
    int samples_left = 3;
};
Census &census() {
    static Census c;
    return c;
}

std::string census_name(std::uint32_t word) {
    const std::uint32_t command = word >> 24u, data = word & 0x00FFFFFFu;
    switch (command) {
    case 0x00u: return data == 0u ? "NOP" : (data & kMarkerTag) != 0u ? "native marker" : "NOP with data " + hex8(data);
    case 0x01u: return "VADDR";
    case 0x02u: return "IADDR";
    case 0x04u: return "PRIM";
    case 0x08u: return "JUMP";
    case 0x0Au: return "CALL";
    case 0x0Bu: return "RET";
    case 0x0Cu: return "END";
    case 0x0Eu: return "SIGNAL " + hex8(data);
    case 0x0Fu: return "FINISH " + hex8(data);
    case 0x10u: return "BASE";
    case 0x12u: return "VTYPE";
    case 0xD3u: return "CLEARMODE";
    case 0xEAu: return "TRANSFER START";
    default: {
        char text[16];
        std::snprintf(text, sizeof(text), "cmd %02X", command);
        return text;
    }
    }
}

void census_walk(const psprecomp::GuestMemory &memory, CensusList &list, std::uint32_t stall) {
    auto &c = census();
    for (std::uint32_t steps = 0; steps < (1u << 20u) && !list.ended; ++steps) {
        if (stall != 0u && list.pc == stall) return;
        if (!memory.contains(list.pc, 4u)) {
            ++c.commands["(list outside memory)"];
            list.ended = true;
            return;
        }
        const std::uint32_t word = memory.load32(list.pc);
        list.pc += 4u;
        const std::uint32_t command = word >> 24u, data = word & 0x00FFFFFFu;
        std::string name = census_name(word);
        if (command == 0x0Cu && list.after_signal) name = "END (closing a SIGNAL)";
        ++c.commands[name];
        if (list.sample) {
            if (!list.sequence.empty() && list.sequence.back().first == name) ++list.sequence.back().second;
            else list.sequence.emplace_back(name, 1u);
        }
        list.after_signal = command == 0x0Eu;
        switch (command) {
        case 0x10u: list.base = (data << 8u) & 0x0F000000u; break;
        case 0x08u: list.pc = (list.base | data) & 0x0FFFFFFCu; break;
        case 0x0Au:
            list.returns.push_back(list.pc);
            list.pc = (list.base | data) & 0x0FFFFFFCu;
            break;
        case 0x0Bu:
            if (!list.returns.empty()) {
                list.pc = list.returns.back();
                list.returns.pop_back();
            }
            break;
        case 0x0Cu:
            if (name == "END") list.ended = true;
            break;
        default: break;
        }
    }
    if (list.ended && list.sample) {
        std::ostringstream line;
        line << "[vc2-list-census] sample list:";
        for (const auto &[name, count] : list.sequence) line << " " << name << (count > 1u ? " x" + std::to_string(count) : "");
        std::cerr << line.str() << "\n";
        list.sample = false;
        list.sequence.clear();
    }
}

void census_call(const char *what, std::uint32_t caller) { ++census().calls[std::string(what) + " from " + hex8(caller)]; }

void register_list_census(HleRegistrar &hle) {
    if (!list_census_enabled()) return;
    int bound = 0;
    if (HleFunction inner = hle.bound_function("sceGe_user", 0xAB49E76Au)) {
        ++bound;
        hle.bind("sceGe_user", 0xAB49E76Au, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t pc = ctx.gpr[4] & 0x0FFFFFFFu, stall = ctx.gpr[5] & 0x0FFFFFFFu;
            census_call("sceGeListEnQueue", ctx.gpr[31]);
            inner(rt, ctx);
            auto &c = census();
            CensusList &list = c.lists[ctx.gpr[2]];
            list = CensusList{};
            list.pc = pc;
            ++c.lists_seen;
            if (c.flips >= 1200u && c.samples_left > 0) {
                --c.samples_left;
                list.sample = true;
            }
            census_walk(rt.memory(), list, stall);
        });
    }
    if (HleFunction inner = hle.bound_function("sceGe_user", 0x1C0D95A6u)) {
        ++bound;
        hle.bind("sceGe_user", 0x1C0D95A6u, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            census_call("sceGeListEnQueueHead", ctx.gpr[31]);
            inner(rt, ctx);
        });
    }
    if (HleFunction inner = hle.bound_function("sceGe_user", 0xE0D68148u)) {
        ++bound;
        hle.bind("sceGe_user", 0xE0D68148u, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t id = ctx.gpr[4], stall = ctx.gpr[5] & 0x0FFFFFFFu;
            census_call("sceGeListUpdateStallAddr", ctx.gpr[31]);
            inner(rt, ctx);
            auto &c = census();
            if (auto found = c.lists.find(id); found != c.lists.end()) census_walk(rt.memory(), found->second, stall);
            else ++c.commands["(stall update for an unknown list)"];
        });
    }
    if (HleFunction inner = hle.bound_function("sceGe_user", 0x03444EB4u)) {
        ++bound;
        hle.bind("sceGe_user", 0x03444EB4u, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            census_call(ctx.gpr[5] == 0u ? "sceGeListSync (wait)" : "sceGeListSync (peek)", ctx.gpr[31]);
            inner(rt, ctx);
        });
    }
    if (HleFunction inner = hle.bound_function("sceGe_user", 0xB287BD61u)) {
        ++bound;
        hle.bind("sceGe_user", 0xB287BD61u, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            census_call(ctx.gpr[4] == 0u ? "sceGeDrawSync (wait)" : "sceGeDrawSync (peek)", ctx.gpr[31]);
            inner(rt, ctx);
        });
    }
    if (HleFunction inner = hle.bound_function("sceGe_user", 0xA4FC06A4u)) {
        ++bound;
        hle.bind("sceGe_user", 0xA4FC06A4u, [inner = std::move(inner)](Runtime &rt, AllegrexContext &ctx) mutable {
            const std::uint32_t data = ctx.gpr[4];
            if (rt.memory().contains(data, 16u))
                std::cerr << "[vc2-list-census] GE callbacks set from " << hex8(ctx.gpr[31]) << ": signal "
                          << hex8(rt.memory().load32(data)) << " (arg " << hex8(rt.memory().load32(data + 4u))
                          << "), finish " << hex8(rt.memory().load32(data + 8u)) << " (arg "
                          << hex8(rt.memory().load32(data + 12u)) << ")\n";
            inner(rt, ctx);
        });
    }
    std::cerr << "[vc2-list-census] watching " << bound << " of 6 GE services\n";
}

void report_list_census(std::uint64_t flips) {
    auto &c = census();
    c.flips = flips;
    if (!list_census_enabled() || flips % 600u != 0u) return;
    std::cerr << "[vc2-list-census] by flip " << flips << ": " << c.lists_seen << " lists; commands:";
    for (const auto &[name, count] : c.commands) std::cerr << " [" << name << "]=" << count;
    std::cerr << "\n[vc2-list-census] by flip " << flips << ": GE services:";
    for (const auto &[name, count] : c.calls) std::cerr << " [" << name << "]=" << count;
    std::cerr << "\n";
    // Lists that ended are not needed again (ids are not reused).
    std::erase_if(c.lists, [](const auto &item) { return item.second.ended; });
}

// Whether a movie is playing (vc2.backgrounds): sceMpegAvcDecode was called
// within the last ten frames.
void register_movie_watch(HleRegistrar &hle) {
    if (HleFunction decode = hle.bound_function("sceMpeg", 0x0E3C2E9Du)) {
        hle.bind("sceMpeg", 0x0E3C2E9Du, [inner = std::move(decode)](Runtime &rt, AllegrexContext &ctx) mutable {
            movie_until_frame() = wide().frame + 10u;
            inner(rt, ctx);
        });
    }
}

void register_native_gu_report(HleRegistrar &hle) {
    register_movie_watch(hle);
    register_movie_trace(hle);
    register_native_executor(hle);
    register_list_census(hle);
#if !defined(VC2_NATIVE_ONLY)
    if (env("NATIVE_GU") == nullptr) return;
#endif
    HleFunction set_frame_buf = hle.bound_function("sceDisplay", 0x289D82FEu);
    if (!set_frame_buf) return;
    hle.bind("sceDisplay", 0x289D82FEu, [inner = std::move(set_frame_buf), flips = std::uint64_t{0}](
                                             Runtime &rt, AllegrexContext &ctx) mutable {
        note_shown_framebuffer(ctx.gpr[4]);
        {
            auto &w = wide();
            if (scene_trace_enabled()) trace_scene_frame();
            watch_frame(w.frame);
            w.frame = flips + 1u;
            w.view_last_frame = w.view_this_frame;
            w.view_this_frame = false;
            w.backdrop_last_frame = w.backdrop_this_frame;
            w.backdrop_this_frame = false;
            w.view_extent = Extent{};
            w.trace_frame = wide_trace_enabled() && (w.sx < 0.9999f || w.sy < 0.9999f) && w.frame % 300u == 0u;
            w.traced = 0u;
            if (w.trace_frame) std::cerr << "[vc2-wide-trace] frame " << w.frame << " (x " << w.sx << ", y " << w.sy << ")\n";
        }
        report_list_census(flips + 1u);
        report_native_executor(flips + 1u);
        report_camera_trace(flips + 1u);
        if (++flips % 600u == 0u) {
            std::cerr << "[vc2-native-gu] by flip " << flips << ":";
            for (std::size_t i = 0; i < kEntryCount; ++i)
                if (call_counts()[i] != 0u) std::cerr << " " << kEntries[i].name << "=" << call_counts()[i];
            std::cerr << "\n";
            if (native_draw_enabled()) {
                const auto &stats = native_stats();
                std::cerr << "[vc2-native-draw] by flip " << flips << ": built " << stats.built << ", drawn "
                          << stats.submitted << ", not drawable " << stats.skipped << ", lost " << stats.stale
                          << "; clears and blits with host-built vertices " << stats.host_vertices
                          << ", draws continuing their vertex pointer " << stats.continued_pointer << "\n";
                {
                    const auto &e = edge_stats();
                    std::cerr << "[vc2-sprite-edges] by flip " << flips << ": sprites clamped " << e.clamped << " ("
                              << e.nearest << " drawn with nearest texels); skipped:";
                    for (const auto &[why, count] : e.skipped) std::cerr << " [" << why << "]=" << count;
                    std::cerr << "\n";
                }
                const auto &w = wide();
                if (w.camera_widened != 0u)
                    std::cerr << "[vc2-camera] by flip " << flips << ": perspective built widened " << w.camera_widened
                              << ", handed to setMatrix and restored for drawing " << w.camera_restored
                              << "; on-screen tests widened " << w.screen_tests_widened << "\n";
                if (w.sx < 0.9999f || w.sy < 0.9999f)
                    std::cerr << "[vc2-wide] by flip " << flips << ": x " << w.sx << ", y " << w.sy
                              << "; 3D view " << w.view << ", interface draws kept left " << w.left << ", right " << w.right
                              << ", centred " << w.centred << ", panels " << w.panels << ", stretched " << w.stretched
                              << ", pictures " << w.pillarboxed << ", sampling VRAM "
                              << w.sampled_vram << ", extent unknown " << w.unknown << "\n";
            }
            if (native_lists_enabled())
                std::cerr << "[vc2-native-lists] by flip " << flips << ": state words not written "
                          << list_stats().words_not_written << ", primitives the GE drew itself "
                          << list_stats().ge_draws << "\n";
            if (native_check_enabled()) {
                const auto &stats = check_stats();
                std::cerr << "[vc2-native-check] by flip " << flips << ": compared " << stats.compared << ", identical "
                          << stats.matched << ", GE-only draws " << stats.unmatched_ge << ", never reached "
                          << stats.dropped << ";";
                for (const auto &[name, count] : stats.mismatches) std::cerr << " " << name << "=" << count;
                std::cerr << "\n";
            }
        }
        inner(rt, ctx);
    });
}

}  // namespace portablekit::vc2

// The title menu's select (EU 0x08B0C87C, cmake site): the cursor no longer
// skips the old INSTALL slot, which is SETTINGS now. Kept as a hook (patched
// generated code stays patched), passing the index through.
extern "C" std::uint32_t vc2_title_list_select(std::uint32_t list, std::uint32_t index) noexcept {
    (void)list;
    return index;
}

// The title menu's choice (its handler EU 0x08B81A60, a1 = the entry): the
// old INSTALL slot (3) is SETTINGS, which runs OPTIONS (2) with the Settings
// screen asked for.
extern "C" std::uint32_t vc2_title_choice(std::uint32_t index) noexcept {
    using namespace portablekit::vc2;
    if (index != 3u || keep_install() || region() == Region::Japan) return index;
    if (steady_ms() - title_menu_seen_ms.load(std::memory_order_relaxed) > 2000) return index;
    settings_menu::request();
    return 2u;
}
