// Host replacement for CMSIS-DSP arm_sin_q15, which the firmware links from the
// prebuilt platforms/imx/3rdparty/math/libarm_cortexM4lf_math.a.
//
// That library is CMSIS-DSP 1.x: cubic interpolation over a 259-entry Q15 table with
// 128-step spacing, table[i] = round(32768 * sin(2*pi*(i - 1)/256)). This follows its
// arithmetic step by step, including where intermediates are truncated to 16 bits, and
// tools/check_dsp.sh checks it against the library for every input value.
// Algorithm from CMSIS-DSP (Copyright ARM Limited, Apache-2.0).

#include <array>
#include <cmath>
#include <cstdint>

namespace
{
constexpr int kTableSize = 259;
constexpr int kTableSpacing = 128;

std::array<int16_t, kTableSize> makeTable()
{
    std::array<int16_t, kTableSize> table{};
    for (int i = 0; i < kTableSize; i++)
    {
        const double v = std::round(32768.0 * std::sin(2.0 * M_PI * (i - 1) / 256.0));
        table[i] = static_cast<int16_t>(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
    }
    return table;
}

const std::array<int16_t, kTableSize> sinTable = makeTable();

// The library sums in 32-bit registers with wraparound; do the same without signed overflow.
inline int32_t wrapAdd(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
inline int32_t wrapMul(int32_t a, int32_t b)
{
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}
} // namespace

int16_t sin_q15(int16_t x)
{
    constexpr int32_t oneBy6 = 0x1555;

    int32_t tableIndex = x / kTableSpacing; // truncates towards zero, like the library
    const int16_t in2 = static_cast<int16_t>(tableIndex * kTableSpacing);
    const int16_t fract = static_cast<int16_t>((x - in2) << 8);
    const int16_t fractSquare = static_cast<int16_t>((fract * fract) >> 15);
    int16_t fractCube = static_cast<int16_t>((fractSquare * fract) >> 15);

    if (tableIndex < 0)
    {
        tableIndex = 0;
    }
    else if (tableIndex > 256)
    {
        tableIndex = 256;
    }
    const int16_t* table = &sinTable[tableIndex];

    int32_t wa = oneBy6 * fractCube;
    wa += 0x2AAA * fract;
    wa = -(wa >> 15);
    wa += fractSquare >> 1;
    int32_t sinVal = wrapMul(table[0], wa);

    const int32_t wb = (((fractCube >> 1) - fractSquare) - (fract >> 1)) + 0x7FFF;
    sinVal = wrapAdd(sinVal, wrapMul(table[1], wb));

    int32_t wc = -fractCube + fractSquare;
    wc = (wc >> 1) + fract;
    sinVal = wrapAdd(sinVal, wrapMul(table[2], wc));

    fractCube = static_cast<int16_t>(fractCube - fract);
    const int16_t wd = static_cast<int16_t>((oneBy6 * fractCube) >> 15);
    sinVal = wrapAdd(sinVal, wrapMul(table[3], wd));

    sinVal >>= 15;
    return static_cast<int16_t>(sinVal > 32767 ? 32767 : (sinVal < -32768 ? -32768 : sinVal));
}
