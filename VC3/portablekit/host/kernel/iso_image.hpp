#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace portablekit {

// Read-only ISO9660 view of a PSP UMD image. Paths are case-insensitive and
// use '/' separators relative to the image root (e.g. "PSP_GAME/USRDIR/DATA.BIN").
// Listings keep each name as the disc spells it: a game may hash the names
// it reads from a directory (God of War does) and look them up by its own
// mixed-case paths.
class IsoImage {
public:
    static constexpr std::uint32_t kSectorSize = 2048u;

    struct Entry {
        std::uint32_t lba{};
        std::uint32_t size{};
        bool directory{};
        std::string name;  // the last component, as on the disc
    };

    explicit IsoImage(const std::filesystem::path &path);
    ~IsoImage();
    IsoImage(const IsoImage &) = delete;
    IsoImage &operator=(const IsoImage &) = delete;

    // Copies bytes [start, start + size) of the image into memory on a
    // background thread (Gallian Chronicles patch 0038, "Keep disc images in
    // memory"): reads of that range come from memory once the copy has passed
    // them, and from the drive until then. False when the memory can't be had
    // (the drive is used). Call before reads from other threads start.
    bool preload(std::uint64_t start, std::uint64_t size, const std::string &label);

    [[nodiscard]] std::optional<Entry> find(std::string path) const;
    [[nodiscard]] std::uint64_t size_bytes() const noexcept { return size_bytes_; }
    // Reads up to output.size() bytes at an absolute image offset.
    std::size_t read(std::uint64_t offset, std::span<std::uint8_t> output);
    [[nodiscard]] std::vector<std::string> list(std::string directory) const;

private:
    void scan_directory(std::uint32_t lba, std::uint32_t size, const std::string &prefix, int depth);
    [[nodiscard]] static std::string normalize(std::string path);

    struct Preload {
        std::uint64_t start{};
        std::uint64_t size{};
        std::unique_ptr<std::uint8_t[]> data;
        std::atomic<std::uint64_t> loaded{0u};  // bytes from `start` copied so far
        std::thread thread;
    };

    std::filesystem::path path_;
    std::ifstream file_;
    std::uint64_t size_bytes_{};
    std::vector<std::unique_ptr<Preload>> preloads_;
    std::atomic<bool> stopping_{false};
    std::map<std::string, Entry> entries_;
};

} // namespace portablekit
