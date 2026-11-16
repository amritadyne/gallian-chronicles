// See vc3_region.hpp.

#include "vc3_region.hpp"

#include "profile.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace portablekit::vc3 {
namespace {

struct Mapping {
    std::uint32_t eu_address;  // Valkyria Chronicles II Europe (ULES01417)
    std::uint32_t address;     // Valkyria Chronicles 3 (both editions)
};

// Made 2026-11-15 (analysis/claude/gen_table.py) from every VC2 Europe address
// the copied profile code and VC2's cmake patches use. Kinds: anchored = inside
// a 12-word stretch of code that occurs once in each game, address fields
// masked; between = between two such stretches with the same shift, the word
// itself equal; called(n/m) = a function the aligned calls reach, whose whole
// body (to its return) matches; data(n/m) = the target of n of m aligned
// relocations; data-between = between two such data addresses with the same
// shift. Sorted by VC2 address.
constexpr Mapping kVc3[] = {
    {0x08804000u, 0x08804000u},  // load base
    {0x08835714u, 0x08837DD4u},  // anchored
    {0x08837C00u, 0x0883A388u},  // between
    {0x08844AD4u, 0x08847FA8u},  // anchored
    {0x08844C5Cu, 0x0884813Cu},  // called (malloc: the paged box's copy, VC3 0x08B02A38)
    {0x08844CC4u, 0x088481A0u},  // anchored
    {0x0884AB90u, 0x0884E09Cu},  // anchored
    {0x0884B90Cu, 0x0884EE40u},  // anchored
    {0x0884BC68u, 0x0884F198u},  // anchored
    {0x0888FC94u, 0x08891D64u},  // anchored
    {0x0888FECCu, 0x08891FA4u},  // anchored
    {0x0888FED0u, 0x08891FA8u},  // anchored
    {0x088906B4u, 0x088927A4u},  // anchored
    {0x0896ECB4u, 0x08991A58u},  // anchored
    {0x0896ECB8u, 0x08991A5Cu},  // anchored
    {0x0897A03Cu, 0x0899F6A4u},  // between
    {0x08A2D6E4u, 0x08A61DB8u},  // anchored
    {0x08A2D7CCu, 0x08A61EA0u},  // anchored
    {0x08A77E44u, 0x08A7A1FCu},  // anchored
    {0x08A78944u, 0x08A7A9B0u},  // anchored
    {0x08AFB7F8u, 0x08A7E758u},  // anchored
    {0x08AFDA78u, 0x08A81ABCu},  // anchored
    {0x08AFF54Cu, 0x08A835E4u},  // anchored
    {0x08B0C87Cu, 0x08A91140u},  // anchored
    {0x08B0CA24u, 0x08A912E8u},  // anchored
    {0x08B0CD68u, 0x08A91630u},  // anchored
    {0x08B3F2B4u, 0x08AC3AE4u},  // anchored
    {0x08B40778u, 0x08AC4FE8u},  // anchored
    {0x08B40A18u, 0x08AC5294u},  // anchored
    {0x08B60068u, 0x08AE596Cu},  // anchored
    {0x08B60164u, 0x08AE5A70u},  // anchored
    {0x08B77898u, 0x08B02744u},  // anchored
    {0x08B77960u, 0x08B0280Cu},  // anchored
    {0x08B77B40u, 0x08B029F4u},  // paged box set text (2026-11-16: the same steps, checked by hand)
    {0x08B78288u, 0x08B0314Cu},  // called (the message file holding an id, VC3 0x08B0320C)
    {0x08B78324u, 0x08B031ECu},  // anchored
    {0x08B7E730u, 0x08B097E0u},  // anchored
    {0x08B7E874u, 0x08B09924u},  // anchored
    {0x08B81A60u, 0x08B84E9Cu},  // anchored
    {0x08B85968u, 0x08B892E8u},  // anchored
    {0x08B85BD4u, 0x08B89598u},  // anchored
    {0x08B88564u, 0x08B8C61Cu},  // anchored
    {0x08B8856Cu, 0x08B8C624u},  // anchored
    {0x08BA55B0u, 0x08B257CCu},  // anchored
    {0x08BA55D4u, 0x08B257F0u},  // anchored
    {0x08BB4CC4u, 0x08BA598Cu},  // anchored
    {0x08BB4CCCu, 0x08BA5994u},  // anchored
    {0x08BB4CD4u, 0x08BA599Cu},  // anchored
    {0x08BB4CE8u, 0x08BA59B0u},  // anchored
    {0x08BB4CFCu, 0x08BA59C4u},  // anchored
    {0x08BB4F20u, 0x08BA5BE8u},  // anchored
    {0x08BB4F44u, 0x08BA5C0Cu},  // anchored
    {0x08BB4F58u, 0x08BA5C20u},  // anchored
    {0x08BB4FF4u, 0x08BA5CBCu},  // anchored
    {0x08BB517Cu, 0x08BA5E44u},  // anchored
    {0x08BB51C4u, 0x08BA5E8Cu},  // anchored
    {0x08BB8CDCu, 0x08BA99A4u},  // anchored
    {0x08BB8CE8u, 0x08BA99B0u},  // anchored
    {0x08BB8CF0u, 0x08BA99B8u},  // anchored
    {0x08BC8C0Cu, 0x08BB98D4u},  // anchored
    {0x08BC95A4u, 0x08BBA26Cu},  // anchored
    {0x08BC9614u, 0x08BBA2DCu},  // anchored
    {0x08BC98C8u, 0x08BBA590u},  // anchored
    {0x08BD1558u, 0x08BC2220u},  // anchored
    {0x08BD16A4u, 0x08BC236Cu},  // anchored
    {0x08BD19F4u, 0x08BC26BCu},  // anchored
    {0x08BD1AE4u, 0x08BC27ACu},  // anchored
    {0x08BD2EACu, 0x08BC3B74u},  // anchored
    {0x08BD31F4u, 0x08BC3EBCu},  // anchored
    {0x08BDA8DCu, 0x08BCB5E0u},  // anchored
    {0x08BEA65Cu, 0x08BDB360u},  // anchored
    {0x08BEAAD8u, 0x08BDB7DCu},  // anchored
    {0x08BEBBE0u, 0x08BDC8E4u},  // anchored
    {0x08BEBC54u, 0x08BDC958u},  // anchored
    {0x08BEBC9Cu, 0x08BDC9A0u},  // anchored
    {0x08C3B910u, 0x08C2E518u},  // anchored
    {0x08C592E4u, 0x08C4CD68u},  // anchored
    {0x08C5953Cu, 0x08C4CFC0u},  // anchored
    {0x08C59550u, 0x08C4CFD4u},  // anchored
    {0x08C59584u, 0x08C4D008u},  // anchored
    {0x08C69D78u, 0x08C5D7FCu},  // anchored
    {0x08C86000u, 0x08C6FD18u},  // between
    {0x08C86EC8u, 0x08C70BE0u},  // anchored
    {0x08C87020u, 0x08C70D38u},  // anchored
    {0x08C87074u, 0x08C70D8Cu},  // anchored
    {0x08C87380u, 0x08C71098u},  // anchored
    {0x08C873D0u, 0x08C710E8u},  // anchored
    {0x08C87424u, 0x08C7113Cu},  // anchored
    {0x08C8747Cu, 0x08C71194u},  // anchored
    {0x08C874D8u, 0x08C711F0u},  // anchored
    {0x08C87570u, 0x08C71288u},  // anchored
    {0x08C8759Cu, 0x08C712B4u},  // anchored
    {0x08C8766Cu, 0x08C71384u},  // anchored
    {0x08C87688u, 0x08C713A0u},  // anchored
    {0x08C876E4u, 0x08C713FCu},  // anchored
    {0x08C876F8u, 0x08C71410u},  // anchored
    {0x08C87708u, 0x08C71420u},  // anchored
    {0x08C87718u, 0x08C71430u},  // anchored
    {0x08C87740u, 0x08C71458u},  // anchored
    {0x08C877E4u, 0x08C714FCu},  // anchored
    {0x08C87810u, 0x08C71528u},  // anchored
    {0x08C87838u, 0x08C71550u},  // anchored
    {0x08C87874u, 0x08C7158Cu},  // anchored
    {0x08C878B4u, 0x08C715CCu},  // anchored
    {0x08C878E0u, 0x08C715F8u},  // anchored
    {0x08C8791Cu, 0x08C71634u},  // anchored
    {0x08C87944u, 0x08C7165Cu},  // anchored
    {0x08C8796Cu, 0x08C71684u},  // anchored
    {0x08C879A8u, 0x08C716C0u},  // anchored
    {0x08C879D0u, 0x08C716E8u},  // anchored
    {0x08C87A04u, 0x08C7171Cu},  // anchored
    {0x08C87A40u, 0x08C71758u},  // anchored
    {0x08C87A6Cu, 0x08C71784u},  // anchored
    {0x08C87AACu, 0x08C717C4u},  // anchored
    {0x08C87AD8u, 0x08C717F0u},  // anchored
    {0x08C87B30u, 0x08C71848u},  // anchored
    {0x08C87B80u, 0x08C71898u},  // anchored
    {0x08C87C00u, 0x08C71918u},  // anchored
    {0x08C87C44u, 0x08C7195Cu},  // anchored
    {0x08C87C64u, 0x08C7197Cu},  // anchored
    {0x08C87C90u, 0x08C719A8u},  // anchored
    {0x08C87CCCu, 0x08C719E4u},  // anchored
    {0x08C87D18u, 0x08C71A30u},  // anchored
    {0x08C87D9Cu, 0x08C71AB4u},  // anchored
    {0x08C87DD8u, 0x08C71AF0u},  // anchored
    {0x08C87DFCu, 0x08C71B14u},  // anchored
    {0x08C87E20u, 0x08C71B38u},  // anchored
    {0x08C87E60u, 0x08C71B78u},  // anchored
    {0x08C87E9Cu, 0x08C71BB4u},  // anchored
    {0x08C87ED0u, 0x08C71BE8u},  // anchored
    {0x08C87F04u, 0x08C71C1Cu},  // anchored
    {0x08C87F5Cu, 0x08C71C74u},  // anchored
    {0x08C88000u, 0x08C71D18u},  // anchored
    {0x08C880F8u, 0x08C71E10u},  // anchored
    {0x08C89118u, 0x08C72E30u},  // anchored
    {0x08C89300u, 0x08C73018u},  // anchored
    {0x08CB09E0u, 0x08C998E8u},  // .text end (VC2 0x4AC9E0, VC3 0x4958E8 past the load base)
    {0x08CD0698u, 0x08CB8180u},  // data(5/5)
    {0x08CD39FCu, 0x08CBAAC8u},  // data(2/2)
    {0x08CE9A38u, 0x08CD4C10u},  // data(2/2)
    {0x08CF99E0u, 0x08CF16D8u},  // data(9/9)
    {0x08CF9A7Cu, 0x08CF1774u},  // data(16/16)
    {0x08CFCC94u, 0x08CF4A1Cu},  // data(5/5)
    {0x08D0287Cu, 0x08CFBB7Cu},  // data-between
    {0x08D2F5F0u, 0x08D2B278u},  // data(2/2)
    {0x08D36A00u, 0x08D32100u},  // data(3/3)
    {0x08D43F98u, 0x08D432B8u},  // data(8/8)
    {0x08D565A4u, 0x08D5590Cu},  // data(2/2)
    {0x08D565A8u, 0x08D55910u},  // data(2/2)
    {0x08D565ACu, 0x08D55914u},  // data(2/2)
    {0x08D565B0u, 0x08D55918u},  // data(2/2)
    {0x08D5AEC0u, 0x08D59394u},  // data(29/29)
    {0x08D5AF08u, 0x08D593DCu},  // data(52/52)
    {0x08D5B918u, 0x08D59D98u},  // data(36/36)
    {0x08D5B934u, 0x08D59DB4u},  // data-between
    {0x08D5DA08u, 0x08D5BE88u},  // data(9/9)
    {0x08D61DF8u, 0x08D61F68u},  // data-between
    {0x08D61E40u, 0x08D61FB0u},  // data(13/13)
    {0x08D7D000u, 0x08D7B000u},  // static end (VC3 .bss end 0x08D7A580, rounded)
};

struct Fingerprint {
    std::uint32_t eu_address;
    std::uint32_t hash;  // FNV-1a of the first 8 instructions, address fields masked
};

// VC2's fingerprints (profiles/vc2/host/vc2_region.cpp) of the functions the
// profile replaces, each checked against VC3's code at at() when the table
// was made: all match.
constexpr Fingerprint kFingerprints[] = {
    {0x08BB4CC4u, 0xDDA67AD4u},
    {0x08BB4CCCu, 0x93632379u},
    {0x08BB4CD4u, 0xBAA804D4u},
    {0x08BB4CE8u, 0x5C281FF8u},
    {0x08BB4CFCu, 0x4ED4D113u},
    {0x08BB4F20u, 0xD97EDC14u},
    {0x08BB4F44u, 0xBAA804D4u},
    {0x08BB4F58u, 0x6411F63Cu},
    {0x08BB4FF4u, 0x13A4A54Fu},
    {0x08BD31F4u, 0x91C6F7CCu},
    {0x08BEAAD8u, 0x3A3FA37Cu},
    {0x08C3B910u, 0x1B506FDBu},
    {0x08C86000u, 0x742A9DF5u},
    {0x08C86EC8u, 0x4B45433Au},
    {0x08C87020u, 0x233219B4u},
    {0x08C87074u, 0x920EEC38u},
    {0x08C87380u, 0xE8C491BAu},
    {0x08C873D0u, 0xA8163FA8u},
    {0x08C87424u, 0x9942D27Eu},
    {0x08C8747Cu, 0xC565A1C3u},
    {0x08C874D8u, 0xFA566C33u},
    {0x08C87570u, 0x878A2B08u},
    {0x08C8759Cu, 0xF330A85Eu},
    {0x08C8766Cu, 0x03575CACu},
    {0x08C87688u, 0x9C1A7255u},
    {0x08C876E4u, 0x87857843u},
    {0x08C876F8u, 0x9D2A7009u},
    {0x08C87708u, 0x6FB16BC8u},
    {0x08C87718u, 0xE60AB1BFu},
    {0x08C87740u, 0xF95A96CFu},
    {0x08C877E4u, 0x878A2B08u},
    {0x08C87810u, 0xE60AB1BFu},
    {0x08C87838u, 0x52F5AE5Au},
    {0x08C87874u, 0xBFD8B95Fu},
    {0x08C878B4u, 0x7AC87598u},
    {0x08C878E0u, 0xA90711B7u},
    {0x08C8791Cu, 0xE60AB1BFu},
    {0x08C87944u, 0xE60AB1BFu},
    {0x08C8796Cu, 0xA90711B7u},
    {0x08C879A8u, 0xCDE544BAu},
    {0x08C879D0u, 0x8C4BF370u},
    {0x08C87A04u, 0xA90711B7u},
    {0x08C87A40u, 0xBFFB3F70u},
    {0x08C87A6Cu, 0x0317230Cu},
    {0x08C87AACu, 0xBFFB3F70u},
    {0x08C87AD8u, 0xBAF9D2DCu},
    {0x08C87B30u, 0x0CE99D96u},
    {0x08C87B80u, 0x0528659Fu},
    {0x08C87C00u, 0x920EEC38u},
    {0x08C87C44u, 0x2101020Fu},
    {0x08C87C64u, 0x8CB3BF96u},
    {0x08C87C90u, 0x1575A30Fu},
    {0x08C87CCCu, 0x4926487Au},
    {0x08C87D18u, 0x2F72A842u},
    {0x08C87D9Cu, 0xDBF6FE81u},
    {0x08C87DD8u, 0x9913B000u},
    {0x08C87DFCu, 0x9913B000u},
    {0x08C87E20u, 0xBFD8B95Fu},
    {0x08C87E60u, 0xA90711B7u},
    {0x08C87E9Cu, 0xFAFE7A12u},
    {0x08C87ED0u, 0xFAFE7A12u},
    {0x08C87F04u, 0xC5787F9Au},
    {0x08C87F5Cu, 0xA02630A4u},
    {0x08C88000u, 0x54EEAF3Cu},
    {0x08C880F8u, 0x4071F6E4u},
    {0x08C89118u, 0xE68B5AA6u},
    {0x08C89300u, 0x5187FACCu},
};

std::uint32_t masked(std::uint32_t word) {
    const std::uint32_t op = word >> 26;
    if (op == 2u || op == 3u) return word & 0xFC000000u;  // j, jal: target
    switch (op) {                                          // immediates that may hold address halves
    case 0x0Fu: case 0x09u: case 0x0Du: case 0x20u: case 0x21u: case 0x23u:
    case 0x24u: case 0x25u: case 0x28u: case 0x29u: case 0x2Bu: case 0x31u: case 0x39u:
        return word & 0xFFFF0000u;
    default:
        return word;
    }
}

}  // namespace

Region region() { return Region::Vc3; }

Edition edition() {
    const ProfileVariant *variant = active_variant();
    return variant != nullptr && std::strcmp(variant->key, "english") == 0 ? Edition::English : Edition::Japanese;
}

const char *region_name() {
    return edition() == Edition::English ? "English ReTranslation v1.0.8 (ULJM-05957)"
                                         : "Japanese Extra Edition (ULJM-05957)";
}

std::uint32_t at(std::uint32_t eu_address) {
    const auto *found = std::lower_bound(std::begin(kVc3), std::end(kVc3), eu_address,
                                         [](const Mapping &m, std::uint32_t a) { return m.eu_address < a; });
    return found != std::end(kVc3) && found->eu_address == eu_address ? found->address : 0u;
}

bool code_matches(Runtime &rt, std::uint32_t eu_function) {
    const auto *fp = std::find_if(std::begin(kFingerprints), std::end(kFingerprints),
                                  [&](const Fingerprint &f) { return f.eu_address == eu_function; });
    const std::uint32_t address = at(eu_function);
    if (fp == std::end(kFingerprints) || address == 0u || !rt.memory().contains(address, 32u)) return false;
    std::uint32_t hash = 0x811C9DC5u;
    for (std::uint32_t i = 0; i < 8u; ++i) {
        const std::uint32_t word = masked(rt.memory().load32(address + i * 4u));
        for (std::uint32_t b = 0; b < 4u; ++b) {
            hash ^= (word >> (8u * b)) & 0xFFu;
            hash *= 0x01000193u;
        }
    }
    return hash == fp->hash;
}

}  // namespace portablekit::vc3
