// Hands the profile's hooks to the running edition's corpus library (see
// vc2_corpus_hooks.hpp). Europe's corpus is linked into the executable and
// calls them directly; the US and Japanese corpora are libraries (us.dll,
// jp.dll) patched at their own addresses (cmake/vc2_step_sites.cmake).

#include "vc2_corpus_hooks.hpp"
#include "vc2_region.hpp"

#include "corpus_library.hpp"
#include "profile.hpp"

#include <filesystem>
#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

extern "C" {
std::uint32_t vc2_step_trunc(std::uint32_t pc, float value) noexcept;
float vc2_frame_scale(std::uint32_t pc, float value) noexcept;
std::uint32_t vc2_frame_count(std::uint32_t pc, std::uint32_t amount) noexcept;
float vc2_lod_distance(std::uint32_t address, float value) noexcept;
void vc2_lod_params_begin(std::uint32_t destination) noexcept;
void vc2_lod_params_end() noexcept;
void vc2_camera_turn(std::uint32_t unit) noexcept;
void vc2_camera_begin(std::uint32_t unit) noexcept;
void vc2_aim_stick(std::uint32_t sp, std::uint32_t obj) noexcept;
void vc2_aim_target(float *x, float *y, float step) noexcept;
std::uint32_t vc2_preview_arrived(std::uint32_t arrived, float distance, std::uint32_t mover) noexcept;
std::uint32_t vc2_title_list_select(std::uint32_t list, std::uint32_t index) noexcept;
std::uint32_t vc2_title_choice(std::uint32_t index) noexcept;
void vc2_aim_nub(std::uint32_t sp) noexcept;
void vc2_aim_fine(std::uint32_t sp) noexcept;
void vc2_move_stick(std::uint32_t vector, std::uint32_t sp) noexcept;
std::uint32_t vc2_morale_step(std::uint32_t amount, std::uint32_t position, std::uint32_t morale) noexcept;
void vc2_glyph_begin(std::uint32_t font, std::uint32_t flags, std::uint32_t code, std::uint32_t table) noexcept;
void vc2_glyph_end(std::uint32_t font) noexcept;
}

namespace portablekit::vc2 {
namespace {

constexpr Vc2CorpusHooks kHooks{
    kVc2CorpusHooksVersion,
    &vc2_step_trunc,
    &vc2_frame_scale,
    &vc2_frame_count,
    &vc2_lod_distance,
    &vc2_lod_params_begin,
    &vc2_lod_params_end,
    &vc2_camera_turn,
    &vc2_camera_begin,
    &vc2_aim_stick,
    &vc2_aim_target,
    &vc2_preview_arrived,
    &vc2_title_list_select,
    &vc2_title_choice,
    &vc2_aim_nub,
    &vc2_aim_fine,
    &vc2_move_stick,
    &vc2_morale_step,
    &vc2_glyph_begin,
    &vc2_glyph_end,
};

// The library main.cpp loads for this edition (the same instance: a library
// already loaded is not loaded again).
void *corpus_symbol(const std::filesystem::path &path, const char *name) {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleW(path.wstring().c_str());
    if (module == nullptr) module = LoadLibraryW(path.wstring().c_str());
    return module != nullptr ? reinterpret_cast<void *>(GetProcAddress(module, name)) : nullptr;
#else
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    return handle != nullptr ? dlsym(handle, name) : nullptr;
#endif
}

}  // namespace

void bind_corpus_hooks() {
    if (region() == Region::Europe) return;
    const ProfileVariant *variant = active_variant();
    if (variant == nullptr) return;
    const std::filesystem::path path = variant_corpus_path(*variant);
    auto *bind = reinterpret_cast<Vc2CorpusBind>(corpus_symbol(path, kVc2CorpusBindSymbol));
    if (bind == nullptr) {
        std::cerr << "[vc2-region] " << path.filename().string()
                  << " has no hooks (built before its hook sites): " << region_name() << " runs without them\n";
        return;
    }
    const std::uint32_t version = bind(&kHooks);
    if (version != kVc2CorpusHooksVersion)
        std::cerr << "[vc2-region] " << path.filename().string() << " speaks hook version " << version << ", not "
                  << kVc2CorpusHooksVersion << ": rebuild it; " << region_name() << " runs without the hooks\n";
    else
        std::cerr << "[vc2-region] hooks handed to " << path.filename().string() << " (" << region_name() << ")\n";
}

}  // namespace portablekit::vc2
