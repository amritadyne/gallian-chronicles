#pragma once

#include "audio/soundtrack_import.hpp"

#include <vector>

namespace vc2 {

// Every VC2 music track the soundtrack import can replace.
[[nodiscard]] const std::vector<portablekit::audio::soundtrack::Entry> &soundtrack_table();

} // namespace vc2
