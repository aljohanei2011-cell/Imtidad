// BGRA -> NV12 (BT.601, limited range). Portable, so it can be unit-tested.
#pragma once

#include <cstddef>
#include <cstdint>

namespace screenlink {

// src: BGRA rows of `src_pitch` bytes. dst: Y plane (width*height) followed by
// interleaved UV plane (width*height/2). width and height must be even.
inline void bgra_to_nv12(const std::uint8_t* src, std::size_t src_pitch, int width, int height, std::uint8_t* dst) {
    std::uint8_t* y_plane = dst;
    std::uint8_t* uv_plane = dst + static_cast<std::size_t>(width) * height;
    for (int y = 0; y < height; y += 2) {
        const std::uint8_t* row0 = src + static_cast<std::size_t>(y) * src_pitch;
        const std::uint8_t* row1 = row0 + src_pitch;
        std::uint8_t* y0 = y_plane + static_cast<std::size_t>(y) * width;
        std::uint8_t* y1 = y0 + width;
        std::uint8_t* uv = uv_plane + static_cast<std::size_t>(y / 2) * width;
        for (int x = 0; x < width; x += 2) {
            int sum_b = 0, sum_g = 0, sum_r = 0;
            const std::uint8_t* px[4] = {row0 + x * 4, row0 + x * 4 + 4, row1 + x * 4, row1 + x * 4 + 4};
            std::uint8_t* out[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int i = 0; i < 4; ++i) {
                const int b = px[i][0], g = px[i][1], r = px[i][2];
                *out[i] = static_cast<std::uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
                sum_b += b; sum_g += g; sum_r += r;
            }
            const int b = sum_b / 4, g = sum_g / 4, r = sum_r / 4;
            uv[x] = static_cast<std::uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[x + 1] = static_cast<std::uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

}  // namespace screenlink
