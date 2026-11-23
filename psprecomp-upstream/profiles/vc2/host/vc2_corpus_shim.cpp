// Built into an edition's corpus library (profiles/vc2/CMakeLists.txt), not
// into the executable: the profile's hook names that the patched generated
// code calls, forwarded through the table the profile hands over
// (vc2_corpus_hooks.hpp). Before that, each does what the original
// instruction did.

#include "vc2_corpus_hooks.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

#if defined(_WIN32)
#define VC2_CORPUS_EXPORT __declspec(dllexport)
#else
#define VC2_CORPUS_EXPORT __attribute__((visibility("default")))
#endif

namespace {
std::atomic<const Vc2CorpusHooks *> g_hooks{nullptr};

const Vc2CorpusHooks *hooks() noexcept { return g_hooks.load(std::memory_order_acquire); }

// trunc.w.s as the PSP does it: out of range and NaN give 0x7FFFFFFF.
std::uint32_t psp_trunc(float value) noexcept {
    if (!std::isfinite(value) || value >= 2147483648.0f || value < -2147483648.0f) return 0x7FFFFFFFu;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}
}  // namespace

extern "C" VC2_CORPUS_EXPORT std::uint32_t vc2_corpus_bind(const Vc2CorpusHooks *table) {
    if (table != nullptr && table->version == kVc2CorpusHooksVersion) g_hooks.store(table, std::memory_order_release);
    return kVc2CorpusHooksVersion;
}

extern "C" std::uint32_t vc2_step_trunc(std::uint32_t pc, float value) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->step_trunc(pc, value) : psp_trunc(value);
}
extern "C" float vc2_frame_scale(std::uint32_t pc, float value) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->frame_scale(pc, value) : value;
}
extern "C" std::uint32_t vc2_frame_count(std::uint32_t pc, std::uint32_t amount) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->frame_count(pc, amount) : amount;
}
extern "C" float vc2_lod_distance(std::uint32_t address, float value) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->lod_distance(address, value) : value;
}
extern "C" void vc2_lod_params_begin(std::uint32_t destination) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->lod_params_begin(destination);
}
extern "C" void vc2_lod_params_end() noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->lod_params_end();
}
extern "C" void vc2_camera_turn(std::uint32_t unit) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->camera_turn(unit);
}
extern "C" void vc2_camera_begin(std::uint32_t unit) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->camera_begin(unit);
}
extern "C" void vc2_aim_stick(std::uint32_t sp, std::uint32_t obj) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->aim_stick(sp, obj);
}
extern "C" void vc2_aim_target(float *x, float *y, float step) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->aim_target(x, y, step);
}
extern "C" std::uint32_t vc2_preview_arrived(std::uint32_t arrived, float distance, std::uint32_t mover) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->preview_arrived(arrived, distance, mover) : arrived;
}
extern "C" std::uint32_t vc2_title_list_select(std::uint32_t list, std::uint32_t index) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->title_list_select(list, index) : index;
}
extern "C" std::uint32_t vc2_title_choice(std::uint32_t index) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->title_choice(index) : index;
}
extern "C" void vc2_aim_nub(std::uint32_t sp) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->aim_nub(sp);
}
extern "C" void vc2_aim_fine(std::uint32_t sp) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->aim_fine(sp);
}
extern "C" void vc2_move_stick(std::uint32_t vector, std::uint32_t sp) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->move_stick(vector, sp);
}
extern "C" std::uint32_t vc2_morale_step(std::uint32_t amount, std::uint32_t position, std::uint32_t morale) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->morale_step(amount, position, morale) : amount;
}
extern "C" void vc2_glyph_begin(std::uint32_t font, std::uint32_t flags, std::uint32_t code,
                                std::uint32_t table) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->glyph_begin(font, flags, code, table);
}
extern "C" void vc2_glyph_end(std::uint32_t font) noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->glyph_end(font);
}
extern "C" bool vc2_attack_hold(std::uint32_t state) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr && h->attack_hold(state);
}
extern "C" void vc2_attack_done() noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->attack_done();
}
extern "C" std::uint32_t vc2_attack_ready(std::uint32_t state, std::uint32_t ready) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->attack_ready(state, ready) : ready;
}
extern "C" std::uint32_t vc2_pause_steps(std::uint32_t object, std::uint32_t whole) noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr ? h->pause_steps(object, whole) : whole;
}
extern "C" bool vc2_manager_hold() noexcept {
    const Vc2CorpusHooks *h = hooks();
    return h != nullptr && h->manager_hold();
}
extern "C" void vc2_manager_done() noexcept {
    if (const Vc2CorpusHooks *h = hooks()) h->manager_done();
}
