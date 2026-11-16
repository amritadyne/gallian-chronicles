// sceUtilitySavedata*: the PSP's save-data dialog, backed by PSP-layout
// folders on the host (see save_data/savedata_store.hpp).
//
// Load, save, delete, sizes and the list modes are answered from those
// folders; each game uses its own few. <prefix>_TRACE_SAVEDATA prints every
// request, which is how to find out which a game asks for.
// No save-data UI is drawn: modes that would show a list act as if the player
// confirmed the first entry. Every mode, the failures included, reports QUIT
// after a few polls; see utility_dialog.hpp.
#include "../profile.hpp"
#include "hle_common.hpp"
#include "utility_dialog.hpp"

#include "crypto_keys.hpp"
#include "save_data/param_sfo.hpp"
#include "save_data/savedata_crypto.hpp"
#include "save_data/savedata_store.hpp"
#include "save_data/save_transfer.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace portablekit {
namespace {

// SceUtilitySavedataParam, after the common dialog header.
namespace param {
constexpr std::uint32_t kMode = 0x30u;
// Set by the system on a load: whether the save was made on this console
// (1021 here: its low two bits 1 mean bound to this PSP). VC2 shows
// "Save data creation date does not match the PSP system" (MTPA_SYS 49) and
// turns off data-link and password content when the low two bits are not 1.
constexpr std::uint32_t kBind = 0x34u;
constexpr std::uint32_t kBoundHere = 1021u;
constexpr std::uint32_t kOverwrite = 0x38u;
constexpr std::uint32_t kGameName = 0x3Cu;      // char[13]
constexpr std::uint32_t kSaveName = 0x4Cu;      // char[20]
constexpr std::uint32_t kSaveNameList = 0x60u;  // pointer to char[20] entries, "" terminated
constexpr std::uint32_t kFileName = 0x64u;      // char[13]
constexpr std::uint32_t kDataBuf = 0x74u;
constexpr std::uint32_t kDataBufSize = 0x78u;
constexpr std::uint32_t kDataSize = 0x7Cu;
constexpr std::uint32_t kTitle = 0x80u;          // char[128]
constexpr std::uint32_t kSavedataTitle = 0x100u; // char[128]
constexpr std::uint32_t kDetail = 0x180u;        // char[1024]
constexpr std::uint32_t kParentalLevel = 0x580u;
constexpr std::uint32_t kIcon0 = 0x584u;  // {buf, bufSize, size, unknown}
constexpr std::uint32_t kIcon1 = 0x594u;
constexpr std::uint32_t kPic1 = 0x5A4u;
constexpr std::uint32_t kSnd0 = 0x5B4u;
constexpr std::uint32_t kFocus = 0x5C8u;
constexpr std::uint32_t kMsFree = 0x5D0u;
constexpr std::uint32_t kMsData = 0x5D4u;
constexpr std::uint32_t kUtilityData = 0x5D8u;
constexpr std::uint32_t kKey = 0x5DCu;  // char[16], firmware 2.00 and later
constexpr std::uint32_t kSecureVersion = 0x5ECu;
// LIST: a pointer to {maxCount, resultCount, entries}. Read off a game's own
// request (the only pointer in the block past the names), not recalled.
constexpr std::uint32_t kIdList = 0x5F4u;
// FILES: a pointer to SceUtilitySavedataFileListInfo, the field after idList.
constexpr std::uint32_t kFileList = 0x5F8u;
// GETSIZE: a pointer to PspUtilitySavedataSizeInfo, after fileList.
constexpr std::uint32_t kSizeInfo = 0x5FCu;
constexpr std::uint32_t kMinimumSizeWithKey = 0x5ECu;
} // namespace param

enum Mode : std::uint32_t {
    kAutoLoad = 0,
    kAutoSave = 1,
    kLoad = 2,
    kSave = 3,
    kListLoad = 4,
    kListSave = 5,
    kListDelete = 6,
    kDelete = 7,
    kSizes = 8,
    kAutoDelete = 9,
    kSingleDelete = 10,
    kList = 11,
    kFiles = 12,
    kMakeDataSecure = 13,
    kMakeData = 14,
    kReadDataSecure = 15,
    kReadData = 16,
    kWriteDataSecure = 17,
    kWriteData = 18,
    kEraseSecure = 19,
    kErase = 20,
    kDeleteData = 21,
    kGetSize = 22,
};

const char *mode_name(std::uint32_t mode) {
    static const char *const names[] = {"AUTOLOAD", "AUTOSAVE", "LOAD", "SAVE", "LISTLOAD", "LISTSAVE",
                                        "LISTDELETE", "DELETE", "SIZES", "AUTODELETE", "SINGLEDELETE", "LIST",
                                        "FILES", "MAKEDATASECURE", "MAKEDATA", "READDATASECURE", "READDATA",
                                        "WRITEDATASECURE", "WRITEDATA", "ERASESECURE", "ERASE", "DELETEDATA",
                                        "GETSIZE"};
    return mode < std::size(names) ? names[mode] : "UNKNOWN";
}

// Result codes written to the parameter block's result field.
namespace result {
constexpr std::uint32_t kOk = 0u;
constexpr std::uint32_t kLoadDataBroken = 0x80110306u;
constexpr std::uint32_t kLoadNoData = 0x80110307u;
constexpr std::uint32_t kLoadParam = 0x80110308u;
constexpr std::uint32_t kRwDataBroken = 0x80110326u;
constexpr std::uint32_t kRwNoData = 0x80110327u;
constexpr std::uint32_t kRwBadParams = 0x80110328u;
constexpr std::uint32_t kRwFileNotFound = 0x80110329u;
constexpr std::uint32_t kDeleteNoData = 0x80110347u;
constexpr std::uint32_t kSaveAccessError = 0x80110385u;
constexpr std::uint32_t kSaveParam = 0x80110388u;
constexpr std::uint32_t kSizesNoData = 0x801103C7u;
} // namespace result

constexpr std::uint32_t kClusterSize = 0x8000u;
constexpr std::uint32_t kFreeClusters = 0x8000u;  // 1 GiB

struct SavedataState {
    std::filesystem::path memory_stick;
    DialogLifecycle dialog;
    std::uint32_t pending_params{};
    std::chrono::steady_clock::time_point ready_at{};
};

SavedataState &state() {
    static SavedataState s;
    return s;
}

bool trace_savedata() {
    static const bool enabled = portablekit::env("TRACE_SAVEDATA") != nullptr;
    return enabled;
}

std::string hex_bytes(const savedata::Block &block) {
    std::string text;
    char digits[3];
    for (const std::uint8_t b : block) {
        std::snprintf(digits, sizeof digits, "%02x", b);
        text += digits;
    }
    return text;
}

// "<>" stands for "no save name": the folder is the game name alone.
std::string save_name_at(const psprecomp::GuestMemory &memory, std::uint32_t address) {
    std::string name = read_cstring(memory, address, 20u);
    return name == "<>" ? std::string{} : name;
}

std::vector<std::string> save_name_list(const psprecomp::GuestMemory &memory, std::uint32_t list) {
    std::vector<std::string> names;
    if (list == 0u) return names;
    for (std::uint32_t i = 0; i < 64u; ++i) {
        const std::uint32_t entry = list + i * 20u;
        if (memory.load8(entry) == 0u) break;
        names.push_back(save_name_at(memory, entry));
    }
    return names;
}

std::vector<std::uint8_t> read_guest(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t size) {
    std::vector<std::uint8_t> bytes(size);
    for (std::uint32_t i = 0; i < size; ++i) bytes[i] = memory.load8(address + i);
    return bytes;
}

// {buf, bufSize, size, unknown}: the bytes the game supplies for an icon file.
std::vector<std::uint8_t> read_file_data(const psprecomp::GuestMemory &memory, std::uint32_t field) {
    const std::uint32_t buffer = memory.load32(field);
    const std::uint32_t size = std::min(memory.load32(field + 8u), memory.load32(field + 4u));
    if (buffer == 0u || size == 0u) return {};
    return read_guest(memory, buffer, size);
}

savedata::SaveFiles files_for(const psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    savedata::SaveFiles files;
    files.game_name = read_cstring(memory, params + param::kGameName, 13u);
    files.save_name = save_name;
    files.file_name = read_cstring(memory, params + param::kFileName, 13u);
    if (memory.load32(params + dialog_common::kSizeOffset) > param::kMinimumSizeWithKey) {
        savedata::Block key{};
        for (std::uint32_t i = 0; i < key.size(); ++i) key[i] = memory.load8(params + param::kKey + i);
        if (!savedata::is_zero(key)) {
            // Without the console's keys the save is kept unencrypted; the
            // game cannot tell, since it only ever sees the plain data. The
            // key is still remembered, so the save can be encrypted for a PSP
            // once keys are added.
            if (savedata_keys_available()) files.key = key;
            // The menu's Import checks saves with it.
            savedata::remember_game_key(files.game_name, key);
        }
    }
    return files;
}

void log_request(const psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t mode = memory.load32(params + param::kMode);
    std::cerr << "[savedata] " << mode_name(mode) << " (" << mode << ")"
              << " game=\"" << read_cstring(memory, params + param::kGameName, 13u) << "\""
              << " save=\"" << read_cstring(memory, params + param::kSaveName, 20u) << "\""
              << " file=\"" << read_cstring(memory, params + param::kFileName, 13u) << "\"";
    if (trace_savedata()) {
        const std::uint32_t size = memory.load32(params + dialog_common::kSizeOffset);
        std::cerr << " size=" << psprecomp::hex32(size)
                  << " buf=" << psprecomp::hex32(memory.load32(params + param::kDataBuf))
                  << " bufSize=" << psprecomp::hex32(memory.load32(params + param::kDataBufSize))
                  << " dataSize=" << psprecomp::hex32(memory.load32(params + param::kDataSize))
                  << " overwrite=" << memory.load32(params + param::kOverwrite)
                  << " focus=" << memory.load32(params + param::kFocus)
                  << " icon0=" << memory.load32(params + param::kIcon0 + 8u)
                  << " icon1=" << memory.load32(params + param::kIcon1 + 8u)
                  << " pic1=" << memory.load32(params + param::kPic1 + 8u)
                  << " snd0=" << memory.load32(params + param::kSnd0 + 8u);
        if (size > param::kMinimumSizeWithKey) {
            savedata::Block key{};
            for (std::uint32_t i = 0; i < key.size(); ++i) key[i] = memory.load8(params + param::kKey + i);
            std::cerr << " key=" << hex_bytes(key)
                      << " secureVersion=" << memory.load32(params + param::kSecureVersion);
        }
        const auto names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
        if (!names.empty()) {
            std::cerr << " list=[";
            for (std::size_t i = 0; i < names.size(); ++i) std::cerr << (i ? "," : "") << "\"" << names[i] << "\"";
            std::cerr << "]";
        }
        if (mode == kFiles) {
            const std::uint32_t file_list = memory.load32(params + param::kFileList);
            std::cerr << " fileList=" << psprecomp::hex32(file_list);
            if (file_list != 0u && memory.contains(file_list, 36u)) {
                std::cerr << " words=[";
                for (std::uint32_t i = 0u; i < 9u; ++i)
                    std::cerr << (i == 0u ? "" : ",") << psprecomp::hex32(memory.load32(file_list + i * 4u));
                std::cerr << "]";
            }
        }
        if (mode == kGetSize) {
            const std::uint32_t size_info = memory.load32(params + param::kSizeInfo);
            std::cerr << " sizeInfo=" << psprecomp::hex32(size_info);
            if (size_info != 0u && memory.contains(size_info, 60u)) {
                std::cerr << " words=[";
                for (std::uint32_t i = 0u; i < 15u; ++i)
                    std::cerr << (i == 0u ? "" : ",") << psprecomp::hex32(memory.load32(size_info + i * 4u));
                std::cerr << "]";
            }
        }
    }
    std::cerr << "\n";
}

std::uint32_t do_load(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    const std::uint32_t buffer = memory.load32(params + param::kDataBuf);
    const std::uint32_t capacity = memory.load32(params + param::kDataBufSize);
    if (files.game_name.empty() || files.file_name.empty() || buffer == 0u) return result::kLoadParam;

    const auto loaded = savedata::load_save(state().memory_stick, files);
    if (loaded.status == savedata::LoadStatus::NoData) {
        std::cerr << "[savedata] no save data in " << savedata::save_folder(state().memory_stick, files).string() << "\n";
        return result::kLoadNoData;
    }
    if (loaded.status == savedata::LoadStatus::Broken) {
        std::cerr << "[savedata] broken save in " << savedata::save_folder(state().memory_stick, files).string()
                  << ": " << loaded.reason << "\n";
        return result::kLoadDataBroken;
    }
    const auto &data = loaded.contents.data;
    const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(data.size(), capacity));
    memory.copy_in(buffer, std::span<const std::uint8_t>(data.data(), count));
    memory.store32(params + param::kDataSize, count);
    memory.store32(params + param::kBind, param::kBoundHere);  // made on this console
    write_cstring(memory, params + param::kTitle, loaded.contents.title, 128u);
    write_cstring(memory, params + param::kSavedataTitle, loaded.contents.savedata_title, 128u);
    write_cstring(memory, params + param::kDetail, loaded.contents.detail, 1024u);
    std::cerr << "[savedata] loaded " << count << " bytes from "
              << savedata::save_folder(state().memory_stick, files).string()
              << (files.key ? " (decrypted)" : "") << "\n";
    // An encrypted file is padded with zeros to a multiple of 16 bytes, so a
    // buffer the size the game saved leaves up to 15 zero bytes behind. Only
    // data that does not fit is worth saying.
    const bool cut = data.size() > capacity &&
                     (data.size() - capacity >= 16u ||
                      std::any_of(data.begin() + capacity, data.end(), [](std::uint8_t b) { return b != 0u; }));
    if (cut)
        std::cerr << "[savedata] " << files.file_name << " holds " << data.size() << " bytes; the buffer takes "
                  << capacity << "\n";
    return result::kOk;
}

// The data, PARAM.SFO text and media files a save request supplies.
savedata::SaveContents contents_from(const psprecomp::GuestMemory &memory, std::uint32_t params) {
    savedata::SaveContents contents;
    if (const std::uint32_t buffer = memory.load32(params + param::kDataBuf); buffer != 0u)
        contents.data = read_guest(memory, buffer, memory.load32(params + param::kDataSize));
    contents.title = read_cstring(memory, params + param::kTitle, 128u);
    contents.savedata_title = read_cstring(memory, params + param::kSavedataTitle, 128u);
    contents.detail = read_cstring(memory, params + param::kDetail, 1024u);
    contents.parental_level = memory.load8(params + param::kParentalLevel);
    contents.icon0 = read_file_data(memory, params + param::kIcon0);
    contents.icon1 = read_file_data(memory, params + param::kIcon1);
    contents.pic1 = read_file_data(memory, params + param::kPic1);
    contents.snd0 = read_file_data(memory, params + param::kSnd0);
    return contents;
}

// --- Creation dates --------------------------------------------------------------
// The PSP keeps a save file's creation date. The host does not keep one a
// game can rely on: saves are written to a temporary file and renamed, and a
// copied save gets a new one. Valkyria Chronicles II stores its save's
// creation date and, when FILES reports another, treats the save as one from
// another console ("Save data creation date does not match the PSP system")
// and switches off content unlocked by data link or password. So the date
// each file was first written (or, for a save made before this record, first
// seen) is kept beside it in portablekit-created.txt (lower case: never
// listed as part of the save, and copied with it), and FILES reports that as
// the file's creation date; accessed and modified stay the host's write time.

constexpr const char *kCreatedRecord = "portablekit-created.txt";

std::time_t host_time(std::filesystem::file_time_type written) {
    // file_time_type's clock is not the system clock everywhere; offset by now.
    const auto system_time = std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                 written - std::filesystem::file_time_type::clock::now());
    return static_cast<std::time_t>(std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch()).count());
}

// File name -> creation time (seconds since 1970), as recorded.
std::map<std::string, long long> read_created(const std::filesystem::path &folder) {
    std::map<std::string, long long> created;
    std::ifstream in(folder / kCreatedRecord);
    std::string name;
    long long when = 0;
    while (in >> name >> when) created[name] = when;
    return created;
}

void write_created(const std::filesystem::path &folder, const std::map<std::string, long long> &created) {
    std::error_code error;
    if (created.empty()) {
        std::filesystem::remove(folder / kCreatedRecord, error);
        return;
    }
    std::ofstream out(folder / kCreatedRecord, std::ios::trunc);
    for (const auto &[name, when] : created) out << name << ' ' << when << '\n';
}

// After a save: each file of the save without a recorded creation date gets
// its write time (new saves: when they were made); files gone are dropped.
void record_created(const std::filesystem::path &folder) {
    auto created = read_created(folder);
    std::map<std::string, long long> now;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(folder, error)) {
        if (!entry.is_regular_file(error)) continue;
        const std::string name = entry.path().filename().string();
        if (name == kCreatedRecord) continue;
        const auto found = created.find(name);
        now[name] = found != created.end() ? found->second : static_cast<long long>(host_time(entry.last_write_time(error)));
    }
    if (now != created) write_created(folder, now);
}

std::uint32_t do_save(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    const std::uint32_t buffer = memory.load32(params + param::kDataBuf);
    const std::uint32_t size = memory.load32(params + param::kDataSize);
    if (files.game_name.empty() || files.file_name.empty() || buffer == 0u) return result::kSaveParam;

    const savedata::SaveContents contents = contents_from(memory, params);

    std::string error;
    if (!savedata::write_save(state().memory_stick, files, contents, error)) {
        std::cerr << "[savedata] save failed: " << error << "\n";
        return result::kSaveAccessError;
    }
    std::cerr << "[savedata] saved " << size << " bytes to " << savedata::save_folder(state().memory_stick, files).string()
              << (files.key ? " (encrypted)" : "") << "\n";
    record_created(savedata::save_folder(state().memory_stick, files));
    return result::kOk;
}

std::uint32_t do_delete(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    if (!savedata::delete_save(state().memory_stick, files)) return result::kDeleteNoData;
    {
        std::error_code error;
        const std::filesystem::path folder = savedata::save_folder(state().memory_stick, files);
        if (std::filesystem::is_directory(folder, error)) record_created(folder);
    }
    std::cerr << "[savedata] deleted " << savedata::save_folder(state().memory_stick, files).string() << "\n";
    return result::kOk;
}

void write_size_string(psprecomp::GuestMemory &memory, std::uint32_t address, std::uint64_t kilobytes) {
    const std::string text = kilobytes >= 1024u * 1024u ? std::to_string(kilobytes / (1024u * 1024u)) + " GB"
                             : kilobytes >= 1024u     ? std::to_string(kilobytes / 1024u) + " MB"
                                                      : std::to_string(kilobytes) + " KB";
    write_cstring(memory, address, text, 8u);
}

// SIZES: free space on the stick, the space an existing save takes, and the
// space the requested save would need.
std::uint32_t do_sizes(psprecomp::GuestMemory &memory, std::uint32_t params) {
    std::uint32_t outcome = result::kOk;
    if (const std::uint32_t free = memory.load32(params + param::kMsFree); free != 0u) {
        memory.store32(free, kClusterSize);
        memory.store32(free + 4u, kFreeClusters);
        const std::uint64_t free_kb = static_cast<std::uint64_t>(kClusterSize) * kFreeClusters / 1024u;
        memory.store32(free + 8u, static_cast<std::uint32_t>(free_kb));
        write_size_string(memory, free + 12u, free_kb);
    }
    if (const std::uint32_t data = memory.load32(params + param::kMsData); data != 0u) {
        savedata::SaveFiles files;
        files.game_name = read_cstring(memory, data, 13u);
        files.save_name = save_name_at(memory, data + 16u);
        const std::uint64_t bytes = savedata::save_size(state().memory_stick, files);
        const std::uint32_t info = data + 36u;
        const auto clusters = static_cast<std::uint32_t>((bytes + kClusterSize - 1u) / kClusterSize);
        memory.store32(info, clusters);
        memory.store32(info + 4u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        write_size_string(memory, info + 8u, (bytes + 1023u) / 1024u);
        memory.store32(info + 16u, static_cast<std::uint32_t>((bytes + 1023u) / 1024u));
        write_size_string(memory, info + 20u, (bytes + 1023u) / 1024u);
        if (bytes == 0u) outcome = result::kSizesNoData;
    }
    if (const std::uint32_t needed = memory.load32(params + param::kUtilityData); needed != 0u) {
        std::uint64_t bytes = memory.load32(params + param::kDataSize) + 16u;
        for (const std::uint32_t field : {param::kIcon0, param::kIcon1, param::kPic1, param::kSnd0})
            bytes += memory.load32(params + field + 8u);
        bytes += 0x2000u;  // PARAM.SFO and directory entries
        const auto clusters = static_cast<std::uint32_t>((bytes + kClusterSize - 1u) / kClusterSize);
        const std::uint64_t kb = static_cast<std::uint64_t>(clusters) * kClusterSize / 1024u;
        memory.store32(needed, clusters);
        memory.store32(needed + 4u, static_cast<std::uint32_t>(kb));
        write_size_string(memory, needed + 8u, kb);
        memory.store32(needed + 16u, static_cast<std::uint32_t>(kb));
        write_size_string(memory, needed + 20u, kb);
    }
    return outcome;
}

// GETSIZE has a separate PSP structure from SIZES.  VC3 uses it before
// creating its root system save: it supplies the secure/normal files it plans
// to write and expects the memory-stick capacity fields back.  Treating it as
// SIZES leaves that structure untouched and makes its first-run loop endless.
std::uint32_t do_get_size(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    constexpr std::uint32_t kNumSecure = 0u;
    constexpr std::uint32_t kNumNormal = 4u;
    constexpr std::uint32_t kSecureEntries = 8u;
    constexpr std::uint32_t kNormalEntries = 12u;
    constexpr std::uint32_t kSectorSize = 16u;
    constexpr std::uint32_t kFreeSectors = 20u;
    constexpr std::uint32_t kFreeKb = 24u;
    constexpr std::uint32_t kFreeText = 28u;
    constexpr std::uint32_t kNeededKb = 36u;
    constexpr std::uint32_t kNeededText = 40u;
    constexpr std::uint32_t kOverwriteKb = 48u;
    constexpr std::uint32_t kOverwriteText = 52u;
    constexpr std::uint32_t kInfoSize = 60u;
    constexpr std::uint32_t kEntrySize = 24u;  // uint64 size + char name[16]

    const std::uint32_t info = memory.load32(params + param::kSizeInfo);
    if (info == 0u || !memory.contains(info, kInfoSize)) return result::kRwBadParams;
    const std::int32_t secure_count = static_cast<std::int32_t>(memory.load32(info + kNumSecure));
    const std::int32_t normal_count = static_cast<std::int32_t>(memory.load32(info + kNumNormal));
    const std::uint32_t secure_entries = memory.load32(info + kSecureEntries);
    const std::uint32_t normal_entries = memory.load32(info + kNormalEntries);
    if (secure_count < 0 || normal_count < 0 ||
        (secure_count != 0 && (secure_entries == 0u || !memory.contains(secure_entries, static_cast<std::uint32_t>(secure_count) * kEntrySize))) ||
        (normal_count != 0 && (normal_entries == 0u || !memory.contains(normal_entries, static_cast<std::uint32_t>(normal_count) * kEntrySize))) ||
        secure_count > 99 || normal_count > 8192)
        return result::kRwBadParams;

    std::uint64_t requested = 0u;
    auto sum_entries = [&](std::uint32_t entries, std::int32_t count, bool secure) {
        for (std::int32_t i = 0; i < count; ++i) {
            const std::uint32_t at = entries + static_cast<std::uint32_t>(i) * kEntrySize;
            const std::uint64_t bytes = static_cast<std::uint64_t>(memory.load32(at)) |
                                        (static_cast<std::uint64_t>(memory.load32(at + 4u)) << 32u);
            requested += bytes + (secure ? 16u : 0u);
        }
    };
    sum_entries(secure_entries, secure_count, true);
    sum_entries(normal_entries, normal_count, false);

    const auto files = files_for(memory, params, save_name);
    const std::uint64_t existing = files.game_name.empty() ? 0u : savedata::save_size(state().memory_stick, files);
    constexpr std::uint64_t free_bytes = static_cast<std::uint64_t>(kClusterSize) * kFreeClusters;
    memory.store32(info + kSectorSize, kClusterSize);
    memory.store32(info + kFreeSectors, kFreeClusters);
    memory.store32(info + kFreeKb, static_cast<std::uint32_t>(free_bytes / 1024u));
    write_cstring(memory, info + kFreeText, "1 GB", 8u);
    const std::uint64_t additional = requested > existing ? requested - existing : 0u;
    const std::uint32_t needed_kb = additional > free_bytes ? static_cast<std::uint32_t>((additional - free_bytes + 1023u) / 1024u) : 0u;
    memory.store32(info + kNeededKb, needed_kb);
    memory.store32(info + kOverwriteKb, needed_kb);
    if (needed_kb != 0u) {
        write_size_string(memory, info + kNeededText, needed_kb);
        write_size_string(memory, info + kOverwriteText, needed_kb);
    }
    if (trace_savedata())
        std::cerr << "[savedata] GETSIZE requested=" << requested << " existing=" << existing
                  << " neededKB=" << needed_kb << "\n";
    return existing == 0u ? result::kRwNoData : result::kOk;
}

// Picks the save a list dialog would land on when the player just confirms:
// for loading and deleting the first listed save that exists, for saving the
// first listed name.
std::optional<std::string> pick_from_list(psprecomp::GuestMemory &memory, std::uint32_t params, bool must_exist) {
    const auto names = save_name_list(memory, memory.load32(params + param::kSaveNameList));
    for (const auto &name : names) {
        if (!must_exist || savedata::save_exists(state().memory_stick, files_for(memory, params, name))) return name;
    }
    if (!must_exist) return save_name_at(memory, params + param::kSaveName);
    return std::nullopt;
}

// A save name pattern as LIST takes it: '?' stands for any one character.
bool name_matches(std::string_view pattern, std::string_view name) {
    if (pattern.size() != name.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (pattern[i] != '?' && pattern[i] != name[i]) return false;
    return true;
}

// LIST: which saves of this game match the save name, up to the list's
// capacity. Each entry is a folder mode word, three 16-byte date records
// (created, accessed, modified) and the 20-byte save name. Only the count has
// been seen used, by a game with no saves yet; the entry layout is the
// documented one, not traced.
std::uint32_t do_list(psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t list = memory.load32(params + param::kIdList);
    if (list == 0u || !memory.contains(list, 12u)) return result::kLoadParam;
    const std::uint32_t capacity = memory.load32(list);
    const std::uint32_t entries = memory.load32(list + 8u);
    const std::string game_name = read_cstring(memory, params + param::kGameName, 13u);
    const std::string pattern = save_name_at(memory, params + param::kSaveName);
    std::vector<std::pair<std::string, std::filesystem::file_time_type>> found;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(savedata::savedata_root(state().memory_stick), error)) {
        if (!entry.is_directory()) continue;
        const std::string folder = entry.path().filename().string();
        if (!folder.starts_with(game_name)) continue;
        const std::string save = folder.substr(game_name.size());
        if (!name_matches(pattern, save)) continue;
        found.emplace_back(save, entry.last_write_time(error));
    }
    std::sort(found.begin(), found.end());
    constexpr std::uint32_t kEntrySize = 4u + 3u * 16u + 20u;
    std::uint32_t count = 0u;
    for (const auto &[save, written] : found) {
        if (count >= capacity || entries == 0u) break;
        const std::uint32_t at = entries + count * kEntrySize;
        if (!memory.contains(at, kEntrySize)) break;
        memory.store32(at, 0x11FFu);  // a folder, readable and writable
        // file_time_type's clock is not the system clock everywhere, and not
        // every standard library converts between them; offset by now.
        const auto system_time = std::chrono::system_clock::now() +
                                 std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                     written - std::filesystem::file_time_type::clock::now());
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(system_time.time_since_epoch());
        const std::time_t time = static_cast<std::time_t>(seconds.count());
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        for (std::uint32_t record = 0; record < 3u; ++record) {
            const std::uint32_t date = at + 4u + record * 16u;
            memory.store16(date, static_cast<std::uint16_t>(tm.tm_year + 1900));
            memory.store16(date + 2u, static_cast<std::uint16_t>(tm.tm_mon + 1));
            memory.store16(date + 4u, static_cast<std::uint16_t>(tm.tm_mday));
            memory.store16(date + 6u, static_cast<std::uint16_t>(tm.tm_hour));
            memory.store16(date + 8u, static_cast<std::uint16_t>(tm.tm_min));
            memory.store16(date + 10u, static_cast<std::uint16_t>(tm.tm_sec));
            memory.store32(date + 12u, 0u);
        }
        write_cstring(memory, at + 52u, save, 20u);
        ++count;
    }
    memory.store32(list + 4u, count);
    if (trace_savedata())
        std::cerr << "[savedata] LIST " << game_name << pattern << ": " << count << " of " << found.size()
                  << " matching saves\n";
    return result::kOk;
}

// A ScePspDateTime (year, month, day, hour, minute, second as u16, then a
// u32 of microseconds) for a host file time, in local time.
void store_psp_date(psprecomp::GuestMemory &memory, std::uint32_t at, std::time_t time) {
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &time);
#else
    localtime_r(&time, &tm);
#endif
    memory.store16(at, static_cast<std::uint16_t>(tm.tm_year + 1900));
    memory.store16(at + 2u, static_cast<std::uint16_t>(tm.tm_mon + 1));
    memory.store16(at + 4u, static_cast<std::uint16_t>(tm.tm_mday));
    memory.store16(at + 6u, static_cast<std::uint16_t>(tm.tm_hour));
    memory.store16(at + 8u, static_cast<std::uint16_t>(tm.tm_min));
    memory.store16(at + 10u, static_cast<std::uint16_t>(tm.tm_sec));
    memory.store32(at + 12u, 0u);
}

// FILES: the files in one save folder, in the three lists the PSP keeps:
// secure (named in PARAM.SFO's SAVEDATA_FILE_LIST), normal, and system
// (PARAM.SFO and the icon/sound files). The info block is nine words:
// max secure/normal/system, result counts, then the three entry arrays.
// Each 80-byte entry is a mode word, an unused word, a 64-bit size, the
// created/accessed/modified dates and a 16-byte name. A folder that does
// not exist reports RW_NO_DATA, which is how a game learns there is no save
// yet. The caller can then use GETSIZE and MAKEDATA/WRITEDATA to create it.
std::uint32_t do_files(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const std::uint32_t info = memory.load32(params + param::kFileList);
    if (info == 0u || !memory.contains(info, 36u)) return result::kRwBadParams;
    const auto files = files_for(memory, params, save_name);
    const std::filesystem::path folder = savedata::save_folder(state().memory_stick, files);
    std::error_code error;
    if (files.game_name.empty() || !std::filesystem::is_directory(folder, error)) {
        if (trace_savedata()) std::cerr << "[savedata] FILES: no folder " << folder.string() << "\n";
        return result::kRwNoData;
    }
    for (std::uint32_t i = 0; i < 3u; ++i) memory.store32(info + 12u + i * 4u, 0u);

    std::ifstream sfo_file(folder / "PARAM.SFO", std::ios::binary);
    const std::vector<std::uint8_t> sfo_bytes{std::istreambuf_iterator<char>(sfo_file), std::istreambuf_iterator<char>()};
    const auto sfo = savedata::ParamSfo::parse(sfo_bytes);
    if (!sfo) return result::kRwDataBroken;
    std::vector<std::string> secure_names;
    std::vector<std::string> encrypted_names;
    if (const auto *list = sfo->binary("SAVEDATA_FILE_LIST"); list != nullptr) {
        // 32-byte entries: a 13-byte name, its 16-byte hash and padding. An
        // encrypted file carries a hash; a plain secure file is listed with
        // a zero one.
        for (std::size_t offset = 0; offset + 32u <= list->size(); offset += 32u) {
            std::string name;
            for (std::size_t i = 0; i < 13u && (*list)[offset + i] != 0u; ++i) name.push_back(static_cast<char>((*list)[offset + i]));
            if (name.empty()) continue;
            if (std::any_of(list->begin() + static_cast<std::ptrdiff_t>(offset + 13u),
                            list->begin() + static_cast<std::ptrdiff_t>(offset + 29u), [](std::uint8_t b) { return b != 0u; }))
                encrypted_names.push_back(name);
            secure_names.push_back(std::move(name));
        }
    }

    std::vector<std::filesystem::directory_entry> entries;
    for (const auto &entry : std::filesystem::directory_iterator(folder, error))
        if (entry.is_regular_file(error)) entries.push_back(entry);
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return a.path().filename() < b.path().filename(); });

    constexpr std::uint32_t kEntrySize = 80u;
    auto created = read_created(folder);
    bool created_changed = false;
    for (const auto &entry : entries) {
        const std::string name = entry.path().filename().string();
        // PSP file names are upper case; anything else (a host temporary
        // file, say) is not part of the save.
        if (name.empty() || name.size() > 15u ||
            std::any_of(name.begin(), name.end(), [](unsigned char c) { return std::islower(c) != 0; }))
            continue;
        std::uint32_t kind = 1u;  // normal
        if (name == "PARAM.SFO" || name == "ICON0.PNG" || name == "ICON1.PMF" || name == "PIC1.PNG" || name == "SND0.AT3")
            kind = 2u;
        else if (std::find(secure_names.begin(), secure_names.end(), name) != secure_names.end())
            kind = 0u;
        const std::uint32_t capacity = memory.load32(info + kind * 4u);
        const std::uint32_t count = memory.load32(info + 12u + kind * 4u);
        const std::uint32_t array = memory.load32(info + 24u + kind * 4u);
        if (array == 0u || count >= capacity) continue;
        const std::uint32_t at = array + count * kEntrySize;
        if (!memory.contains(at, kEntrySize)) continue;
        std::uint64_t size = entry.file_size(error);
        // An encrypted secure file holds 16 bytes more than the data the game
        // wrote, and the PSP reports the game's size. Valkyria Chronicles II
        // compares these sizes with what it saved and deletes the save as
        // corrupt when they differ.
        if (kind == 0u && size >= 16u &&
            std::find(encrypted_names.begin(), encrypted_names.end(), name) != encrypted_names.end())
            size -= 16u;
        const std::time_t written = host_time(entry.last_write_time(error));
        // The creation date: the recorded one (see "Creation dates"); a save
        // made before the record gets its current write time, kept from now on.
        std::time_t made = written;
        if (const auto found = created.find(name); found != created.end()) made = static_cast<std::time_t>(found->second);
        else created[name] = written, created_changed = true;
        memory.store32(at, 0x21FFu);  // a file, readable and writable
        memory.store32(at + 4u, 0u);
        memory.store32(at + 8u, static_cast<std::uint32_t>(size));
        memory.store32(at + 12u, static_cast<std::uint32_t>(size >> 32u));
        store_psp_date(memory, at + 16u, made);     // created
        store_psp_date(memory, at + 32u, written);  // accessed
        store_psp_date(memory, at + 48u, written);  // modified
        write_cstring(memory, at + 64u, name, 16u);
        memory.store32(info + 12u + kind * 4u, count + 1u);
    }
    if (created_changed) write_created(folder, created);
    if (trace_savedata())
        std::cerr << "[savedata] FILES " << folder.string() << ": secure=" << memory.load32(info + 12u)
                  << " normal=" << memory.load32(info + 16u) << " system=" << memory.load32(info + 20u) << "\n";
    return result::kOk;
}

// The data modes (MAKEDATA, READDATA, WRITEDATA, their SECURE forms, ERASE
// and DELETEDATA) work on one save folder with no dialog and report the RW_
// family of errors. Traced in Valkyria Chronicles II (ULES01417): on first
// boot it makes its system save with MAKEDATASECURE (no file name, no
// buffer) and then writes DATA0-DATA4 into it with WRITEDATASECURE.

// PARAM.SFO text a data request leaves empty keeps what the folder has.
void keep_existing_text(const savedata::SaveFiles &files, savedata::SaveContents &contents) {
    std::ifstream in(savedata::save_folder(state().memory_stick, files) / "PARAM.SFO", std::ios::binary);
    if (!in) return;
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    const auto sfo = savedata::ParamSfo::parse(bytes);
    if (!sfo) return;
    if (contents.title.empty()) contents.title = sfo->string("TITLE").value_or("");
    if (contents.savedata_title.empty()) contents.savedata_title = sfo->string("SAVEDATA_TITLE").value_or("");
    if (contents.detail.empty()) contents.detail = sfo->string("SAVEDATA_DETAIL").value_or("");
    if (contents.parental_level == 0u) contents.parental_level = sfo->integer("PARENTAL_LEVEL").value_or(0u);
}

// MAKEDATA and WRITEDATA: write the folder, PARAM.SFO and any media the game
// passes, and the data file when the request names one and supplies a buffer.
std::uint32_t do_write_data(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name,
                            bool secure) {
    auto files = files_for(memory, params, save_name);
    if (files.game_name.empty()) return result::kRwBadParams;
    files.secure = secure;
    if (memory.load32(params + param::kDataBuf) == 0u) files.file_name.clear();
    savedata::SaveContents contents = contents_from(memory, params);
    keep_existing_text(files, contents);
    std::string error;
    if (!savedata::write_save(state().memory_stick, files, contents, error)) {
        std::cerr << "[savedata] write failed: " << error << "\n";
        return result::kSaveAccessError;
    }
    std::cerr << "[savedata] wrote " << (files.file_name.empty() ? std::string("PARAM.SFO") : files.file_name) << " ("
              << contents.data.size() << " bytes) to " << savedata::save_folder(state().memory_stick, files).string()
              << (secure ? " [secure]" : "") << (files.key ? " (encrypted)" : "") << "\n";
    return result::kOk;
}

// READDATA: the named file of an existing save, into the game's buffer.
std::uint32_t do_read_data(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    if (files.game_name.empty() || files.file_name.empty()) return result::kRwBadParams;
    const std::filesystem::path folder = savedata::save_folder(state().memory_stick, files);
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return result::kRwNoData;
    if (!std::filesystem::is_regular_file(folder / files.file_name, error)) return result::kRwFileNotFound;
    switch (const std::uint32_t outcome = do_load(memory, params, save_name)) {
    case result::kLoadNoData:
        return result::kRwNoData;
    case result::kLoadDataBroken:
        return result::kRwDataBroken;
    case result::kLoadParam:
        return result::kRwBadParams;
    default:
        return outcome;
    }
}

// ERASE: one file of an existing save.
std::uint32_t do_erase(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    const auto files = files_for(memory, params, save_name);
    std::error_code error;
    if (!std::filesystem::is_directory(savedata::save_folder(state().memory_stick, files), error))
        return result::kRwNoData;
    if (!savedata::erase_file(state().memory_stick, files)) return result::kRwFileNotFound;
    std::cerr << "[savedata] erased " << files.file_name << " from "
              << savedata::save_folder(state().memory_stick, files).string() << "\n";
    return result::kOk;
}

// DELETEDATA: the whole save folder.
std::uint32_t do_delete_data(psprecomp::GuestMemory &memory, std::uint32_t params, const std::string &save_name) {
    return do_delete(memory, params, save_name) == result::kOk ? result::kOk : result::kRwNoData;
}

std::uint32_t run_request(psprecomp::GuestMemory &memory, std::uint32_t params) {
    const std::uint32_t mode = memory.load32(params + param::kMode);
    const std::string save_name = save_name_at(memory, params + param::kSaveName);
    switch (mode) {
    case kAutoLoad:
    case kLoad:
        return do_load(memory, params, save_name);
    case kAutoSave:
    case kSave:
        return do_save(memory, params, save_name);
    case kListLoad: {
        const auto chosen = pick_from_list(memory, params, true);
        if (!chosen) return result::kLoadNoData;
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_load(memory, params, *chosen);
    }
    case kListSave: {
        const auto chosen = pick_from_list(memory, params, false);
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_save(memory, params, *chosen);
    }
    case kListDelete: {
        const auto chosen = pick_from_list(memory, params, true);
        if (!chosen) return result::kDeleteNoData;
        write_cstring(memory, params + param::kSaveName, *chosen, 20u);
        return do_delete(memory, params, *chosen);
    }
    case kDelete:
    case kAutoDelete:
    case kSingleDelete:
        return do_delete(memory, params, save_name);
    case kSizes:
        return do_sizes(memory, params);
    case kGetSize:
        return do_get_size(memory, params, save_name);
    case kList:
        return do_list(memory, params);
    case kFiles:
        return do_files(memory, params, save_name);
    case kMakeDataSecure:
    case kWriteDataSecure:
        return do_write_data(memory, params, save_name, true);
    case kMakeData:
    case kWriteData:
        return do_write_data(memory, params, save_name, false);
    case kReadDataSecure:
    case kReadData:
        return do_read_data(memory, params, save_name);
    case kEraseSecure:
    case kErase:
        return do_erase(memory, params, save_name);
    case kDeleteData:
        return do_delete_data(memory, params, save_name);
    default:
        // Not used by this game. Report a parameter error so the guest takes
        // its failure path instead of reading results that were never written.
        std::cerr << "[savedata] mode " << mode << " (" << mode_name(mode) << ") is not implemented\n";
        if (trace_savedata()) {
            // What the game put in the block past the names, so a mode can be
            // implemented from what a game sends rather than from memory.
            const std::uint32_t size = memory.load32(params);
            for (std::uint32_t offset = 0x580u; offset + 4u <= std::min<std::uint32_t>(size, 0x800u); offset += 4u)
                if (const std::uint32_t word = memory.load32(params + offset); word != 0u) {
                    std::cerr << "[savedata]   +" << psprecomp::hex32(offset) << " = " << psprecomp::hex32(word);
                    // A pointer into RAM: what it points at, too.
                    if (memory.contains(word, 16u))
                        for (std::uint32_t i = 0; i < 4u; ++i)
                            std::cerr << (i == 0u ? " -> " : " ") << psprecomp::hex32(memory.load32(word + i * 4u));
                    std::cerr << "\n";
                }
        }
        return result::kLoadParam;
    }
}

// A PSP utility request does not finish inside InitStart.  Even the operations
// that draw no UI stay in INIT briefly before their result can be observed.
// Update draws/pumps that dialog; it does not advance its status.  Games use
// that distinction when deciding whether a missing root save is a first-run
// condition or an I/O failure.
void finish_pending_request(psprecomp::GuestMemory &memory) {
    const std::uint32_t params = state().pending_params;
    if (params == 0u) return;
    state().pending_params = 0u;
    const std::uint32_t outcome = run_request(memory, params);
    memory.store32(params + dialog_common::kResultOffset, outcome);
    if (outcome != result::kOk || trace_savedata())
        std::cerr << "[savedata] result " << psprecomp::hex32(outcome) << " (visible)\n";
}

bool savedata_ready() {
    const SavedataState &savedata = state();
    return savedata.dialog.status() != dialog_status::kInit || std::chrono::steady_clock::now() >= savedata.ready_at;
}

} // namespace

void register_savedata(HleRegistrar &hle, const std::filesystem::path &memory_stick) {
    state().memory_stick = memory_stick;
    savedata::set_memory_stick(memory_stick);

    hle.add("sceUtility", "sceUtilitySavedataInitStart", [](Runtime &rt, AllegrexContext &ctx) {
        auto &memory = rt.memory();
        const std::uint32_t params = arg(ctx, 0);
        // A new request while one is still active replaces the old pending
        // request. The PSP does its work after InitStart, not synchronously.
        if (state().dialog.active())
            std::cerr << "[savedata] InitStart while a dialog is active (status " << state().dialog.status() << ")\n";
        log_request(memory, params);
        state().pending_params = params;
        state().dialog.start();
        // PPSSPP uses a 200 ms initialization delay for savedata.  A short
        // real-time delay is important here: callers are permitted to poll
        // and update while the utility still reports INIT.
        state().ready_at = std::chrono::steady_clock::now() + std::chrono::milliseconds{200};
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUtility", "sceUtilitySavedataUpdate", [](Runtime &rt, AllegrexContext &ctx) {
        if (state().dialog.status() == dialog_status::kVisible) {
            // The guest has observed VISIBLE and is now pumping the utility.
            // Complete the non-visual request here, then make QUIT observable
            // to its next GetStatus call.  Completing it in GetStatus skips
            // this guest-visible phase entirely.
            finish_pending_request(rt.memory());
            (void)state().dialog.poll();
        }
        if (trace_savedata())
            std::cerr << "[savedata] update status " << state().dialog.status() << "\n";
        kernel().finish(ctx, 0u);
    });

    hle.add("sceUtility", "sceUtilitySavedataGetStatus", [](Runtime &rt, AllegrexContext &ctx) {
        if (!savedata_ready()) {
            if (trace_savedata()) std::cerr << "[savedata] status " << dialog_status::kInit << " (initializing)\n";
            kernel().finish(ctx, dialog_status::kInit);
            return;
        }
        if (state().dialog.status() == dialog_status::kVisible) {
            // VISIBLE is stable until Update performs the operation.
            if (trace_savedata()) std::cerr << "[savedata] status " << dialog_status::kVisible << "\n";
            kernel().finish(ctx, dialog_status::kVisible);
            return;
        }
        const std::uint32_t status = state().dialog.poll();
        if (trace_savedata()) std::cerr << "[savedata] status " << status << "\n";
        kernel().finish(ctx, status);
    });

    hle.add("sceUtility", "sceUtilitySavedataShutdownStart", [](Runtime &, AllegrexContext &ctx) {
        if (!state().dialog.active()) {
            kernel().finish(ctx, kErrorUtilityInvalidStatus);
            return;
        }
        if (!state().dialog.shutdown())
            std::cerr << "[savedata] ShutdownStart before the dialog finished\n";
        state().pending_params = 0u;
        state().ready_at = {};
        kernel().finish(ctx, 0u);
    });
}

} // namespace portablekit
