#include "audio/soundtrack_import_run.hpp"

#include "audio/atrac_decoder.hpp"
#include "audio/soundtrack_media.hpp"
#include "install/user_data.hpp"
#include "profile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <set>

namespace portablekit::audio::soundtrack {

namespace {

std::uint32_t le32(const std::uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24; }
std::uint16_t le16(const std::uint8_t *p) { return std::uint16_t(p[0] | p[1] << 8); }

struct Wave {
    std::size_t data_offset{}, data_size{};
    unsigned channels{}, block_align{};
    AtracCodec codec{AtracCodec::Atrac3Plus};
    std::vector<std::uint8_t> extradata;
    std::uint32_t samples{}, delay{};
    bool ok{};
};

Wave parse_wave(const std::vector<std::uint8_t> &w) {
    Wave out;
    if (w.size() < 12 || std::memcmp(w.data(), "RIFF", 4) != 0 || std::memcmp(&w[8], "WAVE", 4) != 0) return out;
    bool have_format = false, have_fact = false;
    for (std::size_t at = 12; at + 8 <= w.size();) {
        const std::uint32_t size = le32(&w[at + 4]);
        const std::size_t body = at + 8;
        if (body + size > w.size() && std::memcmp(&w[at], "data", 4) != 0) break;
        if (std::memcmp(&w[at], "fmt ", 4) == 0 && size >= 16) {
            const std::uint16_t tag = le16(&w[body]);
            out.channels = le16(&w[body + 2]);
            out.block_align = le16(&w[body + 12]);
            if (tag == 0x0270) {
                out.codec = AtracCodec::Atrac3;
                if (size >= 18 + 14) out.extradata.assign(w.begin() + body + 18, w.begin() + body + 18 + 14);
            } else {
                out.codec = AtracCodec::Atrac3Plus; // 0xFFFE with the ATRAC3plus sub-format
            }
            have_format = true;
        } else if (std::memcmp(&w[at], "fact", 4) == 0 && size >= 8) {
            out.samples = le32(&w[body]);
            out.delay = le32(&w[body + 4]);
            have_fact = true;
        } else if (std::memcmp(&w[at], "data", 4) == 0) {
            out.data_offset = body;
            out.data_size = std::min<std::size_t>(size, w.size() - body);
            break;
        }
        at = body + size + (size & 1u);
    }
    out.ok = have_format && have_fact && out.data_size > 0 && out.block_align > 0;
    return out;
}

std::string lower_alnum(const std::string &s) {
    std::string out;
    for (unsigned char c : s)
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool is_audio_file(const std::filesystem::path &p) {
    static const std::set<std::string> kinds{".flac", ".mp3", ".m4a", ".aac", ".ogg", ".oga", ".opus", ".wav",
                                             ".wma", ".ape", ".wv", ".alac", ".aiff", ".aif", ".mka", ".tta"};
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return kinds.count(ext) != 0;
}

struct AlbumFile {
    std::filesystem::path path;
    FileInfo info;
    int disc{}, track{}; // from the tags, else the file and folder names
};

// "1-02 Title", "1-02. Title", "102 Title", "02 - Title"; the disc from a
// "Disc 2" / "CD2" folder when the name has none.
void numbers_from_names(AlbumFile &f) {
    const std::string stem = f.path.stem().string();
    int disc = 0, track = 0;
    std::size_t i = 0;
    while (i < stem.size() && stem[i] == ' ') ++i;
    std::size_t digits = i;
    while (digits < stem.size() && std::isdigit(static_cast<unsigned char>(stem[digits]))) ++digits;
    const std::string first = stem.substr(i, digits - i);
    if (!first.empty()) {
        if (digits < stem.size() && (stem[digits] == '-' || stem[digits] == '.' || stem[digits] == '_') &&
            digits + 1 < stem.size() && std::isdigit(static_cast<unsigned char>(stem[digits + 1])) && first.size() == 1) {
            disc = std::stoi(first);
            std::size_t j = digits + 1, k = j;
            while (k < stem.size() && std::isdigit(static_cast<unsigned char>(stem[k]))) ++k;
            track = std::stoi(stem.substr(j, k - j));
        } else if (first.size() == 3) {
            disc = first[0] - '0';
            track = std::stoi(first.substr(1));
        } else if (first.size() <= 2) {
            track = std::stoi(first);
        }
    }
    if (disc == 0) {
        const std::string folder = lower_alnum(f.path.parent_path().filename().string());
        for (const char *word : {"disc", "disk", "cd"}) {
            const auto at = folder.find(word);
            if (at != std::string::npos && at + std::strlen(word) < folder.size() &&
                std::isdigit(static_cast<unsigned char>(folder[at + std::strlen(word)])))
                disc = folder[at + std::strlen(word)] - '0';
        }
    }
    if (f.disc == 0) f.disc = disc;
    if (f.track == 0) f.track = track;
}

bool number_matches(const AlbumFile &f, const std::string &number) {
    if (number.size() < 3 || number[1] != '-') return false;
    const int disc = number[0] - '0';
    const int track = std::atoi(number.c_str() + 2);
    return f.track == track && (f.disc == disc || (f.disc == 0 && disc == 1));
}

// Decoded album files, a few at a time (several game tracks can use one).
class AlbumCache {
public:
    const Stereo *get(const std::filesystem::path &path) {
        for (auto it = items_.begin(); it != items_.end(); ++it)
            if (it->first == path) {
                items_.splice(items_.begin(), items_, it);
                return items_.front().second ? &*items_.front().second : nullptr;
            }
        std::string error;
        items_.emplace_front(path, decode(path, error));
        if (!items_.front().second) std::cout << "  can't read " << path.filename().string() << ": " << error << "\n";
        while (items_.size() > 6) items_.pop_back();
        return items_.front().second ? &*items_.front().second : nullptr;
    }

private:
    std::list<std::pair<std::filesystem::path, std::optional<Stereo>>> items_;
};

void progress(std::size_t done, std::size_t total) {
    const int percent = total == 0 ? 100 : static_cast<int>(done * 100 / total);
    std::cout << "Importing the soundtrack: " << percent << "%" << std::endl;
}

} // namespace

std::filesystem::path default_soundtrack_folder() {
    for (const char *name : {"GAME_DIR", "DATA_DIR"})
        if (const char *dir = portablekit::env(name); dir != nullptr && *dir != '\0')
            return portablekit::install::path_from_utf8(dir) / "soundtrack";
    return portablekit::install::user_data_directory() / "soundtrack";
}

std::string wave_key(const std::vector<std::uint8_t> &wave) {
    const Wave w = parse_wave(wave);
    constexpr std::size_t kKeyBytes = 4096;
    if (!w.ok || w.data_size < kKeyBytes) return {};
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (std::size_t i = 0; i < kKeyBytes; ++i) hash = (hash ^ wave[w.data_offset + i]) * 0x100000001b3ull;
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

Stereo decode_game_track(const std::vector<std::uint8_t> &wave) {
    Stereo out;
    const Wave w = parse_wave(wave);
    if (!w.ok) return out;
    AtracDecoder decoder;
    if (!decoder.open(w.codec, w.channels, w.block_align, w.extradata)) return out;
    std::vector<std::int16_t> frame(atrac_frame_samples(w.codec) * 2);
    std::vector<float> all;
    for (std::size_t at = w.data_offset; at + w.block_align <= w.data_offset + w.data_size; at += w.block_align) {
        const std::size_t got = decoder.decode(std::span(wave.data() + at, w.block_align), frame.data());
        if (got == 0) {
            std::fill(frame.begin(), frame.end(), 0);
            all.insert(all.end(), atrac_frame_samples(w.codec) * 2, 0.0f);
            continue;
        }
        for (std::size_t i = 0; i < got * 2; ++i) all.push_back(frame[i]);
    }
    // Game position p = decoded sample p + the stream's delay + 368 (the
    // decoder's own lead-in), as the game's player counts them.
    const std::size_t skip = static_cast<std::size_t>(w.delay) + 368u;
    if (all.size() / 2 <= skip) return out;
    const std::size_t frames = std::min<std::size_t>(w.samples, all.size() / 2 - skip);
    out.samples.assign(all.begin() + static_cast<std::ptrdiff_t>(skip * 2),
                       all.begin() + static_cast<std::ptrdiff_t>((skip + frames) * 2));
    return out;
}

int run_import(const std::filesystem::path &album_folder, const std::filesystem::path &out,
               const std::vector<GameTrack> &tracks, const std::vector<Entry> &table) {
    if (!media_available()) {
        std::cout << "This build can't read audio files (no FFmpeg format libraries).\n";
        return 1;
    }
    std::error_code ec;
    if (!std::filesystem::is_directory(album_folder, ec)) {
        std::cout << "No folder " << album_folder.string() << "\n";
        return 1;
    }
    // The player's files.
    std::vector<AlbumFile> files;
    for (auto it = std::filesystem::recursive_directory_iterator(
             album_folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || !is_audio_file(it->path())) continue;
        const std::optional<FileInfo> info = probe(it->path());
        if (!info || info->seconds <= 0.0) continue;
        AlbumFile f{it->path(), *info, info->disc, info->track};
        numbers_from_names(f);
        files.push_back(std::move(f));
    }
    std::cout << files.size() << " audio files in " << album_folder.string() << "\n";
    if (files.empty()) return 1;

    std::filesystem::create_directories(out, ec);
    for (auto it = std::filesystem::directory_iterator(out, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".flac") std::filesystem::remove(it->path(), ec);

    // What to build: every game track the table knows (by key, else by name);
    // then, for a game whose music can't be read from the disc, every entry
    // that can be built from the album alone (Entry::game_frames).
    struct Job {
        const Entry *entry;
        const GameTrack *track; // null: from the album alone
    };
    std::vector<Job> jobs;
    std::set<const Entry *> covered;
    for (const GameTrack &track : tracks) {
        const std::string key = wave_key(track.wave);
        if (key.empty()) continue;
        const Entry *entry = nullptr;
        for (const Entry &e : table)
            if (e.key == key) entry = &e;
        if (entry == nullptr)
            for (const Entry &e : table)
                if (e.track == track.name) entry = &e;
        if (entry == nullptr || !covered.insert(entry).second) continue;
        jobs.push_back({entry, &track});
    }
    for (const Entry &e : table)
        if (e.game_frames > 0 && covered.insert(&e).second) jobs.push_back({&e, nullptr});

    std::ofstream report(out / "soundtrack.txt");
    report << "Soundtrack import from " << album_folder.string() << "\n";
    AlbumCache cache;
    std::size_t built = 0, done = 0;
    progress(0, jobs.size());
    for (const Job &job : jobs) {
        const Entry &entry = *job.entry;
        const std::string label = entry.number + " " + entry.title;
        Stereo game;
        if (job.track != nullptr) {
            game = decode_game_track(job.track->wave);
            if (game.frames() == 0) {
                report << label << ": the game's track can't be decoded\n";
                progress(++done, jobs.size());
                continue;
            }
        }
        // Candidates: the right number or title at about the right length; else
        // (only when the game's audio can confirm it) any file of about that length.
        std::vector<const AlbumFile *> candidates;
        const std::string title = lower_alnum(entry.title);
        for (const AlbumFile &f : files) {
            const bool titled = !title.empty() && (lower_alnum(f.info.title) == title ||
                                                   lower_alnum(f.path.stem().string()).find(title) != std::string::npos);
            const double off = std::fabs(f.info.seconds - entry.album_seconds);
            if ((titled && off <= 4.0) || (number_matches(f, entry.number) && off <= (job.track ? 4.0 : 1.5)))
                candidates.push_back(&f);
        }
        if (candidates.empty() && job.track != nullptr)
            for (const AlbumFile &f : files)
                if (std::fabs(f.info.seconds - entry.album_seconds) <= 1.0) candidates.push_back(&f);
        const AlbumFile *best = nullptr;
        if (job.track != nullptr) {
            double best_score = 0.3;
            for (const AlbumFile *f : candidates) {
                const Stereo *album = cache.get(f->path);
                if (album == nullptr) continue;
                const double score = quick_score(entry, game, *album);
                if (score > best_score) {
                    best_score = score;
                    best = f;
                }
            }
        } else {
            // Without the game's audio: the closest length, a title match first.
            double best_off = 1e9;
            for (const AlbumFile *f : candidates) {
                const bool titled = !title.empty() && (lower_alnum(f->info.title) == title ||
                                                       lower_alnum(f->path.stem().string()).find(title) != std::string::npos);
                const double off = std::fabs(f->info.seconds - entry.album_seconds) - (titled ? 100.0 : 0.0);
                if (off < best_off) {
                    best_off = off;
                    best = f;
                }
            }
        }
        if (best == nullptr) {
            report << label << ": not found in the folder (the game's music plays)\n";
            std::cout << "  " << label << ": not found" << std::endl;
            progress(++done, jobs.size());
            continue;
        }
        const Stereo *album = cache.get(best->path);
        Result r;
        if (album == nullptr) r.reason = "can't read the file";
        else r = job.track ? build(entry, game, *album, change_tempo) : build_from_reference(entry, *album);
        std::string error;
        if (r.ok && write_flac(out / (entry.key + ".flac"), r.pcm, error)) {
            ++built;
            report << label << ": from " << best->path.filename().string();
            if (r.rip_offset != 0) report << " (offset " << r.rip_offset << " samples from the reference)";
            report << "\n";
            std::cout << "  " << label << ": from " << best->path.filename().string() << std::endl;
        } else {
            const std::string why = r.ok ? error : r.reason;
            report << label << ": kept the game's music (" << why << ")\n";
            std::cout << "  " << label << ": kept the game's music (" << why << ")" << std::endl;
        }
        progress(++done, jobs.size());
    }
    report << built << " of " << jobs.size() << " tracks from the soundtrack\n";
    std::cout << built << " of " << jobs.size() << " tracks from the soundtrack" << std::endl;
    return built > 0 ? 0 : 1;
}

} // namespace portablekit::audio::soundtrack
