// Voices from another release's disc (2026-11-24): a Japanese dub for the
// European and North American releases, or English voices for the Japanese one.
//
// The game's spoken lines are CRI ADX recordings in one AFS archive on the
// disc, PSP_GAME/USRDIR/STREAM/OD_VOICE.AFS (Europe: 5,257 of them, about
// 150 MB). The game reads the archive's own table of contents when it opens
// it (CRI ADXF), and finds each line by its index in that table.
//
// An AFS archive: "AFS\0", the number of entries, then each entry's offset and
// size (32-bit little-endian), then the offset and size of a table of names
// (48 bytes per entry, the name in the first 32, e.g. "100020010.adx").
// Entries start on 2048-byte boundaries.
//
// With the option on, the game reads an archive built from the two discs
// instead of its own: this release's table, in its order, each entry taken
// from the other disc's archive when that has an entry of the same name, and
// this release's own recording when it hasn't. Lines are matched by name, not
// by position, so a recording the other release lacks keeps this release's
// voice rather than playing the wrong line. Nothing is copied or written: the
// archive is laid out once from the two tables (a few hundred KB), and each
// read is served from whichever disc image holds those bytes.
//
// The movies (PSP_GAME/USRDIR/MOVIE/MVnn.PMF) carry their own voices. With the
// option on, a movie keeps this release's picture and gets the other disc's
// sound (owner, 2026-11-24: the whole other movie brought its picture too).
// A PSMF movie: a 2048-byte header (its size at +8, big-endian), then 2048-byte
// MPEG program stream packs, each one packet: video (stream 0xE0), the system
// header (0xBB) or ATRAC3plus sound (private stream 0xBD, sub-stream 0), each
// sound packet with its time stamp, in time order. The game reads the file in
// order and hands it to sceMpeg. The movie the game reads is this release's
// file with its sound packets replaced, in order, by the other disc's, each
// time stamp moved by the difference between the two movies' first ones. Video
// packets stay where they are, so the header's entry points stay right. When
// the two sounds are not in the same format (the ATRAC3plus frame header
// differs) or a file is not laid out like this, the movie is left alone. When
// the other movie has fewer sound packets, the last slots keep this release's.

#include "vc2_voice.hpp"

#include "vc2_settings.hpp"
#include "kernel/iso_image.hpp"
#include "settings/settings.hpp"
#include "portablekit/hle_extension.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace portablekit::vc2 {
namespace {

constexpr const char *kVoiceArchive = "PSP_GAME/USRDIR/STREAM/OD_VOICE.AFS";
constexpr const char *kMovieFolder = "PSP_GAME/USRDIR/MOVIE/";
constexpr std::uint64_t kSector = 2048u;
constexpr std::size_t kNameRecord = 48u;
constexpr std::size_t kNameLength = 32u;
constexpr std::uint32_t kMaxEntries = 200000u;

std::uint64_t align(std::uint64_t value) { return (value + kSector - 1u) / kSector * kSector; }

std::uint32_t le32(const std::uint8_t *p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8u) |
           (static_cast<std::uint32_t>(p[2]) << 16u) | (static_cast<std::uint32_t>(p[3]) << 24u);
}

void put32(std::uint8_t *p, std::uint64_t value) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::string upper(std::string text) {
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// Reads `length` bytes at `offset` of one archive; fewer at its end.
using Reader = std::function<std::size_t(std::uint64_t offset, std::uint8_t *destination, std::size_t length)>;

// One archive's table: entries and their names.
struct Archive {
    struct Entry {
        std::uint64_t offset{};
        std::uint64_t size{};
        std::string name;
    };
    std::vector<Entry> entries;
    std::uint64_t names_offset{};
    std::uint64_t names_size{};
    std::uint64_t first_offset{};  // where the first entry starts (the header's room)
};

std::optional<Archive> read_archive(const Reader &read, std::uint64_t file_size, std::string &why) {
    std::array<std::uint8_t, 8> head{};
    if (read(0u, head.data(), head.size()) != head.size() || std::memcmp(head.data(), "AFS\0", 4) != 0) {
        why = "not an AFS archive";
        return std::nullopt;
    }
    const std::uint32_t count = le32(head.data() + 4);
    if (count == 0u || count > kMaxEntries) {
        why = "entry count " + std::to_string(count);
        return std::nullopt;
    }
    std::vector<std::uint8_t> table(static_cast<std::size_t>(count) * 8u + 8u);
    if (read(8u, table.data(), table.size()) != table.size()) {
        why = "table of contents cut short";
        return std::nullopt;
    }
    Archive archive;
    archive.entries.resize(count);
    archive.first_offset = file_size;
    for (std::uint32_t i = 0; i < count; ++i) {
        auto &entry = archive.entries[i];
        entry.offset = le32(table.data() + i * 8u);
        entry.size = le32(table.data() + i * 8u + 4u);
        if (entry.offset + entry.size > file_size) {
            why = "entry " + std::to_string(i) + " lies past the end";
            return std::nullopt;
        }
        archive.first_offset = std::min(archive.first_offset, entry.offset);
    }
    archive.names_offset = le32(table.data() + count * 8u);
    archive.names_size = le32(table.data() + count * 8u + 4u);
    if (archive.names_offset == 0u || archive.names_size < static_cast<std::uint64_t>(count) * kNameRecord ||
        archive.names_offset + archive.names_size > file_size) {
        why = "no table of names";
        return std::nullopt;
    }
    std::vector<std::uint8_t> names(static_cast<std::size_t>(count) * kNameRecord);
    if (read(archive.names_offset, names.data(), names.size()) != names.size()) {
        why = "table of names cut short";
        return std::nullopt;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const char *name = reinterpret_cast<const char *>(names.data() + i * kNameRecord);
        archive.entries[i].name = upper(std::string(name, std::find(name, name + kNameLength, '\0')));
    }
    return archive;
}

// The archive the game reads: where each part of it comes from.
struct Layout {
    enum class From : std::uint8_t { Header, Own, Other };
    struct Part {
        std::uint64_t start{};   // in the built archive
        std::uint64_t length{};
        From from{};
        std::uint64_t source{};  // offset in the source archive (Header: in `header`)
    };
    std::vector<std::uint8_t> header;
    std::vector<Part> parts;  // in order of start; the gaps between them read as zeros
    std::uint64_t size{};
    std::size_t replaced{};
    std::size_t total{};
};

std::shared_ptr<const Layout> build_layout(const Archive &own, const Archive &other) {
    std::unordered_map<std::string, const Archive::Entry *> by_name;
    by_name.reserve(other.entries.size());
    for (const auto &entry : other.entries)
        if (!entry.name.empty()) by_name.emplace(entry.name, &entry);

    auto layout = std::make_shared<Layout>();
    const std::size_t count = own.entries.size();
    layout->total = count;
    const std::uint64_t header_size = std::max<std::uint64_t>(align(8u + count * 8u + 8u), own.first_offset);
    layout->header.assign(static_cast<std::size_t>(header_size), 0u);
    std::memcpy(layout->header.data(), "AFS\0", 4);
    put32(layout->header.data() + 4, count);
    layout->parts.push_back({0u, header_size, Layout::From::Header, 0u});

    std::uint64_t cursor = header_size;
    for (std::size_t i = 0; i < count; ++i) {
        const auto &entry = own.entries[i];
        const auto found = entry.name.empty() ? by_name.end() : by_name.find(entry.name);
        const bool from_other = found != by_name.end();
        const std::uint64_t size = from_other ? found->second->size : entry.size;
        const std::uint64_t source = from_other ? found->second->offset : entry.offset;
        if (from_other) ++layout->replaced;
        put32(layout->header.data() + 8 + i * 8, cursor);
        put32(layout->header.data() + 12 + i * 8, size);
        if (size != 0u) layout->parts.push_back({cursor, size, from_other ? Layout::From::Other : Layout::From::Own, source});
        cursor = align(cursor + size);
    }
    // The names, as this release has them.
    put32(layout->header.data() + 8 + count * 8, cursor);
    put32(layout->header.data() + 12 + count * 8, own.names_size);
    layout->parts.push_back({cursor, own.names_size, Layout::From::Own, own.names_offset});
    layout->size = align(cursor + own.names_size);
    return layout;
}

// The other disc: its image and where its archive is in it.
struct OtherDisc {
    std::unique_ptr<IsoImage> image;
    std::uint64_t start{};
    std::uint64_t size{};
    std::filesystem::path path;

    std::size_t read(std::uint64_t offset, std::uint8_t *destination, std::size_t length) {
        return read_at(start, size, offset, destination, length);
    }
    // Any file on this disc, given where it starts and its size.
    std::size_t read_at(std::uint64_t file_start, std::uint64_t file_size, std::uint64_t offset,
                        std::uint8_t *destination, std::size_t length) {
        if (!image || offset >= file_size) return 0u;
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(length, file_size - offset));
        return image->read(file_start + offset, std::span<std::uint8_t>(destination, count));
    }
};

// A movie with this release's picture and the other disc's sound.
struct MovieMix {
    std::uint64_t size{};
    std::vector<std::int64_t> donor;  // per pack of this release's file: the other file's pack offset, or -1
    std::int64_t pts_shift{};         // added to each replaced packet's time stamp
    std::size_t replaced{};
    std::size_t slots{};
};

constexpr std::size_t kPack = 2048u;

// A pack's packet: where its stream id and header are (offsets into the pack).
struct Packet {
    std::size_t at{};  // the packet start code
    std::uint8_t stream{};
    bool has_pts{};
    std::size_t pts_at{};
    std::size_t payload{};  // the first payload byte (after the PES header)
};

std::optional<Packet> packet_of(const std::uint8_t *pack) {
    if (pack[0] != 0u || pack[1] != 0u || pack[2] != 1u || pack[3] != 0xBAu) return std::nullopt;
    Packet packet;
    packet.at = 14u + (pack[13] & 7u);
    const std::uint8_t *q = pack + packet.at;
    if (packet.at + 9u > kPack || q[0] != 0u || q[1] != 0u || q[2] != 1u) return std::nullopt;
    packet.stream = q[3];
    if (packet.stream == 0xBBu) return packet;
    packet.has_pts = (q[7] & 0x80u) != 0u;
    packet.pts_at = packet.at + 9u;
    packet.payload = packet.at + 9u + q[8];
    if (packet.payload + 8u > kPack) return std::nullopt;
    return packet;
}

std::int64_t read_pts(const std::uint8_t *p) {
    return (static_cast<std::int64_t>((p[0] >> 1) & 7u) << 30) | (static_cast<std::int64_t>(p[1]) << 22) |
           (static_cast<std::int64_t>(p[2] >> 1) << 15) | (static_cast<std::int64_t>(p[3]) << 7) | (p[4] >> 1);
}

void write_pts(std::uint8_t *p, std::int64_t pts) {
    pts &= (std::int64_t{1} << 33) - 1;
    p[0] = static_cast<std::uint8_t>((p[0] & 0xF0u) | ((pts >> 29) & 0x0Eu) | 1u);
    p[1] = static_cast<std::uint8_t>(pts >> 22);
    p[2] = static_cast<std::uint8_t>(((pts >> 14) & 0xFEu) | 1u);
    p[3] = static_cast<std::uint8_t>(pts >> 7);
    p[4] = static_cast<std::uint8_t>(((pts << 1) & 0xFEu) | 1u);
}

// One movie's sound packets: their pack offsets, the first time stamp and the
// first ATRAC3plus frame header. Empty when the file isn't laid out as above.
struct SoundTrack {
    std::vector<std::uint64_t> packs;
    std::optional<std::int64_t> first_pts;
    std::array<std::uint8_t, 4> frame{};
    bool frame_found{};
    bool ok{};
};

SoundTrack scan_movie(const Reader &read, std::uint64_t size) {
    SoundTrack track;
    std::array<std::uint8_t, 16> head{};
    if (read(0u, head.data(), head.size()) != head.size() || std::memcmp(head.data(), "PSMF", 4) != 0) return track;
    const std::uint64_t start = (static_cast<std::uint64_t>(head[8]) << 24) | (head[9] << 16) | (head[10] << 8) | head[11];
    if (start == 0u || start % kPack != 0u || start >= size) return track;
    std::vector<std::uint8_t> chunk(kPack * 256u);
    for (std::uint64_t offset = start; offset < size;) {
        const auto want = static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), size - offset));
        if (read(offset, chunk.data(), want) != want) return track;
        for (std::size_t at = 0; at + kPack <= want; at += kPack) {
            const std::uint8_t *pack = chunk.data() + at;
            const auto packet = packet_of(pack);
            if (!packet) return track;
            if (packet->stream != 0xBDu) continue;
            track.packs.push_back(offset + at);
            if (!track.first_pts && packet->has_pts) track.first_pts = read_pts(pack + packet->pts_at);
            if (!track.frame_found) {
                // The first ATRAC3plus frame header (sync 0x0FD0 and its two parameter bytes).
                for (std::size_t i = packet->payload; i + 4u <= kPack; ++i)
                    if (pack[i] == 0x0Fu && pack[i + 1] == 0xD0u) {
                        std::memcpy(track.frame.data(), pack + i, 4u);
                        track.frame_found = true;
                        break;
                    }
            }
        }
        offset += want;
    }
    track.ok = !track.packs.empty() && track.first_pts && track.frame_found;
    return track;
}

class MovieFile final : public hle_extension::FileFilter {
public:
    MovieFile(std::shared_ptr<const MovieMix> mix, hle_extension::DiscFileReader own, std::shared_ptr<OtherDisc> disc,
              std::uint64_t donor_start, std::uint64_t donor_size)
        : mix_(std::move(mix)), own_(std::move(own)), disc_(std::move(disc)), donor_start_(donor_start),
          donor_size_(donor_size) {}

    [[nodiscard]] std::uint64_t size() const override { return mix_->size; }

    std::size_t read(std::uint64_t offset, std::uint8_t *destination, std::size_t length) override {
        if (offset >= mix_->size) return 0u;
        length = static_cast<std::size_t>(std::min<std::uint64_t>(length, mix_->size - offset));
        const std::size_t got = own_(offset, destination, length);
        // Replaced packs inside the range: the other movie's packet, retimed.
        const std::uint64_t first = offset / kPack;
        const std::uint64_t last = (offset + got + kPack - 1u) / kPack;
        std::array<std::uint8_t, kPack> pack{};
        for (std::uint64_t index = first; index < last && index < mix_->donor.size(); ++index) {
            const std::int64_t from = mix_->donor[index];
            if (from < 0) continue;
            if (disc_->read_at(donor_start_, donor_size_, static_cast<std::uint64_t>(from), pack.data(), kPack) != kPack)
                continue;
            if (const auto packet = packet_of(pack.data()); packet && packet->has_pts)
                write_pts(pack.data() + packet->pts_at, read_pts(pack.data() + packet->pts_at) + mix_->pts_shift);
            const std::uint64_t pack_start = index * kPack;
            const std::uint64_t from_byte = std::max(pack_start, offset);
            const std::uint64_t to_byte = std::min(pack_start + kPack, offset + got);
            std::memcpy(destination + (from_byte - offset), pack.data() + (from_byte - pack_start),
                        static_cast<std::size_t>(to_byte - from_byte));
        }
        return got;
    }

private:
    std::shared_ptr<const MovieMix> mix_;
    hle_extension::DiscFileReader own_;
    std::shared_ptr<OtherDisc> disc_;
    std::uint64_t donor_start_{};
    std::uint64_t donor_size_{};
};

class VoiceArchive final : public hle_extension::FileFilter {
public:
    VoiceArchive(std::shared_ptr<const Layout> layout, hle_extension::DiscFileReader own,
                 std::shared_ptr<OtherDisc> other)
        : layout_(std::move(layout)), own_(std::move(own)), other_(std::move(other)) {}

    [[nodiscard]] std::uint64_t size() const override { return layout_->size; }

    std::size_t read(std::uint64_t offset, std::uint8_t *destination, std::size_t length) override {
        const auto &parts = layout_->parts;
        std::size_t done = 0u;
        while (done < length && offset < layout_->size) {
            // The part holding this byte: the last one starting at or before it.
            auto next = std::upper_bound(parts.begin(), parts.end(), offset,
                                         [](std::uint64_t at, const Layout::Part &part) { return at < part.start; });
            const Layout::Part *part = next == parts.begin() ? nullptr : &*(next - 1);
            const std::uint64_t limit = next == parts.end() ? layout_->size : next->start;
            std::size_t step;
            if (part != nullptr && offset < part->start + part->length) {
                const std::uint64_t within = offset - part->start;
                step = static_cast<std::size_t>(std::min<std::uint64_t>(length - done, part->length - within));
                std::size_t got = 0u;
                switch (part->from) {
                case Layout::From::Header:
                    std::memcpy(destination + done, layout_->header.data() + within, step);
                    got = step;
                    break;
                case Layout::From::Own: got = own_(part->source + within, destination + done, step); break;
                case Layout::From::Other: got = other_->read(part->source + within, destination + done, step); break;
                }
                if (got < step) std::memset(destination + done + got, 0, step - got);
            } else {
                // Padding between parts.
                step = static_cast<std::size_t>(std::min<std::uint64_t>(length - done, limit - offset));
                std::memset(destination + done, 0, step);
            }
            done += step;
            offset += step;
        }
        return done;
    }

private:
    std::shared_ptr<const Layout> layout_;
    hle_extension::DiscFileReader own_;
    std::shared_ptr<OtherDisc> other_;
};

struct VoiceState {
    std::shared_ptr<OtherDisc> other;
    std::optional<Archive> other_archive;
    std::shared_ptr<const Layout> layout;  // built at the first open
    std::uint64_t layout_for_size{};       // this release's archive size it was built for
    bool given_up{};
    bool announced{};
    std::unordered_map<std::string, std::shared_ptr<const MovieMix>> movies;  // by path, with own size
    std::unordered_map<std::string, bool> movie_refused;
};

VoiceState &voice() {
    static VoiceState state;
    return state;
}

std::filesystem::path path_from_setting(const std::string &value) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(value.data()), value.size()));
}

// A movie: this release's picture with the other disc's sound, when it has a
// movie of the same name with sound in the same format.
std::shared_ptr<hle_extension::FileFilter> open_movie(const std::string &path, std::uint64_t size,
                                                      hle_extension::DiscFileReader own) {
    VoiceState &state = voice();
    const std::string key = upper(path) + "|" + std::to_string(size);
    if (state.movie_refused.count(key) != 0u) return nullptr;
    const auto entry = state.other->image->find(path);
    if (!entry || entry->directory) return nullptr;
    const std::uint64_t donor_start = static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize;
    const std::uint64_t donor_size = entry->size;
    const std::string name = std::filesystem::path(path).filename().string();
    auto found = state.movies.find(key);
    if (found == state.movies.end()) {
        const SoundTrack mine = scan_movie(own, size);
        OtherDisc *disc = state.other.get();
        const Reader donor_reader = [disc, donor_start, donor_size](std::uint64_t offset, std::uint8_t *destination,
                                                                    std::size_t length) {
            return disc->read_at(donor_start, donor_size, offset, destination, length);
        };
        const SoundTrack theirs = scan_movie(donor_reader, donor_size);
        std::string why;
        if (!mine.ok) why = "this release's movie isn't laid out as expected";
        else if (!theirs.ok) why = "the voice disc's movie isn't laid out as expected";
        else if (mine.frame != theirs.frame) why = "the voice disc's movie sound is in another format";
        if (!why.empty()) {
            std::cerr << "[vc2-voice] " << name << ": " << why << ": keeping this release's sound\n";
            state.movie_refused[key] = true;
            return nullptr;
        }
        auto mix = std::make_shared<MovieMix>();
        mix->size = size;
        mix->pts_shift = *mine.first_pts - *theirs.first_pts;
        mix->slots = mine.packs.size();
        mix->donor.assign(static_cast<std::size_t>((size + kPack - 1u) / kPack), -1);
        const std::size_t count = std::min(mine.packs.size(), theirs.packs.size());
        for (std::size_t i = 0; i < count; ++i)
            mix->donor[static_cast<std::size_t>(mine.packs[i] / kPack)] = static_cast<std::int64_t>(theirs.packs[i]);
        mix->replaced = count;
        std::cerr << "[vc2-voice] " << name << ": the voice disc's sound (" << count << " of " << mix->slots
                  << " sound packets";
        if (theirs.packs.size() > mine.packs.size())
            std::cerr << "; its last " << theirs.packs.size() - mine.packs.size() << " don't fit";
        std::cerr << ")\n";
        found = state.movies.emplace(key, std::move(mix)).first;
    }
    return std::make_shared<MovieFile>(found->second, std::move(own), state.other, donor_start, donor_size);
}

std::shared_ptr<hle_extension::FileFilter> open_replacement(const std::string &path, std::uint64_t size,
                                                            hle_extension::DiscFileReader own) {
    const std::string name = upper(path);
    if (name.rfind(kMovieFolder, 0) == 0 && name.size() > 4u && name.compare(name.size() - 4u, 4u, ".PMF") == 0)
        return open_movie(path, size, std::move(own));
    if (name != kVoiceArchive) return nullptr;
    VoiceState &state = voice();
    if (state.given_up) return nullptr;
    if (!state.layout || state.layout_for_size != size) {
        std::string why;
        const auto own_archive = read_archive(own, size, why);
        if (!own_archive) {
            std::cerr << "[vc2-voice] this release's voice archive can't be read (" << why
                      << "): keeping its own voices\n";
            state.given_up = true;
            return nullptr;
        }
        state.layout = build_layout(*own_archive, *state.other_archive);
        state.layout_for_size = size;
        if (state.layout->replaced == 0u) {
            std::cerr << "[vc2-voice] the voice disc has none of this release's lines: keeping its own voices\n";
            state.given_up = true;
            state.layout.reset();
            return nullptr;
        }
    }
    if (!state.announced) {
        state.announced = true;
        std::cerr << "[vc2-voice] voices from " << state.other->path.filename().string() << ": "
                  << state.layout->replaced << " of " << state.layout->total << " lines";
        if (state.layout->replaced < state.layout->total)
            std::cerr << " (the other " << state.layout->total - state.layout->replaced << " keep this release's voice)";
        std::cerr << "\n";
    }
    return std::make_shared<VoiceArchive>(state.layout, std::move(own), state.other);
}

}  // namespace

void install_voice() {
    if (options::get(options::Key::VoiceReplace) != "on") return;
    const std::string &setting = options::get(options::Key::VoiceDisc);
    if (setting.empty()) {
        std::cerr << "[vc2-voice] Replace language audio is on, but no voice disc image is chosen\n";
        return;
    }
    VoiceState &state = voice();
    auto other = std::make_shared<OtherDisc>();
    other->path = path_from_setting(setting);
    try {
        other->image = std::make_unique<IsoImage>(other->path);
    } catch (const std::exception &e) {
        std::cerr << "[vc2-voice] can't open the voice disc image: " << e.what() << "\n";
        return;
    }
    const auto entry = other->image->find(kVoiceArchive);
    if (!entry || entry->directory) {
        std::cerr << "[vc2-voice] " << other->path.filename().string()
                  << " has no voice archive (STREAM/OD_VOICE.AFS): is it a Valkyria Chronicles II disc image?\n";
        return;
    }
    other->start = static_cast<std::uint64_t>(entry->lba) * IsoImage::kSectorSize;
    other->size = entry->size;
    std::string why;
    const Reader reader = [disc = other.get()](std::uint64_t offset, std::uint8_t *destination, std::size_t length) {
        return disc->read(offset, destination, length);
    };
    auto archive = read_archive(reader, other->size, why);
    if (!archive) {
        std::cerr << "[vc2-voice] the voice disc's archive can't be read (" << why << ")\n";
        return;
    }
    // "Keep disc images in memory": the voice disc's voices and movies too
    // (only those are read from it), copied in the background like the game's
    // own disc image (patch 0038).
    if (settings::current().disc_in_memory) {
        const std::string name = other->path.filename().string();
        (void)other->image->preload(other->start, other->size, name + " voices");
        for (const std::string &file : other->image->list(kMovieFolder)) {
            const auto movie = other->image->find(std::string(kMovieFolder) + file);
            if (movie && !movie->directory)
                (void)other->image->preload(static_cast<std::uint64_t>(movie->lba) * IsoImage::kSectorSize,
                                            movie->size, name + " " + file);
        }
    }
    state.other = std::move(other);
    state.other_archive = std::move(archive);
    hle_extension::set_disc_file_source(open_replacement);
    std::cerr << "[vc2-voice] voice disc " << state.other->path.filename().string() << ": "
              << state.other_archive->entries.size() << " recordings\n";
}

}  // namespace portablekit::vc2
