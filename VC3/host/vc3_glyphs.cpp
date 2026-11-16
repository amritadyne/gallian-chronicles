// VC2 button glyphs: the prompts of the device in use, and the inventory
// tools that found where the game keeps them.
//
// Prompts. Every button prompt the game draws (hint bar, menus, map) comes
// from one 64x64 CLUT4 image, the prompt atlas (EU 0x09B4DFF0, a copy at
// 0x09B38C30; see the project doc button-glyphs.md): PSP circle, triangle,
// square and cross, L and R, the stick, d-pads, SELECT and START, each at its
// own place. It is found by its shape (the alpha mask's hash), whatever its
// address or palette. The renderer's texture override (PortableKit patch
// 0020) hands it here with the texture pack's image of it when a pack has one,
// and the button cells are painted over that image (or over the original,
// enlarged); the rest is left alone. PlayStation: the face buttons stay, L and
// R become L1 and R1, SELECT and START become SHARE and OPTIONS. The device is the one used
// last; a change, or for the keyboard a change of context (its keys mean
// different buttons in Action Mode, Target Mode, on the map and elsewhere),
// makes the renderer build the atlas again.
//   VC3_PROMPTS=auto (default) | playstation | xbox | keyboard
//   VC3_NO_PROMPTS=1  leave the atlas alone
// Xbox: positional (Cross A, Circle B, Square X, Triangle Y, L LB, R RB,
// SELECT View, START Menu); SDL reports buttons by position, so a Nintendo pad
// also gets these. Keyboard and mouse: what vc3_controls.cpp's table gives that
// button in the current context (kbm_prompt); a button nothing gives there
// gets its PlayStation glyph; the d-pads become W A S D, lit as the d-pad
// directions were; the stick becomes what does its job there (W A S D in
// Action Mode and menus, the mouse in Target Mode and on the map). Letters are drawn with the system's Bahnschrift (Windows 10
// and later), else Segoe UI, else Arial.
//
// Inventory (VC3_GLYPH_TRACE=<folder>). The font object's staging function
// (EU 0x08BC95A4, a vtable method of the font class at 0x08D2F5F0: a0 = font,
// a1 = flags, a2 = the character's code, a3 = which table) expands characters
// into the font's 32-bit glyph surface (pointer at font+3524, 256 pixels a
// row; glyph width font+3556, height font+3558), which the game then copies
// into the sheet it draws text from (0x09B57FC0 for symbols and kana,
// 0x09B98000 for ASCII). cmake/vc3_step_sites.cmake makes the function call
// vc3_glyph_begin on entry and vc3_glyph_end once the buffer is complete (EU
// L_08BC95A4 and L_08BC98C8, unit 0241). Each distinct code is logged once and
// written as <folder>\glyph-<code>-<table>.pam. Not every character passes
// through it, so the sheet at 0x09B57FC0 is read every 30 flips too and each
// new cell written as <folder>\sheet-<column>-<row>-<hash>.pam (up to 800).
#include "vc3_glyphs.hpp"
#include "vc3_controls.hpp"
#include "vc3_region.hpp"
#include "vc3_settings.hpp"
#include "vc3_text.hpp"

#include "profile.hpp"
#include "kernel/kernel.hpp"
#include "gpu/ge_state.hpp"

#if defined(PORTABLEKIT_HAS_SDL)
#include <SDL3/SDL.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <vector>

namespace portablekit::vc3 {
namespace {

// =============================================================================
// Inventory
// =============================================================================

constexpr std::uint32_t kSurfaceWidth = 256u, kSurfaceRows = 256u;

struct GlyphTrace {
    std::uint32_t code = 0u, table = 0u, flags = 0u;
    bool pending = false;
    std::vector<std::uint32_t> before;  // the surface before the glyph
    std::unordered_set<std::uint32_t> seen;
    std::uint64_t staged = 0u;
};

GlyphTrace &trace() {
    static GlyphTrace t;
    return t;
}

const char *trace_folder() {
    static const char *folder = [] {
        const char *text = env("GLYPH_TRACE");
        if (text != nullptr && *text != '\0') {
            std::error_code error;
            std::filesystem::create_directories(text, error);
            std::cerr << "[vc3-glyph] tracing glyphs into " << text << "\n";
            return text;
        }
        return static_cast<const char *>(nullptr);
    }();
    return folder;
}

std::string hex4(std::uint32_t value) {
    static const char digits[] = "0123456789abcdef";
    std::string out(4, '0');
    for (int i = 3; i >= 0; --i, value >>= 4u) out[static_cast<std::size_t>(i)] = digits[value & 0xFu];
    return out;
}

void scan_sheet() {
    const char *folder = trace_folder();
    if (folder == nullptr) return;
    static std::uint64_t flips = 0u;
    if (++flips % 30u != 0u) return;
    static std::unordered_set<std::uint64_t> cells_seen;
    static std::uint32_t written = 0u;
    if (written >= 800u) return;
    constexpr std::uint32_t kSheet = 0x09B57FC0u, kWidth = 256u, kCell = 16u;
    auto &memory = portablekit::kernel().runtime().memory();
    std::uint32_t texels[kCell * kCell];
    for (std::uint32_t row = 0; row < kWidth / kCell; ++row)
        for (std::uint32_t column = 0; column < kWidth / kCell; ++column) {
            std::uint64_t hash = 1469598103934665603ull;
            bool empty = true;
            for (std::uint32_t y = 0; y < kCell; ++y)
                for (std::uint32_t x = 0; x < kCell; ++x) {
                    const std::uint32_t texel =
                        memory.load32(kSheet + ((row * kCell + y) * kWidth + column * kCell + x) * 4u);
                    texels[y * kCell + x] = texel;
                    if ((texel >> 24u) != 0u) empty = false;
                    hash = (hash ^ texel) * 1099511628211ull;
                }
            if (empty || !cells_seen.insert(hash).second) continue;
            ++written;
            const std::string path = std::string(folder) + "/sheet-" + std::to_string(column) + "-" +
                                     std::to_string(row) + "-" + hex4(static_cast<std::uint32_t>(hash & 0xFFFFu)) +
                                     ".pam";
            std::ofstream out(path, std::ios::binary);
            out << "P7\nWIDTH 16\nHEIGHT 16\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
            for (const std::uint32_t texel : texels) {
                const char rgba[4] = {static_cast<char>(texel & 0xFFu), static_cast<char>((texel >> 8u) & 0xFFu),
                                      static_cast<char>((texel >> 16u) & 0xFFu), static_cast<char>(texel >> 24u)};
                out.write(rgba, 4);
            }
            if (written % 50u == 1u)
                std::cerr << "[vc3-glyph] sheet cells written: " << written << " (latest " << column << "," << row
                          << ")\n";
        }
}

// =============================================================================
// Prompts: the device
// =============================================================================

enum class Device : int { PlayStation = 0, Xbox = 1, Keyboard = 2 };

const char *device_name(Device device) {
    switch (device) {
    case Device::PlayStation: return "PlayStation";
    case Device::Xbox: return "Xbox";
    case Device::Keyboard: return "keyboard and mouse";
    }
    return "?";
}

struct Prompts {
    bool enabled = true;
    bool forced = false;  // VC3_PROMPTS names a device
    std::atomic<int> device{static_cast<int>(Device::Keyboard)};
    std::atomic<int> context{3};
    int variant = -1;  // device and, for the keyboard, context: what the atlas was built for
    bool pad_was_active = false, keys_were_active = false, started = false;
    std::uint64_t atlases = 0u;
};

// The prompt style chosen (vc3.prompts): a fixed device, or automatic.
void choose(Prompts &p) {
    const std::string &name = options::get(options::Key::Prompts);
    p.forced = true;
    if (name == "playstation") p.device = static_cast<int>(Device::PlayStation);
    else if (name == "xbox") p.device = static_cast<int>(Device::Xbox);
    else if (name == "keyboard") p.device = static_cast<int>(Device::Keyboard);
    else p.forced = false;
}

Prompts &prompts() {
    static Prompts p;
    static const bool configured = [] {
        const char *off = env("NO_PROMPTS");
        p.enabled = !(off != nullptr && *off != '\0' && std::strcmp(off, "0") != 0);
        choose(p);
        return true;
    }();
    (void)configured;
    return p;
}

#if defined(PORTABLEKIT_HAS_SDL)
Device pad_device(SDL_Gamepad *pad) {
    switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return Device::PlayStation;
    default: return Device::Xbox;
    }
}

// The pad with a button down or a stick or trigger well off centre, if any;
// `any_pad` is set to the first pad open, active or not.
bool active_pad(Device &device, bool &any_pad, Device &first) {
    bool active = false;
    any_pad = false;
    int count = 0;
    if (SDL_JoystickID *ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count && !active; ++i) {
            SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
            if (pad == nullptr) continue;
            if (!any_pad) any_pad = true, first = pad_device(pad);
            for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT && !active; ++b)
                active = SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b));
            for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT && !active; ++a)
                active = std::abs(static_cast<int>(SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(a)))) > 16000;
            if (active) device = pad_device(pad);
        }
        SDL_free(ids);
    }
    return active;
}

bool keys_active() {
    if (SDL_GetKeyboardFocus() != nullptr) {
        int count = 0;
        if (const bool *keys = SDL_GetKeyboardState(&count))
            for (int k = 0; k < count; ++k)
                if (keys[k]) return true;
    }
    return (SDL_GetMouseFocus() != nullptr || SDL_GetKeyboardFocus() != nullptr) &&
           SDL_GetMouseState(nullptr, nullptr) != 0u;
}
#endif

// Once a flip: the device used last (it changes when another one starts being
// used), and the keyboard's context. A change rebuilds the atlas.
void update_device() {
    Prompts &p = prompts();
    if (!p.enabled) return;
#if defined(PORTABLEKIT_HAS_SDL)
    if (!p.forced) {
        Device pad = Device::Xbox, first = Device::Xbox;
        bool any_pad = false;
        const bool pad_active = active_pad(pad, any_pad, first);
        const bool keys = keys_active();
        int device = p.device.load();
        if (!p.started) {
            p.started = true;
            device = static_cast<int>(any_pad ? first : Device::Keyboard);
        }
        if (pad_active && !p.pad_was_active) device = static_cast<int>(pad);
        else if (keys && !p.keys_were_active) device = static_cast<int>(Device::Keyboard);
        p.pad_was_active = pad_active;
        p.keys_were_active = keys;
        p.device = device;
    }
#endif
    const int context = controls_context();
    p.context = context;
    const int device = p.device.load();
    // The keyboard's prompts change with every context; a pad's only in and
    // out of Action Mode (Modern: Square's prompt is the R shoulder there).
    const bool modern_pad = device != static_cast<int>(Device::Keyboard) &&
                            controls_preset() == ControlsPreset::Modern;
    const int variant = device * 8 + (device == static_cast<int>(Device::Keyboard) ? context
                                      : modern_pad && context == 0             ? 1
                                                                               : 0);
    if (variant == p.variant) return;
    const bool first = p.variant < 0;
    p.variant = variant;
    static std::uint32_t logged = 0u;
    if (++logged <= 200u)
        std::cerr << "[vc3-prompts] " << device_name(static_cast<Device>(device)) << " prompts"
                  << (device == static_cast<int>(Device::Keyboard)
                          ? std::string(" (context ") + std::to_string(context) + ")"
                          : std::string())
                  << "\n";
    if (!first) gpu::bump_texture_override_generation();
}

// =============================================================================
// Prompts: painting
// =============================================================================

// The atlas's alpha mask (one byte per texel, 1 where alpha is not 0), FNV-1a.
constexpr std::uint64_t kAtlasMask = 0x497252549b2ece79ull;
// VC3's (2026-11-16, its first battle's 0x09B9A370 in the owner's capture):
// the same layout, its button cells where VC2's are, with a filled block at
// its top right that VC2's lacks.
constexpr std::uint64_t kAtlasMaskVc3 = 0x08c0c767408f7799ull;
constexpr std::uint32_t kAtlasSide = 64u;

std::uint64_t mask_hash(const std::uint32_t *texels) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::uint32_t i = 0; i < kAtlasSide * kAtlasSide; ++i)
        hash = (hash ^ ((texels[i] >> 24u) != 0u ? 1u : 0u)) * 1099511628211ull;
    return hash;
}

struct Rgba {
    float r, g, b, a;  // 0..1, straight alpha
};

constexpr Rgba rgb(int r, int g, int b, float a = 1.0f) { return {r / 255.0f, g / 255.0f, b / 255.0f, a}; }

// The atlas being painted: `scale` canvas pixels to an atlas texel. Shapes are
// given in atlas texels.
struct Canvas {
    std::uint32_t side = 0u;
    float scale = 1.0f;
    std::vector<std::uint32_t> pixels;

    void blend(std::uint32_t x, std::uint32_t y, const Rgba &c, float coverage) {
        const float a = c.a * std::clamp(coverage, 0.0f, 1.0f);
        if (a <= 0.0f) return;
        std::uint32_t &p = pixels[static_cast<std::size_t>(y) * side + x];
        const float dr = (p & 0xFFu) / 255.0f, dg = ((p >> 8u) & 0xFFu) / 255.0f, db = ((p >> 16u) & 0xFFu) / 255.0f,
                    da = (p >> 24u) / 255.0f;
        const float oa = a + da * (1.0f - a);
        const auto mix = [&](float s, float d) { return oa > 0.0f ? (s * a + d * da * (1.0f - a)) / oa : 0.0f; };
        const auto byte = [](float v) { return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
        p = byte(mix(c.r, dr)) | byte(mix(c.g, dg)) << 8u | byte(mix(c.b, db)) << 16u | byte(oa) << 24u;
    }

    // Makes the texels [x0, x1) x [y0, y1) transparent.
    void clear(float x0, float y0, float x1, float y1) {
        const std::uint32_t px0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(x0 * scale)));
        const std::uint32_t py0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(y0 * scale)));
        const std::uint32_t px1 = std::min(side, static_cast<std::uint32_t>(std::ceil(x1 * scale)));
        const std::uint32_t py1 = std::min(side, static_cast<std::uint32_t>(std::ceil(y1 * scale)));
        for (std::uint32_t y = py0; y < py1; ++y)
            for (std::uint32_t x = px0; x < px1; ++x) pixels[static_cast<std::size_t>(y) * side + x] = 0u;
    }

    // Fills where `inside` (atlas texels) holds, 4 x 4 samples a pixel, within
    // the box [x0, x1) x [y0, y1).
    void fill(float x0, float y0, float x1, float y1, const Rgba &c, const std::function<bool(float, float)> &inside) {
        const std::uint32_t px0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(x0 * scale)));
        const std::uint32_t py0 = static_cast<std::uint32_t>(std::max(0.0f, std::floor(y0 * scale)));
        const std::uint32_t px1 = std::min(side, static_cast<std::uint32_t>(std::ceil(x1 * scale)));
        const std::uint32_t py1 = std::min(side, static_cast<std::uint32_t>(std::ceil(y1 * scale)));
        for (std::uint32_t y = py0; y < py1; ++y)
            for (std::uint32_t x = px0; x < px1; ++x) {
                int hits = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) {
                        const float tx = (x + (sx + 0.5f) / 4.0f) / scale, ty = (y + (sy + 0.5f) / 4.0f) / scale;
                        if (tx >= x0 && tx < x1 && ty >= y0 && ty < y1 && inside(tx, ty)) ++hits;
                    }
                if (hits > 0) blend(x, y, c, hits / 16.0f);
            }
    }
};

float rounded_box_distance(float x, float y, float cx, float cy, float half_w, float half_h, float radius) {
    const float qx = std::abs(x - cx) - (half_w - radius), qy = std::abs(y - cy) - (half_h - radius);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - radius;
}

void disc(Canvas &c, float cx, float cy, float r, const Rgba &colour) {
    c.fill(cx - r, cy - r, cx + r, cy + r, colour,
           [=](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; });
}

void ring(Canvas &c, float cx, float cy, float r, float width, const Rgba &colour) {
    const float inner = r - width;
    c.fill(cx - r, cy - r, cx + r, cy + r, colour, [=](float x, float y) {
        const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        return d <= r * r && d >= inner * inner;
    });
}

void box(Canvas &c, float cx, float cy, float half_w, float half_h, float radius, const Rgba &colour) {
    c.fill(cx - half_w, cy - half_h, cx + half_w, cy + half_h, colour,
           [=](float x, float y) { return rounded_box_distance(x, y, cx, cy, half_w, half_h, radius) <= 0.0f; });
}

void box_outline(Canvas &c, float cx, float cy, float half_w, float half_h, float radius, float width,
                 const Rgba &colour) {
    c.fill(cx - half_w, cy - half_h, cx + half_w, cy + half_h, colour, [=](float x, float y) {
        const float d = rounded_box_distance(x, y, cx, cy, half_w, half_h, radius);
        return d <= 0.0f && d >= -width;
    });
}

// A convex polygon (atlas texels, either winding): filled, or only its edge
// `width` wide inside it.
struct Point {
    float x, y;
};
float polygon_distance(const std::vector<Point> &pts, float x, float y) {
    // Negative inside: the largest signed distance to an edge's line.
    float area = 0.0f;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Point &a = pts[i], &b = pts[(i + 1) % pts.size()];
        area += a.x * b.y - b.x * a.y;
    }
    const float side = area >= 0.0f ? 1.0f : -1.0f;
    float d = -1e9f;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const Point &a = pts[i], &b = pts[(i + 1) % pts.size()];
        const float ex = b.x - a.x, ey = b.y - a.y, len = std::sqrt(ex * ex + ey * ey);
        if (len <= 0.0f) continue;
        d = std::max(d, side * ((x - a.x) * ey - (y - a.y) * ex) / len);
    }
    return d;
}
void polygon(Canvas &c, const std::vector<Point> &pts, const Rgba &colour, float width = 0.0f) {
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Point &p : pts) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    c.fill(x0, y0, x1, y1, colour, [&](float x, float y) {
        const float d = polygon_distance(pts, x, y);
        return d <= 0.0f && (width <= 0.0f || d >= -width);
    });
}
// A stroke from (ax, ay) to (bx, by), `width` wide, round ends.
void stroke(Canvas &c, float ax, float ay, float bx, float by, float width, const Rgba &colour) {
    const float r = width / 2.0f;
    c.fill(std::min(ax, bx) - r, std::min(ay, by) - r, std::max(ax, bx) + r, std::max(ay, by) + r, colour,
           [=](float x, float y) {
               const float ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey;
               const float t = len2 > 0.0f ? std::clamp(((x - ax) * ex + (y - ay) * ey) / len2, 0.0f, 1.0f) : 0.0f;
               const float dx = x - (ax + t * ex), dy = y - (ay + t * ey);
               return dx * dx + dy * dy <= r * r;
           });
}

// --- Letters ----------------------------------------------------------------

#if defined(_WIN32)
// A label drawn by GDI, as coverage: grey anti-aliasing on black.
struct Label {
    int width = 0, height = 0;
    int ink_x0 = 0, ink_y0 = 0, ink_x1 = 0, ink_y1 = 0;  // the inked part
    std::vector<std::uint8_t> coverage;
};

const std::vector<std::wstring> &faces(bool condensed) {
    static const std::vector<std::wstring> wide = {L"Bahnschrift SemiBold", L"Bahnschrift", L"Segoe UI Semibold",
                                                   L"Segoe UI", L"Arial"};
    static const std::vector<std::wstring> narrow = {L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed",
                                                     L"Bahnschrift SemiBold", L"Arial Narrow", L"Arial"};
    return condensed ? narrow : wide;
}

HFONT make_font(int em, bool condensed) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT chosen = nullptr;
    for (const std::wstring &name : faces(condensed)) {
        HFONT font = CreateFontW(-em, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                                 CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, name.c_str());
        if (font == nullptr) continue;
        HGDIOBJ old = SelectObject(dc, font);
        wchar_t selected[LF_FACESIZE] = {};
        GetTextFaceW(dc, LF_FACESIZE, selected);
        SelectObject(dc, old);
        if (_wcsnicmp(selected, name.c_str(), std::min<std::size_t>(name.size(), std::wcslen(selected))) == 0 &&
            std::wcslen(selected) > 0) {
            chosen = font;
            break;
        }
        DeleteObject(font);
    }
    DeleteDC(dc);
    return chosen;
}

Label render_label(const std::wstring &text, int em, bool condensed) {
    Label label;
    HFONT font = make_font(em, condensed);
    if (font == nullptr) return label;
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_font = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    label.width = size.cx + em / 2 + 4;
    label.height = size.cy + 4;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = label.width;
    info.bmiHeader.biHeight = -label.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap != nullptr && bits != nullptr) {
        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        std::memset(bits, 0, static_cast<std::size_t>(label.width) * label.height * 4u);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutW(dc, 2 + em / 4, 2, text.c_str(), static_cast<int>(text.size()));
        GdiFlush();
        label.coverage.resize(static_cast<std::size_t>(label.width) * label.height);
        label.ink_x0 = label.width, label.ink_y0 = label.height;
        const auto *row = static_cast<const std::uint32_t *>(bits);
        for (int y = 0; y < label.height; ++y)
            for (int x = 0; x < label.width; ++x) {
                const std::uint32_t p = row[static_cast<std::size_t>(y) * label.width + x];
                const std::uint8_t v = static_cast<std::uint8_t>(
                    std::max({p & 0xFFu, (p >> 8u) & 0xFFu, (p >> 16u) & 0xFFu}));
                label.coverage[static_cast<std::size_t>(y) * label.width + x] = v;
                if (v != 0u) {
                    label.ink_x0 = std::min(label.ink_x0, x), label.ink_y0 = std::min(label.ink_y0, y);
                    label.ink_x1 = std::max(label.ink_x1, x + 1), label.ink_y1 = std::max(label.ink_y1, y + 1);
                }
            }
        SelectObject(dc, old_bitmap);
        DeleteObject(bitmap);
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    if (label.ink_x1 <= label.ink_x0) label.coverage.clear();
    return label;
}
#endif

// Draws `text` centred on (cx, cy) (atlas texels), its capitals `cap` texels
// tall, no wider than `max_width` texels. Returns the capital height it used
// (smaller when it had to shrink to fit), 0 when nothing was drawn.
float text(Canvas &c, const char *utf8, float cx, float cy, float cap, float max_width, const Rgba &colour) {
#if defined(_WIN32)
    std::wstring wide;
    for (const char *p = utf8; *p != '\0'; ++p) wide.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    const bool condensed = wide.size() > 2u;
    // Capitals are about 0.7 of the em; shrink until the ink fits.
    int em = std::max(4, static_cast<int>(std::lround(cap * c.scale / 0.7f)));
    Label label;
    for (int attempt = 0; attempt < 12; ++attempt) {
        label = render_label(wide, em, condensed);
        if (label.coverage.empty()) return 0.0f;
        const float ink_w = static_cast<float>(label.ink_x1 - label.ink_x0) / c.scale;
        if (ink_w <= max_width || em <= 4) break;
        em = std::max(4, static_cast<int>(em * std::max(0.6f, max_width / ink_w * 0.98f)));
    }
    const float ink_cx = (label.ink_x0 + label.ink_x1) / 2.0f, ink_cy = (label.ink_y0 + label.ink_y1) / 2.0f;
    // A whole-pixel offset, so no column or row is doubled or dropped.
    const long ox = std::lround(cx * c.scale - ink_cx), oy = std::lround(cy * c.scale - ink_cy);
    for (int y = label.ink_y0; y < label.ink_y1; ++y)
        for (int x = label.ink_x0; x < label.ink_x1; ++x) {
            const std::uint8_t v = label.coverage[static_cast<std::size_t>(y) * label.width + x];
            if (v == 0u) continue;
            const long tx = x + ox, ty = y + oy;
            if (tx < 0 || ty < 0 || tx >= static_cast<long>(c.side) || ty >= static_cast<long>(c.side)) continue;
            c.blend(static_cast<std::uint32_t>(tx), static_cast<std::uint32_t>(ty), colour, v / 255.0f);
        }
    return em * 0.7f / c.scale;
#else
    (void)c, (void)utf8, (void)cx, (void)cy, (void)cap, (void)max_width, (void)colour;
    return 0.0f;
#endif
}

// --- The atlas's button cells (atlas texels; measured from the EU dump) -------

struct Cell {
    std::uint32_t button;  // PSP button bit
    float x0, y0, x1, y1;  // the original glyph's box, which the new one stays inside
    // What is cleared first: the box and its margin, where an enlarged image
    // (the texture pack's) blurs past it, short of the neighbours.
    float cx0, cy0, cx1, cy1;
};

constexpr std::uint32_t kSelect = 0x0001u, kStart = 0x0008u, kL = 0x0100u, kR = 0x0200u, kTriangle = 0x1000u,
                        kCircle = 0x2000u, kCross = 0x4000u, kSquare = 0x8000u;

constexpr Cell kCells[] = {
    {kCircle, 2, 2, 10, 10, 1, 1, 11.5f, 11},
    {kTriangle, 13, 2, 23, 10, 11.5f, 1, 24.5f, 11},
    {kSquare, 26, 2, 34, 10, 24.5f, 1, 36, 11},
    {kCross, 38, 2, 46, 10, 36, 1, 47.5f, 11},
    {kL, 0, 15, 12, 22, 0, 14, 12, 23},
    {kR, 12, 15, 24, 22, 12, 14, 24.75f, 23},
    {kSelect, 0, 51, 23, 58, 0, 50, 25, 59.75f},
    {kStart, 27, 51, 46, 58, 25.5f, 50, 48, 59.75f},
};

const Rgba kInk = rgb(0x1E, 0x1F, 0x24, 0.92f);   // button faces
const Rgba kRim = rgb(0xE8, 0xE8, 0xEA);          // their rims
const Rgba kWhite = rgb(0xFA, 0xFA, 0xFA);
const Rgba kKeyFace = rgb(0xEE, 0xEE, 0xF0);      // key caps
const Rgba kKeyEdge = rgb(0x2A, 0x2B, 0x30);
const Rgba kKeyInk = rgb(0x1A, 0x1B, 0x20);
const Rgba kAccent = rgb(0xF2, 0xB2, 0x2E);       // the pressed mouse button
const Rgba kDim = rgb(0xFF, 0xFF, 0xFF, 0.5f);     // unlit parts, as the d-pads' own

void xbox_face(Canvas &c, const Cell &cell, const char *letter, const Rgba &colour) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float r = std::min(cell.x1 - cell.x0, cell.y1 - cell.y0) / 2.0f;
    disc(c, cx, cy, r, kInk);
    ring(c, cx, cy, r, 0.55f, kRim);
    text(c, letter, cx, cy + 0.05f, r * 1.05f, r * 1.3f, colour);
}

void pill(Canvas &c, const Cell &cell, const char *label) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float hw = (cell.x1 - cell.x0) / 2.0f - 0.25f, hh = (cell.y1 - cell.y0) / 2.0f;
    box(c, cx, cy, hw, hh, hh * 0.9f, kInk);
    box_outline(c, cx, cy, hw, hh, hh * 0.9f, 0.5f, kRim);
    text(c, label, cx, cy, hh * 1.05f, hw * 1.5f, kWhite);
}

// View: two overlapping windows; Menu: three lines; in a small round button.
void xbox_small(Canvas &c, const Cell &cell, bool view) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float r = (cell.y1 - cell.y0) / 2.0f;
    disc(c, cx, cy, r, kInk);
    ring(c, cx, cy, r, 0.5f, kRim);
    if (view) {
        box_outline(c, cx - 0.55f, cy - 0.55f, 1.35f, 1.0f, 0.2f, 0.45f, kWhite);
        box(c, cx + 0.55f, cy + 0.55f, 1.35f, 1.0f, 0.2f, kInk);
        box_outline(c, cx + 0.55f, cy + 0.55f, 1.35f, 1.0f, 0.2f, 0.45f, kWhite);
    } else {
        for (const float dy : {-1.1f, 0.0f, 1.1f}) box(c, cx, cy + dy, 1.6f, 0.26f, 0.2f, kWhite);
    }
}

void key_cap(Canvas &c, const Cell &cell, const char *label) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float hh = (cell.y1 - cell.y0) / 2.0f;
    // As wide as its label needs, up to the cell; at least square.
    const float wanted = std::strlen(label) <= 1u ? hh : hh + 0.9f * static_cast<float>(std::strlen(label) - 1u);
    const float hw = std::min((cell.x1 - cell.x0) / 2.0f, std::max(hh, wanted));
    box(c, cx, cy, hw, hh, std::min(hw, hh) * 0.35f, kKeyFace);
    box_outline(c, cx, cy, hw, hh, std::min(hw, hh) * 0.35f, 0.5f, kKeyEdge);
    text(c, label, cx, cy, hh * 0.95f, hw * 2.0f - 1.4f, kKeyInk);
}

void mouse(Canvas &c, const Cell &cell, int button) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float hh = (cell.y1 - cell.y0) / 2.0f, hw = hh * 0.72f;
    const float radius = hw * 0.95f;
    box(c, cx, cy, hw, hh, radius, kKeyFace);
    // The pressed button: the top part on its side.
    const float split = cy - hh * 0.15f;
    const bool left = button == 1;
    c.fill(cx - hw, cy - hh, cx + hw, split, kAccent, [=](float x, float y) {
        return rounded_box_distance(x, y, cx, cy, hw, hh, radius) <= 0.0f && (left ? x < cx : x >= cx);
    });
    box_outline(c, cx, cy, hw, hh, radius, 0.45f, kKeyEdge);
    box(c, cx, split, hw, 0.2f, 0.0f, kKeyEdge);
    box(c, cx, (cy - hh + split) / 2.0f, 0.2f, (split - (cy - hh)) / 2.0f, 0.0f, kKeyEdge);
}

// PlayStation: the face buttons stay as they are; L and R become L1 and R1,
// SELECT and START become SHARE and OPTIONS (DualShock 4 names).
void paint_playstation(Canvas &c, const Cell &cell) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float hw = (cell.x1 - cell.x0) / 2.0f, hh = (cell.y1 - cell.y0) / 2.0f;
    switch (cell.button) {
    case kL:
    case kR:
        c.clear(cell.cx0, cell.cy0, cell.cx1, cell.cy1);
        box_outline(c, cx, cy, hw - 0.4f, hh, hh * 0.6f, 0.5f, kDim);
        text(c, cell.button == kL ? "L1" : "R1", cx, cy, hh * 1.05f, hw * 1.5f, kWhite);
        break;
    case kSelect:
    case kStart: {
        // Both at one size: the one OPTIONS fits its narrower place at.
        static float shared_cap = 0.0f;
        static float shared_scale = 0.0f;
        if (shared_scale != c.scale) {
            Canvas probe;
            probe.side = c.side;
            probe.scale = c.scale;
            probe.pixels.assign(static_cast<std::size_t>(c.side) * c.side, 0u);
            const Cell &start = kCells[7];
            shared_cap = text(probe, "OPTIONS", 32.0f, 32.0f, (start.y1 - start.y0), start.x1 - start.x0, kWhite);
            shared_scale = c.scale;
        }
        const float cap = shared_cap > 0.0f ? std::min(shared_cap, hh * 2.0f) : hh * 2.0f;
        c.clear(cell.cx0, cell.cy0, cell.cx1, cell.cy1);
        text(c, cell.button == kSelect ? "SHARE" : "OPTIONS", cx, cy, cap, hw * 2.0f, kWhite);
        break;
    }
    default: break;  // circle, triangle, square, cross: as drawn
    }
}

// The d-pads, for the keyboard: W A S D, the directions each one shows lit,
// the others dim (as the d-pads show theirs).
struct Pad {
    float x0, y0;      // its 10 x 10 box
    const char *lit;   // which of W A S D are lit
};
constexpr Pad kPads[] = {
    {1, 25, "WASD"}, {13, 25, "AD"}, {25, 25, "WS"},              // all, left and right, up and down
    {1, 37, "D"},    {13, 37, "W"},  {25, 37, "A"}, {37, 37, "S"},  // right, up, left, down
};

// W A S D in the box at (x0, y0), 10 wide and `height` tall (10 for a
// d-pad's place, 8 for the stick's), the keys in `lit` lit and the rest dim.
void wasd(Canvas &c, float x0, float y0, float height, const char *lit_keys) {
    struct Key {
        char letter;
        float cx, cy;
    };
    const float hh = height * 0.22f;  // a key's half height
    const Key keys[] = {{'W', x0 + 5.0f, y0 + height * 0.25f},
                        {'A', x0 + 1.65f, y0 + height * 0.74f},
                        {'S', x0 + 5.0f, y0 + height * 0.74f},
                        {'D', x0 + 8.35f, y0 + height * 0.74f}};
    for (const Key &key : keys) {
        const bool lit = std::strchr(lit_keys, key.letter) != nullptr;
        const char label[2] = {key.letter, '\0'};
        if (lit) {
            box(c, key.cx, key.cy, 1.55f, hh, 0.6f, kKeyFace);
            box_outline(c, key.cx, key.cy, 1.55f, hh, 0.6f, 0.35f, kKeyEdge);
            text(c, label, key.cx, key.cy, hh * 1.05f, 2.6f, kKeyInk);
        } else {
            box_outline(c, key.cx, key.cy, 1.55f, hh, 0.6f, 0.35f, kDim);
            text(c, label, key.cx, key.cy, hh * 1.05f, 2.6f, kDim);
        }
    }
}

void wasd(Canvas &c, const Pad &pad) {
    c.clear(pad.x0 - 0.75f, pad.y0 - 0.75f, pad.x0 + 10.75f, pad.y0 + 10.75f);
    wasd(c, pad.x0, pad.y0, 10.0f, pad.lit);
}

// The PSP's stick, for the keyboard: what does its job in this context. It
// moves the unit in Action Mode and the cursor in menus (W A S D), aims in
// Target Mode and moves the academy map's cursor (the mouse: a mouse with
// arrows either side, "move the mouse").
constexpr float kStickX0 = 25.0f, kStickY0 = 15.0f, kStickX1 = 35.0f, kStickY1 = 23.0f;

void stick(Canvas &c, int context) {
    c.clear(24.8f, 14.0f, 36.5f, 24.0f);
    if (context != 1 && context != 2) {  // Action Mode, menus: W A S D
        wasd(c, kStickX0, kStickY0, kStickY1 - kStickY0, "WASD");
        return;
    }
    const float cx = (kStickX0 + kStickX1) / 2.0f, cy = (kStickY0 + kStickY1) / 2.0f;
    const float hh = (kStickY1 - kStickY0) / 2.0f, hw = hh * 0.62f, radius = hw * 0.95f;
    box(c, cx, cy, hw, hh, radius, kKeyFace);
    box_outline(c, cx, cy, hw, hh, radius, 0.4f, kKeyEdge);
    box(c, cx, cy - hh * 0.2f, hw, 0.18f, 0.0f, kKeyEdge);
    box(c, cx, cy - hh * 0.6f, 0.18f, hh * 0.4f, 0.0f, kKeyEdge);
    // Arrowheads pointing out, left and right.
    for (const float side : {-1.0f, 1.0f}) {
        const float tip = cx + side * (hw + 2.4f), base = cx + side * (hw + 0.8f);
        c.fill(std::min(tip, base), cy - 1.3f, std::max(tip, base), cy + 1.3f, kWhite, [=](float x, float y) {
            const float t = (x - base) / (tip - base);  // 0 at the base, 1 at the tip
            return t >= 0.0f && t <= 1.0f && std::abs(y - cy) <= 1.3f * (1.0f - t);
        });
    }
}

// Modern pads in Action Mode: Target Mode is the R shoulder (R1 / RB; the
// game's Square still works too), so Square's prompt shows that (owner,
// 2026-11-09 13:07).
void target_shoulder(Canvas &c, Device device) {
    const Cell &square = kCells[2];
    c.clear(square.cx0, square.cy0, square.cx1, square.cy1);
    const float cx = (square.x0 + square.x1) / 2.0f, cy = (square.y0 + square.y1) / 2.0f;
    const float hw = (square.x1 - square.x0) / 2.0f + 0.6f, hh = 3.0f;
    if (device == Device::Xbox) {
        box(c, cx, cy, hw, hh, hh * 0.9f, kInk);
        box_outline(c, cx, cy, hw, hh, hh * 0.9f, 0.5f, kRim);
        text(c, "RB", cx, cy, hh * 1.05f, hw * 1.5f, kWhite);
    } else {
        box_outline(c, cx, cy, hw, hh, hh * 0.6f, 0.5f, kDim);
        text(c, "R1", cx, cy, hh * 1.05f, hw * 1.5f, kWhite);
    }
}

// Without a texture pack, the sheet's own pictures redrawn smooth (they are
// 1-texel pixel art, white with soft edges): the face buttons, the seven
// d-pads (lit arms solid, the rest outlined dim), the stick and the ":". The
// device's own glyphs are then painted over them as with the pack.
void redraw_originals(Canvas &c) {
    const Rgba white = kWhite;
    // Circle, triangle, square, cross: white outlines 1.1 texels wide.
    c.clear(1, 1, 47.5f, 11);
    ring(c, 6.0f, 6.0f, 4.0f, 1.15f, white);
    polygon(c, {{18.0f, 2.1f}, {22.9f, 9.9f}, {13.1f, 9.9f}}, white, 1.1f);
    box_outline(c, 30.0f, 6.0f, 4.0f, 4.0f, 0.15f, 1.05f, white);
    stroke(c, 38.7f, 2.7f, 45.3f, 9.3f, 1.15f, white);
    stroke(c, 45.3f, 2.7f, 38.7f, 9.3f, 1.15f, white);
    // The d-pads: four arms around the box's centre, pointed inwards.
    for (const Pad &pad : kPads) {
        c.clear(pad.x0 - 0.75f, pad.y0 - 0.75f, pad.x0 + 10.75f, pad.y0 + 10.75f);
        const float cx = pad.x0 + 5.0f, cy = pad.y0 + 5.0f;
        struct Arm {
            char key;
            float dx, dy;  // outwards
        };
        for (const Arm arm : {Arm{'W', 0, -1}, Arm{'S', 0, 1}, Arm{'A', -1, 0}, Arm{'D', 1, 0}}) {
            // The up arm, turned: across (-2..2), out (0.7..5): flat ends out, a point in.
            const auto at = [&](float across, float out) {
                return Point{cx + arm.dx * out - arm.dy * across, cy + arm.dy * out + arm.dx * across};
            };
            const std::vector<Point> shape = {at(-2.0f, 4.9f), at(2.0f, 4.9f), at(2.0f, 2.5f), at(0.0f, 0.6f),
                                              at(-2.0f, 2.5f)};
            if (std::strchr(pad.lit, arm.key) != nullptr) polygon(c, shape, white);
            else polygon(c, shape, kDim, 0.7f);
        }
    }
    // The stick: a ridged cap on a short stem and base.
    c.clear(24.8f, 14.0f, 36.5f, 24.0f);
    c.fill(25.3f, 15.0f, 34.7f, 19.8f, rgb(0xFF, 0xFF, 0xFF, 0.75f), [](float x, float y) {
        const float u = (x - 30.0f) / 4.7f, v = (y - 17.4f) / 2.4f;
        return u * u + v * v <= 1.0f;
    });
    for (const float x : {27.0f, 29.0f, 31.0f, 33.0f}) box(c, x, 17.4f, 0.32f, 1.6f, 0.3f, white);
    box(c, 30.0f, 20.6f, 0.55f, 0.6f, 0.2f, kDim);
    c.fill(27.0f, 21.0f, 33.0f, 23.0f, kDim, [](float x, float y) {
        const float u = (x - 30.0f) / 3.0f, v = (y - 22.0f) / 0.95f;
        return u * u + v * v <= 1.0f;
    });
    // ":"
    c.clear(39.5f, 14.5f, 44.0f, 23.5f);
    disc(c, 41.5f, 16.5f, 0.85f, white);
    disc(c, 41.5f, 21.5f, 0.85f, white);
}

void paint(Canvas &c, Device device, bool redraw) {
    if (redraw) redraw_originals(c);
    const bool shoulder_target = device != Device::Keyboard && controls_preset() == ControlsPreset::Modern &&
                                 controls_context() == 0;
    if (device == Device::PlayStation) {
        for (const Cell &cell : kCells) paint_playstation(c, cell);
        if (shoulder_target) target_shoulder(c, device);
        return;
    }
    if (device == Device::Keyboard) {
        for (const Pad &pad : kPads) wasd(c, pad);
        stick(c, controls_context());
    }
    for (const Cell &cell : kCells) {
        if (device == Device::Xbox) {
            c.clear(cell.cx0, cell.cy0, cell.cx1, cell.cy1);
            switch (cell.button) {
            case kCross: xbox_face(c, cell, "A", rgb(0x6C, 0xC2, 0x4A)); break;
            case kCircle: xbox_face(c, cell, "B", rgb(0xE5, 0x45, 0x40)); break;
            case kSquare: xbox_face(c, cell, "X", rgb(0x3A, 0x8E, 0xEA)); break;
            case kTriangle: xbox_face(c, cell, "Y", rgb(0xF4, 0xBE, 0x2A)); break;
            case kL: pill(c, cell, "LB"); break;
            case kR: pill(c, cell, "RB"); break;
            case kSelect: xbox_small(c, cell, true); break;
            case kStart: xbox_small(c, cell, false); break;
            default: break;
            }
            continue;
        }
        KbmPrompt prompt;
        if (!kbm_prompt(cell.button, prompt)) {  // nothing gives it here: the PlayStation glyph
            paint_playstation(c, cell);
            continue;
        }
        c.clear(cell.cx0, cell.cy0, cell.cx1, cell.cy1);
        if (prompt.cap != nullptr) key_cap(c, cell, prompt.cap);
        else mouse(c, cell, prompt.mouse);
    }
    if (shoulder_target) target_shoulder(c, device);
}

// =============================================================================
// Tutorial pictures (2026-11-12)
// =============================================================================
//
// The tutorials' own pictures are screenshots without buttons; the buttons
// come from small sheets drawn over them, found with VC3_TUTORIAL_CAPTURE in
// the first battle (Europe):
//  - the hint label sheet, 128 x 64 CLUT4 (a dark pill "<button> : <action>"
//    with the action's word below, e.g. "O : Crouch"): its button glyph is
//    painted over with the device's;
//  - the PSP's face buttons and the PSP's SELECT and START, 128 x 64 CLUT8
//    each, with the rings their pages blink over the button meant (a 64 x 64
//    CLUT4 sheet of rings each): all left out, the pages show their text only
//    (owner, 2026-11-12: no controller pictures at all; a drawn controller
//    in their place was tried and dropped).
// Nothing of the game's is shipped: the pictures are painted as they load.

// The texels' FNV-1a 64, as the capture names them.
std::uint64_t texels_hash(const std::uint32_t *texels, std::uint32_t w, std::uint32_t h) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) hash = (hash ^ texels[i]) * 0x100000001b3ull;
    return hash ^ (static_cast<std::uint64_t>(w) << 48u) ^ (static_cast<std::uint64_t>(h) << 32u);
}

constexpr std::uint64_t kPspFaceButtons = 0x8caa9f9b1752f6faull;   // EU 0x09A19370 in the first battle
constexpr std::uint64_t kPspSelectStart = 0x93f4b87220b6bdf5ull;   // EU 0x09A29170
constexpr std::uint64_t kPspFaceRings = 0xbb7af98264615325ull;     // EU 0x09A1B6B0, with the face buttons
constexpr std::uint64_t kPspSelectStartRings = 0xb6fd9f1e71615325ull;  // EU 0x09A2B4B0, with SELECT and START

// PlayStation face button: a dark disc, the symbol in its colour.
void ps_face(Canvas &c, const Cell &cell) {
    const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
    const float r = std::min(cell.x1 - cell.x0, cell.y1 - cell.y0) / 2.0f;
    disc(c, cx, cy, r, kInk);
    ring(c, cx, cy, r, 0.55f, kRim);
    const float k = r * 0.5f, w = std::max(0.45f, r * 0.14f);
    switch (cell.button) {
    case kCircle: ring(c, cx, cy, k * 1.05f, w, rgb(0xE8, 0x50, 0x5B)); break;
    case kCross:
        stroke(c, cx - k, cy - k, cx + k, cy + k, w, rgb(0x7F, 0xA6, 0xE0));
        stroke(c, cx + k, cy - k, cx - k, cy + k, w, rgb(0x7F, 0xA6, 0xE0));
        break;
    case kSquare: box_outline(c, cx, cy, k * 0.9f, k * 0.9f, 0.1f, w, rgb(0xD8, 0x8F, 0xC0)); break;
    case kTriangle:
        polygon(c, {{cx, cy - k * 1.05f}, {cx + k * 1.05f, cy + k * 0.75f}, {cx - k * 1.05f, cy + k * 0.75f}},
                rgb(0x40, 0xC8, 0xA0), w);
        break;
    default: break;
    }
}

// One PSP button's place, drawn for `device`.
// `context`: the keyboard's (as vc3_controls.cpp numbers them); -1: the current one.
void paint_button(Canvas &c, const Cell &cell, Device device, int context = -1) {
    if (device == Device::Xbox) {
        switch (cell.button) {
        case kCross: xbox_face(c, cell, "A", rgb(0x6C, 0xC2, 0x4A)); return;
        case kCircle: xbox_face(c, cell, "B", rgb(0xE5, 0x45, 0x40)); return;
        case kSquare: xbox_face(c, cell, "X", rgb(0x3A, 0x8E, 0xEA)); return;
        case kTriangle: xbox_face(c, cell, "Y", rgb(0xF4, 0xBE, 0x2A)); return;
        case kSelect: xbox_small(c, cell, true); return;
        case kStart: xbox_small(c, cell, false); return;
        default: return;
        }
    }
    if (device == Device::Keyboard) {
        KbmPrompt prompt;
        if (context >= 0 ? kbm_prompt_in(cell.button, context, prompt) : kbm_prompt(cell.button, prompt)) {
            if (prompt.cap != nullptr) key_cap(c, cell, prompt.cap);
            else mouse(c, cell, prompt.mouse);
            return;
        }
    }
    if (cell.button == kSelect || cell.button == kStart) {
        const float cx = (cell.x0 + cell.x1) / 2.0f, cy = (cell.y0 + cell.y1) / 2.0f;
        const float hw = (cell.x1 - cell.x0) / 2.0f, hh = (cell.y1 - cell.y0) / 2.0f;
        box(c, cx, cy, hw, hh, hh * 0.9f, kInk);
        box_outline(c, cx, cy, hw, hh, hh * 0.9f, 0.5f, kRim);
        text(c, cell.button == kSelect ? "SHARE" : "OPTIONS", cx, cy, hh * 0.9f, hw * 1.7f, kWhite);
        return;
    }
    ps_face(c, cell);
}

// The texture (w x h texels) at `scale`, on a square canvas w * scale wide:
// the texture pack's image when it is that shape, else the original enlarged.
Canvas sheet_canvas(const std::uint32_t *texels, std::uint32_t w, std::uint32_t h,
                    const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale) {
    Canvas c;
    if (pack_image != nullptr && pack_image->width >= w && pack_image->width % w == 0u &&
        pack_image->height * w == pack_image->width * h &&
        pack_image->pixels.size() == std::size_t{pack_image->width} * pack_image->height) {
        c.side = pack_image->width;
        c.pixels.assign(static_cast<std::size_t>(c.side) * c.side, 0u);
        std::copy(pack_image->pixels.begin(), pack_image->pixels.end(), c.pixels.begin());
    } else {
        const std::uint32_t scale = std::clamp(wanted_scale, 4u, 8u);
        c.side = w * scale;
        c.pixels.assign(static_cast<std::size_t>(c.side) * c.side, 0u);
        for (std::uint32_t y = 0; y < h * scale; ++y)
            for (std::uint32_t x = 0; x < c.side; ++x) {
                const float u = (x + 0.5f) / static_cast<float>(scale) - 0.5f, v = (y + 0.5f) / static_cast<float>(scale) - 0.5f;
                const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
                const float fx = u - static_cast<float>(ix), fy = v - static_cast<float>(iy);
                float sum[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    const int tx = std::clamp(ix + (k & 1), 0, static_cast<int>(w) - 1);
                    const int ty = std::clamp(iy + (k >> 1), 0, static_cast<int>(h) - 1);
                    const std::uint32_t p = texels[static_cast<std::size_t>(ty) * w + static_cast<std::size_t>(tx)];
                    const float wgt = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
                    const float a = static_cast<float>(p >> 24u) * wgt;
                    sum[0] += static_cast<float>(p & 0xFFu) * a, sum[1] += static_cast<float>((p >> 8u) & 0xFFu) * a;
                    sum[2] += static_cast<float>((p >> 16u) & 0xFFu) * a, sum[3] += a;
                }
                const auto byte = [](float value) {
                    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
                };
                c.pixels[static_cast<std::size_t>(y) * c.side + x] =
                    sum[3] > 0.0f ? byte(sum[0] / sum[3]) | byte(sum[1] / sum[3]) << 8u | byte(sum[2] / sum[3]) << 16u |
                                        byte(sum[3]) << 24u
                                  : 0u;
            }
    }
    c.scale = static_cast<float>(c.side) / static_cast<float>(w);
    return c;
}

void solid(Canvas &c, float x0, float y0, float x1, float y1, const Rgba &colour) {
    c.fill(x0, y0, x1, y1, colour, [](float, float) { return true; });
}

// The hint label sheet: its glyph place, and which PSP button the glyph is.
constexpr int kLabelGlyphX0 = 12, kLabelGlyphY0 = 4, kLabelGlyphSide = 9;
bool label_sheet(const std::uint32_t *texels, std::uint32_t &button, std::uint32_t &pill) {
    const auto at = [&](int x, int y) { return texels[static_cast<std::size_t>(y) * 128u + static_cast<std::size_t>(x)]; };
    const auto opaque = [&](int x, int y) { return (at(x, y) >> 24u) > 128u; };
    const auto bright = [&](int x, int y) { return opaque(x, y) && (at(x, y) & 0xFFu) > 150u; };
    // The pill's pointed left end: its first opaque texel in rows 3..13.
    const int left[] = {6, 5, 4, 3, 2, 1, 2, 3, 4, 5, 6};
    for (int i = 0; i < 11; ++i) {
        const int y = 3 + i;
        if (opaque(left[i] - 1, y) || !opaque(left[i], y)) return false;
    }
    // Dark and opaque after the glyph and the ":", rows 4..12.
    for (int y = 4; y <= 12; ++y)
        for (int x = 30; x < 70; ++x)
            if (!opaque(x, y) || bright(x, y)) return false;
    // The glyph: white texels in its place.
    bool m[kLabelGlyphSide][kLabelGlyphSide]{};
    int count = 0;
    for (int y = 0; y < kLabelGlyphSide; ++y)
        for (int x = 0; x < kLabelGlyphSide; ++x)
            count += (m[y][x] = bright(kLabelGlyphX0 + x, kLabelGlyphY0 + y)) ? 1 : 0;
    if (count < 8) return false;
    pill = at(24, 8);
    const auto row = [&](int y) { int n = 0; for (int x = 0; x < kLabelGlyphSide; ++x) n += m[y][x]; return n; };
    int top = 0, bottom = kLabelGlyphSide - 1;
    while (top < kLabelGlyphSide && row(top) == 0) ++top;
    while (bottom > 0 && row(bottom) == 0) --bottom;
    const int mid = (top + bottom) / 2;
    if (m[mid][4] && row(top) >= 2 && row(bottom) >= 2 && row(mid) <= 3) button = kCross;
    else if (row(top) >= 5 && row(bottom) >= 5) button = kSquare;
    else if (row(top) <= 2 && row(bottom) >= 5) button = kTriangle;
    else button = kCircle;
    return true;
}

// The "<L R>" badge: two white arrow tabs with L and R cut out, 24 x 10
// texels. It is part of many menu sheets (IF_MLX_COMMON, ACD_DRILL (the class
// change menu), ACD_ACADEMY, BRIEFING, OPTION, SLG, VS_INFO and others, at
// different places; 13 sheets found in the disc's MLX images). Found by its
// alpha shape anywhere in a 16- or 256-colour sheet (owner 2026-11-12: on the
// keyboard they showed L and R, not 1 and 2).
constexpr int kBadgeW = 24, kBadgeH = 10;
constexpr const char *kBadgeMask[kBadgeH] = {
    "000000000000000000000000", "000000111110011111000000", "000001111110011111100000",
    "000011011110010000110000", "000111011110010110111000", "001111011110010001111100",
    "000111011110010110111000", "000011000010010110110000", "000001111110011111100000",
    "000000111110011111000000",
};
// Places (the mask's top-left texel) where the badge is: at most 2 of its 240
// texels may differ (edge texels drawn faint in some sheets).
std::vector<std::pair<int, int>> lr_badges(const std::uint32_t *texels, std::uint32_t w, std::uint32_t h) {
    std::vector<std::pair<int, int>> found;
    if (w < static_cast<std::uint32_t>(kBadgeW) || h < static_cast<std::uint32_t>(kBadgeH)) return found;
    const auto solid = [&](int x, int y) { return (texels[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] >> 24u) > 128u; };
    for (int y = 0; y + kBadgeH <= static_cast<int>(h); ++y)
        for (int x = 0; x + kBadgeW <= static_cast<int>(w); ++x) {
            // quick test: the left tab's top edge
            if (!solid(x + 6, y + 1) || !solid(x + 10, y + 1) || solid(x + 11, y + 1)) continue;
            int wrong = 0;
            for (int my = 0; my < kBadgeH && wrong <= 2; ++my)
                for (int mx = 0; mx < kBadgeW; ++mx)
                    if (solid(x + mx, y + my) != (kBadgeMask[my][mx] == '1') && ++wrong > 2) break;
            if (wrong <= 2) found.emplace_back(x, y);
        }
    return found;
}

// The two tabs again, white, with the device's names for L and R in dark ink:
// 1 and 2 on the keyboard (whatever the KBM table gives L and R in menus),
// LB and RB on Xbox, L1 and R1 on PlayStation. (ox, oy): the mask's place.
// The keys are the menus' (context 3): the sheets are menu art.
void paint_lr_badge(Canvas &c, Device device, float ox, float oy) {
    c.clear(ox + 1.0f, oy + 0.4f, ox + 23.0f, oy + 9.6f);
    const Rgba white = rgb(0xFF, 0xFF, 0xFF);
    polygon(c, {{ox + 1.6f, oy + 5.0f}, {ox + 5.6f, oy + 0.9f}, {ox + 11.0f, oy + 0.9f}, {ox + 11.0f, oy + 9.1f}, {ox + 5.6f, oy + 9.1f}}, white);
    polygon(c, {{ox + 13.0f, oy + 0.9f}, {ox + 18.4f, oy + 0.9f}, {ox + 22.4f, oy + 5.0f}, {ox + 18.4f, oy + 9.1f}, {ox + 13.0f, oy + 9.1f}}, white);
    std::string left = "L1", right = "R1";
    if (device == Device::Xbox) left = "LB", right = "RB";
    if (device == Device::Keyboard) {
        KbmPrompt prompt;
        if (kbm_prompt_in(kL, 3, prompt) && prompt.cap != nullptr) left = prompt.cap;
        if (kbm_prompt_in(kR, 3, prompt) && prompt.cap != nullptr) right = prompt.cap;
    }
    text(c, left.c_str(), ox + 8.2f, oy + 5.0f, 4.6f, 5.6f, kKeyInk);
    text(c, right.c_str(), ox + 15.8f, oy + 5.0f, 4.6f, 5.6f, kKeyInk);
}

// The square button's symbol drawn on its own: a 9 x 7 texel outline (2 texels
// thick) with a clear texel all round, which the game draws inside a grey disc
// (IF_MLX_BRIEFING p001 at (93, 62): the "Leaders" heading of the Squad Draft
// menu; owner 2026-11-14). The only sheet of the decoded menu sheets with it.
constexpr int kSquareW = 11, kSquareH = 9;  // with the clear margin
constexpr const char *kSquareMask[kSquareH] = {
    "...........", ".#########.", ".#########.", ".##.....##.", ".##.....##.",
    ".##.....##.", ".#########.", ".#########.", "...........",
};
std::vector<std::pair<int, int>> square_symbols(const std::uint32_t *texels, std::uint32_t w, std::uint32_t h) {
    std::vector<std::pair<int, int>> found;
    if (w < static_cast<std::uint32_t>(kSquareW) || h < static_cast<std::uint32_t>(kSquareH)) return found;
    const auto solid = [&](int x, int y) { return (texels[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] >> 24u) > 128u; };
    for (int y = 0; y + kSquareH <= static_cast<int>(h); ++y)
        for (int x = 0; x + kSquareW <= static_cast<int>(w); ++x) {
            if (!solid(x + 1, y + 1) || !solid(x + 9, y + 7) || solid(x + 5, y + 4)) continue;  // quick test
            bool same = true;
            for (int my = 0; my < kSquareH && same; ++my)
                for (int mx = 0; mx < kSquareW; ++mx)
                    if (solid(x + mx, y + my) != (kSquareMask[my][mx] == '#')) {
                        same = false;
                        break;
                    }
            if (same) found.emplace_back(x + 1, y + 1);  // the outline's top-left texel
            if (found.size() > 4u) return {};  // a pattern, not the symbol
        }
    return found;
}

// The square symbol as the device names that button, white like the original
// (the disc behind it is drawn separately): X on Xbox, the key on the keyboard
// (menus), the square itself on PlayStation. False: left as it is.
bool paint_square_symbol(Canvas &c, Device device, float ox, float oy) {
    std::string label;
    if (device == Device::Xbox) label = "X";
    if (device == Device::Keyboard) {
        KbmPrompt prompt;
        if (kbm_prompt_in(kSquare, 3, prompt) && prompt.cap != nullptr) label = prompt.cap;
    }
    if (label.empty()) return false;
    c.clear(ox - 0.5f, oy - 0.5f, ox + 9.5f, oy + 7.5f);
    text(c, label.c_str(), ox + 4.5f, oy + 3.5f, 7.0f, label.size() > 1u ? 11.0f : 9.0f, kWhite);
    return true;
}

// Any 16- or 256-colour sheet holding the badge: repainted for the device.
gpu::TextureOverrideResult lr_badge_override(const gpu::TextureState &texture, const std::uint32_t *texels,
                                             const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                             gpu::TextureOverrideImage &out) {
    using Result = gpu::TextureOverrideResult;
    if (texture.format != gpu::TextureFormat::Clut4 && texture.format != gpu::TextureFormat::Clut8) return Result::Ignore;
    if (texture.width > 512u || texture.height > texture.width) return Result::Ignore;
    if (texels == nullptr) return Result::Keep;
    const auto places = lr_badges(texels, texture.width, texture.height);
    const auto squares = square_symbols(texels, texture.width, texture.height);
    Prompts &p = prompts();
    const Device device = static_cast<Device>(p.device.load());
    const Device painted =
        device == Device::Keyboard && controls_preset() != ControlsPreset::Modern ? Device::PlayStation : device;
    if (places.empty() && (squares.empty() || painted == Device::PlayStation)) return Result::Ignore;
    if (pack_image == nullptr) return Result::WantsPackImage;
    Canvas c = sheet_canvas(texels, texture.width, texture.height, pack_image, wanted_scale);
    for (const auto &[x, y] : places) paint_lr_badge(c, painted, static_cast<float>(x), static_cast<float>(y));
    int squares_painted = 0;
    for (const auto &[x, y] : squares)
        squares_painted += paint_square_symbol(c, painted, static_cast<float>(x), static_cast<float>(y)) ? 1 : 0;
    out.width = c.side;
    out.height = static_cast<std::uint32_t>(std::lround(c.scale * static_cast<float>(texture.height)));
    out.pixels.assign(c.pixels.begin(), c.pixels.begin() + static_cast<std::ptrdiff_t>(out.width) * out.height);
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 40)
        std::cerr << "[vc3-prompts] L/R badge (" << places.size() << "), square symbol (" << squares_painted << ") in "
                  << texture.width << "x" << texture.height
                  << " sheet at 0x" << std::hex << texture.address << std::dec << " drawn for " << device_name(painted)
                  << " (" << (pack_image->width != 0u ? "over the texture pack's image" : "over the original") << ")\n";
    return Result::Replace;
}

gpu::TextureOverrideResult tutorial_override(const gpu::TextureState &texture, const std::uint32_t *texels,
                                             const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                             gpu::TextureOverrideImage &out) {
    using Result = gpu::TextureOverrideResult;
    // The rings over the PSP pictures: left out with them.
    if (texture.width == 64u && texture.height == 64u && texture.format == gpu::TextureFormat::Clut4) {
        if (texels == nullptr) return Result::Keep;
        const std::uint64_t hash = texels_hash(texels, 64u, 64u);
        if (hash != kPspFaceRings && hash != kPspSelectStartRings) return Result::Ignore;
        out.width = out.height = 64u;
        out.pixels.assign(64u * 64u, 0u);
        std::cerr << "[vc3-prompts] tutorial picture at 0x" << std::hex << texture.address << std::dec
                  << ": the rings over a PSP picture, left out\n";
        return Result::Replace;
    }
    if (texture.width != 128u || texture.height != 64u ||
        (texture.format != gpu::TextureFormat::Clut4 && texture.format != gpu::TextureFormat::Clut8))
        return Result::Ignore;
    if (texels == nullptr) return Result::Keep;
    enum class Kind { Label, Face, SelectStart } kind{};
    std::uint32_t button = 0u, pill = 0u;
    if (texture.format == gpu::TextureFormat::Clut4 && label_sheet(texels, button, pill)) kind = Kind::Label;
    else if (texture.format == gpu::TextureFormat::Clut8) {
        const std::uint64_t hash = texels_hash(texels, 128u, 64u);
        if (hash == kPspFaceButtons) kind = Kind::Face;
        else if (hash == kPspSelectStart) kind = Kind::SelectStart;
        else return Result::Ignore;
    } else {
        return Result::Ignore;
    }
    if (kind != Kind::Label) {
        // A PSP picture: left out (transparent), frame and all.
        out.width = 128u;
        out.height = 64u;
        out.pixels.assign(128u * 64u, 0u);
        std::cerr << "[vc3-prompts] tutorial picture at 0x" << std::hex << texture.address << std::dec << ": "
                  << (kind == Kind::Face ? "the PSP's face buttons" : "the PSP's SELECT and START") << ", left out\n";
        return Result::Replace;
    }
    const Device device = static_cast<Device>(prompt_device());
    if (pack_image == nullptr) return Result::WantsPackImage;
    Canvas c = sheet_canvas(texels, 128u, 64u, pack_image, wanted_scale);
    const char *what = "";
    switch (kind) {
    case Kind::Label: {
        const Rgba fill = rgb(static_cast<int>(pill & 0xFFu), static_cast<int>((pill >> 8u) & 0xFFu),
                              static_cast<int>((pill >> 16u) & 0xFFu));
        solid(c, kLabelGlyphX0 - 0.6f, kLabelGlyphY0 - 0.4f, kLabelGlyphX0 + kLabelGlyphSide + 0.6f,
              kLabelGlyphY0 + kLabelGlyphSide + 0.4f, fill);
        const float x0 = kLabelGlyphX0 - 0.2f, y0 = kLabelGlyphY0 - 0.2f, x1 = x0 + kLabelGlyphSide + 0.4f,
                    y1 = y0 + kLabelGlyphSide + 0.4f;
        // Action Mode's keys: the hints are about it, though a tutorial box is
        // open over it while they show (circle: E, crouch; not Esc).
        paint_button(c, Cell{button, x0, y0, x1, y1, x0, y0, x1, y1}, device, 0);
        what = button == kCircle ? "label (circle)" : button == kCross ? "label (cross)"
             : button == kSquare ? "label (square)" : "label (triangle)";
        break;
    }
    case Kind::Face:
    case Kind::SelectStart:
        break;  // left out above
    }
    out.width = c.side;
    out.height = c.side / 2u;
    out.pixels.assign(c.pixels.begin(), c.pixels.begin() + static_cast<std::ptrdiff_t>(out.width) * out.height);
    static std::atomic<int> told{0};
    if (told.fetch_add(1) < 60)
        std::cerr << "[vc3-prompts] tutorial picture at 0x" << std::hex << texture.address << std::dec << ": " << what
                  << ", drawn for " << device_name(device) << "\n";
    return Result::Replace;
}

gpu::TextureOverrideResult prompt_override(const gpu::TextureState &texture, const std::uint32_t *texels,
                                           const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                           gpu::TextureOverrideImage &out) {
    using Result = gpu::TextureOverrideResult;
    if (texture.width != kAtlasSide || texture.height != kAtlasSide || texture.format != gpu::TextureFormat::Clut4)
        return Result::Ignore;
    if (texels == nullptr) return Result::Keep;  // a candidate: decoded at once, then looked at
    if (const std::uint64_t mask = mask_hash(texels); mask != kAtlasMask && mask != kAtlasMaskVc3)
        return Result::Ignore;
    Prompts &p = prompts();
    const Device device = static_cast<Device>(p.device.load());
    // Classic keys are PortableKit's own, which the prompts do not know: the
    // PlayStation glyphs.
    const Device painted =
        device == Device::Keyboard && controls_preset() != ControlsPreset::Modern ? Device::PlayStation : device;
    if (pack_image == nullptr) return Result::WantsPackImage;
    Canvas c;
    bool from_original = false;
    if (pack_image->width >= kAtlasSide && pack_image->width == pack_image->height &&
        pack_image->width % kAtlasSide == 0u && pack_image->pixels.size() == std::size_t{pack_image->width} * pack_image->width) {
        c.side = pack_image->width;
        c.pixels = pack_image->pixels;
    } else {
        // No pack: the original, enlarged smoothly (bilinear, colour weighted
        // by alpha); its pictures are then redrawn (redraw_originals).
        from_original = true;
        const std::uint32_t scale = std::clamp(wanted_scale, 4u, 8u);
        c.side = kAtlasSide * scale;
        c.pixels.resize(static_cast<std::size_t>(c.side) * c.side);
        const auto texel = [&](int x, int y) {
            x = std::clamp(x, 0, static_cast<int>(kAtlasSide) - 1), y = std::clamp(y, 0, static_cast<int>(kAtlasSide) - 1);
            return texels[static_cast<std::size_t>(y) * kAtlasSide + static_cast<std::size_t>(x)];
        };
        for (std::uint32_t y = 0; y < c.side; ++y)
            for (std::uint32_t x = 0; x < c.side; ++x) {
                const float u = (x + 0.5f) / static_cast<float>(scale) - 0.5f, v = (y + 0.5f) / static_cast<float>(scale) - 0.5f;
                const int ix = static_cast<int>(std::floor(u)), iy = static_cast<int>(std::floor(v));
                const float fx = u - static_cast<float>(ix), fy = v - static_cast<float>(iy);
                float sum[4] = {0, 0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    const std::uint32_t p = texel(ix + (k & 1), iy + (k >> 1));
                    const float wgt = ((k & 1) ? fx : 1.0f - fx) * ((k >> 1) ? fy : 1.0f - fy);
                    const float a = static_cast<float>(p >> 24u) * wgt;
                    sum[0] += static_cast<float>(p & 0xFFu) * a, sum[1] += static_cast<float>((p >> 8u) & 0xFFu) * a;
                    sum[2] += static_cast<float>((p >> 16u) & 0xFFu) * a, sum[3] += a;
                }
                const auto byte = [](float value) {
                    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
                };
                c.pixels[static_cast<std::size_t>(y) * c.side + x] =
                    sum[3] > 0.0f ? byte(sum[0] / sum[3]) | byte(sum[1] / sum[3]) << 8u | byte(sum[2] / sum[3]) << 16u |
                                        byte(sum[3]) << 24u
                                  : 0u;
            }
    }
    c.scale = static_cast<float>(c.side) / static_cast<float>(kAtlasSide);
    paint(c, painted, from_original);
    out.width = out.height = c.side;
    out.pixels = std::move(c.pixels);
    if (++p.atlases <= 40u)
        std::cerr << "[vc3-prompts] atlas at 0x" << std::hex << texture.address << std::dec << " drawn for "
                  << device_name(painted) << " at " << c.side << "x" << c.side
                  << (pack_image->width != 0u ? " over the texture pack's image" : " over the original") << "\n";
    return Result::Replace;
}

// =============================================================================
// Tutorial image capture (VC3_TUTORIAL_CAPTURE=<folder>)
// =============================================================================
//
// To find the tutorial pictures that show the PSP or its buttons: every
// texture the game makes (its first look, by content) is written to the folder
// as RGBA PAM, with index.txt saying when, and what the last paged text box
// (tutorials) said by then. Framebuffer copies and the font sheets are left out.

struct Capture {
    bool on = false;
    std::filesystem::path folder;
    std::ofstream index;
    std::unordered_set<std::uint64_t> seen;
    std::uint32_t written = 0u;
    std::string last_text;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), last_text_at{};
    std::mutex lock;
};
Capture &capture() {
    static Capture c;
    static const bool opened = [] {
        if (const char *folder = env("TUTORIAL_CAPTURE"); folder != nullptr && *folder != '\0') {
            std::error_code error;
            c.folder = std::filesystem::path(folder);
            std::filesystem::create_directories(c.folder, error);
            c.index.open(c.folder / "index.txt", std::ios::app);
            c.on = static_cast<bool>(c.index);
            std::cerr << "[vc3-capture] " << (c.on ? "textures go to " : "cannot write to ") << c.folder.string() << "\n";
        }
        return true;
    }();
    (void)opened;
    return c;
}

double seconds_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

void capture_texture(const gpu::TextureState &texture, const std::uint32_t *texels) {
    Capture &c = capture();
    const std::uint32_t w = texture.width, h = texture.height;
    if (w * h < 32u * 32u || (w == 512u && h == 272u) || texture.address == 0x09B57FC0u || texture.address == 0x09B98000u)
        return;
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) hash = (hash ^ texels[i]) * 0x100000001b3ull;
    hash ^= (static_cast<std::uint64_t>(w) << 48u) ^ (static_cast<std::uint64_t>(h) << 32u);
    const std::lock_guard<std::mutex> guard(c.lock);
    if (c.written >= 4000u || !c.seen.insert(hash).second) return;
    char name[96];
    std::snprintf(name, sizeof name, "%04u-%08x-%ux%u-f%u-%016llx.pam", ++c.written, texture.address, w, h,
                  static_cast<unsigned>(texture.format), static_cast<unsigned long long>(hash));
    std::ofstream out(c.folder / name, std::ios::binary);
    out << "P7\nWIDTH " << w << "\nHEIGHT " << h << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i) {
        const std::uint32_t p = texels[i];
        const char rgba[4] = {static_cast<char>(p & 0xFFu), static_cast<char>((p >> 8u) & 0xFFu),
                              static_cast<char>((p >> 16u) & 0xFFu), static_cast<char>(p >> 24u)};
        out.write(rgba, 4);
    }
    c.index << std::fixed << std::setprecision(1) << seconds_since(c.start) << " s  " << name;
    if (!c.last_text.empty())
        c.index << "  (" << std::setprecision(1) << seconds_since(c.last_text_at) << " s after: " << c.last_text << ")";
    c.index << "\n";
    c.index.flush();
}

// The prompt atlas, then the tutorial pictures.
gpu::TextureOverrideResult prompts_and_tutorials(const gpu::TextureState &texture, const std::uint32_t *texels,
                                                 const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                                 gpu::TextureOverrideImage &out) {
    const auto result = prompt_override(texture, texels, pack_image, wanted_scale, out);
    if (result != gpu::TextureOverrideResult::Ignore) return result;
    const auto badge = lr_badge_override(texture, texels, pack_image, wanted_scale, out);
    if (badge != gpu::TextureOverrideResult::Ignore) return badge;
    if (env("NO_TUTORIAL_PICTURES") != nullptr) return result;
    return tutorial_override(texture, texels, pack_image, wanted_scale, out);
}

gpu::TextureOverrideResult capture_override(const gpu::TextureState &texture, const std::uint32_t *texels,
                                            const gpu::TextureOverrideImage *pack_image, std::uint32_t wanted_scale,
                                            gpu::TextureOverrideImage &out) {
    using Result = gpu::TextureOverrideResult;
    const Result prompt = prompts().enabled ? prompts_and_tutorials(texture, texels, pack_image, wanted_scale, out) : Result::Ignore;
    if (texels == nullptr) return prompt == Result::Ignore ? Result::Keep : prompt;  // decoded, then captured
    if (pack_image == nullptr || prompt != Result::WantsPackImage) capture_texture(texture, texels);
    return prompt;
}

}  // namespace

void capture_note_text(const std::string &cp932) {
    Capture &c = capture();
    if (!c.on) return;
    std::string text;
    for (std::size_t i = 0; i < cp932.size() && text.size() < 140u; ++i) {
        const unsigned char b = static_cast<unsigned char>(cp932[i]);
        if (cp932.compare(i, 3, "@fc") == 0) {  // colour codes: @fcXXXXXXXX: and @fci:
            const std::size_t end = cp932.find(':', i);
            if (end != std::string::npos) { i = end; continue; }
        }
        if (b == '\n' || b == '\r') text += " / ";
        else if (b >= 0x20u && b < 0x7Fu) text += static_cast<char>(b);
        else if ((b >= 0x81u && b < 0xA0u) || b >= 0xE0u) { text += '?'; ++i; }
    }
    const std::lock_guard<std::mutex> guard(c.lock);
    if (text == c.last_text) return;
    c.last_text = text;
    c.last_text_at = std::chrono::steady_clock::now();
    c.index << std::fixed << std::setprecision(1) << seconds_since(c.start) << " s  TEXT: " << text << "\n";
    c.index.flush();
}

void prompts_changed() {
    Prompts &p = prompts();
    choose(p);
    p.variant = -1;  // the sheet is painted again for the style now chosen
    gpu::bump_texture_override_generation();
    std::cerr << "[vc3-prompts] now " << (p.forced ? device_name(static_cast<Device>(p.device.load())) : "automatic") << "\n";
}

bool keyboard_prompts() {
    const Prompts &p = prompts();
    return p.enabled && p.device.load() == static_cast<int>(Device::Keyboard) &&
           controls_preset() == ControlsPreset::Modern;
}

int prompt_device() {
    const Prompts &p = prompts();
    if (!p.enabled) return static_cast<int>(Device::PlayStation);
    const int device = p.device.load();
    if (device == static_cast<int>(Device::Keyboard) && controls_preset() != ControlsPreset::Modern)
        return static_cast<int>(Device::PlayStation);
    return device;
}

void install_prompts() {
    // VC3's prompt atlas is VC2's layout (kAtlasMaskVc3, 2026-11-16): the same
    // painting. VC2's tutorial pictures are VC2's own and are not found in VC3.
    if (capture().on) gpu::set_texture_override(&capture_override);
    if (!prompts().enabled) {
        std::cerr << "[vc3-prompts] off (VC3_NO_PROMPTS)\n";
        return;
    }
    if (!capture().on) gpu::set_texture_override(&prompts_and_tutorials);
    std::cerr << "[vc3-prompts] button prompts follow the device in use"
              << (prompts().forced ? std::string(" (fixed: ") + device_name(static_cast<Device>(prompts().device.load())) + ")"
                                   : std::string())
              << "\n";
}

void glyph_frame() {
    update_device();
    scan_sheet();
}

}  // namespace portablekit::vc3

extern "C" void vc3_glyph_begin(std::uint32_t font, std::uint32_t flags, std::uint32_t code,
                                std::uint32_t table) noexcept {
    using namespace portablekit::vc3;
    text_glyph_begin(font, flags, code, table);  // a button glyph of ours (vc3_text.cpp)
    if (trace_folder() == nullptr || font == 0u) return;
    GlyphTrace &t = trace();
    t.code = code & 0xFFFFu;
    t.table = table & 0xFFFFu;
    t.flags = flags;
    t.pending = true;
    if (t.seen.count((t.table << 16u) | t.code) != 0u) return;
    auto &memory = portablekit::kernel().runtime().memory();
    const std::uint32_t surface = memory.load32(font + 3524u);
    t.before.assign(kSurfaceWidth * kSurfaceRows, 0u);
    if (surface != 0u)
        for (std::uint32_t i = 0; i < kSurfaceWidth * kSurfaceRows; ++i) t.before[i] = memory.load32(surface + i * 4u);
}

extern "C" void vc3_glyph_end(std::uint32_t font) noexcept {
    using namespace portablekit::vc3;
    text_glyph_end(font);
    GlyphTrace &t = trace();
    if (!t.pending || font == 0u) return;
    t.pending = false;
    ++t.staged;
    const std::uint32_t key = (t.table << 16u) | t.code;
    if (!t.seen.insert(key).second) return;
    auto &memory = portablekit::kernel().runtime().memory();
    const std::uint32_t surface = memory.load32(font + 3524u);
    const std::uint32_t size = memory.load32(font + 3556u);  // width (+3556) and height (+3558), halfwords
    // Where the glyph went: the pixels that changed since vc3_glyph_begin.
    std::uint32_t x0 = kSurfaceWidth, y0 = kSurfaceRows, x1 = 0u, y1 = 0u;
    if (surface != 0u && t.before.size() == kSurfaceWidth * kSurfaceRows)
        for (std::uint32_t y = 0; y < kSurfaceRows; ++y)
            for (std::uint32_t x = 0; x < kSurfaceWidth; ++x)
                if (memory.load32(surface + (y * kSurfaceWidth + x) * 4u) != t.before[y * kSurfaceWidth + x]) {
                    x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x + 1u), y1 = std::max(y1, y + 1u);
                }
    std::cerr << "[vc3-glyph] code 0x" << hex4(t.code) << " table " << t.table << " flags 0x" << std::hex << t.flags
              << std::dec << ", glyph " << (size & 0xFFFFu) << "x" << (size >> 16u) << ", surface 0x" << std::hex
              << surface << std::dec;
    if (x1 == 0u) {
        std::cerr << ": nothing changed (" << t.seen.size() << " distinct)\n";
        return;
    }
    // A whole 16x16 cell around the change.
    const std::uint32_t cx = x0 / 16u * 16u, cy = y0 / 16u * 16u;
    const std::uint32_t width = std::min(kSurfaceWidth - cx, std::max(16u, (x1 - cx + 15u) / 16u * 16u));
    const std::uint32_t height = std::min(kSurfaceRows - cy, std::max(16u, (y1 - cy + 15u) / 16u * 16u));
    std::cerr << ", changed " << x0 << "," << y0 << ".." << x1 << "," << y1 << " (" << t.seen.size() << " distinct)\n";
    const std::string path = std::string(trace_folder()) + "/glyph-" + hex4(t.code) + "-" + std::to_string(t.table) +
                             ".pam";
    std::ofstream out(path, std::ios::binary);
    out << "P7\nWIDTH " << width << "\nHEIGHT " << height << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint32_t texel = memory.load32(surface + ((cy + y) * kSurfaceWidth + cx + x) * 4u);
            const char rgba[4] = {static_cast<char>(texel & 0xFFu), static_cast<char>((texel >> 8u) & 0xFFu),
                                  static_cast<char>((texel >> 16u) & 0xFFu), static_cast<char>(texel >> 24u)};
            out.write(rgba, 4);
        }
}
