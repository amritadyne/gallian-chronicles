// The game's text rewritten, and its button symbols drawn for the device in use.
//
// Rewrites. vc3-text-overrides.json (made with vc3-script/vc3-text-editor.html)
// holds new text for game messages, keyed "FILE.MTP:id:field", each with an
// "all" text and optionally one for "playstation", "xbox" or "keyboard" (a
// device's own text wins for it). vc3-text-index.tsv, made from the extracted
// originals, gives each key the FNV-1a 64 hash and length of its original
// bytes (no game text). and of each page of a message with several (a tutorial
// may hand its pages over one at a time: page n of the original then shows
// page n of the rewrite). A message is recognised by its bytes when the game
// hands it to one of its two text paths (Europe):
//   - the paged text box (tutorials), EU 0x08B77B40 (this, text): frees its
//     old copy, measures the pages (0x08B77960), copies the text (malloc
//     0x08844C5C) and splits off the first page (0x08B77898);
//   - the text layout, EU 0x0884B90C (this, text): while the text lasts, the
//     parser 0x0884AB90 (this, text) lays out the next piece.
// Both are replaced by the same steps with the rewrite in place of the
// original (Kernel::call_guest runs the game's own pieces). The rewrite, in the
// game's encoding (CP932), lives in kernel-partition memory the game never
// uses (0x08300000-0x0837FFFF; 0x08380000-0x083FFFFF holds converted text in
// a ring). Both files are looked for (VC3_TEXT_DIR, the executable's folder,
// the data folder, then vc3-script beside any folder above the executable)
// and read again when they change, so edits show the next time a message
// opens.
//
// Button symbols. In a rewrite, ○ × △ □ and the editor's tokens ({L}, {START},
// {DPAD_LR}, {STICK}, {RSTICK}, {LMB}, {KEY:E}, ...) become what the device in
// use shows (vc3_glyphs.cpp's choice: PlayStation, Xbox, or keyboard and mouse
// with the Modern controls), the same as the editor previews. In the game's own
// text, ○ × △ □ followed by "button" (or "and"/"or" another symbol, then
// "button") become our drawn PlayStation button, the Xbox button or the
// editor's key (Space, Esc, Q, C). A symbol
// is a character code of ours, taken from codes the Shift-JIS table leaves
// empty in the game's font (so the game's layout treats it like ○: one 16-unit
// cell; a long key cap takes two). When the font stages one of them
// (vc3_glyph_begin / vc3_glyph_end), the cell is drawn here: white ink as
// coverage, like the font's own glyphs, so a red @fc colour turns it red. While
// the symbol sheet (EU 0x09B57FC0) holds one of ours, the renderer's upscaler
// (PortableKit patch 0015) gets the sheet back at the screen's scale (up to
// 8x): ours redrawn from their shapes, the game's own cells enlarged smoothly. Letters come from the system's
// Bahnschrift (else Segoe UI, Arial) through GDI.
//
// VC3_NO_TEXT=1 turns all of this off; VC3_TEXT_TRACE=1 logs each distinct text
// the hooks see (hash, length, start).

#include "vc3_text.hpp"
#include "vc3_discord.hpp"
#include "vc3_controls.hpp"
#include "vc3_glyphs.hpp"
#include "vc3_region.hpp"

#include "app_paths.hpp"
#include "kernel/kernel.hpp"
#include "gpu/ge_state.hpp"
#include "profile.hpp"
#include "psprecomp/corpus_abi.hpp"

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
#include <array>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace portablekit::vc3 {
namespace {

namespace fs = std::filesystem;

// --- Game addresses (Europe) ------------------------------------------------------

constexpr std::uint32_t kEuLayoutSetText = 0x0884B90Cu;
constexpr std::uint32_t kEuLayoutParse = 0x0884AB90u;
constexpr std::uint32_t kEuPagedSet = 0x08B77B40u;
constexpr std::uint32_t kEuPagedMeasure = 0x08B77960u;
constexpr std::uint32_t kEuPagedSplit = 0x08B77898u;
constexpr std::uint32_t kEuMalloc = 0x08844C5Cu;
constexpr std::uint32_t kEuFree = 0x08844CC4u;
constexpr std::uint32_t kEuMessageById = 0x08B78324u;
constexpr std::uint32_t kEuMessageFile = 0x08B78288u;
constexpr std::uint32_t kEuMessageText = 0x08BDA8DCu;
constexpr std::uint32_t kEuMessageFiles = 0x08CD39FCu;  // 28 bytes per message file
// VC3's code (both editions; the same steps as VC2's, scheduled differently).
constexpr std::array<std::uint32_t, 7> kMessageByIdCode{0x27BDFFF0u, 0xAFB00000u, 0xAFB10004u, 0xAFB20008u,
                                                        0xAFBF000Cu, 0x00A08025u, 0x00809025u};
// The first eight instructions of the two replaced functions (no relocations).
constexpr std::array<std::uint32_t, 8> kLayoutSetTextCode{0x27BDFFF0u, 0xAFB00000u, 0xAFBF0004u, 0x8C860014u,
                                                          0x10C0000Au, 0x00808025u, 0x8E040018u, 0x10800007u};
constexpr std::array<std::uint32_t, 8> kPagedSetCode{0x27BDFFF0u, 0xAFB00000u, 0xAFB10004u, 0xAFBF0008u,
                                                     0x00808825u, 0x8E240000u, 0x10800004u, 0x00A08025u};

// Kernel-partition memory no one else uses (kernel.hpp: boot arguments at
// 0x08100000, the interrupt stack below 0x08200000, volatile memory from
// 0x08400000).
constexpr std::uint32_t kArena = 0x08300000u, kArenaEnd = 0x08380000u;
constexpr std::uint32_t kRing = 0x08380000u, kRingEnd = 0x08400000u;
constexpr std::size_t kMaxText = 16384u;

// Font object fields (vc3_text_hd.cpp, EU 0x08BC95A4).
constexpr std::uint32_t kFontSurface = 3524u, kFontGlyphWidth = 3556u, kFontGlyphHeight = 3558u,
                        kFontRowPixels = 3562u, kFontSlotPitch = 3564u;

// The sheet the font's symbols and kana are drawn from: 256x256 RGBA, a grid of
// 16x16 cells from its corner (vc3_text_hd.cpp's 2026-11-07 trace).
constexpr std::uint32_t kEuSymbolSheet = 0x09B57FC0u;
constexpr std::uint32_t kMaxSheetScale = 8u;

constexpr std::uint16_t kSjisCross = 0x817Eu, kSjisCircle = 0x819Bu, kSjisTriangle = 0x81A2u, kSjisSquare = 0x81A0u;

psprecomp::GuestMemory &memory() { return portablekit::kernel().runtime().memory(); }

std::uint64_t fnv64(std::string_view bytes) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (const char c : bytes) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 0x100000001B3ull;
    }
    return h;
}

std::string read_guest_text(std::uint32_t address) {
    std::string out;
    auto &m = memory();
    if (!m.contains(address, 1u)) return out;
    for (std::size_t i = 0; i < kMaxText; ++i) {
        if (!m.contains(address + static_cast<std::uint32_t>(i), 1u)) break;
        const std::uint8_t b = m.load8(address + static_cast<std::uint32_t>(i));
        if (b == 0u) break;
        out.push_back(static_cast<char>(b));
    }
    return out;
}

// --- UTF-8 ------------------------------------------------------------------------

std::vector<char32_t> utf8_decode(std::string_view s) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<std::uint8_t>(s[i]);
        char32_t cp = 0xFFFD;
        std::size_t n = 1;
        if (c < 0x80u) cp = c;
        else if ((c >> 5u) == 0x6u && i + 1 < s.size()) cp = ((c & 0x1Fu) << 6u) | (s[i + 1] & 0x3Fu), n = 2;
        else if ((c >> 4u) == 0xEu && i + 2 < s.size())
            cp = ((c & 0x0Fu) << 12u) | ((s[i + 1] & 0x3Fu) << 6u) | (s[i + 2] & 0x3Fu), n = 3;
        else if ((c >> 3u) == 0x1Eu && i + 3 < s.size())
            cp = ((c & 0x07u) << 18u) | ((s[i + 1] & 0x3Fu) << 12u) | ((s[i + 2] & 0x3Fu) << 6u) | (s[i + 3] & 0x3Fu), n = 4;
        out.push_back(cp);
        i += n;
    }
    return out;
}

void utf8_append(std::string &out, char32_t cp) {
    if (cp < 0x80u) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800u) out.push_back(static_cast<char>(0xC0u | (cp >> 6u))), out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    else if (cp < 0x10000u)
        out.push_back(static_cast<char>(0xE0u | (cp >> 12u))), out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu))),
            out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    else
        out.push_back(static_cast<char>(0xF0u | (cp >> 18u))), out.push_back(static_cast<char>(0x80u | ((cp >> 12u) & 0x3Fu))),
            out.push_back(static_cast<char>(0x80u | ((cp >> 6u) & 0x3Fu))), out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
}

std::size_t utf8_length(std::string_view s) { return utf8_decode(s).size(); }

// The game's encoding (CP932) for one character.
void cp932_append(std::string &out, char32_t cp) {
    if (cp < 0x80u) {
        out.push_back(static_cast<char>(cp));
        return;
    }
#if defined(_WIN32)
    wchar_t wide[2];
    int count = 0;
    if (cp >= 0x10000u) {
        const char32_t v = cp - 0x10000u;
        wide[0] = static_cast<wchar_t>(0xD800u + (v >> 10u)), wide[1] = static_cast<wchar_t>(0xDC00u + (v & 0x3FFu)), count = 2;
    } else {
        wide[0] = static_cast<wchar_t>(cp), count = 1;
    }
    char bytes[8] = {};
    BOOL defaulted = FALSE;
    const int n = WideCharToMultiByte(932, 0, wide, count, bytes, sizeof bytes, "?", &defaulted);
    if (n > 0 && !defaulted) {
        out.append(bytes, static_cast<std::size_t>(n));
        return;
    }
#else
    static const std::map<char32_t, std::uint16_t> table = {
        {U'○', kSjisCircle}, {U'×', kSjisCross}, {U'△', kSjisTriangle}, {U'□', kSjisSquare}, {U'㌃', 0x8765u},
        {U'㍑', 0x8767u},    {U'…', 0x8163u},    {U'‘', 0x8165u},       {U'’', 0x8166u},     {U'“', 0x8167u},
        {U'”', 0x8168u},     {U'—', 0x815Cu},    {U'・', 0x8145u},      {U'　', 0x8140u}};
    if (const auto found = table.find(cp); found != table.end()) {
        out.push_back(static_cast<char>(found->second >> 8u));
        out.push_back(static_cast<char>(found->second & 0xFFu));
        return;
    }
#endif
    out.push_back('?');
}

// --- A small JSON reader (the overrides file) ----------------------------------------

struct Json {
    enum class Type { Null, String, Object, Other } type = Type::Null;
    std::string text;
    std::vector<std::string> keys;  // an object's members, in order
    std::vector<Json> values;
    [[nodiscard]] const Json *find(std::string_view key) const {
        for (std::size_t i = 0; i < keys.size(); ++i)
            if (keys[i] == key) return &values[i];
        return nullptr;
    }
};

class JsonReader {
public:
    explicit JsonReader(std::string_view s) : s_(s) {}
    std::optional<Json> parse() {
        Json value;
        if (!read_value(value)) return std::nullopt;
        return value;
    }

private:
    void space() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\n' || s_[i_] == '\r' || s_[i_] == '\t')) ++i_;
    }
    bool read_string(std::string &out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                auto hex4 = [&](char32_t &v) {
                    if (i_ + 4 > s_.size()) return false;
                    v = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s_[i_++];
                        v <<= 4u;
                        if (h >= '0' && h <= '9') v |= static_cast<char32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= static_cast<char32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= static_cast<char32_t>(h - 'A' + 10);
                        else return false;
                    }
                    return true;
                };
                char32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800u && cp < 0xDC00u && i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2;
                    char32_t low = 0;
                    if (!hex4(low)) return false;
                    cp = 0x10000u + ((cp - 0xD800u) << 10u) + (low - 0xDC00u);
                }
                utf8_append(out, cp);
                break;
            }
            default: out.push_back(e); break;
            }
        }
        return false;
    }
    bool read_value(Json &out) {
        space();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        if (c == '"') {
            out.type = Json::Type::String;
            return read_string(out.text);
        }
        if (c == '{') {
            out.type = Json::Type::Object;
            ++i_;
            space();
            if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
            for (;;) {
                space();
                std::string key;
                if (!read_string(key)) return false;
                space();
                if (i_ >= s_.size() || s_[i_] != ':') return false;
                ++i_;
                Json value;
                if (!read_value(value)) return false;
                out.keys.push_back(std::move(key));
                out.values.push_back(std::move(value));
                space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == '}') return ++i_, true;
                return false;
            }
        }
        if (c == '[') {
            out.type = Json::Type::Other;
            ++i_;
            space();
            if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
            for (;;) {
                Json ignored;
                if (!read_value(ignored)) return false;
                space();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == ']') return ++i_, true;
                return false;
            }
        }
        out.type = Json::Type::Other;  // number, true, false, null
        while (i_ < s_.size() && s_[i_] != ',' && s_[i_] != '}' && s_[i_] != ']' && s_[i_] != ' ' && s_[i_] != '\n' &&
               s_[i_] != '\r' && s_[i_] != '\t')
            ++i_;
        return true;
    }
    std::string_view s_;
    std::size_t i_ = 0;
};

// =============================================================================
// Glyphs: what each button shows on each device
// =============================================================================

enum class Kind { Native, Key, Disc, Face, Pill, Label, Stick, Menu, View, Dpad, Mouse };

struct Spec {
    Kind kind = Kind::Native;
    std::string label;          // letters, d-pad directions (W A S D), mouse part
    std::uint16_t native = 0u;  // Kind::Native: the font's own character
};

Spec key(std::string label) { return {Kind::Key, std::move(label), 0u}; }

// One cell each, however long the label: a glyph across two cells showed a
// line where the cells meet (2026-11-11), so long labels are drawn smaller
// and W A S D / the arrow keys as a cluster of four small keys.
int cells_of(const Spec &) { return 1; }

std::string spec_id(const Spec &spec) {
    static const char *names[] = {"native", "key", "disc", "face", "pill", "label", "stick", "menu", "view", "dpad", "mouse"};
    return std::string(names[static_cast<int>(spec.kind)]) + ":" + spec.label;
}

// The Japanese release with the keys' Cross and Circle swapped (the same test
// as vc3_controls.cpp's swap_confirm_for_japan).
bool japan_confirm_swapped() {
    if (region() != Region::Japan) return false;
    const char *keep = env("JP_SWAP_CONFIRM");
    return keep == nullptr || std::string(keep) != "0";
}

// Devices as vc3_glyphs.cpp numbers them.
constexpr int kPlayStation = 0, kXbox = 1, kKeyboard = 2;

struct Token {
    Spec device[3];  // PlayStation, Xbox, keyboard and mouse
};

// The editor's glyph table (vc3-text-editor.html, GLYPHS), the same per device.
const std::map<std::string, Token> &tokens() {
    static const std::map<std::string, Token> table = [] {
        std::map<std::string, Token> t;
        const auto dpad = [](const char *dirs) { return Spec{Kind::Dpad, dirs, 0u}; };
        // PlayStation face buttons drawn like the Xbox ones: a disc with the
        // symbol cut out (not the font's own ○ × △ □).
        const auto face = [](const char *symbol) { return Spec{Kind::Face, symbol, 0u}; };
        t["×"] = {{face("cross"), {Kind::Disc, "A", 0u}, key("Space")}};
        t["○"] = {{face("circle"), {Kind::Disc, "B", 0u}, key("Esc")}};
        t["△"] = {{face("triangle"), {Kind::Disc, "Y", 0u}, key("Q")}};
        t["□"] = {{face("square"), {Kind::Disc, "X", 0u}, key("C")}};
        t["{L}"] = {{{Kind::Pill, "L1", 0u}, {Kind::Pill, "LB", 0u}, key("1")}};
        t["{R}"] = {{{Kind::Pill, "R1", 0u}, {Kind::Pill, "RB", 0u}, key("2")}};
        t["{START}"] = {{{Kind::Label, "OPTIONS", 0u}, {Kind::Menu, "", 0u}, key("X")}};
        t["{SELECT}"] = {{{Kind::Label, "SHARE", 0u}, {Kind::View, "", 0u}, key("F")}};
        t["{DPAD}"] = {{dpad("WASD"), dpad("WASD"), key("WASD")}};
        t["{DPAD_UD}"] = {{dpad("WS"), dpad("WS"), key("W/S")}};
        t["{DPAD_LR}"] = {{dpad("AD"), dpad("AD"), key("A/D")}};
        t["{UP}"] = {{dpad("W"), dpad("W"), key("W")}};
        t["{DOWN}"] = {{dpad("S"), dpad("S"), key("S")}};
        t["{LEFT}"] = {{dpad("A"), dpad("A"), key("A")}};
        t["{RIGHT}"] = {{dpad("D"), dpad("D"), key("D")}};
        t["{STICK}"] = {{{Kind::Stick, "L", 0u}, {Kind::Stick, "LS", 0u}, key("WASD")}};
        t["{RSTICK}"] = {{{Kind::Stick, "R", 0u}, {Kind::Stick, "RS", 0u}, {Kind::Mouse, "", 0u}}};
        t["{LMB}"] = {{face("cross"), {Kind::Disc, "A", 0u}, {Kind::Mouse, "left", 0u}}};
        t["{RMB}"] = {{{Kind::Pill, "R1", 0u}, {Kind::Pill, "RB", 0u}, {Kind::Mouse, "right", 0u}}};
        const Spec middle{Kind::Mouse, "middle", 0u}, wheel{Kind::Mouse, "wheel", 0u};
        t["{MMB}"] = {{middle, middle, middle}};
        t["{WHEEL}"] = {{wheel, wheel, wheel}};
        // Any key, {KEY:name}: a key cap on every device (the editor's Key menu).
        std::vector<std::pair<std::string, std::string>> keys;
        for (char c = 'A'; c <= 'Z'; ++c) keys.emplace_back(std::string(1, c), std::string(1, c));
        for (char c = '0'; c <= '9'; ++c) keys.emplace_back(std::string(1, c), std::string(1, c));
        for (int f = 1; f <= 12; ++f) keys.emplace_back("F" + std::to_string(f), "F" + std::to_string(f));
        for (const auto &[id, cap] : std::vector<std::pair<std::string, std::string>>{
                 {"ESC", "Esc"},     {"TAB", "Tab"},     {"CAPS", "Caps"},   {"SHIFT", "Shift"},   {"CTRL", "Ctrl"},
                 {"ALT", "Alt"},     {"WIN", "Win"},     {"SPACE", "Space"}, {"ENTER", "Enter"},   {"BKSP", "Bksp"},
                 {"INS", "Ins"},     {"DEL", "Del"},     {"HOME", "Home"},   {"END", "End"},       {"PGUP", "PgUp"},
                 {"PGDN", "PgDn"},   {"PRTSC", "PrtSc"}, {"PAUSE", "Pause"}, {"UP", "↑"},          {"DOWN", "↓"},
                 {"LEFT", "←"},      {"RIGHT", "→"},     {"ARROWS", "↑↓←→"}, {"`", "`"},           {"-", "-"},
                 {"=", "="},         {"[", "["},         {"]", "]"},         {"\\", "\\"},         {";", ";"},
                 {"'", "'"},         {",", ","},         {".", "."},         {"/", "/"},           {"#", "#"},
                 {"NUM+", "Num+"},   {"NUM-", "Num-"},   {"NUM*", "Num*"},   {"NUM/", "Num/"},     {"NUM.", "Num."},
                 {"NUMENTER", "Enter"}})
            keys.emplace_back(id, cap);
        for (char c = '0'; c <= '9'; ++c) keys.emplace_back(std::string("NUM") + c, std::string("Num") + c);
        for (const auto &[id, cap] : keys) t["{KEY:" + id + "}"] = {{key(cap), key(cap), key(cap)}};
        // The Japanese release confirms with Circle: there the keys' Cross and
        // Circle trade places (vc3_controls.cpp), so ○ is Space and × is Esc,
        // and the left click (confirm) is Circle on a pad.
        if (japan_confirm_swapped()) {
            std::swap(t["×"].device[kKeyboard], t["○"].device[kKeyboard]);
            t["{LMB}"].device[kPlayStation] = face("circle");
            t["{LMB}"].device[kXbox] = {Kind::Disc, "B", 0u};
        }
        return t;
    }();
    return table;
}

// The longest token at text[i] ("{...}" or a symbol), or empty.
std::string token_at(std::string_view text, std::size_t i) {
    if (text[i] == '{') {
        const std::size_t end = text.find('}', i + 1);
        if (end == std::string_view::npos || end - i > 16u) return {};
        std::string name(text.substr(i, end - i + 1));
        return tokens().count(name) != 0u ? name : std::string{};
    }
    for (const char *symbol : {"×", "○", "△", "□"}) {
        const std::size_t n = std::strlen(symbol);
        if (text.substr(i, n) == symbol) return symbol;
    }
    return {};
}

// =============================================================================
// Glyphs: drawing
// =============================================================================

// Coverage in a W x H grid of pixels, `scale` pixels to a glyph unit (a cell
// is 16 units square).
struct Coverage {
    int width = 0, height = 0;
    float scale = 1.0f;
    std::vector<float> a;
    float &at(int x, int y) { return a[static_cast<std::size_t>(y) * width + x]; }
};

struct Clip {
    float x0 = -1e9f, y0 = -1e9f, x1 = 1e9f, y1 = 1e9f;
    [[nodiscard]] bool inside(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
};

using Inside = std::function<bool(float, float)>;

// Adds (fill) or cuts out (knock) a shape, sampled 4x4 per pixel.
void apply(Coverage &c, const Inside &inside, bool knock, const Clip &clip = {}) {
    constexpr int kSub = 4;
    for (int y = 0; y < c.height; ++y)
        for (int x = 0; x < c.width; ++x) {
            int hits = 0;
            for (int sy = 0; sy < kSub; ++sy)
                for (int sx = 0; sx < kSub; ++sx) {
                    const float u = (static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSub) / c.scale;
                    const float v = (static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSub) / c.scale;
                    if (clip.inside(u, v) && inside(u, v)) ++hits;
                }
            if (hits == 0) continue;
            const float cover = static_cast<float>(hits) / (kSub * kSub);
            float &a = c.at(x, y);
            a = knock ? a * (1.0f - cover) : std::max(a, cover);
        }
}

Inside circle(float cx, float cy, float r) {
    return [=](float x, float y) { return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r; };
}
Inside ring(float cx, float cy, float r, float w) {
    return [=](float x, float y) {
        const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        return std::abs(d - r) <= w / 2.0f;
    };
}
bool in_rrect(float x, float y, float x0, float y0, float x1, float y1, float r) {
    if (x < x0 || x > x1 || y < y0 || y > y1) return false;
    r = std::min({r, (x1 - x0) / 2.0f, (y1 - y0) / 2.0f});
    const float qx = std::max({x0 + r - x, 0.0f, x - (x1 - r)}), qy = std::max({y0 + r - y, 0.0f, y - (y1 - r)});
    return qx * qx + qy * qy <= r * r;
}
Inside rrect(float x0, float y0, float x1, float y1, float r) {
    return [=](float x, float y) { return in_rrect(x, y, x0, y0, x1, y1, r); };
}
Inside rrect_stroke(float x0, float y0, float x1, float y1, float r, float w) {
    const float h = w / 2.0f;
    return [=](float x, float y) {
        return in_rrect(x, y, x0 - h, y0 - h, x1 + h, y1 + h, r + h) &&
               !in_rrect(x, y, x0 + h, y0 + h, x1 - h, y1 - h, std::max(0.0f, r - h));
    };
}
float segment_distance(float x, float y, float ax, float ay, float bx, float by) {
    const float dx = bx - ax, dy = by - ay;
    const float len = dx * dx + dy * dy;
    const float t = len > 0.0f ? std::clamp(((x - ax) * dx + (y - ay) * dy) / len, 0.0f, 1.0f) : 0.0f;
    const float px = ax + t * dx - x, py = ay + t * dy - y;
    return std::sqrt(px * px + py * py);
}
Inside segment(float ax, float ay, float bx, float by, float w) {
    return [=](float x, float y) { return segment_distance(x, y, ax, ay, bx, by) <= w / 2.0f; };
}
using Points = std::vector<std::pair<float, float>>;
Inside polygon(Points p) {
    return [p = std::move(p)](float x, float y) {
        bool in = false;
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) {
            const auto [xi, yi] = p[i];
            const auto [xj, yj] = p[j];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
    };
}
Inside polygon_stroke(Points p, float w) {
    return [p = std::move(p), w](float x, float y) {
        for (std::size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
            if (segment_distance(x, y, p[j].first, p[j].second, p[i].first, p[i].second) <= w / 2.0f) return true;
        return false;
    };
}

// Letters: centred on (cx, cy) units, capitals `cap` units tall, no wider than
// `max_width` units; filled or cut out.
void letters(Coverage &c, const std::string &utf8, float cx, float cy, float cap, float max_width, bool knock) {
#if defined(_WIN32)
    if (utf8.empty()) return;
    std::wstring wide;
    for (const char32_t cp : utf8_decode(utf8)) wide.push_back(static_cast<wchar_t>(cp));
    const bool condensed = wide.size() > 2u;
    static const std::vector<std::wstring> wide_faces = {L"Bahnschrift SemiBold", L"Bahnschrift", L"Segoe UI Semibold",
                                                         L"Segoe UI", L"Arial"};
    static const std::vector<std::wstring> narrow_faces = {L"Bahnschrift SemiBold Condensed", L"Bahnschrift Condensed",
                                                           L"Bahnschrift SemiBold", L"Arial Narrow", L"Arial"};
    const auto make_font = [&](int em) -> HFONT {
        HDC dc = CreateCompatibleDC(nullptr);
        HFONT chosen = nullptr;
        for (const std::wstring &name : condensed ? narrow_faces : wide_faces) {
            HFONT font = CreateFontW(-em, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS,
                                     CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, name.c_str());
            if (font == nullptr) continue;
            HGDIOBJ old = SelectObject(dc, font);
            wchar_t selected[LF_FACESIZE] = {};
            GetTextFaceW(dc, LF_FACESIZE, selected);
            SelectObject(dc, old);
            if (std::wcslen(selected) > 0 &&
                _wcsnicmp(selected, name.c_str(), std::min<std::size_t>(name.size(), std::wcslen(selected))) == 0) {
                chosen = font;
                break;
            }
            DeleteObject(font);
        }
        DeleteDC(dc);
        return chosen;
    };
    struct Ink {
        int width = 0, height = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::vector<std::uint8_t> v;
    };
    const auto render = [&](int em) {
        Ink ink;
        HFONT font = make_font(em);
        if (font == nullptr) return ink;
        HDC dc = CreateCompatibleDC(nullptr);
        HGDIOBJ old_font = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, wide.c_str(), static_cast<int>(wide.size()), &size);
        ink.width = size.cx + em / 2 + 4, ink.height = size.cy + 4;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = ink.width;
        info.bmiHeader.biHeight = -ink.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void *bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap != nullptr && bits != nullptr) {
            HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
            std::memset(bits, 0, static_cast<std::size_t>(ink.width) * ink.height * 4u);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            TextOutW(dc, 2 + em / 4, 2, wide.c_str(), static_cast<int>(wide.size()));
            GdiFlush();
            ink.v.resize(static_cast<std::size_t>(ink.width) * ink.height);
            ink.x0 = ink.width, ink.y0 = ink.height;
            const auto *px = static_cast<const std::uint32_t *>(bits);
            for (int y = 0; y < ink.height; ++y)
                for (int x = 0; x < ink.width; ++x) {
                    const std::uint32_t p = px[static_cast<std::size_t>(y) * ink.width + x];
                    const auto v = static_cast<std::uint8_t>(std::max({p & 0xFFu, (p >> 8u) & 0xFFu, (p >> 16u) & 0xFFu}));
                    ink.v[static_cast<std::size_t>(y) * ink.width + x] = v;
                    if (v != 0u)
                        ink.x0 = std::min(ink.x0, x), ink.y0 = std::min(ink.y0, y), ink.x1 = std::max(ink.x1, x + 1),
                        ink.y1 = std::max(ink.y1, y + 1);
                }
            SelectObject(dc, old_bitmap);
            DeleteObject(bitmap);
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        if (ink.x1 <= ink.x0) ink.v.clear();
        return ink;
    };
    // Capitals are about 0.7 of the em; shrink until the ink fits.
    int em = std::max(4, static_cast<int>(std::lround(cap * c.scale / 0.7f)));
    Ink ink;
    for (int attempt = 0; attempt < 12; ++attempt) {
        ink = render(em);
        if (ink.v.empty()) return;
        const float ink_w = static_cast<float>(ink.x1 - ink.x0) / c.scale;
        if (ink_w <= max_width || em <= 4) break;
        em = std::max(4, static_cast<int>(static_cast<float>(em) * std::max(0.6f, max_width / ink_w * 0.98f)));
    }
    const float icx = (ink.x0 + ink.x1) / 2.0f, icy = (ink.y0 + ink.y1) / 2.0f;
    const long ox = std::lround(cx * c.scale - icx), oy = std::lround(cy * c.scale - icy);
    for (int y = ink.y0; y < ink.y1; ++y)
        for (int x = ink.x0; x < ink.x1; ++x) {
            const std::uint8_t v = ink.v[static_cast<std::size_t>(y) * ink.width + x];
            if (v == 0u) continue;
            const long tx = x + ox, ty = y + oy;
            if (tx < 0 || ty < 0 || tx >= c.width || ty >= c.height) continue;
            float &a = c.at(static_cast<int>(tx), static_cast<int>(ty));
            const float cover = v / 255.0f;
            a = knock ? a * (1.0f - cover) : std::max(a, cover);
        }
#else
    (void)c, (void)utf8, (void)cx, (void)cy, (void)cap, (void)max_width, (void)knock;
#endif
}

// One glyph at `scale` pixels to the unit: 16 * cells units wide, 16 tall. The
// ink sits where the font's does (rows 2..14).
Coverage draw_glyph(const Spec &spec, int cells, float scale) {
    Coverage c;
    const float w = 16.0f * static_cast<float>(cells);
    c.scale = scale;
    c.width = static_cast<int>(std::lround(w * scale));
    c.height = static_cast<int>(std::lround(16.0f * scale));
    c.a.assign(static_cast<std::size_t>(c.width) * c.height, 0.0f);
    const std::string &label = spec.label;
    switch (spec.kind) {
    case Kind::Native: break;
    case Kind::Key: {
        const std::vector<char32_t> chars = utf8_decode(label);
        if (label == "WASD" || label == "↑↓←→") {
            // Four small keys as on the keyboard: one above, three below.
            const std::array<std::array<float, 4>, 4> keys{{{5.6f, 1.6f, 10.4f, 7.4f},
                                                            {0.4f, 8.4f, 5.2f, 14.2f},
                                                            {5.6f, 8.4f, 10.4f, 14.2f},
                                                            {10.8f, 8.4f, 15.6f, 14.2f}}};
            // Top, then left, down, right: W A S D, or the arrows in that order.
            const std::array<int, 4> order = label == "WASD" ? std::array<int, 4>{0, 1, 2, 3} : std::array<int, 4>{0, 2, 1, 3};
            for (int k = 0; k < 4; ++k) {
                const auto &b = keys[static_cast<std::size_t>(k)];
                apply(c, rrect(b[0], b[1], b[2], b[3], 1.2f), false);
                std::string one;
                utf8_append(one, chars[static_cast<std::size_t>(order[static_cast<std::size_t>(k)])]);
                letters(c, one, (b[0] + b[2]) / 2.0f, (b[1] + b[3]) / 2.0f, 3.4f, 3.6f, true);
            }
            break;
        }
        apply(c, rrect(0.6f, 2.2f, w - 0.6f, 13.8f, 2.6f), false);
        const std::size_t n = chars.size();
        letters(c, label, w / 2.0f, 8.0f, n <= 1u ? 7.0f : n <= 3u ? 5.8f : 4.8f, w - 3.0f, true);
        break;
    }
    case Kind::Disc:
        apply(c, circle(8.0f, 8.0f, 6.4f), false);
        letters(c, label, 8.0f, 8.0f, 7.2f, 8.5f, true);
        break;
    case Kind::Face:
        apply(c, circle(8.0f, 8.0f, 6.4f), false);
        if (label == "cross") {
            apply(c, segment(5.5f, 5.5f, 10.5f, 10.5f, 1.35f), true);
            apply(c, segment(10.5f, 5.5f, 5.5f, 10.5f, 1.35f), true);
        } else if (label == "circle") {
            apply(c, ring(8.0f, 8.0f, 2.9f, 1.3f), true);
        } else if (label == "triangle") {
            apply(c, polygon_stroke({{8.0f, 4.7f}, {11.2f, 10.2f}, {4.8f, 10.2f}}, 1.25f), true);
        } else if (label == "square") {
            apply(c, rrect_stroke(5.3f, 5.3f, 10.7f, 10.7f, 0.3f, 1.25f), true);
        }
        break;
    case Kind::Pill:
        apply(c, rrect(0.4f, 3.4f, 15.6f, 12.6f, 4.6f), false);
        letters(c, label, 8.0f, 8.0f, 5.4f, 12.0f, true);
        break;
    case Kind::Label: letters(c, label, w / 2.0f, 8.0f, utf8_length(label) <= 3u ? 6.4f : 4.6f, w - 0.6f, false); break;
    case Kind::Stick:
        apply(c, circle(8.0f, 8.0f, 6.6f), false);
        apply(c, ring(8.0f, 8.0f, 5.0f, 0.9f), true);
        letters(c, label, 8.0f, 8.0f, 4.6f, 6.6f, true);
        break;
    case Kind::Menu:
        apply(c, circle(8.0f, 8.0f, 6.4f), false);
        for (const float y : {5.6f, 8.0f, 10.4f}) apply(c, segment(5.0f, y, 11.0f, y, 1.2f), true);
        break;
    case Kind::View:
        apply(c, circle(8.0f, 8.0f, 6.4f), false);
        apply(c, rrect_stroke(4.6f, 4.6f, 9.2f, 8.6f, 0.6f, 0.9f), true);
        apply(c, rrect(6.8f, 7.0f, 11.6f, 11.2f, 0.6f), true);
        apply(c, rrect_stroke(7.2f, 7.4f, 11.2f, 10.8f, 0.4f, 0.9f), false);
        break;
    case Kind::Dpad: {
        // Four arms round the centre: lit ones solid, the rest outlined.
        constexpr float s = 16.0f, cx = 8.0f, cy = 8.0f;
        const std::array<std::pair<float, float>, 5> shape{{{-0.17f, 0.46f}, {0.17f, 0.46f}, {0.17f, 0.24f}, {0.0f, 0.07f}, {-0.17f, 0.24f}}};
        for (const auto &[dir, dx, dy] : std::array<std::tuple<char, float, float>, 4>{
                 {{'W', 0.0f, -1.0f}, {'S', 0.0f, 1.0f}, {'A', -1.0f, 0.0f}, {'D', 1.0f, 0.0f}}}) {
            Points arm;
            for (const auto &[a, o] : shape) arm.emplace_back(cx + dx * o * s - dy * a * s, cy + dy * o * s + dx * a * s);
            if (label.find(dir) != std::string::npos) apply(c, polygon(arm), false);
            else apply(c, polygon_stroke(arm, 0.8f), false);
        }
        break;
    }
    case Kind::Mouse: {
        // A mouse; the part in use (left, right, middle, wheel) solid.
        constexpr float x0 = 3.6f, y0 = 1.4f, x1 = 12.4f, y1 = 14.6f, r = 4.2f, split = 7.0f;
        apply(c, rrect_stroke(x0, y0, x1, y1, r, 1.0f), false);
        apply(c, segment(x0, split, x1, split, 0.9f), false);
        apply(c, segment(8.0f, y0, 8.0f, split, 0.9f), false);
        if (label == "left") apply(c, rrect(x0, y0, x1, y1, r), false, Clip{-1e9f, -1e9f, 8.0f, split});
        if (label == "right") apply(c, rrect(x0, y0, x1, y1, r), false, Clip{8.0f, -1e9f, 1e9f, split});
        if (label == "middle" || label == "wheel") {
            apply(c, rrect(6.9f, 2.6f, 9.1f, 6.0f, 1.1f), true);
            apply(c, rrect(7.2f, 2.9f, 8.8f, 5.7f, 0.8f), false);
        }
        if (label == "wheel") {
            apply(c, polygon({{14.6f, 0.8f}, {13.5f, 2.6f}, {15.7f, 2.6f}}), false);
            apply(c, polygon({{14.6f, 15.2f}, {13.5f, 13.4f}, {15.7f, 13.4f}}), false);
        }
        break;
    }
    }
    return c;
}

std::uint32_t white(float a) {
    const auto alpha = static_cast<std::uint32_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
    return (alpha << 24u) | 0x00FFFFFFu;
}

std::uint64_t cell_hash(const std::uint32_t *cell) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (std::uint32_t i = 0; i < 256u; ++i)
        for (std::uint32_t k = 0; k < 4u; ++k) {
            h ^= (cell[i] >> (8u * k)) & 0xFFu;
            h *= 0x100000001B3ull;
        }
    return h;
}

// =============================================================================
// Glyphs: codes
// =============================================================================

struct Glyph {
    Spec spec;
    int cells = 1;
    std::array<std::array<std::uint32_t, 256>, 2> pixels{};  // each cell, 16x16 RGBA
    std::map<std::uint32_t, Coverage> hd;                     // by scale
};

// Codes the game's font leaves empty in the Shift-JIS rows it lays out like ○
// (outside the kana range 0x829F-0x8396 the layout treats apart).
std::vector<std::uint16_t> free_codes() {
    std::vector<std::uint16_t> out;
    const std::pair<std::uint16_t, std::uint16_t> ranges[] = {{0x81ADu, 0x81B7u}, {0x81C0u, 0x81C7u}, {0x81CFu, 0x81D9u},
                                                              {0x81E9u, 0x81EFu}, {0x81F8u, 0x81FBu}, {0x8240u, 0x824Eu},
                                                              {0x8259u, 0x825Fu}, {0x827Au, 0x8280u}, {0x829Bu, 0x829Eu}};
    for (const auto &[first, last] : ranges)
        for (std::uint32_t code = first; code <= last; ++code) out.push_back(static_cast<std::uint16_t>(code));
    return out;
}

struct Glyphs {
    std::mutex lock;
    std::map<std::string, std::unique_ptr<Glyph>> by_id;
    std::map<std::string, std::uint16_t> code_of;  // "id#cell"
    std::unordered_map<std::uint16_t, std::pair<Glyph *, int>> at_code;
    std::unordered_map<std::uint64_t, std::pair<Glyph *, int>> by_cell;
    std::vector<std::uint16_t> unused = free_codes();
    std::deque<std::string> handed;  // oldest first, for reuse when all are taken
    // The glyph being staged.
    bool pending = false;
    std::uint32_t flags = 0u;
    std::uint16_t code = 0u;
};

Glyphs &glyphs() {
    static Glyphs g;
    return g;
}

Glyph &glyph_for(Glyphs &g, const Spec &spec) {
    const std::string id = spec_id(spec);
    auto &slot = g.by_id[id];
    if (slot == nullptr) {
        slot = std::make_unique<Glyph>();
        slot->spec = spec;
        slot->cells = cells_of(spec);
        // 4x and averaged down to the font's 16x16 cells.
        const Coverage big = draw_glyph(spec, slot->cells, 4.0f);
        for (int cell = 0; cell < slot->cells; ++cell) {
            for (int y = 0; y < 16; ++y)
                for (int x = 0; x < 16; ++x) {
                    float sum = 0.0f;
                    for (int sy = 0; sy < 4; ++sy)
                        for (int sx = 0; sx < 4; ++sx)
                            sum += big.a[static_cast<std::size_t>(y * 4 + sy) * big.width + (cell * 16 + x) * 4 + sx];
                    // Eight levels of ink: the HD text upscaler takes a sheet as
                    // glyphs only with few levels (vc3_text_hd.cpp), and then
                    // draws these cells again from their shapes.
                    const float level = std::round(sum / 16.0f * 7.0f) / 7.0f;
                    slot->pixels[cell][static_cast<std::size_t>(y) * 16 + x] = white(level);
                }
            g.by_cell[cell_hash(slot->pixels[cell].data())] = {slot.get(), cell};
        }
    }
    return *slot;
}

// The character codes that show `spec` (one per cell).
std::vector<std::uint16_t> codes_for(const Spec &spec) {
    if (spec.kind == Kind::Native) return {spec.native};
    Glyphs &g = glyphs();
    std::lock_guard<std::mutex> hold(g.lock);
    Glyph &glyph = glyph_for(g, spec);
    std::vector<std::uint16_t> out;
    for (int cell = 0; cell < glyph.cells; ++cell) {
        const std::string name = spec_id(spec) + "#" + std::to_string(cell);
        if (const auto found = g.code_of.find(name); found != g.code_of.end()) {
            out.push_back(found->second);
            continue;
        }
        std::uint16_t code = 0u;
        if (!g.unused.empty()) {
            code = g.unused.front();
            g.unused.erase(g.unused.begin());
        } else {
            // All taken: the one handed out longest ago is reused.
            const std::string oldest = g.handed.front();
            g.handed.pop_front();
            code = g.code_of[oldest];
            g.code_of.erase(oldest);
        }
        g.code_of[name] = code;
        g.at_code[code] = {&glyph, cell};
        g.handed.push_back(name);
        static std::uint32_t logged = 0u;
        if (++logged <= 200u)
            std::cerr << "[vc3-text] glyph " << name << " is code 0x" << std::hex << code << std::dec << "\n";
        out.push_back(code);
    }
    return out;
}

void append_codes(std::string &out, const std::vector<std::uint16_t> &codes) {
    for (const std::uint16_t code : codes) {
        out.push_back(static_cast<char>(code >> 8u));
        out.push_back(static_cast<char>(code & 0xFFu));
    }
}

// =============================================================================
// Rewrites
// =============================================================================

int device_now() {
    const int device = prompt_device();
    return device >= kPlayStation && device <= kKeyboard ? device : kPlayStation;
}
const char *device_field(int device) {
    return device == kXbox ? "xbox" : device == kKeyboard ? "keyboard" : "playstation";
}

struct Override {
    std::string key;
    std::uint32_t id = 0u;  // the message id and field from the key ("FILE.MTP:id:field")
    int field = 0;
    int pages = 0;  // the original's pages in the index (0: one page)
    std::optional<std::string> all, device[3];
    // Guest address of the encoded text per device and page (0: the whole
    // text, n: its page n), made when first needed.
    std::array<std::map<int, std::uint32_t>, 3> encoded;
    std::array<std::set<int>, 3> shown;
};

// What a string the game hands over is: a whole message, or one of its pages.
struct Hit {
    Override *o = nullptr;
    int page = 0;
};

struct Rewrites {
    std::vector<std::unique_ptr<Override>> list;
    std::unordered_map<std::uint64_t, std::vector<Hit>> by_hash;  // hash ^ length
    // Messages the game also fetches by id (field 1): the rewrite by id. Their
    // text is matched by bytes only when no other message has the same bytes.
    std::unordered_map<std::uint32_t, Override *> by_id;
    std::unordered_map<std::uint64_t, std::uint32_t> keys_with_text;  // whole messages per hash ^ length
    fs::path overrides_path, index_path;
    fs::file_time_type overrides_time{}, index_time{};
    std::chrono::steady_clock::time_point checked{};
    std::uint32_t arena = kArena, ring = kRing;
    bool installed = false;
    bool trace = false;
    std::set<std::uint64_t> traced;
    std::set<std::uint64_t> missed;  // texts logged as not in the index
};

Rewrites &rewrites() {
    static Rewrites r;
    return r;
}

std::uint64_t text_key(std::uint64_t hash, std::size_t length) { return hash ^ (static_cast<std::uint64_t>(length) * 0x9E3779B97F4A7C15ull); }

// The edition's files: the Japanese release has its own (its messages are
// Japanese, so other bytes and other rewrites): vc3-text-overrides-jp.json and
// vc3-text-index-jp.tsv.
const char *overrides_name() {
    return edition() == Edition::Japanese ? "vc3-text-overrides-jp.json" : "vc3-text-overrides.json";
}
const char *index_name() { return edition() == Edition::Japanese ? "vc3-text-index-jp.tsv" : "vc3-text-index.tsv"; }

std::optional<fs::path> find_file(const char *name) {
    std::vector<fs::path> places;
    if (const char *dir = env("TEXT_DIR"); dir != nullptr && *dir != '\0') places.emplace_back(dir);
    std::error_code error;
    const fs::path exe = portablekit::executable_path();
    if (!exe.empty()) places.push_back(exe.parent_path());
    if (const char *data = env("DATA_DIR"); data != nullptr && *data != '\0') places.emplace_back(data);
    for (const fs::path &place : places)
        if (fs::is_regular_file(place / name, error)) return place / name;
    // Development: vc3-script beside a folder above the executable.
    for (fs::path dir = exe.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_regular_file(dir / "vc3-script" / name, error)) return dir / "vc3-script" / name;
        if (dir == dir.parent_path()) break;
    }
    return std::nullopt;
}

std::string read_file(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void load(Rewrites &r) {
    r.list.clear();
    r.by_hash.clear();
    r.by_id.clear();
    r.keys_with_text.clear();
    r.arena = kArena;
    const auto overrides = find_file(overrides_name());
    if (!overrides) {
        if (!r.overrides_path.empty() || !r.installed)
            std::cerr << "[vc3-text] no " << overrides_name() << " (the executable's folder, the data folder, VC3_TEXT_DIR "
                         "or vc3-script above the executable): the game's own text, with button symbols for the device\n";
        r.overrides_path.clear();
        return;
    }
    std::optional<fs::path> index;
    {
        std::error_code error;
        const fs::path beside = overrides->parent_path() / index_name();
        index = fs::is_regular_file(beside, error) ? std::optional<fs::path>(beside) : find_file(index_name());
    }
    std::error_code error;
    r.overrides_path = *overrides;
    r.overrides_time = fs::last_write_time(*overrides, error);
    if (!index) {
        std::cerr << "[vc3-text] " << overrides->string() << " found, but no " << index_name() << " beside it: rewrites off\n";
        r.index_path.clear();
        return;
    }
    r.index_path = *index;
    r.index_time = fs::last_write_time(*index, error);
    const std::optional<Json> json = JsonReader(read_file(*overrides)).parse();
    const Json *entries = json && json->type == Json::Type::Object ? json->find("overrides") : nullptr;
    if (entries == nullptr || entries->type != Json::Type::Object) {
        std::cerr << "[vc3-text] " << overrides->string() << " could not be read (not a vc3-text-overrides file)\n";
        return;
    }
    std::unordered_map<std::string, Override *> by_key;
    for (std::size_t e = 0; e < entries->keys.size(); ++e) {
        const std::string &key = entries->keys[e];
        const Json &value = entries->values[e];
        if (value.type != Json::Type::Object) continue;
        auto o = std::make_unique<Override>();
        o->key = key;
        if (const Json *all = value.find("all"); all != nullptr && all->type == Json::Type::String) o->all = all->text;
        for (int d = 0; d < 3; ++d)
            if (const Json *t = value.find(device_field(d)); t != nullptr && t->type == Json::Type::String) o->device[d] = t->text;
        if (const std::size_t c2 = key.rfind(':'), c1 = c2 == std::string::npos ? c2 : key.rfind(':', c2 - 1);
            c1 != std::string::npos) {
            o->id = static_cast<std::uint32_t>(std::strtoul(key.c_str() + c1 + 1, nullptr, 10));
            o->field = std::atoi(key.c_str() + c2 + 1);
            if (o->field == 1 && o->id != 0u) r.by_id[o->id] = o.get();
        }
        by_key[key] = o.get();
        r.list.push_back(std::move(o));
    }
    std::ifstream in(*index);
    std::string line;
    std::size_t matched = 0u;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
        if (b == std::string::npos) continue;
        const std::uint64_t hash = std::strtoull(line.substr(a + 1, b - a - 1).c_str(), nullptr, 16);
        const std::size_t c = line.find('\t', b + 1);
        const std::size_t length = std::strtoull(line.substr(b + 1, c == std::string::npos ? c : c - b - 1).c_str(), nullptr, 10);
        const int page = c == std::string::npos ? 0 : std::atoi(line.c_str() + c + 1);
        if (page == 0) ++r.keys_with_text[text_key(hash, length)];
        const auto found = by_key.find(line.substr(0, a));
        if (found == by_key.end()) continue;
        r.by_hash[text_key(hash, length)].push_back({found->second, page});
        found->second->pages = std::max(found->second->pages, page);
        if (page == 0) ++matched;
    }
    std::cerr << "[vc3-text] " << r.list.size() << " rewritten messages from " << overrides->string() << " (" << matched
              << " found in " << index->filename().string() << ")\n";
    if (matched < r.list.size())
        for (const auto &o : r.list) {
            bool in_index = false;
            for (const auto &[k, v] : r.by_hash)
                for (const Hit &h : v) in_index = in_index || h.o == o.get();
            if (!in_index) std::cerr << "[vc3-text]   not in the index: " << o->key << "\n";
        }
}

// Read again when a file changed (checked at most once a second).
void refresh(Rewrites &r) {
    const auto now = std::chrono::steady_clock::now();
    if (now - r.checked < std::chrono::seconds(1)) return;
    r.checked = now;
    std::error_code error;
    bool changed = r.overrides_path.empty() ? find_file(overrides_name()).has_value() : false;
    if (!r.overrides_path.empty()) {
        const auto t = fs::last_write_time(r.overrides_path, error);
        changed = changed || (!error && t != r.overrides_time);
    }
    if (!r.index_path.empty()) {
        const auto t = fs::last_write_time(r.index_path, error);
        changed = changed || (!error && t != r.index_time);
    }
    if (changed) {
        std::cerr << "[vc3-text] the rewrites changed: read again\n";
        load(r);
    }
}

// Texts we gave the game (its copies of them come back through the layout).
std::set<std::uint64_t> &own_texts() {
    static std::set<std::uint64_t> own;
    return own;
}

std::uint32_t store(std::uint32_t &cursor, std::uint32_t start, std::uint32_t end, const std::string &bytes, bool wrap) {
    if (own_texts().size() < 20000u) own_texts().insert(fnv64(bytes));
    const std::uint32_t need = static_cast<std::uint32_t>(bytes.size()) + 1u;
    if (cursor + need > end) {
        if (!wrap || start + need > end) return 0u;
        cursor = start;
    }
    const std::uint32_t at = cursor;
    auto &m = memory();
    for (std::size_t i = 0; i < bytes.size(); ++i) m.store8(at + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(bytes[i]));
    m.store8(at + static_cast<std::uint32_t>(bytes.size()), 0u);
    cursor = (at + need + 3u) & ~3u;
    return at;
}

// A rewrite in the game's encoding, its symbols those of `device`.
std::string encode(std::string_view text, int device) {
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        if (const std::string name = token_at(text, i); !name.empty()) {
            append_codes(out, codes_for(tokens().at(name).device[device]));
            i += name.size();
            continue;
        }
        const auto c = static_cast<std::uint8_t>(text[i]);
        std::size_t n = c < 0x80u ? 1 : (c >> 5u) == 0x6u ? 2 : (c >> 4u) == 0xEu ? 3 : (c >> 3u) == 0x1Eu ? 4 : 1;
        n = std::min(n, text.size() - i);
        const std::vector<char32_t> cp = utf8_decode(text.substr(i, n));
        if (!cp.empty() && cp[0] != U'\r') cp932_append(out, cp[0]);
        i += n;
    }
    return out;
}

// What a PSP button symbol in the game's own text shows on `device` (on the
// PlayStation too: our drawn face button in place of the font's symbol).
std::optional<Spec> converted(std::uint16_t symbol, int device) {
    const char *name = symbol == kSjisCross ? "×" : symbol == kSjisCircle ? "○" : symbol == kSjisTriangle ? "△" : "□";
    // Square in Action Mode on the keyboard: aiming, which is the right
    // button (owner, 2026-11-11: the battle's own aim prompt showed C; C is
    // Square too, but in Target Mode it is the weapon change).
    if (device == kKeyboard && symbol == kSjisSquare && controls_preset() == ControlsPreset::Modern &&
        controls_context() == 0)
        return Spec{Kind::Mouse, "right", 0u};
    // Otherwise the editor's choice (Space, Esc, Q, C on the keyboard),
    // whatever the context: a tutorial box is open over a context that says little.
    return tokens().at(name).device[device];
}

bool is_symbol(std::uint16_t code) {
    return code == kSjisCross || code == kSjisCircle || code == kSjisTriangle || code == kSjisSquare;
}

// The game's own text with its PSP button symbols drawn for `device`: a symbol
// followed by "button"/"key", or by "and"/"or"/"/" and another symbol (the
// first of "△ and × buttons"). Others (a × in "×2") are left.
std::optional<std::string> convert_symbols(const std::string &bytes, int device) {
    const auto lead = [](std::uint8_t b) { return (b >= 0x81u && b < 0xA0u) || b >= 0xE0u; };
    // Positions of the characters, and whether each is a button symbol.
    std::vector<std::size_t> at;
    for (std::size_t i = 0; i < bytes.size();) {
        at.push_back(i);
        i += lead(static_cast<std::uint8_t>(bytes[i])) && i + 1 < bytes.size() ? 2 : 1;
    }
    const auto code_at = [&](std::size_t i) -> std::uint16_t {
        if (i + 1 >= bytes.size() || !lead(static_cast<std::uint8_t>(bytes[i]))) return 0u;
        return static_cast<std::uint16_t>((static_cast<std::uint8_t>(bytes[i]) << 8u) | static_cast<std::uint8_t>(bytes[i + 1]));
    };
    // After position `i`: skips a colour end and spaces.
    const auto after = [&](std::size_t i) {
        for (;;) {
            if (bytes.compare(i, 5, "@fci:") == 0) i += 5;
            else if (bytes.compare(i, 4, "@fci") == 0) i += 4;
            else if (i < bytes.size() && bytes[i] == ' ') ++i;
            else return i;
        }
    };
    // Japanese text: ボタン (button), キー (key); と, か, ・, ／ join two symbols,
    // or the next symbol follows at once (○×ボタン).
    static const std::string kJpButton = "\x83\x7B\x83\x5E\x83\x93", kJpKey = "\x83\x4C\x81\x5B";
    const auto button_word = [&](std::size_t i) {
        i = after(i);
        return bytes.compare(i, 6, "button") == 0 || bytes.compare(i, 6, "Button") == 0 || bytes.compare(i, 3, "key") == 0 ||
               bytes.compare(i, kJpButton.size(), kJpButton) == 0 || bytes.compare(i, kJpKey.size(), kJpKey) == 0;
    };
    std::function<bool(std::size_t, int)> is_button = [&](std::size_t i, int depth) {
        if (depth > 4) return false;
        const std::size_t next = i + 2;
        if (button_word(next)) return true;
        std::size_t j = after(next);
        if (bytes.compare(j, 4, "and ") == 0) j += 4;
        else if (bytes.compare(j, 3, "or ") == 0) j += 3;
        else if (j < bytes.size() && bytes[j] == '/') j += 1;
        else if (bytes.compare(j, 2, "\x82\xC6") == 0 || bytes.compare(j, 2, "\x82\xA9") == 0 ||
                 bytes.compare(j, 2, "\x81\x45") == 0 || bytes.compare(j, 2, "\x81\x5E") == 0)
            j += 2;  // と か ・ ／
        else if (!is_symbol(code_at(j)))
            return false;
        j = after(j);
        if (bytes.compare(j, 3, "@fc") == 0) {  // a colour (VC3: hex to the first non-hex, no ':')
            std::size_t k = j + 3;
            while (k < bytes.size() && std::isxdigit(static_cast<unsigned char>(bytes[k])) != 0) ++k;
            if (k > j + 3) j = after(k < bytes.size() && bytes[k] == ':' ? k + 1 : k);
        }
        return is_symbol(code_at(j)) && is_button(j, depth + 1);
    };
    std::string out;
    bool changed = false;
    for (std::size_t k = 0; k < at.size(); ++k) {
        const std::size_t i = at[k];
        const std::uint16_t code = code_at(i);
        if (is_symbol(code) && is_button(i, 0)) {
            if (const std::optional<Spec> spec = converted(code, device)) {
                append_codes(out, codes_for(*spec));
                changed = true;
                continue;
            }
        }
        const std::size_t end = k + 1 < at.size() ? at[k + 1] : bytes.size();
        out.append(bytes, i, end - i);
    }
    if (!changed) return std::nullopt;
    return out;
}

std::string printable(const std::string &bytes) {
    std::string out;
    for (const char c : bytes.substr(0, 400)) out.push_back(c == '\n' ? '/' : (static_cast<std::uint8_t>(c) < 0x20u || static_cast<std::uint8_t>(c) >= 0x80u) ? '.' : c);
    return out;
}

// The text to give the game instead of the one at `text` (or `text` itself).
std::uint32_t substitute(std::uint32_t text, bool paged) {
    Rewrites &r = rewrites();
    refresh(r);
    const std::string bytes = read_guest_text(text);
    const std::uint64_t hash = fnv64(bytes);
    const int device = device_now();
    if (r.trace && r.traced.size() < 4000u && r.traced.insert(hash).second)
        std::cerr << "[vc3-text-trace] " << (paged ? "paged" : "layout") << " " << std::hex << hash << std::dec << " "
                  << bytes.size() << " \"" << printable(bytes) << "\"\n";
    if (const auto found = r.by_hash.find(text_key(hash, bytes.size())); found != r.by_hash.end()) {
        // A whole message before a page of another with the same bytes.
        Hit hit = found->second.front();
        for (const Hit &h : found->second)
            if (h.page == 0) {
                hit = h;
                break;
            }
        Override *o = hit.o;
        // The same bytes are also another message's, one not rewritten (as
        // "Password" in EXTRAS and in co-op): a message fetched by id gets its
        // rewrite there (message_by_id), so here it is left alone.
        std::size_t rewritten = 0u;
        for (const Hit &h : found->second) rewritten += h.page == 0 ? 1u : 0u;
        const auto same = r.keys_with_text.find(text_key(hash, bytes.size()));
        const bool shared = hit.page == 0 && same != r.keys_with_text.end() && same->second > rewritten;
        const std::optional<std::string> &chosen = o->device[device] ? o->device[device] : o->all;
        std::optional<std::string> wanted;
        bool joined = false;  // the last page with the rewrite's extra pages
        const bool by_id_only = shared && o->field == 1;
        if (by_id_only) {
            static std::set<std::string> told;
            if (told.insert(o->key).second)
                std::cerr << "[vc3-text] " << o->key << ": its text is also another message's; rewritten by id only\n";
        }
        if (chosen && !by_id_only && hit.page == 0) wanted = *chosen;
        if (chosen && !by_id_only && hit.page > 0) {
            // One page of the message: the same page of the rewrite.
            // VC3 breaks a page at any '#' (0x08B02744; "##" is a literal #),
            // and one with a line break on both sides takes one from each side
            // with it (0x08B026B0: CR LF before; LF, or LF CR, after), as the
            // index's pages (vc3text/gen.py game_pages).
            std::vector<std::string> pages(1);
            const std::string &t = *chosen;
            const auto is_break = [](char c) { return c == '\r' || c == '\n'; };
            for (std::size_t i = 0; i < t.size();) {
                if (t[i] != '#') { pages.back().push_back(t[i++]); continue; }
                if (i + 1 < t.size() && t[i + 1] == '#') { pages.back() += "##"; i += 2; continue; }
                std::string &page = pages.back();
                const bool before = !page.empty() && is_break(page.back());
                const bool after = i + 1 < t.size() && is_break(t[i + 1]);
                std::size_t skip = 1u;
                if (before && after) {
                    const char last = page.back();
                    page.pop_back();
                    if (last == '\n' && !page.empty() && page.back() == '\r') page.pop_back();
                    skip = 2u + (t[i + 1] == '\n' && i + 2 < t.size() && t[i + 2] == '\r' ? 1u : 0u);
                }
                pages.emplace_back();
                i += skip;
            }
            const auto n = static_cast<std::size_t>(hit.page);
            if (n <= pages.size()) wanted = pages[n - 1];
            // The original's last page handed to the paged box, and the rewrite
            // has more: they go with it (the box pages through them itself).
            if (paged && hit.page == o->pages && pages.size() > n) {
                joined = true;
                for (std::size_t more = n; more < pages.size(); ++more) *wanted += "#" + pages[more];
            }
            if (n > pages.size() && o->shown[device].insert(-hit.page).second)
                std::cerr << "[vc3-text] " << o->key << " page " << hit.page << ": the rewrite has only " << pages.size()
                          << " pages (the game shows the original's pages one by one)\n";
        }
        if (wanted) {
            std::uint32_t &address = o->encoded[device][joined ? -hit.page : hit.page];
            if (address == 0u) address = store(r.arena, kArena, kArenaEnd, encode(*wanted, device), false);
            if (address != 0u) {
                if (o->shown[device].insert(joined ? 1000 + hit.page : hit.page).second)
                    std::cerr << "[vc3-text] rewrite shown: " << o->key << (hit.page > 0 ? " page " + std::to_string(hit.page) : std::string{})
                              << (joined ? " and the rewrite's extra pages" : "")
                              << " (" << device_field(device) << (o->device[device] ? "" : ", all devices") << ", "
                              << (paged ? "paged box" : "layout") << ")"
                              << (found->second.size() > 1u ? " [the same original text has other keys]" : "") << "\n";
                return address;
            }
            std::cerr << "[vc3-text] no room left for the rewrites (0x80000 bytes)\n";
        }
    }
    // Not one of the index's messages but showing what the rewrites are about
    // (a button, the PSP, its Media Storage): logged once, the place a rewrite
    // missed (an index built from other bytes than the game shows).
    const bool ours = (text >= kArena && text < kArenaEnd) || (text >= kRing && text < kRingEnd) ||
                      own_texts().count(hash) != 0u;
    if (!ours && r.by_hash.find(text_key(hash, bytes.size())) == r.by_hash.end() && r.missed.size() < 300u &&
        (bytes.find("PSP") != std::string::npos || bytes.find("Media Storage") != std::string::npos ||
         bytes.find("utton") != std::string::npos || bytes.find("\x81\x9b") != std::string::npos ||
         bytes.find("\x81\x7e") != std::string::npos || bytes.find("\x81\xa2") != std::string::npos ||
         bytes.find("\x81\xa0") != std::string::npos) &&
        r.missed.insert(hash).second)
        std::cerr << "[vc3-text] not in the index (" << (paged ? "paged box" : "layout") << ", " << bytes.size()
                  << " bytes, " << std::hex << hash << std::dec << "): \"" << printable(bytes) << "\"\n";
    if (paged) return text;
    if (const std::optional<std::string> out = convert_symbols(bytes, device)) {
        if (const std::uint32_t at = store(r.ring, kRing, kRingEnd, *out, true); at != 0u) return at;
    }
    return text;
}

// --- The symbol sheet in high resolution ------------------------------------------------

// Our cell `cell` of `glyph` drawn from its shapes at `scale`, into `dst`.
void draw_hd_cell(Glyph &glyph, int cell, std::uint32_t scale, std::uint32_t *dst, std::size_t stride) {
    auto drawn = glyph.hd.find(scale);
    if (drawn == glyph.hd.end()) drawn = glyph.hd.emplace(scale, draw_glyph(glyph.spec, glyph.cells, static_cast<float>(scale))).first;
    const Coverage &c = drawn->second;
    const std::uint32_t side = 16u * scale, x0 = static_cast<std::uint32_t>(cell) * side;
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const std::uint32_t sx = x0 + x;
            const float a = (sx < static_cast<std::uint32_t>(c.width) && y < static_cast<std::uint32_t>(c.height))
                                ? c.a[static_cast<std::size_t>(y) * c.width + sx]
                                : 0.0f;
            dst[y * stride + x] = white(a);
        }
}

// Any other cell: enlarged smoothly within the cell (as the GPU would, without
// the neighbouring cell bleeding in).
std::vector<std::uint32_t> smooth_cell(const std::uint32_t *cell, std::uint32_t scale) {
    const std::uint32_t side = 16u * scale;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(side) * side);
    std::array<std::array<float, 4>, 256> premultiplied{};
    for (std::size_t i = 0; i < 256u; ++i) {
        const float a = static_cast<float>(cell[i] >> 24u) / 255.0f;
        premultiplied[i] = {static_cast<float>(cell[i] & 0xFFu) * a, static_cast<float>((cell[i] >> 8u) & 0xFFu) * a,
                            static_cast<float>((cell[i] >> 16u) & 0xFFu) * a, a};
    }
    for (std::uint32_t y = 0; y < side; ++y)
        for (std::uint32_t x = 0; x < side; ++x) {
            const float u = std::clamp((static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f, 0.0f, 15.0f);
            const float v = std::clamp((static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f, 0.0f, 15.0f);
            const int x0 = std::min(14, static_cast<int>(u)), y0 = std::min(14, static_cast<int>(v));
            const float fx = u - static_cast<float>(x0), fy = v - static_cast<float>(y0);
            std::array<float, 4> p{};
            for (int k = 0; k < 4; ++k) {
                const float top = premultiplied[y0 * 16 + x0][k] * (1.0f - fx) + premultiplied[y0 * 16 + x0 + 1][k] * fx;
                const float bottom =
                    premultiplied[(y0 + 1) * 16 + x0][k] * (1.0f - fx) + premultiplied[(y0 + 1) * 16 + x0 + 1][k] * fx;
                p[k] = top * (1.0f - fy) + bottom * fy;
            }
            const float a = p[3];
            const auto channel = [&](int k) {
                return a > 0.0f ? static_cast<std::uint32_t>(std::clamp(std::lround(p[k] / a), 0l, 255l)) : 0u;
            };
            out[static_cast<std::size_t>(y) * side + x] = channel(0) | (channel(1) << 8u) | (channel(2) << 16u) |
                                                           (static_cast<std::uint32_t>(std::lround(a * 255.0f)) << 24u);
        }
    return out;
}

// The renderer's upscaler (PortableKit patch 0015): the symbol sheet, while it
// holds a glyph of ours, comes back `scale` times larger with ours drawn sharp.
// The font's surface our glyphs were last drawn into (text_glyph_end): VC3's
// symbol sheet (VC2's sat at a fixed 0x09B57FC0, VC3's has no such place).
std::atomic<std::uint32_t> &glyph_surface() {
    static std::atomic<std::uint32_t> surface{0u};
    return surface;
}

bool upscale_symbols(const gpu::TextureState &texture, const std::uint32_t *texels, std::uint32_t wanted,
                     std::vector<std::uint32_t> &out, std::uint32_t &scale) {
    // The sheet is found by what it holds: the font's surface our glyphs are
    // drawn into (text_glyph_end) was never drawn from (2026-11-16: the game
    // draws a copy of it), so after the first glyph each 32-bit texture of
    // whole 16x16 cells is looked through (8 times per place and size at
    // most) and one holding a glyph of ours is drawn sharp from then on.
    const std::uint32_t sheet = glyph_surface().load(std::memory_order_relaxed);
    if (sheet == 0u || texture.format != gpu::TextureFormat::Rgba8888 || texture.width % 16u != 0u ||
        texture.height % 16u != 0u || texture.width == 0u || texture.height == 0u || texture.width > 1024u ||
        texture.height > 1024u)
        return false;
    static std::mutex verdict_lock;
    static std::map<std::uint64_t, int> verdicts;  // > 0 holds ours; < 0 times it did not
    const std::uint64_t place = (static_cast<std::uint64_t>(texture.address) << 24u) ^
                                (static_cast<std::uint64_t>(texture.width) << 12u) ^ texture.height;
    {
        std::lock_guard<std::mutex> hold(verdict_lock);
        const int v = verdicts[place];
        if (texels == nullptr) return v > 0 || v > -8;
    }
    Glyphs &g = glyphs();
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.by_cell.empty()) return false;
    const std::uint32_t width = texture.width, height = texture.height;
    std::array<std::uint32_t, 256> cell{};
    const auto read = [&](std::uint32_t cx, std::uint32_t cy) {
        for (std::uint32_t y = 0; y < 16u; ++y)
            std::memcpy(&cell[y * 16u], &texels[(cy * 16u + y) * width + cx * 16u], 16u * sizeof(std::uint32_t));
    };
    bool ours = false;
    for (std::uint32_t cy = 0; cy < height / 16u && !ours; ++cy)
        for (std::uint32_t cx = 0; cx < width / 16u && !ours; ++cx) {
            read(cx, cy);
            ours = g.by_cell.count(cell_hash(cell.data())) != 0u;
        }
    {
        std::lock_guard<std::mutex> hold(verdict_lock);
        int &v = verdicts[place];
        if (!ours) {
            if (v <= 0) --v;
            return false;
        }
        if (v <= 0)
            std::cerr << "[vc3-text] button symbols found in the texture at 0x" << std::hex << texture.address << std::dec
                      << " (" << texture.width << "x" << texture.height << "): drawn sharp\n";
        v = 1;
    }
    scale = std::clamp(wanted, 2u, kMaxSheetScale);
    const std::uint32_t side = 16u * scale;
    const std::size_t big_width = static_cast<std::size_t>(width) * scale;
    out.assign(big_width * height * scale, 0u);
    static std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> cache;
    if (cache.size() > 4096u) cache.clear();
    for (std::uint32_t cy = 0; cy < height / 16u; ++cy)
        for (std::uint32_t cx = 0; cx < width / 16u; ++cx) {
            read(cx, cy);
            std::uint32_t *dst = &out[static_cast<std::size_t>(cy) * side * big_width + static_cast<std::size_t>(cx) * side];
            const std::uint64_t hash = cell_hash(cell.data());
            if (const auto mine = g.by_cell.find(hash); mine != g.by_cell.end()) {
                draw_hd_cell(*mine->second.first, mine->second.second, scale, dst, big_width);
                continue;
            }
            bool empty = true;
            for (const std::uint32_t texel : cell) empty = empty && (texel >> 24u) == 0u;
            if (empty) continue;
            const std::uint64_t key = hash ^ (static_cast<std::uint64_t>(scale) << 59u);
            auto found = cache.find(key);
            if (found == cache.end()) found = cache.emplace(key, smooth_cell(cell.data(), scale)).first;
            for (std::uint32_t y = 0; y < side; ++y)
                std::memcpy(dst + y * big_width, &found->second[static_cast<std::size_t>(y) * side], side * sizeof(std::uint32_t));
        }
    static bool told = false;
    if (!told) {
        told = true;
        std::cerr << "[vc3-text] symbol sheet drawn at " << scale << "x, our glyphs from their shapes\n";
    }
    return true;
}

// --- The hooks ------------------------------------------------------------------------

using GuestArgs = portablekit::GuestCallArguments;

// A field-1 message's rewrite for the device in use, by its id (0: none).
std::uint32_t rewrite_for_id(std::uint32_t id) {
    Rewrites &r = rewrites();
    refresh(r);
    const auto found = r.by_id.find(id);
    if (found == r.by_id.end()) return 0u;
    Override *o = found->second;
    const int device = device_now();
    const std::optional<std::string> &chosen = o->device[device] ? o->device[device] : o->all;
    if (!chosen) return 0u;
    std::uint32_t &address = o->encoded[device][0];
    if (address == 0u) address = store(r.arena, kArena, kArenaEnd, encode(*chosen, device), false);
    if (address != 0u && o->shown[device].insert(0).second)
        std::cerr << "[vc3-text] rewrite shown: " << o->key << " (" << device_field(device)
                  << (o->device[device] ? "" : ", all devices") << ", by id)\n";
    return address;
}

// EU 0x08B78324 (messages, id): a message's text by its id. A rewrite's text
// when there is one; otherwise as the game's: the message file holding the id
// (EU 0x08B78288), then the text in it (EU 0x08BDA8DC).
void message_by_id(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t messages = ctx.gpr[4], id = ctx.gpr[5];
    ctx.pc = ctx.gpr[31];
    discord_note_message(id);
    if (const std::uint32_t mine = rewrite_for_id(id); mine != 0u) {
        ctx.set_gpr(2u, mine);
        return;
    }
    portablekit::kernel().call_guest(ctx, at(kEuMessageFile), GuestArgs{messages, id}, [messages, id](AllegrexContext &c, std::uint32_t file) {
        auto &m = memory();
        const std::uint32_t handle = m.load32(messages + 64u + file * 24u + 20u);
        if (handle == 0u) {
            c.set_gpr(2u, 0u);
            return;
        }
        const std::uint32_t which = m.load32(at(kEuMessageFiles) + file * 28u + 16u);
        portablekit::kernel().call_guest(c, at(kEuMessageText), GuestArgs{handle, id, which, 0u},
                                         [](AllegrexContext &c2, std::uint32_t text) { c2.set_gpr(2u, text); });
    });
}

void layout_next(AllegrexContext &ctx, std::uint32_t self, std::uint32_t text) {
    if (memory().load8(text) == 0u) {
        ctx.set_gpr(2u, text);
        return;
    }
    portablekit::kernel().call_guest(ctx, at(kEuLayoutParse), GuestArgs{self, text},
                                     [self](AllegrexContext &c, std::uint32_t next) { layout_next(c, self, next); });
}

// EU 0x0884B90C (this, text): lays the text out piece by piece.
void layout_set_text(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t self = ctx.gpr[4], text = ctx.gpr[5];
    ctx.pc = ctx.gpr[31];
    auto &m = memory();
    if (m.load32(self + 20u) == 0u || m.load32(self + 24u) == 0u || text == 0u) return;
    layout_next(ctx, self, substitute(text, false));
}

void paged_copy(AllegrexContext &ctx, std::uint32_t self, std::uint32_t text) {
    std::uint32_t length = 0u;
    while (memory().load8(text + length) != 0u) ++length;
    portablekit::kernel().call_guest(ctx, at(kEuMalloc), GuestArgs{length + 1u}, [self, text, length](AllegrexContext &c, std::uint32_t copy) {
        auto &m = memory();
        m.store32(self, copy);
        if (copy == 0u) return;
        for (std::uint32_t i = 0; i <= length; ++i) m.store8(copy + i, m.load8(text + i));
        m.store32(self + 4u, copy);
        portablekit::kernel().call_guest(c, at(kEuPagedSplit), GuestArgs{self, copy}, [self](AllegrexContext &c2, std::uint32_t next) {
            memory().store32(self + 8u, next);
            c2.set_gpr(2u, next);
        });
    });
}

void paged_measure(AllegrexContext &ctx, std::uint32_t self, std::uint32_t text) {
    portablekit::kernel().call_guest(ctx, at(kEuPagedMeasure), GuestArgs{self, text},
                                     [self, text](AllegrexContext &c, std::uint32_t) { paged_copy(c, self, text); });
}

// EU 0x08B77B40 (this, text): the paged box's new text (old copy freed, pages
// measured, the text copied, the first page split off).
void paged_set(psprecomp::CorpusRuntime &, AllegrexContext &ctx) {
    const std::uint32_t self = ctx.gpr[4];
    ctx.pc = ctx.gpr[31];
    capture_note_text(read_guest_text(ctx.gpr[5]));  // VC3_TUTORIAL_CAPTURE only
    const std::uint32_t text = substitute(ctx.gpr[5], true);
    const std::uint32_t old = memory().load32(self);
    if (old == 0u) {
        paged_measure(ctx, self, text);
        return;
    }
    portablekit::kernel().call_guest(ctx, at(kEuFree), GuestArgs{old}, [self, text](AllegrexContext &c, std::uint32_t) {
        memory().store32(self, 0u);
        paged_measure(c, self, text);
    });
}

bool code_is(Runtime &rt, std::uint32_t address, const std::array<std::uint32_t, 8> &words) {
    if (!rt.memory().contains(address, 32u)) return false;
    for (std::uint32_t i = 0; i < 8u; ++i)
        if (rt.memory().load32(address + i * 4u) != words[i]) return false;
    return true;
}

}  // namespace

void install_text(Runtime &rt) {
    if (const char *off = env("NO_TEXT"); off != nullptr && *off != '\0' && std::strcmp(off, "0") != 0) {
        std::cerr << "[vc3-text] off (VC3_NO_TEXT)\n";
        return;
    }
    if (!code_is(rt, at(kEuLayoutSetText), kLayoutSetTextCode) || !code_is(rt, at(kEuPagedSet), kPagedSetCode)) {
        std::cerr << "[vc3-text] the text functions are not where expected: the game's own text\n";
        return;
    }
    Rewrites &r = rewrites();
    if (const char *trace = env("TEXT_TRACE"); trace != nullptr && *trace != '\0' && std::strcmp(trace, "0") != 0) r.trace = true;
    load(r);
    r.checked = std::chrono::steady_clock::now();
    r.installed = true;
    rt.register_function(at(kEuLayoutSetText), &layout_set_text, "vc3_text_layout");
    rt.register_function(at(kEuPagedSet), &paged_set, "vc3_text_paged");
    bool by_id = true;
    for (std::uint32_t i = 0; i < kMessageByIdCode.size(); ++i)
        by_id = by_id && rt.memory().load32(at(kEuMessageById) + i * 4u) == kMessageByIdCode[i];
    if (by_id) rt.register_function(at(kEuMessageById), &message_by_id, "vc3_text_by_id");
    else std::cerr << "[vc3-text] the message lookup is not where expected: rewrites by text only\n";
    gpu::set_texture_upscaler(&upscale_symbols);
    std::cerr << "[vc3-text] text hooks in: rewrites, and button symbols for the device in use\n";
}

void text_glyph_begin(std::uint32_t, std::uint32_t flags, std::uint32_t code, std::uint32_t table) {
    if (!rewrites().installed) return;
    Glyphs &g = glyphs();
    std::lock_guard<std::mutex> hold(g.lock);
    g.pending = table != 1u && g.at_code.count(static_cast<std::uint16_t>(code & 0xFFFFu)) != 0u;
    g.flags = flags;
    g.code = static_cast<std::uint16_t>(code & 0xFFFFu);
}

void text_glyph_end(std::uint32_t font) {
    Glyphs &g = glyphs();
    std::lock_guard<std::mutex> hold(g.lock);
    if (!g.pending || font == 0u) return;
    g.pending = false;
    const auto found = g.at_code.find(g.code);
    if (found == g.at_code.end()) return;
    auto &m = memory();
    const std::uint32_t surface = m.load32(font + kFontSurface);
    const std::uint32_t width = m.load16(font + kFontGlyphWidth), height = m.load16(font + kFontGlyphHeight);
    const std::uint32_t row = m.load16(font + kFontRowPixels), pitch = m.load16(font + kFontSlotPitch);
    if (surface == 0u || width != 16u || height != 16u || row == 0u) return;
    if (glyph_surface().exchange(surface) != surface)
        std::cerr << "[vc3-text] button symbols drawn into the font's sheet at 0x" << std::hex << surface << std::dec << "\n";
    // The staging's slot: the flags' even bits are its row, the odd bits its
    // column (EU 0x08BC9614).
    std::uint32_t slot_row = 0u, slot_column = 0u;
    for (std::uint32_t i = 0; i < 4u; ++i) {
        if (g.flags & (1u << (2u * i))) slot_row += 1u << i;
        if (g.flags & (1u << (2u * i + 1u))) slot_column += 1u << i;
    }
    const auto &pixels = found->second.first->pixels[static_cast<std::size_t>(found->second.second)];
    for (std::uint32_t y = 0; y < 16u; ++y)
        for (std::uint32_t x = 0; x < 16u; ++x) {
            const std::uint32_t address = surface + 4u * ((slot_row * pitch + y) * row + slot_column * pitch + x);
            if (m.contains(address, 4u)) m.store32(address, pixels[y * 16u + x]);
        }
}

bool text_glyph_cell_hd(const std::uint32_t *cell, std::uint32_t scale, std::vector<std::uint32_t> &out) {
    Glyphs &g = glyphs();
    std::lock_guard<std::mutex> hold(g.lock);
    if (g.by_cell.empty() || scale < 2u) return false;
    const auto found = g.by_cell.find(cell_hash(cell));
    if (found == g.by_cell.end()) return false;
    out.assign(static_cast<std::size_t>(16u * scale) * 16u * scale, 0u);
    draw_hd_cell(*found->second.first, found->second.second, scale, out.data(), 16u * scale);
    return true;
}

}  // namespace portablekit::vc3
