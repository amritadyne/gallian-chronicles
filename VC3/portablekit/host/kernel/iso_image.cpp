#include "iso_image.hpp"

#include "psprecomp/common.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <new>

namespace portablekit {
namespace {

std::uint32_t read_le32(const std::vector<std::uint8_t> &data, std::size_t offset) {
    return static_cast<std::uint32_t>(data[offset]) | (static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
           (static_cast<std::uint32_t>(data[offset + 2]) << 16u) | (static_cast<std::uint32_t>(data[offset + 3]) << 24u);
}

} // namespace

IsoImage::IsoImage(const std::filesystem::path &path) : path_(path), file_(path, std::ios::binary) {
    if (!file_) throw psprecomp::Error("Cannot open disc image: " + path.string());
    size_bytes_ = std::filesystem::file_size(path);
    std::vector<std::uint8_t> primary(kSectorSize);
    if (read(16u * kSectorSize, primary) != primary.size() || primary[1] != 'C' || primary[2] != 'D' ||
        primary[3] != '0' || primary[4] != '0' || primary[5] != '1')
        throw psprecomp::Error("Not an ISO9660 image: " + path.string());
    const std::uint32_t root_lba = read_le32(primary, 156u + 2u);
    const std::uint32_t root_size = read_le32(primary, 156u + 10u);
    entries_[""] = Entry{root_lba, root_size, true};
    scan_directory(root_lba, root_size, "", 0);
}

IsoImage::~IsoImage() {
    stopping_.store(true, std::memory_order_relaxed);
    for (const auto &region : preloads_)
        if (region->thread.joinable()) region->thread.join();
}

bool IsoImage::preload(std::uint64_t start, std::uint64_t size, const std::string &label) {
    if (start >= size_bytes_ || size == 0u) return false;
    size = std::min(size, size_bytes_ - start);
    auto region = std::make_unique<Preload>();
    region->start = start;
    region->size = size;
    region->data.reset(new (std::nothrow) std::uint8_t[static_cast<std::size_t>(size)]);
    if (!region->data) {
        std::cerr << "[disc] not enough memory to keep " << label << " in memory (" << (size >> 20u)
                  << " MB): it is read from the drive\n";
        return false;
    }
    Preload *target = region.get();
    target->thread = std::thread([this, target, label] {
        const auto begin = std::chrono::steady_clock::now();
        std::ifstream file(path_, std::ios::binary);
        if (file) file.seekg(static_cast<std::streamoff>(target->start));
        constexpr std::uint64_t kChunk = 8u << 20u;
        std::uint64_t done = 0u;
        while (file && done < target->size && !stopping_.load(std::memory_order_relaxed)) {
            const std::uint64_t want = std::min(kChunk, target->size - done);
            file.read(reinterpret_cast<char *>(target->data.get() + done), static_cast<std::streamsize>(want));
            const auto got = file.gcount();
            if (got <= 0) break;
            done += static_cast<std::uint64_t>(got);
            target->loaded.store(done, std::memory_order_release);
        }
        if (stopping_.load(std::memory_order_relaxed)) return;
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        if (done == target->size)
            std::cerr << "[disc] " << label << " in memory: " << (done >> 20u) << " MB in " << seconds << " s\n";
        else
            std::cerr << "[disc] " << label << ": the copy stopped after " << (done >> 20u)
                      << " MB; the rest is read from the drive\n";
    });
    preloads_.push_back(std::move(region));
    return true;
}

std::string IsoImage::normalize(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '/' && (result.empty() || result.back() == '/')) continue;
        result.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(path[i]))));
    }
    while (!result.empty() && result.back() == '/') result.pop_back();
    return result;
}

void IsoImage::scan_directory(std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth) {
    if (depth > 16) return;
    std::vector<std::uint8_t> data(size);
    if (read(static_cast<std::uint64_t>(lba) * kSectorSize, data) != data.size()) return;
    std::size_t offset = 0u;
    while (offset < data.size()) {
        const std::uint8_t length = data[offset];
        if (length == 0u) {
            offset = (offset / kSectorSize + 1u) * kSectorSize;
            continue;
        }
        if (offset + length > data.size() || length < 34u) break;
        const std::uint8_t name_length = data[offset + 32u];
        const std::uint32_t entry_lba = read_le32(data, offset + 2u);
        const std::uint32_t entry_size = read_le32(data, offset + 10u);
        const bool directory = (data[offset + 25u] & 2u) != 0u;
        std::string name(reinterpret_cast<const char *>(data.data() + offset + 33u), name_length);
        offset += length;
        if (name_length == 1u && (name[0] == '\0' || name[0] == '\1')) continue;
        if (const auto version = name.find(';'); version != std::string::npos) name.resize(version);
        const std::string full = normalize(prefix.empty() ? name : prefix + "/" + name);
        entries_[full] = Entry{entry_lba, entry_size, directory, name};
        if (directory) scan_directory(entry_lba, entry_size, full, depth + 1);
    }
}

std::optional<IsoImage::Entry> IsoImage::find(std::string path) const {
    const auto found = entries_.find(normalize(std::move(path)));
    if (found == entries_.end()) return std::nullopt;
    return found->second;
}

std::size_t IsoImage::read(std::uint64_t offset, std::span<std::uint8_t> output) {
    if (offset >= size_bytes_) return 0u;
    const std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), size_bytes_ - offset));
    // From memory when a preload has already copied the whole range.
    for (const auto &region : preloads_) {
        if (offset >= region->start &&
            offset + count <= region->start + region->loaded.load(std::memory_order_acquire)) {
            std::memcpy(output.data(), region->data.get() + (offset - region->start), count);
            return count;
        }
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char *>(output.data()), static_cast<std::streamsize>(count));
    return static_cast<std::size_t>(file_.gcount());
}

std::vector<std::string> IsoImage::list(std::string directory) const {
    const std::string prefix = normalize(std::move(directory));
    std::vector<std::string> names;
    for (const auto &[path, entry] : entries_) {
        if (path.empty() || path.size() <= prefix.size()) continue;
        if (!prefix.empty() && (path.compare(0, prefix.size(), prefix) != 0 || path[prefix.size()] != '/')) continue;
        const std::string rest = prefix.empty() ? path : path.substr(prefix.size() + 1u);
        if (rest.find('/') == std::string::npos) names.push_back(entry.name.empty() ? rest : entry.name);
    }
    return names;
}

} // namespace portablekit
