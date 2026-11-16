// VC2 level of detail: the game's own distance-based model swaps.
//
// Kurofune keeps, in its render parameters (a 0x440-byte block; the debug
// menu's "%s LOD Lv [1] [2] [3]", 0x08B7E368), three switch distances for each
// of three kinds of model at +896..+931. The loader 0x08B7E99C copies a
// version 2 block whole (memcpy of 0x440 bytes, call at EU 0x08B7EA58; every
// map tested) or converts a version 1 block through 0x08B7E730 (store
// `swc1 f12, 896(t0)` at EU 0x08B7E874). cmake/vc2_step_sites.cmake routes
// both through here (vc2_lod_params_begin/end around the copy, and
// vc2_lod_distance for the store), which multiply each distance by the
// chosen scale:
//   VC2_LOD=original (or unset)  the PSP's distances (the minimum-spec build)
//   VC2_LOD=full                 x1000: every model keeps its full detail
//   VC2_LOD_SCALE=<number>       any other multiple (overrides VC2_LOD)
// The distances are logged as they load ([vc2-lod]).
#include "vc2_lod.hpp"
#include "vc2_region.hpp"
#include "vc2_settings.hpp"

#include "profile.hpp"
#include "kernel/kernel.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <array>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace portablekit::vc2 {
namespace {

// The scale now: VC2_LOD_SCALE, else the setting (which the in-game Settings
// screen can change while the game runs).
float lod_scale() {
    static const float fixed = [] {
        if (const char *text = env("LOD_SCALE")) {
            const float value = std::strtof(text, nullptr);
            if (value >= 0.1f && value <= 100000.0f) return value;
        }
        return 0.0f;
    }();
    if (fixed > 0.0f) return fixed;
    return options::get(options::Key::Lod) == "full" ? 1000.0f : 1.0f;
}

// Each render parameter block's own nine distances, as loaded, so a change of
// setting can scale them again (the game loads two blocks on some maps).
struct Block {
    std::uint32_t params = 0u;
    std::array<float, 9> original{};
    float scale = 1.0f;
};
std::mutex &blocks_mutex() {
    static std::mutex m;
    return m;
}
std::vector<Block> &blocks() {
    static std::vector<Block> b;
    return b;
}
void remember(std::uint32_t params, std::uint32_t index, float original, float scale) {
    std::lock_guard lock(blocks_mutex());
    auto &list = blocks();
    auto found = std::find_if(list.begin(), list.end(), [&](const Block &b) { return b.params == params; });
    if (found == list.end()) {
        if (list.size() >= 8u) list.erase(list.begin());
        list.push_back(Block{params, {}, scale});
        found = list.end() - 1;
    }
    found->original[index] = original;
    found->scale = scale;
}

}  // namespace

void lod_apply_live() {
    const float scale = lod_scale();
    auto &memory = portablekit::kernel().runtime().memory();
    std::lock_guard lock(blocks_mutex());
    for (Block &b : blocks()) {
        for (std::uint32_t i = 0; i < 9u; ++i) {
            const std::uint32_t address = b.params + 896u + i * 4u;
            // Only where the block still holds what was written (not reused).
            const float written = std::bit_cast<float>(memory.load32(address));
            if (written != b.original[i] * b.scale) continue;
            memory.store32(address, std::bit_cast<std::uint32_t>(b.original[i] * scale));
        }
        b.scale = scale;
    }
    std::cerr << "[vc2-lod] level of detail now x" << scale << " (" << blocks().size() << " parameter block(s))\n";
}
}  // namespace portablekit::vc2

extern "C" float vc2_lod_distance(std::uint32_t address, float value) noexcept {
    using namespace portablekit::vc2;
    const float scale = lod_scale();
    // +896 + kind x 12 + level x 4: log one line per kind as it loads.
    static std::uint32_t table = 0u;
    static float kind_values[3]{};
    if (table == 0u || address < table || address >= table + 36u) table = address;
    const std::uint32_t index = (address - table) / 4u;
    kind_values[index % 3u] = value;
    remember(table - 896u, index, value, scale);
    if (index % 3u == 2u)
        std::cerr << "[vc2-lod] render parameters at 0x" << std::hex << table - 896u << std::dec << ", kind "
                  << index / 3u << " switch distances " << kind_values[0] << " / " << kind_values[1] << " / "
                  << kind_values[2] << (scale != 1.0f ? " (x" + std::to_string(scale) + ")" : std::string()) << "\n";
    return value * scale;
}

// Around the version 2 copy: the destination (a0 before the memcpy call), then
// the nine distances scaled once the copy has returned.
namespace {
std::uint32_t g_pending_params = 0u;
}

extern "C" void vc2_lod_params_begin(std::uint32_t destination) noexcept { g_pending_params = destination; }

extern "C" void vc2_lod_params_end() noexcept {
    using namespace portablekit::vc2;
    const std::uint32_t params = g_pending_params;
    g_pending_params = 0u;
    if (params == 0u) return;
    auto &memory = portablekit::kernel().runtime().memory();
    for (std::uint32_t i = 0; i < 9u; ++i) {
        const std::uint32_t address = params + 896u + i * 4u;
        const float value = std::bit_cast<float>(memory.load32(address));
        memory.store32(address, std::bit_cast<std::uint32_t>(vc2_lod_distance(address, value)));
    }
}
