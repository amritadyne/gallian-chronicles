// See vc2_region.hpp and research/REGIONS.md.

#include "vc2_region.hpp"

#include "profile.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace portablekit::vc2 {
namespace {

struct Shift {
    std::uint32_t start, end;  // European addresses [start, end)
    std::int32_t delta;        // added for this edition
};

// ULUS10515 against ULES01417 (research/REGIONS.md): the US executable adds
// two calls in one function at ELF 0x2296E4 and 0x2297CC and drops two
// alignment words at ELF 0x465D78. Code before the first edit is where it
// was; .rodata and .data follow the code (+0x10); .bss and everything after
// it are not moved.
constexpr Shift kUsaShifts[] = {
    {0x08A2D6E4u, 0x08A2D7CCu, 0x08},
    {0x08A2D7CCu, 0x08C69D78u, 0x18},
    {0x08C69D78u, 0x08D36A00u, 0x10},
};

struct Mapping {
    std::uint32_t eu_address;
    std::uint32_t address;
};

// NPJH50145 (research/REGIONS.md): a separate build, so a table, not ranges.
// Made 2026-11-15 (tools/jp_addresses.py) by aligning the two executables:
// every 12-word stretch of code that occurs once in each, address fields
// masked, pairs its instructions; code addresses come from that alignment
// (four from the calls that reach them or a stretch both sides of them with
// the same shift), data addresses from the relocation targets of the paired
// instructions. Every European address the profile and its corpus patches
// use (cmake/vc2_jp_addresses.cmake holds the patches' share). The earlier
// hand-made entries (GU library, CRI, the matching stub) all agree with it.
// Sorted by European address.
constexpr Mapping kJapan[] = {
    {0x08804000u, 0x08804000u},
    {0x0880EBCCu, 0x0880E5D8u},
    {0x0880ED54u, 0x0880E754u},
    {0x08813600u, 0x08812C44u},
    {0x08813EDCu, 0x08813520u},
    {0x08835714u, 0x08834D18u},
    {0x08837C00u, 0x08837204u},
    {0x08837DE0u, 0x088373E4u},
    {0x08844AD4u, 0x088440D8u},
    {0x08844B04u, 0x08844108u},
    {0x08844C5Cu, 0x08844260u},
    {0x08844CC4u, 0x088442C8u},
    {0x0884AB90u, 0x0884A194u},
    {0x0884B90Cu, 0x0884AF10u},
    {0x0884BC68u, 0x0884B26Cu},
    {0x08886958u, 0x08885E60u},
    {0x0888B8B0u, 0x0888ADB8u},
    {0x0888F500u, 0x0888EA08u},
    {0x0888FC94u, 0x0888F19Cu},
    {0x0888FD8Cu, 0x0888F294u},
    {0x0888FD90u, 0x0888F298u},
    {0x0888FECCu, 0x0888F3D4u},
    {0x0888FED0u, 0x0888F3D8u},
    {0x0888FF00u, 0x0888F408u},
    {0x0888FF04u, 0x0888F40Cu},
    {0x08890028u, 0x0888F530u},
    {0x0889002Cu, 0x0888F534u},
    {0x088906B4u, 0x0888FBBCu},
    {0x08893C48u, 0x08893150u},
    {0x0889DEF8u, 0x0889D400u},
    {0x088ACEF0u, 0x088AC3F8u},
    {0x088AECE4u, 0x088AE1ECu},
    {0x088B2CA0u, 0x088B21A8u},
    {0x088BCC0Cu, 0x088BC114u},
    {0x088BFCC8u, 0x088BF1D0u},
    {0x088D37C0u, 0x088D2C6Cu},
    {0x088D388Cu, 0x088D2D38u},
    {0x088E1CECu, 0x088E1184u},
    {0x088E1CF4u, 0x088E118Cu},
    {0x088E1D60u, 0x088E11F8u},
    {0x088E1DB4u, 0x088E124Cu},
    {0x088E1DBCu, 0x088E1254u},
    {0x088FBF60u, 0x088FB2B8u},
    {0x0890D84Cu, 0x0890CBA0u},
    {0x0891B8ECu, 0x0891AC68u},
    {0x0892E6ACu, 0x0892D93Cu},
    {0x0893F04Cu, 0x0893E2DCu},
    {0x08941778u, 0x08940A08u},
    {0x0895AEB0u, 0x08959DF8u},
    {0x0896EB00u, 0x0896DA44u},
    {0x0896EC90u, 0x0896DBD4u},
    {0x0896EC94u, 0x0896DBD8u},
    {0x0896ECB4u, 0x0896DBF8u},
    {0x0896ECB8u, 0x0896DBFCu},
    {0x0897A03Cu, 0x08978F24u},
    {0x0897ACE0u, 0x08979BC8u},
    {0x0897C2C0u, 0x0897B1A8u},
    {0x089A2468u, 0x089A11D4u},
    {0x089AC8DCu, 0x089AB638u},
    {0x089ACB04u, 0x089AB860u},
    {0x089ACB40u, 0x089AB89Cu},
    {0x08A2D6E4u, 0x08A2C440u},
    {0x08A2D7CCu, 0x08A2C528u},
    {0x08A4C0E8u, 0x08A4AD3Cu},
    {0x08A4C170u, 0x08A4ADC4u},
    {0x08A4C178u, 0x08A4ADCCu},
    {0x08A54438u, 0x08A53084u},
    {0x08A5D240u, 0x08A5BE8Cu},
    {0x08A77E44u, 0x08A76A60u},
    {0x08A7869Cu, 0x08A772B8u},
    {0x08A78944u, 0x08A77560u},
    {0x08A928A4u, 0x08A91724u},
    {0x08AFA0D4u, 0x08AF8FC4u},
    {0x08AFB7F8u, 0x08AFA6E8u},
    {0x08AFDA78u, 0x08AFC968u},
    {0x08AFF54Cu, 0x08AFE43Cu},
    {0x08B09730u, 0x08B0862Cu},
    {0x08B09910u, 0x08B0880Cu},
    {0x08B09990u, 0x08B0888Cu},
    {0x08B09998u, 0x08B08894u},
    {0x08B09A9Cu, 0x08B08998u},
    {0x08B0C87Cu, 0x08B0B74Cu},
    {0x08B0CA24u, 0x08B0B8F4u},
    {0x08B0CD68u, 0x08B0BC38u},
    {0x08B3E244u, 0x08B3D068u},
    {0x08B3F2B4u, 0x08B3E0D8u},
    {0x08B40778u, 0x08B3F59Cu},
    {0x08B40A18u, 0x08B3F83Cu},
    {0x08B40C4Cu, 0x08B3FA70u},
    {0x08B40C54u, 0x08B3FA78u},
    {0x08B60068u, 0x08B5EE80u},
    {0x08B60164u, 0x08B5EF7Cu},
    {0x08B6B57Cu, 0x08B6A384u},
    {0x08B77898u, 0x08B763D8u},
    {0x08B77960u, 0x08B764A0u},
    {0x08B77B40u, 0x08B76680u},
    {0x08B78288u, 0x08B76DC8u},
    {0x08B78324u, 0x08B76E64u},
    {0x08B7E368u, 0x08B7CEA8u},
    {0x08B7E730u, 0x08B7D270u},
    {0x08B7E874u, 0x08B7D3B4u},
    {0x08B7E99Cu, 0x08B7D4DCu},
    {0x08B7EA58u, 0x08B7D598u},
    {0x08B7EA60u, 0x08B7D5A0u},
    {0x08B81A60u, 0x08B80598u},
    {0x08B83D54u, 0x08B828ECu},
    {0x08B85968u, 0x08B844DCu},
    {0x08B85BD4u, 0x08B84748u},
    {0x08B86F9Cu, 0x08B85B10u},
    {0x08B87000u, 0x08B85B74u},
    {0x08B87018u, 0x08B85B8Cu},
    {0x08B87064u, 0x08B85BD8u},
    {0x08B87094u, 0x08B85C08u},
    {0x08B87848u, 0x08B863BCu},
    {0x08B88280u, 0x08B86DF4u},
    {0x08B882D8u, 0x08B86E4Cu},
    {0x08B88564u, 0x08B870D8u},
    {0x08B8856Cu, 0x08B870E0u},
    {0x08B97648u, 0x08B961BCu},
    {0x08BA55B0u, 0x08BA40E4u},
    {0x08BA55D4u, 0x08BA4108u},
    {0x08BB4CC4u, 0x08BB36DCu},
    {0x08BB4CCCu, 0x08BB36E4u},
    {0x08BB4CD4u, 0x08BB36ECu},
    {0x08BB4CE8u, 0x08BB3700u},
    {0x08BB4CFCu, 0x08BB3714u},
    {0x08BB4F20u, 0x08BB3938u},
    {0x08BB4F44u, 0x08BB395Cu},
    {0x08BB4F58u, 0x08BB3970u},
    {0x08BB4FF4u, 0x08BB3A0Cu},
    {0x08BB517Cu, 0x08BB3B94u},
    {0x08BB51C4u, 0x08BB3BDCu},
    {0x08BB8CDCu, 0x08BB76F4u},
    {0x08BB8CE8u, 0x08BB7700u},
    {0x08BB8CF0u, 0x08BB7708u},
    {0x08BC8C0Cu, 0x08BC7624u},
    {0x08BC95A4u, 0x08BC7FBCu},
    {0x08BC9614u, 0x08BC802Cu},
    {0x08BC98C8u, 0x08BC82E0u},
    {0x08BD1558u, 0x08BCFF70u},
    {0x08BD16A4u, 0x08BD00BCu},
    {0x08BD19F4u, 0x08BD040Cu},
    {0x08BD1AE4u, 0x08BD04FCu},
    {0x08BD2EACu, 0x08BD18C4u},
    {0x08BD31F4u, 0x08BD1C0Cu},
    {0x08BDA8DCu, 0x08BD92F4u},
    {0x08BEA65Cu, 0x08BE9074u},
    {0x08BEAAD8u, 0x08BE94F0u},
    {0x08BEBBE0u, 0x08BEA5F8u},
    {0x08BEBC54u, 0x08BEA66Cu},
    {0x08BEBC9Cu, 0x08BEA6B4u},
    {0x08C3B910u, 0x08C3A328u},
    {0x08C592E4u, 0x08C57CFCu},
    {0x08C5953Cu, 0x08C57F54u},
    {0x08C59550u, 0x08C57F68u},
    {0x08C59584u, 0x08C57F9Cu},
    {0x08C86000u, 0x08C84A10u},
    {0x08C86EC8u, 0x08C858D8u},
    {0x08C87020u, 0x08C85A30u},
    {0x08C87074u, 0x08C85A84u},
    {0x08C87380u, 0x08C85D90u},
    {0x08C873D0u, 0x08C85DE0u},
    {0x08C87424u, 0x08C85E34u},
    {0x08C8747Cu, 0x08C85E8Cu},
    {0x08C874D8u, 0x08C85EE8u},
    {0x08C87570u, 0x08C85F80u},
    {0x08C8759Cu, 0x08C85FACu},
    {0x08C8766Cu, 0x08C8607Cu},
    {0x08C87688u, 0x08C86098u},
    {0x08C876E4u, 0x08C860F4u},
    {0x08C876F8u, 0x08C86108u},
    {0x08C87708u, 0x08C86118u},
    {0x08C87718u, 0x08C86128u},
    {0x08C87740u, 0x08C86150u},
    {0x08C877E4u, 0x08C861F4u},
    {0x08C87810u, 0x08C86220u},
    {0x08C87838u, 0x08C86248u},
    {0x08C87874u, 0x08C86284u},
    {0x08C878B4u, 0x08C862C4u},
    {0x08C878E0u, 0x08C862F0u},
    {0x08C8791Cu, 0x08C8632Cu},
    {0x08C87944u, 0x08C86354u},
    {0x08C8796Cu, 0x08C8637Cu},
    {0x08C879A8u, 0x08C863B8u},
    {0x08C879D0u, 0x08C863E0u},
    {0x08C87A04u, 0x08C86414u},
    {0x08C87A40u, 0x08C86450u},
    {0x08C87A6Cu, 0x08C8647Cu},
    {0x08C87AACu, 0x08C864BCu},
    {0x08C87AD8u, 0x08C864E8u},
    {0x08C87B30u, 0x08C86540u},
    {0x08C87B80u, 0x08C86590u},
    {0x08C87C00u, 0x08C86610u},
    {0x08C87C44u, 0x08C86654u},
    {0x08C87C64u, 0x08C86674u},
    {0x08C87C90u, 0x08C866A0u},
    {0x08C87CCCu, 0x08C866DCu},
    {0x08C87D18u, 0x08C86728u},
    {0x08C87D9Cu, 0x08C867ACu},
    {0x08C87DD8u, 0x08C867E8u},
    {0x08C87DFCu, 0x08C8680Cu},
    {0x08C87E20u, 0x08C86830u},
    {0x08C87E60u, 0x08C86870u},
    {0x08C87E9Cu, 0x08C868ACu},
    {0x08C87ED0u, 0x08C868E0u},
    {0x08C87F04u, 0x08C86914u},
    {0x08C87F5Cu, 0x08C8696Cu},
    {0x08C88000u, 0x08C86A10u},
    {0x08C880F8u, 0x08C86B08u},
    {0x08C89118u, 0x08C87B28u},
    {0x08C89300u, 0x08C87D10u},
    {0x08CB0EC0u, 0x08CAF7C4u},
    {0x08CD0698u, 0x08CCEBB8u},
    {0x08CD39FCu, 0x08CD1F04u},
    {0x08CD6818u, 0x08CD4680u},
    {0x08CE9A38u, 0x08CE7910u},
    {0x08CF99E0u, 0x08CF84E8u},
    {0x08CF9A7Cu, 0x08CF8584u},
    {0x08CFCC94u, 0x08CFB7A4u},
    {0x08D0287Cu, 0x08CF0D8Cu},
    {0x08D2F5F0u, 0x08D2E0C0u},
    {0x08D36A00u, 0x08D35500u},
    {0x08D43F98u, 0x08D42A98u},
    {0x08D565A4u, 0x08D550A4u},
    {0x08D565A8u, 0x08D550A8u},
    {0x08D565ACu, 0x08D550ACu},
    {0x08D565B0u, 0x08D550B0u},
    {0x08D565B4u, 0x08D550B4u},
    {0x08D5AEC0u, 0x08D599C0u},
    {0x08D5AF08u, 0x08D59A08u},
    {0x08D5B918u, 0x08D5A418u},
    {0x08D5B934u, 0x08D5A434u},
    {0x08D5DA08u, 0x08D5C508u},
    {0x08D61DF8u, 0x08D608F8u},
    {0x08D61E40u, 0x08D60940u},
};

struct Fingerprint {
    std::uint32_t eu_address;
    std::uint32_t hash;  // FNV-1a of the first 8 instructions, address fields masked
};

// Every function address the profile uses, fingerprinted from the European
// executable and checked against the US one when these tables were made.
constexpr Fingerprint kFingerprints[] = {
    {0x08804000u, 0x58D04E06u},
    {0x08BB4CC4u, 0xDDA67AD4u},  // camera getters (VC2_TRACE_WIDE counts their callers)
    {0x08BB4CCCu, 0x93632379u},
    {0x08BB4CD4u, 0xBAA804D4u},
    {0x08BB4CE8u, 0x5C281FF8u},
    {0x08BB4CFCu, 0x4ED4D113u},
    {0x08BB4F20u, 0xD97EDC14u},
    {0x08BB4F44u, 0xBAA804D4u},
    {0x08BB4F58u, 0x6411F63Cu},
    {0x08BB4FF4u, 0x13A4A54Fu},  // Kurofune perspective builder
    {0x08BD31F4u, 0x91C6F7CCu},  // frame-rate module: set the frame interval (vc2_tick.cpp)
    {0x08BEAAD8u, 0x3A3FA37Cu},  // Kurofune screen size getter
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
    {0x08C89300u, 0x5187FACCu},};

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

Region region() {
    const ProfileVariant *variant = active_variant();
    if (variant != nullptr && std::strcmp(variant->key, "us") == 0) return Region::Usa;
    if (variant != nullptr && std::strcmp(variant->key, "jp") == 0) return Region::Japan;
    return Region::Europe;
}

const char *region_name() {
    switch (region()) {
    case Region::Usa: return "US (ULUS10515)";
    case Region::Japan: return "Japan (NPJH50145)";
    default: return "Europe (ULES01417)";
    }
}

std::uint32_t at(std::uint32_t eu_address) {
    if (region() == Region::Europe) return eu_address;
    if (region() == Region::Japan) {
        const auto *found = std::lower_bound(std::begin(kJapan), std::end(kJapan), eu_address,
                                             [](const Mapping &m, std::uint32_t a) { return m.eu_address < a; });
        return found != std::end(kJapan) && found->eu_address == eu_address ? found->address : 0u;
    }
    for (const Shift &shift : kUsaShifts)
        if (eu_address >= shift.start && eu_address < shift.end)
            return static_cast<std::uint32_t>(static_cast<std::int64_t>(eu_address) + shift.delta);
    return eu_address;
}

bool code_matches(Runtime &rt, std::uint32_t eu_function) {
    for (const Fingerprint &fingerprint : kFingerprints) {
        if (fingerprint.eu_address != eu_function) continue;
        const std::uint32_t address = at(eu_function);
        if (address == 0u || !rt.memory().contains(address, 32u)) return false;
        std::uint32_t hash = 0x811C9DC5u;
        for (std::uint32_t i = 0; i < 8u; ++i) {
            const std::uint32_t word = masked(rt.memory().load32(address + i * 4u));
            for (std::uint32_t k = 0; k < 4u; ++k) {
                hash ^= (word >> (8u * k)) & 0xFFu;
                hash *= 0x01000193u;
            }
        }
        return hash == fingerprint.hash;
    }
    return false;
}

}  // namespace portablekit::vc2
