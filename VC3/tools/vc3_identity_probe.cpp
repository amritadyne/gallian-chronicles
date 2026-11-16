#include "install/executable_preparation.hpp"
#include "profile.hpp"

#include "psprecomp/sha256.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace portablekit {

// executable_preparation.cpp also defines the installer-facing wrapper, which
// references game().  This probe calls decrypt_executable directly, but keeps
// a deliberately empty profile definition so the preparation object links.
const GameProfile &game() {
    static const GameProfile profile{};
    return profile;
}

} // namespace portablekit

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_new_file(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
    if (std::filesystem::exists(path))
        throw std::runtime_error("Refusing to overwrite existing output: " + path.string());
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot create " + path.string());
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("Cannot write " + path.string());
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 3) {
            std::cerr << "Usage: vc3_identity_probe <EBOOT.BIN> <new-EBOOT.ELF>\n";
            return 2;
        }
        const std::filesystem::path encrypted_path = argv[1];
        const std::filesystem::path output_path = argv[2];
        const std::vector<std::uint8_t> encrypted = read_file(encrypted_path);
        const auto encrypted_hash = psprecomp::sha256_bytes(encrypted);
        constexpr std::string_view expected_encrypted =
            "3879146a4c4a9f31fa0435edd6c53c94e5e9e55bb838be269246cfd35e95dcbf";
        if (encrypted_hash != expected_encrypted)
            throw std::runtime_error("EBOOT.BIN does not match ULJM-05957 Extra Edition");

        const portablekit::install::TagKeyMaterial tag{
            0xD91613F0u,
            {0xEBu, 0xFFu, 0x40u, 0xD8u, 0xB4u, 0x1Au, 0xE1u, 0x66u,
             0x91u, 0x3Bu, 0x8Fu, 0x64u, 0xB6u, 0xFCu, 0xB7u, 0x12u},
            {}};
        const std::vector<std::uint8_t> executable = portablekit::install::decrypt_executable(encrypted, tag);
        if (executable.size() < 4u || executable[0] != 0x7Fu || executable[1] != 'E' ||
            executable[2] != 'L' || executable[3] != 'F')
            throw std::runtime_error("Decryption did not produce an ELF executable");

        write_new_file(output_path, executable);
        std::cout << "prepared_sha256=" << psprecomp::sha256_bytes(executable) << "\n";
        std::cout << "prepared_bytes=" << executable.size() << "\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "vc3_identity_probe: " << error.what() << "\n";
        return 1;
    }
}
