#include "vc2_music_reader.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>

// The CPK reading below follows work/claude-tools/cpk_dump.cpp (the port's own
// tool): ISO 9660 lookup, CRI @UTF tables, CRILAYLA, and the game's file cipher
// (its decryptor: EU 0x088456BC): a 16-byte header holding the first four words
// of the keystream w[i+1] = 3*w[i] + 1, then whole 128-byte units XORed per
// 16-byte block with w[j+1..j+4], the rest byte by byte with k = 3*k + 1.

namespace vc2 {

namespace {

std::uint32_t le32(const std::uint8_t *p) {
    return p[0] | p[1] << 8 | p[2] << 16 | std::uint32_t(p[3]) << 24;
}
std::uint16_t be16(const std::uint8_t *p) { return std::uint16_t(p[0] << 8 | p[1]); }
std::uint32_t be32(const std::uint8_t *p) { return std::uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
std::uint64_t be64(const std::uint8_t *p) { return std::uint64_t(be32(p)) << 32 | be32(p + 4); }

class Image {
public:
    explicit Image(const std::filesystem::path &path) : file_(path, std::ios::binary) {}
    explicit operator bool() const { return static_cast<bool>(file_); }
    bool read(std::uint64_t offset, void *out, std::size_t size) {
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(offset));
        file_.read(static_cast<char *>(out), static_cast<std::streamsize>(size));
        return static_cast<std::size_t>(file_.gcount()) == size;
    }

private:
    std::ifstream file_;
};

struct Extent {
    std::uint64_t offset{}, size{};
};

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool find_in_dir(Image &image, const Extent &dir, const std::string &name, Extent &out) {
    std::vector<std::uint8_t> data(dir.size);
    if (!image.read(dir.offset, data.data(), data.size())) return false;
    std::size_t at = 0;
    while (at < data.size()) {
        const std::uint8_t len = data[at];
        if (len == 0) {
            at = (at / 2048 + 1) * 2048;
            continue;
        }
        if (at + 33 > data.size()) break;
        const std::uint8_t *r = &data[at];
        std::string n(reinterpret_cast<const char *>(r + 33), std::min<std::size_t>(r[32], data.size() - at - 33));
        if (const auto semi = n.find(';'); semi != std::string::npos) n.resize(semi);
        if (upper(n) == upper(name)) {
            out.offset = std::uint64_t(le32(r + 2)) * 2048u;
            out.size = le32(r + 10);
            return true;
        }
        at += len;
    }
    return false;
}

bool find_path(Image &image, const std::string &path, Extent &out) {
    std::uint8_t pvd[2048];
    if (!image.read(16 * 2048, pvd, sizeof pvd) || std::memcmp(pvd + 1, "CD001", 5) != 0) return false;
    Extent cur{std::uint64_t(le32(pvd + 156 + 2)) * 2048u, le32(pvd + 156 + 10)};
    std::size_t start = 1;
    while (start < path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::string part = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        Extent next;
        if (!find_in_dir(image, cur, part, next)) return false;
        cur = next;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    out = cur;
    return true;
}

struct Value {
    std::uint64_t u{};
    std::string s;
};
using Row = std::map<std::string, Value>;

bool parse_utf(std::vector<std::uint8_t> d, std::vector<Row> &rows) {
    if (d.size() < 32) return false;
    if (std::memcmp(d.data(), "@UTF", 4) != 0) {
        std::uint32_t m = 0x655f;
        for (auto &b : d) {
            b ^= static_cast<std::uint8_t>(m & 0xFF);
            m = m * 0x4115u;
        }
        if (std::memcmp(d.data(), "@UTF", 4) != 0) return false;
    }
    const std::uint8_t *p = d.data();
    const std::uint32_t table_size = be32(p + 4);
    const std::uint32_t rows_offset = be32(p + 8) + 8, strings_offset = be32(p + 12) + 8;
    const std::uint16_t columns = be16(p + 24), row_length = be16(p + 26);
    const std::uint32_t count = be32(p + 28);
    if (std::uint64_t(table_size) + 8 > d.size()) return false;
    const auto str = [&](std::uint32_t off) {
        std::string s;
        for (std::size_t i = strings_offset + off; i < d.size() && d[i] != 0; ++i) s.push_back(static_cast<char>(d[i]));
        return s;
    };
    const auto read_value = [&](std::uint8_t type, std::size_t &pos, Value &v) -> bool {
        const auto need = [&](std::size_t n) { return pos + n <= d.size(); };
        switch (type) {
        case 0: case 1: if (!need(1)) return false; v.u = d[pos]; pos += 1; return true;
        case 2: case 3: if (!need(2)) return false; v.u = be16(&d[pos]); pos += 2; return true;
        case 4: case 5: case 8: if (!need(4)) return false; v.u = be32(&d[pos]); pos += 4; return true;
        case 6: case 7: case 9: if (!need(8)) return false; v.u = be64(&d[pos]); pos += 8; return true;
        case 0xA: if (!need(4)) return false; v.s = str(be32(&d[pos])); pos += 4; return true;
        case 0xB: if (!need(8)) return false; pos += 8; return true;
        default: return false;
        }
    };
    struct Column {
        std::uint8_t flags;
        std::string name;
        Value constant;
    };
    std::vector<Column> cols;
    std::size_t at = 32;
    for (std::uint16_t c = 0; c < columns; ++c) {
        if (at + 5 > d.size()) return false;
        Column col{d[at], str(be32(&d[at + 1])), {}};
        at += 5;
        if ((col.flags & 0xF0) == 0x30 && !read_value(col.flags & 0x0F, at, col.constant)) return false;
        cols.push_back(std::move(col));
    }
    for (std::uint32_t r = 0; r < count; ++r) {
        std::size_t pos = rows_offset + std::size_t(r) * row_length;
        Row row;
        for (const Column &col : cols) {
            if ((col.flags & 0xF0) == 0x30) row[col.name] = col.constant;
            else if ((col.flags & 0xF0) == 0x50) {
                Value v;
                if (!read_value(col.flags & 0x0F, pos, v)) return false;
                row[col.name] = v;
            }
        }
        rows.push_back(std::move(row));
    }
    return true;
}

bool read_chunk(Image &image, std::uint64_t offset, const char *magic, std::vector<Row> &rows) {
    std::uint8_t head[16];
    if (!image.read(offset, head, 16) || std::memcmp(head, magic, 4) != 0) return false;
    const std::uint64_t size = le32(head + 8) | (std::uint64_t(le32(head + 12)) << 32);
    if (size == 0 || size > (64u << 20)) return false;
    std::vector<std::uint8_t> d(size);
    if (!image.read(offset + 16, d.data(), d.size())) return false;
    return parse_utf(std::move(d), rows);
}

bool crilayla(const std::vector<std::uint8_t> &in, std::vector<std::uint8_t> &out) {
    if (in.size() < 0x110 || std::memcmp(in.data(), "CRILAYLA", 8) != 0) return false;
    const std::uint32_t usize = le32(&in[8]), hoff = le32(&in[12]);
    if (std::uint64_t(hoff) + 0x110 > in.size()) return false;
    out.assign(std::size_t(usize) + 0x100, 0);
    std::memcpy(out.data(), &in[hoff + 0x10], 0x100);
    long long in_at = static_cast<long long>(in.size()) - 0x100 - 1;
    const long long out_end = 0x100 + static_cast<long long>(usize) - 1;
    std::uint32_t pool = 0;
    int left = 0;
    const auto bits = [&](int n) -> std::uint32_t {
        std::uint32_t v = 0;
        int got = 0;
        while (got < n) {
            if (left == 0) {
                if (in_at < 0) return v;
                pool = in[static_cast<std::size_t>(in_at--)];
                left = 8;
            }
            const int take = std::min(left, n - got);
            v = (v << take) | ((pool >> (left - take)) & ((1u << take) - 1u));
            left -= take;
            got += take;
        }
        return v;
    };
    long long done = 0;
    const int vle[4] = {2, 3, 5, 8};
    while (done < static_cast<long long>(usize)) {
        if (bits(1)) {
            long long from = out_end - done + bits(13) + 3;
            long long length = 3;
            int level = 0;
            for (; level < 4; ++level) {
                const std::uint32_t v = bits(vle[level]);
                length += v;
                if (v != (1u << vle[level]) - 1u) break;
            }
            if (level == 4) {
                std::uint32_t v;
                do {
                    v = bits(8);
                    length += v;
                } while (v == 255u);
            }
            for (long long i = 0; i < length && done < static_cast<long long>(usize); ++i) {
                if (from < 0 || from >= static_cast<long long>(out.size())) return false;
                out[static_cast<std::size_t>(out_end - done)] = out[static_cast<std::size_t>(from--)];
                ++done;
            }
        } else {
            out[static_cast<std::size_t>(out_end - done)] = static_cast<std::uint8_t>(bits(8));
            ++done;
        }
    }
    return true;
}

bool encrypted(const std::vector<std::uint8_t> &d) {
    if (d.size() < 32 || d.size() % 16 != 0) return false;
    std::uint32_t s[4];
    for (int i = 0; i < 4; ++i) s[i] = le32(&d[4 * i]);
    return s[1] == 3u * s[0] + 1u && s[2] == 3u * s[1] + 1u && s[3] == 3u * s[2] + 1u;
}

std::vector<std::uint8_t> decrypt(const std::vector<std::uint8_t> &d) {
    const std::size_t body = d.size() - 16, whole = body / 128 * 128, blocks = whole / 16;
    std::vector<std::uint8_t> out(body);
    std::uint32_t w[5];
    w[0] = le32(&d[0]);
    for (int i = 1; i < 5; ++i) w[i] = 3u * w[i - 1] + 1u;
    for (std::size_t j = 0; j < blocks; ++j) {
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t v = le32(&d[16 + 16 * j + 4 * i]) ^ w[1 + i];
            std::uint8_t *o = &out[16 * j + 4 * i];
            o[0] = std::uint8_t(v), o[1] = std::uint8_t(v >> 8), o[2] = std::uint8_t(v >> 16), o[3] = std::uint8_t(v >> 24);
        }
        for (int i = 0; i < 4; ++i) w[i] = w[i + 1];
        w[4] = 3u * w[3] + 1u;
    }
    std::uint8_t k = d[0];
    for (std::size_t i = whole; i < body; ++i) {
        k = std::uint8_t(3u * k + 1u);
        out[i] = d[16 + i] ^ k;
    }
    return out;
}

// "M_STR_01_02.ATX" -> {"STR_01", 2}; false for anything else.
bool music_piece(const std::string &name, std::string &track, int &piece) {
    const std::string n = upper(name);
    if (n.size() != 15 || n.compare(0, 6, "M_STR_") != 0 || n.compare(11, 4, ".ATX") != 0 || n[8] != '_') return false;
    if (!std::isdigit(static_cast<unsigned char>(n[6])) || !std::isdigit(static_cast<unsigned char>(n[7])) ||
        !std::isdigit(static_cast<unsigned char>(n[9])) || !std::isdigit(static_cast<unsigned char>(n[10])))
        return false;
    track = n.substr(2, 6);
    piece = (n[9] - '0') * 10 + (n[10] - '0');
    return true;
}

} // namespace

std::vector<std::uint8_t> join_pieces(const std::vector<std::vector<std::uint8_t>> &pieces) {
    std::vector<std::uint8_t> out;
    for (const auto &p : pieces) {
        if (p.size() < 32 || std::memcmp(p.data(), "APA3", 4) != 0) return {};
        const std::uint32_t size = le32(&p[4]);
        if (std::uint64_t(size) + 32 > p.size()) return {};
        out.insert(out.end(), p.begin() + 32, p.begin() + 32 + size);
    }
    if (out.size() < 12 || std::memcmp(out.data(), "RIFF", 4) != 0 || std::memcmp(&out[8], "WAVE", 4) != 0) return {};
    const std::uint64_t riff = std::uint64_t(le32(&out[4])) + 8;
    if (riff > out.size()) return {};
    out.resize(static_cast<std::size_t>(riff));
    return out;
}

std::vector<MusicTrack> read_music(const std::filesystem::path &path, std::string &error) {
    Image image(path);
    if (!image) {
        error = "can't open the disc image";
        return {};
    }
    Extent cpk;
    if (!find_path(image, "/PSP_GAME/USRDIR/ODIN.CPK", cpk)) {
        error = "ODIN.CPK is not in the disc image";
        return {};
    }
    std::vector<Row> header;
    if (!read_chunk(image, cpk.offset, "CPK ", header) || header.empty()) {
        error = "ODIN.CPK has no header";
        return {};
    }
    const auto get = [&](const char *k) {
        auto it = header[0].find(k);
        return it == header[0].end() ? 0ull : static_cast<unsigned long long>(it->second.u);
    };
    const std::uint64_t toc = get("TocOffset"), content = get("ContentOffset");
    std::vector<Row> files;
    if (toc == 0 || !read_chunk(image, cpk.offset + toc, "TOC ", files)) {
        error = "ODIN.CPK has no file list";
        return {};
    }
    // File offsets count from the TOC or from the content: whichever holds the
    // first compressed file's CRILAYLA header.
    std::uint64_t base = std::min(toc, content);
    for (const Row &row : files) {
        const auto size = row.count("FileSize") ? row.at("FileSize").u : 0;
        const auto extract = row.count("ExtractSize") ? row.at("ExtractSize").u : size;
        const auto offset = row.count("FileOffset") ? row.at("FileOffset").u : 0;
        if (extract == size || size < 8) continue;
        bool found = false;
        for (const std::uint64_t b : {toc, content}) {
            char magic[8];
            if (image.read(cpk.offset + b + offset, magic, 8) && std::memcmp(magic, "CRILAYLA", 8) == 0) {
                base = b;
                found = true;
                break;
            }
        }
        if (found) break;
    }
    std::map<std::string, std::map<int, std::vector<std::uint8_t>>> pieces;
    for (const Row &row : files) {
        const std::string name = row.count("FileName") ? row.at("FileName").s : "";
        std::string track;
        int piece = 0;
        if (!music_piece(name, track, piece)) continue;
        const auto size = row.count("FileSize") ? row.at("FileSize").u : 0;
        const auto extract = row.count("ExtractSize") ? row.at("ExtractSize").u : size;
        const auto offset = row.count("FileOffset") ? row.at("FileOffset").u : 0;
        if (size == 0 || size > (16u << 20)) continue;
        std::vector<std::uint8_t> raw(size);
        if (!image.read(cpk.offset + base + offset, raw.data(), raw.size())) {
            error = "can't read " + name;
            return {};
        }
        std::vector<std::uint8_t> data;
        if (extract != size) {
            if (!crilayla(raw, data)) {
                error = "can't unpack " + name;
                return {};
            }
        } else {
            data = std::move(raw);
        }
        if (encrypted(data)) data = decrypt(data);
        pieces[track][piece] = std::move(data);
    }
    std::vector<MusicTrack> out;
    for (auto &[track, parts] : pieces) {
        std::vector<std::vector<std::uint8_t>> ordered;
        for (auto &[n, p] : parts) ordered.push_back(std::move(p));
        std::vector<std::uint8_t> wave = join_pieces(ordered);
        if (wave.empty()) continue;
        out.push_back({track, std::move(wave)});
    }
    if (out.empty()) error = "no music tracks in ODIN.CPK";
    return out;
}

bool soundtrack_music(const std::filesystem::path &image, std::vector<portablekit::audio::soundtrack::GameTrack> &tracks,
                      std::string &error) {
    std::vector<MusicTrack> music = read_music(image, error);
    if (music.empty()) return false;
    for (MusicTrack &m : music) tracks.push_back({std::move(m.name), std::move(m.wave)});
    return true;
}

} // namespace vc2
