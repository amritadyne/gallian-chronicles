#pragma once

#include "psprecomp/guest_memory.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

// Graphics Engine state machine: turns a PSP display list into draw calls with
// decoded vertices, independent of any rendering backend.
namespace portablekit::gpu {

using psprecomp::GuestMemory;

enum class PrimitiveType : std::uint8_t {
    Points = 0,
    Lines = 1,
    LineStrip = 2,
    Triangles = 3,
    TriangleStrip = 4,
    TriangleFan = 5,
    Sprites = 6,
};

enum class TextureFormat : std::uint8_t {
    Rgba5650 = 0,
    Rgba5551 = 1,
    Rgba4444 = 2,
    Rgba8888 = 3,
    Clut4 = 4,
    Clut8 = 5,
    Clut16 = 6,
    Clut32 = 7,
    Dxt1 = 8,
    Dxt3 = 9,
    Dxt5 = 10,
};

// One decoded vertex in the format the backend consumes.
struct Vertex {
    std::array<float, 4> position{0.0f, 0.0f, 0.0f, 1.0f};  // object or screen space
    std::array<float, 3> normal{};
    std::array<float, 2> texcoord{};
    std::uint32_t color{0xFFFFFFFFu};
};

struct TextureState {
    bool enabled{};
    std::uint32_t address{};
    std::uint32_t buffer_width{};
    std::uint16_t width{};
    std::uint16_t height{};
    TextureFormat format{TextureFormat::Rgba5650};
    bool swizzled{};
    std::uint32_t clut_address{};
    std::uint32_t clut_format{};
    std::uint32_t clut_shift{};
    std::uint32_t clut_mask{};
    std::uint32_t clut_offset{};
    // For texture packs, which hash the palette as it was loaded: the CLUT
    // format command word as the GE received it (command byte included), the
    // bytes the last CLUT load read, and the most any load has read.
    std::uint32_t clut_format_word{0xC5000000u};
    std::uint32_t clut_load_bytes{};
    std::uint32_t clut_max_bytes{};
    std::uint32_t function{};        // TFX: modulate/decal/blend/replace/add
    bool alpha_from_texture{};       // TCC
    std::uint32_t min_filter{};
    std::uint32_t mag_filter{};
    std::uint32_t wrap_s{};
    std::uint32_t wrap_t{};
    // Texture coordinate scale and offset (UV transform).
    float scale_u{1.0f};
    float scale_v{1.0f};
    float offset_u{};
    float offset_v{};
};

struct RenderTarget {
    std::uint32_t color_address{};
    std::uint32_t color_stride{512u};
    std::uint32_t color_format{};  // 0:5650 1:5551 2:4444 3:8888
    std::uint32_t depth_address{};
    std::uint32_t depth_stride{512u};
};

struct BlendState {
    bool enabled{};
    std::uint32_t source_factor{};
    std::uint32_t destination_factor{};
    std::uint32_t equation{};
    std::uint32_t fixed_source{};
    std::uint32_t fixed_destination{};
};

struct DepthState {
    bool test_enabled{};
    bool write_enabled{true};
    std::uint32_t function{};
    std::uint16_t range_near{};
    std::uint16_t range_far{0xFFFFu};
};

struct AlphaTestState {
    bool enabled{};
    std::uint32_t function{};
    std::uint32_t reference{};
    std::uint32_t mask{0xFFu};
};

// STE (0x24), STST (0xDC) and SOP (0xDD). The PSP keeps the stencil in the
// framebuffer's alpha bits; the renderer gives it a stencil buffer of its own.
// Functions use the GE compare encoding (0 never .. 7 greater-or-equal), ops
// the GE stencil-op encoding (0 keep, 1 zero, 2 replace, 3 invert, 4 incr,
// 5 decr).
struct StencilState {
    bool enabled{};
    std::uint32_t function{1u};
    std::uint32_t reference{};
    std::uint32_t mask{0xFFu};
    std::uint32_t fail{};
    std::uint32_t depth_fail{};
    std::uint32_t pass{};
};

struct ViewportState {
    float x_scale{}, y_scale{}, z_scale{};
    float x_offset{}, y_offset{}, z_offset{};
    std::uint32_t scissor_x1{}, scissor_y1{}, scissor_x2{479u}, scissor_y2{271u};
    float offset_x{}, offset_y{};  // screen-space origin, 16 bits with 4 fractional
};

// One of the GE's four lights. The register layout below was read off the
// running game with <prefix>_TRACE_LIGHTING, not taken from a reference table.
struct LightState {
    bool enabled{};
    std::uint32_t kind{};         // bits 0..1: diffuse, diffuse + specular, powered diffuse
    std::uint32_t type{};         // bits 8..9: directional, point, spot
    std::array<float, 3> position{};   // the direction towards the light, for a directional one
    std::array<float, 3> direction{};  // spot axis
    std::array<float, 3> attenuation{1.0f, 0.0f, 0.0f};  // constant, linear, quadratic
    float spot_exponent{};
    float spot_cutoff{};          // cosine of the cone's half angle
    std::uint32_t ambient{};      // 0x00BBGGRR, as every colour register
    std::uint32_t diffuse{};
    std::uint32_t specular{};
};

// Material and light registers a lit draw is evaluated with. The material
// ambient colour and alpha stay in DrawCall::material_color, which unlit draws
// use as well.
struct LightingState {
    std::uint32_t material_update{};     // 0x53: which terms the vertex colour replaces
    std::uint32_t material_emissive{};   // 0x54
    std::uint32_t material_diffuse{};    // 0x56
    std::uint32_t material_specular{};   // 0x57
    float specular_power{1.0f};          // 0x5B
    std::uint32_t ambient_color{};       // 0x5C: global ambient light
    std::uint32_t ambient_alpha{0xFFu};  // 0x5D
    std::uint32_t mode{};                // 0x5E: 1 keeps specular apart, added after texturing
    bool reverse_normals{};              // 0x51
    std::array<LightState, 4> lights{};
};

struct FogState {
    bool enabled{};
    float end{};    // 0xCD: fog is complete at this view distance
    float scale{};  // 0xCE: 1 / (end - start)
    std::uint32_t color{};
};

// A draw call: decoded vertices plus the state they are drawn with.
struct DrawCall {
    PrimitiveType primitive{};
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;  // empty when the draw is not indexed
    bool through{};                      // vertices are already in screen space
    TextureState texture;
    RenderTarget target;
    BlendState blend;
    DepthState depth;
    AlphaTestState alpha_test;
    StencilState stencil;
    // PMSKC (0xE8) and PMSKA (0xE9): a set bit is one the draw may NOT write.
    // The alpha mask also guards the stencil, which lives in the alpha bits.
    std::uint32_t color_write_mask{};
    std::uint32_t alpha_write_mask{};
    ViewportState viewport;
    bool culling_enabled{};
    bool cull_clockwise{};
    bool clear_mode{};                   // CLEARMODE is active for this draw
    // A transformed draw whose vertices carry the texture coordinates each may
    // sample (raw units, before the scale and offset), as through tiles do:
    // min in normal[0], normal[1], max in normal[2], position[3]. Its vertices
    // have no normals of their own (lit, it is lit with a zero normal, as the
    // GE does). Set by a game profile for flat 2D sprites (VC2 native draws).
    bool uv_rects{};
    std::uint32_t clear_flags{};         // CLEARMODE bits 8..10: color, alpha/stencil, depth
    std::uint32_t vertex_type{};
    // Where the vertices and indices were read from and how many the prim
    // consumed: what recognises the same draw in the next frame.
    std::uint32_t vertex_address{};
    std::uint32_t index_address{};
    std::uint32_t primitive_count{};
    std::uint32_t material_color{0xFFFFFFFFu};
    bool lighting_enabled{};
    bool has_vertex_color{};             // the vertex type carries a colour
    LightingState lighting;
    FogState fog;
    // Change counters: environment_version moves whenever a register behind
    // the lights, the global ambient colour or the fog parameters is written,
    // material_version whenever one behind the material is. Equal versions
    // mean equal state, so the renderer can skip rebuilding what it derived.
    std::uint64_t environment_version{};
    std::uint64_t material_version{};
    std::array<float, 16> world{};
    std::array<float, 16> view{};
    std::array<float, 16> projection{};
    std::array<float, 16> texture_matrix{};
    // GPU vertex decode (GeState::set_raw_vertices): a transformed triangle
    // draw whose vertices were not decoded here. `vertices` is then empty, and
    // these give the guest's own bytes of the vertices the draw uses (from its
    // lowest index), valid while the draw sink runs, as are the bone matrices
    // (8 of them, 3x4 each, as uploaded) a skinned vertex type is blended by.
    const std::uint8_t *raw_vertices{};
    std::uint32_t raw_count{};
    std::uint32_t raw_stride{};
    const float *bone_matrices{};
};

// Where each field of a vertex of `vertex_type` lies in its bytes, as
// decode_vertices() reads them; kNoVertexField when the type has none.
inline constexpr std::uint32_t kNoVertexField = 0xFFFFFFFFu;
struct VertexFormat {
    std::uint32_t stride{};  // of one morph target
    std::uint32_t weight_offset{kNoVertexField};
    std::uint32_t texcoord_offset{kNoVertexField};
    std::uint32_t color_offset{kNoVertexField};
    std::uint32_t normal_offset{kNoVertexField};
    std::uint32_t position_offset{kNoVertexField};
};
[[nodiscard]] VertexFormat vertex_format(std::uint32_t vertex_type) noexcept;

// A GE block transfer: a rectangle of `width` x `height` pixels of
// `bytes_per_pixel` bytes copied from one buffer to another, each with its own
// row length in pixels. Addresses are those of the buffers' first pixel.
struct BlockTransfer {
    std::uint32_t source{};
    std::uint32_t source_stride{};
    std::uint32_t source_x{};
    std::uint32_t source_y{};
    std::uint32_t destination{};
    std::uint32_t destination_stride{};
    std::uint32_t destination_x{};
    std::uint32_t destination_y{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t bytes_per_pixel{};
};

// Optional observer of every draw the display-list path builds, with the list
// address of its PRIM command, called just before the draw reaches the sink.
// A game profile uses it to check a draw description it built another way
// (e.g. from the game's own graphics-library calls) against this one.
using DrawObserver = void (*)(const DrawCall &call, std::uint32_t primitive_address);
void set_draw_observer(DrawObserver observer) noexcept;

// Optional source of draws built outside the display list. A NOP whose data is
// non-zero is passed to it as a marker; when it returns a draw, that draw goes
// to the sink at that point of the list, keeping its order with the list's own
// draws. A game profile uses it to draw from the game's graphics-library calls
// directly (VC2 native rendering) instead of from commands describing them.
using NativeDrawSource = const DrawCall *(*)(std::uint32_t marker);
void set_native_draw_source(NativeDrawSource source) noexcept;

// Optional source of more detailed images for some of the game's textures. The
// renderer asks it about every texture it creates, first with texels null:
// true claims the texture (it is then decoded at once, not in the background).
// Then it is given the decoded texels (width x height, RGBA) and the scale the
// render target would use (its height / 272, rounded up) and returns an image
// `scale` times as wide and tall in `out`, or false to keep the original. A
// game profile uses it to draw text in high resolution (VC2's glyph texture).
using TextureUpscaler = bool (*)(const TextureState &texture, const std::uint32_t *texels,
                                 std::uint32_t wanted_scale, std::vector<std::uint32_t> &out, std::uint32_t &scale);
void set_texture_upscaler(TextureUpscaler upscaler) noexcept;
[[nodiscard]] TextureUpscaler texture_upscaler() noexcept;

// Optional images a profile draws in place of some textures, ahead of the
// texture pack and of the upscaler (VC2: the button prompts of the device in
// use). The renderer asks it about every texture it creates, first with
// texels null: anything but Ignore claims the texture (it is then decoded at
// once). Then it is given the decoded texels and the scale the render target
// would use, with `pack_image` null: WantsPackImage asks to be called again
// with the texture pack's image for the texture (RGBA, any size; width 0 when
// there is none), decoded only then. Replace: `out` (a whole multiple of the
// texture's size) is drawn instead, and the pack is not used for it. Keep:
// the texture is drawn as it would have been. Unless the answer with texels
// was Ignore, the texture is made again when texture_override_generation()
// changes (the profile's images changed).
struct TextureOverrideImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint32_t> pixels;  // RGBA, R in the low byte, rows packed
};
enum class TextureOverrideResult : std::uint8_t { Ignore, Keep, Replace, WantsPackImage };
using TextureOverride = TextureOverrideResult (*)(const TextureState &texture, const std::uint32_t *texels,
                                                  const TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                                  TextureOverrideImage &out);
void set_texture_override(TextureOverride override_function) noexcept;
[[nodiscard]] TextureOverride texture_override() noexcept;
// Called by the profile when its images change; any thread.
void bump_texture_override_generation() noexcept;
[[nodiscard]] std::uint32_t texture_override_generation() noexcept;

// Executes display lists and reports the draw calls they produce. The backend
// installs a sink; with no sink the lists are still parsed (for callbacks).
class GeState {
public:
    using DrawSink = std::function<void(const DrawCall &)>;
    using SignalSink = std::function<void(std::uint32_t signal, std::uint32_t pc)>;
    using TransferSink = std::function<void(const BlockTransfer &)>;

    void set_draw_sink(DrawSink sink) { draw_sink_ = std::move(sink); }
    void set_signal_sink(SignalSink sink) { signal_sink_ = std::move(sink); }
    // Block transfers are carried out by the sink, which can write guest memory.
    void set_transfer_sink(TransferSink sink) { transfer_sink_ = std::move(sink); }
    // On, transformed triangles, strips and fans of one morph target whose
    // vertices lie contiguously in host memory reach the sink undecoded
    // (DrawCall::raw_vertices), for the renderer to decode on the GPU.
    // `also_decode` decodes them as well, for checking the GPU's decode.
    void set_raw_vertices(bool raw, bool also_decode = false) noexcept {
        raw_vertices_ = raw;
        raw_also_decoded_ = also_decode;
    }

    // Runs commands from `pc` until `stall` (0 = no stall) or END. Returns the
    // address execution stopped at; `finished` reports whether the list ended.
    std::uint32_t execute(const GuestMemory &memory, std::uint32_t pc, std::uint32_t stall, bool &finished);

    [[nodiscard]] const RenderTarget &target() const noexcept { return target_; }
    [[nodiscard]] std::uint64_t draw_count() const noexcept { return draw_count_; }
    // The display-list address the last view matrix was uploaded from.
    [[nodiscard]] std::uint32_t view_matrix_source() const noexcept { return view_matrix_source_; }
    [[nodiscard]] std::uint64_t vertex_count() const noexcept { return vertex_count_; }
    [[nodiscard]] std::uint64_t unhandled_command_count() const noexcept { return unhandled_commands_; }
    // Every GE command the state machine ignored, with how often and the first
    // value seen, printed once at the end of a run. This is the answer to
    // "what does this game ask of the hardware that the renderer does not
    // do", and it is the game's own answer rather than a reading of the code.
    static void report_ignored_commands();

private:
    // Resolves a display-list address operand against BASE and OFFSET_ADDR.
    [[nodiscard]] std::uint32_t relative_address(std::uint32_t data) const noexcept {
        return (offset_address_ + (base_extended_ | (data & 0x00FFFFFFu))) & 0x0FFFFFFFu;
    }

    void handle_command(const GuestMemory &memory, std::uint32_t command, std::uint32_t data);
    void trace_unhandled(std::uint32_t command, std::uint32_t data);
    void draw_primitive(const GuestMemory &memory, std::uint32_t data);
    // Copies the state every draw is made with into call_.
    void fill_draw_state(DrawCall &call) const;
    // BEZIER and SPLINE: tessellates the surface the control points describe
    // and draws it as triangles (or lines or points, as PATCHPRIMITIVE says).
    void draw_patch(const GuestMemory &memory, std::uint32_t command, std::uint32_t data);

    std::array<std::uint32_t, 256> registers_{};
    RenderTarget target_{};
    TextureState texture_{};
    BlendState blend_{};
    DepthState depth_{};
    AlphaTestState alpha_test_{};
    StencilState stencil_{};
    std::uint32_t color_write_mask_{};
    std::uint32_t alpha_write_mask_{};
    ViewportState viewport_{};
    bool culling_enabled_{};
    bool cull_clockwise_{};
    bool clear_mode_{};
    std::uint32_t clear_flags_{};
    std::uint32_t material_color_{0xFFFFFFFFu};
    bool lighting_enabled_{};
    LightingState lighting_{};
    FogState fog_{};
    std::uint64_t environment_version_{1u};
    std::uint64_t material_version_{1u};
    std::uint32_t vertex_type_{};
    std::uint32_t vertex_address_{};
    std::uint32_t index_address_{};
    std::uint32_t base_extended_{};   // BASE: bits 16..19 become address bits 24..27
    std::uint32_t offset_address_{};  // OFFSET_ADDR: added to every relative address
    std::array<float, 16> world_{};
    std::array<float, 16> view_{};
    std::array<float, 16> projection_{};
    std::array<float, 16> texture_matrix_{};
    std::array<float, 96> bone_matrices_{};
    // The GE keeps one auto-incrementing write index per matrix; sharing a
    // single counter lets interleaved uploads scribble over each other.
    std::uint32_t world_write_index_{};
    std::uint32_t view_write_index_{};
    std::uint32_t view_matrix_source_{};
    std::uint32_t projection_write_index_{};
    std::uint32_t texture_write_index_{};
    std::uint32_t bone_write_index_{};

    std::vector<std::uint32_t> call_stack_;
    DrawCall call_;  // reused by draw_primitive for every draw
    DrawSink draw_sink_;
    SignalSink signal_sink_;
    TransferSink transfer_sink_;
    std::uint64_t draw_count_{};
    std::uint64_t vertex_count_{};
    std::uint64_t unhandled_commands_{};
    bool raw_vertices_{};
    bool raw_also_decoded_{};
    std::uint32_t primitive_address_{};  // list address of the PRIM being drawn
};

// Copies the game makes out of VRAM with the DMA controller, remembered for
// <prefix>_TRACE_FB_TEXTURES so a texture read from a copy can be traced back to
// the framebuffer it came from.
void note_vram_copy(std::uint32_t destination, std::uint32_t source, std::uint32_t size);
// The most recent such copy whose destination holds `address`: its source and
// destination, or false.
bool find_vram_copy(std::uint32_t address, std::uint32_t &source, std::uint32_t &destination);

// Decodes `count` vertices of the given vertex type starting at `address`.
// Returns the number of bytes each vertex occupies.
// `bone_matrices`, when given, points at 8 consecutive 3x4 matrices (96 floats)
// and skinned vertices are blended into their bones' space by their weights.
std::uint32_t decode_vertices(const GuestMemory &memory, std::uint32_t address, std::uint32_t vertex_type,
                              std::uint32_t count, std::vector<Vertex> &out,
                              const float *bone_matrices = nullptr);

} // namespace portablekit::gpu
