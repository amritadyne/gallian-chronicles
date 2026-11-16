#include "profile.hpp"

#include <cstdlib>
#include <string>

namespace portablekit {

const char *env(const char *name) {
    // The prefix is fixed for the life of the process, so the name is built
    // once per call and never kept: getenv's result points into the
    // environment, not into this string.
    std::string full = game().env_prefix;
    full += '_';
    full += name;
    return std::getenv(full.c_str());
}

bool env_set(const char *name) { return env(name) != nullptr; }

std::string env_name(const char *name) { return std::string(game().env_prefix) + '_' + name; }

namespace {
const ProfileVariant *g_active_variant = nullptr;
}

const ProfileVariant *find_variant(std::string_view executable_sha256) {
    for (const ProfileVariant &variant : game().variants)
        if (variant.executable_sha256 != nullptr && executable_sha256 == variant.executable_sha256) return &variant;
    return nullptr;
}

const ProfileVariant *find_variant_by_encrypted(std::string_view eboot_sha256) {
    for (const ProfileVariant &variant : game().variants)
        if (variant.encrypted_executable_sha256 != nullptr && eboot_sha256 == variant.encrypted_executable_sha256)
            return &variant;
    return nullptr;
}

bool is_known_executable(std::string_view executable_sha256) {
    return (game().executable_sha256 != nullptr && executable_sha256 == game().executable_sha256) ||
           find_variant(executable_sha256) != nullptr;
}

void set_active_variant(const ProfileVariant *variant) { g_active_variant = variant; }
const ProfileVariant *active_variant() { return g_active_variant; }

ExtraHleHook active_register_extra_hle() {
    if (g_active_variant != nullptr && g_active_variant->register_extra_hle != nullptr)
        return g_active_variant->register_extra_hle;
    return game().register_extra_hle;
}

PatchImageHook active_patch_loaded_image() {
    if (g_active_variant != nullptr && g_active_variant->patch_loaded_image != nullptr)
        return g_active_variant->patch_loaded_image;
    return game().patch_loaded_image;
}

const char *active_disc_id() {
    if (g_active_variant != nullptr && g_active_variant->disc_id != nullptr) return g_active_variant->disc_id;
    return game().disc_id;
}

const char *active_disc_id_display() {
    if (g_active_variant != nullptr && g_active_variant->disc_id_display != nullptr)
        return g_active_variant->disc_id_display;
    return game().disc_id_display;
}

const char *active_adhoc_product_code() {
    if (g_active_variant != nullptr && g_active_variant->adhoc_product_code != nullptr)
        return g_active_variant->adhoc_product_code;
    return game().adhoc_product_code;
}

std::uint32_t active_system_language() {
    if (g_active_variant != nullptr && g_active_variant->system_language >= 0)
        return static_cast<std::uint32_t>(g_active_variant->system_language);
    return game().system_language;
}

std::uint32_t active_confirm_button() {
    if (g_active_variant != nullptr && g_active_variant->confirm_button >= 0)
        return static_cast<std::uint32_t>(g_active_variant->confirm_button);
    return game().confirm_button;
}

const ProfileVariant *find_variant_by_disc_id(std::string_view disc_id, bool &known) {
    known = disc_id == game().disc_id;
    if (known) return nullptr;
    for (const ProfileVariant &variant : game().variants)
        if (variant.disc_id != nullptr && disc_id == variant.disc_id) {
            known = true;
            return &variant;
        }
    return nullptr;
}

std::span<const std::uint32_t> active_overlay_slots() {
    if (g_active_variant != nullptr && !g_active_variant->overlay_slots.empty()) return g_active_variant->overlay_slots;
    return game().overlay_slots;
}

} // namespace portablekit
