#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Writes an 8-bit RGB PNG with no compression (no zlib dependency).
bool write_png(const std::string& path, int width, int height, const std::vector<uint8_t>& rgb);
