// VC2's text font (sceLibFont), host code of the port's own.
//
// The game draws its own bitmap font for almost everything; sceLibFont is used
// for text the player types: unit, vehicle and squad names (Place Squad's
// "Tank G", "Squad G"; 2026-11-07 trace, `work/font-trace-log.txt`). The PSP
// draws those with its system font (FTT-NewRodin Pro Latin / Japanese, 10.125
// points at 128 dpi: an 18-pixel em), which lives in the console's flash and
// is not on the disc. This file answers the game's sceLibFont calls itself,
// with glyphs from a font installed with Windows, drawn by Windows' own glyph
// rasteriser (GDI GetGlyphOutline), so it needs neither PortableKit's font
// layer nor a bundled font or library.
//
// How the game lays out a name (EU 0x0880D3C0, one call per character, into a
// 128x32 or 256x32 8-bit buffer that becomes a texture):
//   - Reads the font info and the character's info.
//   - Baseline row b = round((max base Y + max descender) / 64) + 2.
//   - The glyph's bitmap goes at x = pen + bitmap left, y = b - bitmap top.
//   - The pen then moves by bitmap left + bitmap width + 1; a space moves it by
//     its advance + 1. '_' is drawn by the game as a line.
// So what places the text is the bitmap metrics and the base Y and descender
// in the font info, and they must agree: PortableKit reported a made-up
// 20x20 cell (ascender 15, descender -5, so b = 12) with capitals 14 rows
// tall, which cut off their top two rows and set names about 4 pixels high
// (the "Iank G" in Place Squad). Here the font info is taken from the real
// glyphs at the PSP's size: ascender = tallest glyph top, descender = lowest
// glyph bottom, and base Y is set so b is one row above the tallest top.
//
// Face: VC3_FONT=<installed face name> if given; else Yu Gothic UI, Meiryo UI,
// Meiryo, MS Gothic (Latin and Japanese; Windows 10 and 11 have Yu Gothic UI).
// A character the face lacks comes from the next face that has it.
// VC3_PORTABLEKIT_FONT=1 keeps PortableKit's font layer; VC3_TRACE_FONT=1 logs
// every call.

#include "vc3_font.hpp"
#include "vc3_region.hpp"

#include "profile.hpp"
#include "hle/hle_common.hpp"
#include "kernel/kernel.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <cwchar>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace portablekit::vc3 {
namespace {

// The PSP system font's size: 10.125 points at 128 dots per inch.
constexpr float kEmPixels = 10.125f * 128.0f / 72.0f;  // 18

constexpr std::uint32_t kLibraryHandle = 0x00F0C000u;
constexpr std::uint32_t kFontHandle = 0x00F0C100u;

bool trace_enabled() {
    static const bool enabled = env("TRACE_FONT") != nullptr;
    return enabled;
}

struct Glyph {
    bool found = false;
    int width = 0, height = 0;  // bitmap
    int left = 0, top = 0;      // bitmap's top-left from the pen on the baseline (top: rows above it)
    float advance = 0.0f;
    std::vector<std::uint8_t> pixels;  // width * height coverage, 0..255
};

struct FontInfo {
    int ascender = 0, descender = 0;  // tallest top, lowest bottom (negative below the baseline)
    int max_width = 0, max_height = 0, max_advance = 0;
    int base_y = 0;                   // reported so the game's baseline row is ascender + 1
};

#if defined(_WIN32)
class Faces {
public:
    bool ready() {
        if (tried_) return !faces_.empty();
        tried_ = true;
        dc_ = CreateCompatibleDC(nullptr);
        if (dc_ == nullptr) return false;
        std::vector<std::wstring> names;
        if (const char *chosen = env("FONT")) names.push_back(widen(chosen));
        for (const wchar_t *name : {L"Yu Gothic UI", L"Meiryo UI", L"Meiryo", L"MS Gothic"}) names.emplace_back(name);
        for (const std::wstring &name : names) {
            HFONT font = CreateFontW(-static_cast<int>(std::lround(kEmPixels)), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
                                     FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS,
                                     ANTIALIASED_QUALITY, DEFAULT_PITCH, name.c_str());
            if (font == nullptr) continue;
            SelectObject(dc_, font);
            wchar_t selected[LF_FACESIZE] = {};
            GetTextFaceW(dc_, LF_FACESIZE, selected);
            // Not installed: Windows substituted another face (a match may add
            // a style to the name, e.g. "Yu Gothic UI Semibold").
            if (_wcsnicmp(selected, name.c_str(), name.size()) != 0) {
                DeleteObject(font);
                continue;
            }
            faces_.push_back(font);
            if (face_names_.empty()) face_names_ = narrow(name);
            else face_names_ += ", " + narrow(name);
        }
        std::cerr << "[vc3-font] " << (faces_.empty() ? std::string("no font found; names are not drawn")
                                                       : "names drawn with " + face_names_ + " at 18 px")
                  << "\n";
        return !faces_.empty();
    }

    const Glyph &glyph(std::uint32_t code) {
        auto found = glyphs_.find(code);
        if (found != glyphs_.end()) return found->second;
        Glyph &g = glyphs_[code];
        if (!ready() || code > 0xFFFFu) return g;
        const wchar_t character = static_cast<wchar_t>(code);
        for (HFONT font : faces_) {
            SelectObject(dc_, font);
            WORD index = 0xFFFFu;
            if (GetGlyphIndicesW(dc_, &character, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) == GDI_ERROR ||
                index == 0xFFFFu)
                continue;
            render(character, g);
            return g;
        }
        return g;
    }

    const std::string &names() const { return face_names_; }

private:
    void render(wchar_t character, Glyph &g) {
        static const MAT2 identity = {{0, 1}, {0, 0}, {0, 0}, {0, 1}};
        GLYPHMETRICS metrics{};
        const DWORD size = GetGlyphOutlineW(dc_, character, GGO_GRAY8_BITMAP, &metrics, 0, nullptr, &identity);
        if (size == GDI_ERROR) return;
        g.found = true;
        g.advance = static_cast<float>(metrics.gmCellIncX);
        if (size == 0u) return;  // a space: no ink
        std::vector<std::uint8_t> buffer(size);
        if (GetGlyphOutlineW(dc_, character, GGO_GRAY8_BITMAP, &metrics, size, buffer.data(), &identity) == GDI_ERROR)
            return;
        g.width = static_cast<int>(metrics.gmBlackBoxX);
        g.height = static_cast<int>(metrics.gmBlackBoxY);
        g.left = metrics.gmptGlyphOrigin.x;
        g.top = metrics.gmptGlyphOrigin.y;
        const int pitch = (g.width + 3) & ~3;  // rows are DWORD-aligned; levels 0..64
        g.pixels.assign(static_cast<std::size_t>(g.width) * g.height, 0u);
        for (int y = 0; y < g.height; ++y)
            for (int x = 0; x < g.width; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * pitch + x;
                if (at >= buffer.size()) continue;
                g.pixels[static_cast<std::size_t>(y) * g.width + x] =
                    static_cast<std::uint8_t>(std::min(255, buffer[at] * 255 / 64));
            }
    }

    static std::wstring widen(const char *text) {
        const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        std::wstring out(static_cast<std::size_t>(std::max(length, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), length);
        out.resize(std::wcslen(out.c_str()));
        return out;
    }
    static std::string narrow(const std::wstring &text) {
        const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<std::size_t>(std::max(length, 1)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
        out.resize(std::strlen(out.c_str()));
        return out;
    }

    bool tried_ = false;
    HDC dc_ = nullptr;
    std::vector<HFONT> faces_;
    std::string face_names_;
    std::unordered_map<std::uint32_t, Glyph> glyphs_;
};
#else
class Faces {  // the port targets Windows; elsewhere names are not drawn
public:
    bool ready() { return false; }
    const Glyph &glyph(std::uint32_t) { return empty_; }
    const std::string &names() const { return none_; }

private:
    Glyph empty_;
    std::string none_;
};
#endif

Faces &faces() {
    static Faces f;
    return f;
}

// The font info, from the glyphs the game's names use at the PSP's size.
const FontInfo &font_info() {
    static FontInfo info;
    static bool made = false;
    if (made) return info;
    made = true;
    const auto take = [&](std::uint32_t code) {
        const Glyph &g = faces().glyph(code);
        if (!g.found) return;
        if (g.width > 0) {
            info.ascender = std::max(info.ascender, g.top);
            info.descender = std::min(info.descender, g.top - g.height);
            info.max_width = std::max(info.max_width, g.width + std::max(g.left, 0));
        }
        info.max_advance = std::max(info.max_advance, static_cast<int>(std::ceil(g.advance)));
    };
    for (std::uint32_t code = 0x20u; code < 0x7Fu; ++code) take(code);
    for (std::uint32_t code : {0x3042u, 0x3041u, 0x30A2u, 0x30FCu, 0x6F22u, 0x5B57u, 0x8ECAu}) take(code);  // kana, kanji
    info.max_height = info.ascender - info.descender;
    // The game's baseline row is base Y + descender + 2; one row above the
    // tallest glyph top.
    info.base_y = info.ascender + 1 - 2 - info.descender;
    return info;
}

std::uint32_t fixed26(float value) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value * 64.0f))); }

void store_float(psprecomp::GuestMemory &memory, std::uint32_t address, float value) {
    memory.store32(address, std::bit_cast<std::uint32_t>(value));
}

// SceFontInfo: ten maxima as 26.6 fixed point, the same as floats, then the
// largest bitmap's width and height, the glyph count and the shadow map size.
void write_font_info(psprecomp::GuestMemory &memory, std::uint32_t address) {
    const FontInfo &f = font_info();
    const float values[10] = {
        static_cast<float>(f.max_width),     // max glyph width
        static_cast<float>(f.max_height),    // max glyph height
        static_cast<float>(f.ascender),      // max ascender
        static_cast<float>(f.descender),     // max descender
        0.0f,                                // max left X
        static_cast<float>(f.base_y),        // max base Y
        0.0f,                                // min centre X
        static_cast<float>(f.ascender),      // max top Y
        static_cast<float>(f.max_advance),   // max advance X
        static_cast<float>(f.max_height),    // max advance Y
    };
    for (std::uint32_t i = 0; i < 10u; ++i) {
        memory.store32(address + i * 4u, fixed26(values[i]));
        store_float(memory, address + 40u + i * 4u, values[i]);
    }
    memory.store16(address + 80u, static_cast<std::uint16_t>(f.max_width));
    memory.store16(address + 82u, static_cast<std::uint16_t>(f.max_height));
    memory.store32(address + 84u, 0x10000u);
    memory.store32(address + 88u, 0u);
}

// SceFontCharInfo (60 bytes): bitmap size and bearings in pixels, then in 26.6
// with the ascender, descender and advances.
void write_char_info(psprecomp::GuestMemory &memory, std::uint32_t address, const Glyph &g) {
    const FontInfo &f = font_info();
    const auto put = [&](std::uint32_t offset, std::uint32_t value) { memory.store32(address + offset, value); };
    put(0u, static_cast<std::uint32_t>(g.width));
    put(4u, static_cast<std::uint32_t>(g.height));
    put(8u, static_cast<std::uint32_t>(g.left));
    put(12u, static_cast<std::uint32_t>(g.top));
    put(16u, fixed26(static_cast<float>(g.width)));
    put(20u, fixed26(static_cast<float>(g.height)));
    put(24u, fixed26(static_cast<float>(f.ascender)));
    put(28u, fixed26(static_cast<float>(f.descender)));
    put(32u, fixed26(static_cast<float>(g.left)));
    put(36u, fixed26(static_cast<float>(g.top)));
    put(40u, 0u);
    put(44u, fixed26(static_cast<float>(g.top)));
    put(48u, fixed26(g.advance));
    put(52u, fixed26(static_cast<float>(f.max_height)));
    memory.store16(address + 56u, 0u);
    memory.store16(address + 58u, 0u);
}

struct Clip {
    std::int32_t x = 0, y = 0, width = 0x7FFFFFFF, height = 0x7FFFFFFF;
};

std::int32_t floor64(std::int32_t value) { return value >= 0 ? value / 64 : -((-value + 63) / 64); }

// SceFontGlyphImage: pixel format, x and y (26.6, the bitmap's top-left),
// buffer width, height and bytes per line (16-bit), buffer address. Ink only
// darkens, so a glyph drawn twice over itself stays as it is.
void draw_glyph(psprecomp::GuestMemory &memory, std::uint32_t image, std::uint32_t code, const Clip &clip) {
    const std::uint32_t format = memory.load32(image);
    const std::int32_t x0 = floor64(static_cast<std::int32_t>(memory.load32(image + 4u)));
    const std::int32_t y0 = floor64(static_cast<std::int32_t>(memory.load32(image + 8u)));
    const std::uint32_t width = memory.load16(image + 12u), height = memory.load16(image + 14u);
    const std::uint32_t pitch = memory.load16(image + 16u);
    const std::uint32_t buffer = memory.load32(image + 20u);
    const Glyph &g = faces().glyph(code);
    if (buffer == 0u || g.pixels.empty()) return;
    for (int row = 0; row < g.height; ++row) {
        const std::int32_t y = y0 + row;
        if (y < 0 || static_cast<std::uint32_t>(y) >= height || y < clip.y || y - clip.y >= clip.height) continue;
        const std::uint32_t line = buffer + static_cast<std::uint32_t>(y) * pitch;
        for (int column = 0; column < g.width; ++column) {
            const std::int32_t x = x0 + column;
            if (x < 0 || static_cast<std::uint32_t>(x) >= width || x < clip.x || x - clip.x >= clip.width) continue;
            const std::uint8_t ink = g.pixels[static_cast<std::size_t>(row) * g.width + column];
            if (ink == 0u) continue;
            switch (format) {
            case 0u:    // 4 bits, low nibble first
            case 1u: {  // 4 bits, high nibble first
                const std::uint32_t at = line + static_cast<std::uint32_t>(x) / 2u;
                const std::uint8_t byte = memory.load8(at);
                const bool high = (format == 0u) == ((x & 1) != 0);
                const std::uint8_t old = high ? static_cast<std::uint8_t>(byte >> 4u) : static_cast<std::uint8_t>(byte & 0x0Fu);
                const std::uint8_t nibble = std::max(old, static_cast<std::uint8_t>(ink >> 4u));
                memory.store8(at, high ? static_cast<std::uint8_t>((byte & 0x0Fu) | (nibble << 4u))
                                       : static_cast<std::uint8_t>((byte & 0xF0u) | nibble));
                break;
            }
            case 2u: {  // 8 bits
                const std::uint32_t at = line + static_cast<std::uint32_t>(x);
                memory.store8(at, std::max(memory.load8(at), ink));
                break;
            }
            case 3u: {  // 24 bits
                const std::uint32_t at = line + static_cast<std::uint32_t>(x) * 3u;
                const std::uint8_t value = std::max(memory.load8(at), ink);
                for (std::uint32_t k = 0; k < 3u; ++k) memory.store8(at + k, value);
                break;
            }
            default: {  // 32 bits: white, ink in alpha
                const std::uint32_t at = line + static_cast<std::uint32_t>(x) * 4u;
                const std::uint8_t value = std::max(static_cast<std::uint8_t>(memory.load32(at) >> 24u), ink);
                memory.store32(at, (static_cast<std::uint32_t>(value) << 24u) | 0x00FFFFFFu);
                break;
            }
            }
        }
    }
}

void trace(const char *text) {
    if (trace_enabled()) std::cerr << "[vc3-font] " << text << "\n";
}

// VC3_FONT_DUMP=<file>: saves the game's own bitmap font (odin_font_16.bf1)
// as its text system holds it, to identify the typeface. The raw file is gone
// once loaded (2026-11-07: no "SFNT" chunk chain left in memory), so the font
// object is saved instead: Kurofune's font class (constructor EU 0x08BC8C0C,
// called by the FTL text system's startup at 0x0884BC68; 0xE30 bytes; its
// class pointer, EU 0x08D2F5F0, at +0x10), followed by the memory each pointer
// in it points to (up to 512 KB each). File: records of address, length and
// bytes (little-endian 32-bit address and length).
constexpr std::uint32_t kEuFontClass = 0x08D2F5F0u;

bool dump_game_font(psprecomp::GuestMemory &memory, const char *path, std::uint32_t font_class) {
    constexpr std::uint32_t kStart = 0x08800000u, kEnd = 0x0A000000u, kObject = 0xE30u, kBlock = 0x80000u;
    std::vector<std::uint32_t> objects;
    for (std::uint32_t at = kStart + 0x10u; at + kObject < kEnd; at += 4u)
        if (memory.load32(at) == font_class) objects.push_back(at - 0x10u);
    if (objects.empty()) return false;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> blocks;
    for (std::uint32_t object : objects) {
        blocks.emplace_back(object, kObject);
        for (std::uint32_t offset = 0; offset < kObject; offset += 4u) {
            const std::uint32_t pointer = memory.load32(object + offset) & ~3u;
            if (pointer < kStart || pointer >= kEnd || pointer == font_class) continue;
            blocks.emplace_back(pointer, std::min(kBlock, kEnd - pointer));
        }
    }
    std::sort(blocks.begin(), blocks.end());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> merged;  // overlapping blocks joined
    for (const auto &[start, length] : blocks) {
        if (!merged.empty() && start <= merged.back().first + merged.back().second) {
            const std::uint32_t end = std::max(merged.back().first + merged.back().second, start + length);
            merged.back().second = end - merged.back().first;
        } else {
            merged.emplace_back(start, length);
        }
    }
    std::FILE *file = std::fopen(path, "wb");
    if (file == nullptr) return false;
    std::uint64_t total = 0u;
    const auto put32 = [&](std::uint32_t value) {
        for (int k = 0; k < 4; ++k) std::fputc(static_cast<int>((value >> (8 * k)) & 0xFFu), file);
    };
    for (const auto &[start, length] : merged) {
        if (total > 0x2000000u) break;  // 32 MB at most
        put32(start);
        put32(length);
        for (std::uint32_t i = 0; i < length; ++i) std::fputc(memory.load8(start + i), file);
        total += length;
    }
    std::fclose(file);
    std::cerr << "[vc3-font] " << objects.size() << " font object(s) (first at 0x" << std::hex << objects.front()
              << std::dec << ") and " << merged.size() << " blocks they point to (" << total << " bytes) written to "
              << path << "\n";
    return true;
}

}  // namespace

void register_font(HleRegistrar &hle) {
    if (const char *path = env("FONT_DUMP")) {
        HleFunction set_frame_buf = hle.bound_function("sceDisplay", 0x289D82FEu);
        if (set_frame_buf)
            hle.bind("sceDisplay", 0x289D82FEu,
                     [inner = std::move(set_frame_buf), path = std::string(path), flips = std::uint64_t{0},
                      done = false](Runtime &rt, AllegrexContext &ctx) mutable {
                         if (!done && ++flips % 300u == 0u) done = dump_game_font(rt.memory(), path.c_str(), at(kEuFontClass));
                         inner(rt, ctx);
                     });
    }
    if (env("PORTABLEKIT_FONT") != nullptr) return;
    constexpr const char *kLibrary = "sceLibFont";
    // NIDs: the first four bytes of the SHA-1 of each name.
    hle.bind(kLibrary, 0x67F17ED7u, [](Runtime &rt, AllegrexContext &ctx) {  // sceFontNewLib(params, *error)
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        faces().ready();
        trace("NewLib");
        kernel().finish(ctx, kLibraryHandle);
    });
    hle.bind(kLibrary, 0x574B6FBCu, [](Runtime &, AllegrexContext &ctx) {  // sceFontDoneLib
        kernel().finish(ctx, 0u);
    });
    hle.bind(kLibrary, 0x27F6E642u, [](Runtime &rt, AllegrexContext &ctx) {  // sceFontGetNumFontList(lib, *error)
        if (arg(ctx, 1) != 0u) rt.memory().store32(arg(ctx, 1), 0u);
        kernel().finish(ctx, 1u);
    });
    // sceFontFindOptimumFont(lib, style, *error): the PSP's system fonts are
    // all 10.125 points, so every request (the game asks for 7 and 10.125)
    // gets the one font.
    hle.bind(kLibrary, 0x099EF33Cu, [](Runtime &rt, AllegrexContext &ctx) {
        if (arg(ctx, 2) != 0u) rt.memory().store32(arg(ctx, 2), 0u);
        kernel().finish(ctx, 0u);
    });
    hle.bind(kLibrary, 0xA834319Du, [](Runtime &rt, AllegrexContext &ctx) {  // sceFontOpen(lib, index, mode, *error)
        if (arg(ctx, 3) != 0u) rt.memory().store32(arg(ctx, 3), 0u);
        kernel().finish(ctx, faces().ready() ? kFontHandle : 0u);
    });
    hle.bind(kLibrary, 0x3AEA8CB6u, [](Runtime &, AllegrexContext &ctx) {  // sceFontClose
        kernel().finish(ctx, 0u);
    });
    hle.bind(kLibrary, 0x0DA7535Eu, [](Runtime &rt, AllegrexContext &ctx) {  // sceFontGetFontInfo(font, *info)
        if (arg(ctx, 1) != 0u && faces().ready()) write_font_info(rt.memory(), arg(ctx, 1));
        if (trace_enabled()) {
            const FontInfo &f = font_info();
            char line[160];
            std::snprintf(line, sizeof line, "GetFontInfo ascender %d descender %d base Y %d max %dx%d advance %d",
                          f.ascender, f.descender, f.base_y, f.max_width, f.max_height, f.max_advance);
            trace(line);
        }
        kernel().finish(ctx, 0u);
    });
    hle.bind(kLibrary, 0xDCC80C2Fu, [](Runtime &rt, AllegrexContext &ctx) {  // sceFontGetCharInfo(font, code, *info)
        if (arg(ctx, 2) != 0u) {
            const Glyph &g = faces().glyph(arg(ctx, 1));
            write_char_info(rt.memory(), arg(ctx, 2), g);
            if (trace_enabled()) {
                char line[160];
                std::snprintf(line, sizeof line, "GetCharInfo %04X %dx%d left %d top %d advance %.2f%s", arg(ctx, 1),
                              g.width, g.height, g.left, g.top, static_cast<double>(g.advance),
                              g.found ? "" : " (no font has it)");
                trace(line);
            }
        }
        kernel().finish(ctx, 0u);
    });
    hle.bind(kLibrary, 0x980F4895u, [](Runtime &rt, AllegrexContext &ctx) {  // sceFontGetCharGlyphImage(font, code, *image)
        if (arg(ctx, 2) != 0u) draw_glyph(rt.memory(), arg(ctx, 2), arg(ctx, 1), Clip{});
        kernel().finish(ctx, 0u);
    });
    // sceFontGetCharGlyphImage_Clip(font, code, *image, x, y, width, height)
    hle.bind(kLibrary, 0xCA1E6945u, [](Runtime &rt, AllegrexContext &ctx) {
        const Clip clip{static_cast<std::int32_t>(arg(ctx, 3)), static_cast<std::int32_t>(arg(ctx, 4)),
                        static_cast<std::int32_t>(arg(ctx, 5)), static_cast<std::int32_t>(arg(ctx, 6))};
        if (arg(ctx, 2) != 0u) draw_glyph(rt.memory(), arg(ctx, 2), arg(ctx, 1), clip);
        kernel().finish(ctx, 0u);
    });
}

}  // namespace portablekit::vc3
