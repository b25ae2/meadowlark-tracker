// Host replacement for CMSIS-DSP arm_sin_q15, which the firmware links from the
// prebuilt libarm_cortexM4lf_math.a.
//
// Same algorithm as CMSIS-DSP: a 512-entry Q15 sine table with linear interpolation,
// input x in [0, 32767] mapping to [0, 2*pi). The table is generated at startup as
// round(32768 * sin(2*pi*i/512)) clamped to Q15.
// TODO(phase 1): confirm bit-exact output against the hardware library, or vendor
// CMSIS-DSP's arm_sin_q15.c and arm_common_tables.c (Apache-2.0).

#include <array>
#include <cmath>
#include <cstdint>

namespace
{
constexpr int kTableSize = 512;
constexpr int kShift = 6; // 15-bit input -> 9-bit table index

std::array<int16_t, kTableSize + 1> makeTable()
{
    std::array<int16_t, kTableSize + 1> table{};
    for (int i = 0; i <= kTableSize; i++)
    {
        const double v = std::round(32768.0 * std::sin(2.0 * M_PI * i / kTableSize));
        table[i] = static_cast<int16_t>(v > 32767.0 ? 32767.0 : (v < -32768.0 ? -32768.0 : v));
    }
    return table;
}

const std::array<int16_t, kTableSize + 1> sinTable = makeTable();
} // namespace

int16_t sin_q15(int16_t x)
{
    // Negative inputs are outside CMSIS's documented range; it masks them the same way.
    const uint32_t ux = static_cast<uint16_t>(x) & 0x7FFF;
    const int32_t index = static_cast<int32_t>(ux >> kShift);
    const int16_t fract = static_cast<int16_t>((ux - (static_cast<uint32_t>(index) << kShift)) << 9);

    const int16_t a = sinTable[index];
    const int16_t b = sinTable[index + 1];

    int32_t sinVal = (static_cast<int32_t>(0x8000 - fract) * a) >> 16;
    sinVal = static_cast<int16_t>(((static_cast<int32_t>(sinVal) << 16) + (static_cast<int32_t>(fract) * b)) >> 16);

    return static_cast<int16_t>(sinVal << 1);
}
