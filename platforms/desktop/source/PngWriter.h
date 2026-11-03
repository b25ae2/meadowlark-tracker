#pragma once

#include <cstdint>

// Writes 0xAARRGGBB pixels as an opaque 8-bit RGB PNG. Uncompressed (stored deflate
// blocks): meant for screenshots and tests, not for size.
bool writePng(const char* path, const uint32_t* pixels, int width, int height);
