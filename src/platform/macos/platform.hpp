#pragma once

#include "stormtty/terminal.hpp"
#include <filesystem>
#include <span>
#include <vector>

namespace stormtty {
// ImageIO is restricted to PNG and preflighted before any pixel decode.
std::optional<DecodedPng> decode_macos_png(std::span<const std::uint8_t>, std::size_t max_decoded_bytes);
void install_macos_png_decoder();
void write_macos_capture(const std::filesystem::path&, std::span<const std::uint8_t> rgba,
                         std::uint32_t width, std::uint32_t height);
}
