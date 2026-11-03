// Bit-exactness check for the plain-C DSP fallbacks against the ARM instructions they replace.
//
// Feeds every dspinst.h helper, Polyverb's Clip16 and sin_q15 the same deterministic input
// stream and prints one hash of the outputs per function ("name hash").
//
// Built two ways from this one file:
//   - host: the C fallbacks in dspinst.h/util.h and platform_math.cpp's table sin_q15;
//   - DSP_VECTORS_ARM: freestanding 32-bit ARM with KINETISK defined, so the original asm is
//     used, linked against CMSIS arm_sin_q15 from the firmware. It runs on an AArch64 Linux
//     kernel with 32-bit support (the uConsole) and talks to the kernel directly, no libc.
// tools/check_dsp.sh builds both, runs them and diffs the output.

#include <stdint.h>

#include "utility/dspinst.h"

#include "Polyverb/util.h"

#ifdef DSP_VECTORS_ARM
extern "C" int16_t arm_sin_q15(int16_t x);
static int16_t referenceSin(int16_t x) { return arm_sin_q15(x); }

static long linuxSyscall3(long number, long a, long b, long c)
{
    register long r0 asm("r0") = a;
    register long r1 asm("r1") = b;
    register long r2 asm("r2") = c;
    register long r7 asm("r7") = number;
    asm volatile("svc 0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(r7) : "memory");
    return r0;
}

static void writeOut(const char* data, long length) { linuxSyscall3(4 /* write */, 1, (long)data, length); }
#else
#include <cstdio>
extern int16_t sin_q15(int16_t value);
static int16_t referenceSin(int16_t x) { return sin_q15(x); }
static void writeOut(const char* data, long length) { std::fwrite(data, 1, (size_t)length, stdout); }
#endif

namespace
{
uint32_t rngState = 0x12345678u;

uint32_t nextRandom()
{
    rngState ^= rngState << 13;
    rngState ^= rngState >> 17;
    rngState ^= rngState << 5;
    return rngState;
}

// Edge values for whole words and for each 16-bit half.
const uint32_t kEdges[] = {
    0x00000000u, 0x00000001u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
    0x7FFF7FFFu, 0x80008000u, 0x7FFF8000u, 0x80007FFFu, 0x00007FFFu, 0x00008000u,
    0x7FFF0000u, 0x80000000u, 0xFFFF0001u, 0x0001FFFFu, 0x40004000u, 0xC000C000u,
};
constexpr int kEdgeCount = sizeof(kEdges) / sizeof(kEdges[0]);

// Half the inputs are edge values, so saturation paths get exercised far more often than
// uniformly random inputs would.
uint32_t nextInput()
{
    uint32_t r = nextRandom();
    if (r & 1)
    {
        return kEdges[(r >> 1) % kEdgeCount];
    }
    return nextRandom();
}

struct Hash
{
    uint32_t value = 2166136261u;
    void add(uint32_t v)
    {
        for (int i = 0; i < 4; i++)
        {
            value ^= (v >> (8 * i)) & 0xFF;
            value *= 16777619u;
        }
    }
    void add64(int64_t v)
    {
        add((uint32_t)(uint64_t)v);
        add((uint32_t)((uint64_t)v >> 32));
    }
};

void emit(const char* name, uint32_t hash)
{
    char line[96];
    int n = 0;
    while (name[n] != 0 && n < 80)
    {
        line[n] = name[n];
        n++;
    }
    line[n++] = ' ';
    for (int i = 7; i >= 0; i--)
    {
        line[n++] = "0123456789abcdef"[(hash >> (4 * i)) & 0xF];
    }
    line[n++] = '\n';
    writeOut(line, n);
}

constexpr int kIterations = 200000;

#define TEST1(name, expr)                                                                       \
    do                                                                                           \
    {                                                                                            \
        Hash h;                                                                                  \
        for (int i = 0; i < kIterations; i++)                                                    \
        {                                                                                        \
            uint32_t a = nextInput();                                                            \
            (void)a;                                                                             \
            h.add((uint32_t)(expr));                                                             \
        }                                                                                        \
        emit(name, h.value);                                                                     \
    } while (0)

#define TEST2(name, expr)                                                                       \
    do                                                                                           \
    {                                                                                            \
        Hash h;                                                                                  \
        for (int i = 0; i < kIterations; i++)                                                    \
        {                                                                                        \
            uint32_t a = nextInput();                                                            \
            uint32_t b = nextInput();                                                            \
            h.add((uint32_t)(expr));                                                             \
        }                                                                                        \
        emit(name, h.value);                                                                     \
    } while (0)

#define TEST3(name, expr)                                                                       \
    do                                                                                           \
    {                                                                                            \
        Hash h;                                                                                  \
        for (int i = 0; i < kIterations; i++)                                                    \
        {                                                                                        \
            uint32_t a = nextInput();                                                            \
            uint32_t b = nextInput();                                                            \
            uint32_t c = nextInput();                                                            \
            h.add((uint32_t)(expr));                                                             \
        }                                                                                        \
        emit(name, h.value);                                                                     \
    } while (0)

#define TEST64(name, expr)                                                                      \
    do                                                                                           \
    {                                                                                            \
        Hash h;                                                                                  \
        for (int i = 0; i < kIterations; i++)                                                    \
        {                                                                                        \
            uint32_t a = nextInput();                                                            \
            uint32_t b = nextInput();                                                            \
            int64_t sum = ((int64_t)nextInput() << 32) | nextInput();                            \
            h.add64(expr);                                                                       \
        }                                                                                        \
        emit(name, h.value);                                                                     \
    } while (0)
} // namespace

// The ARM build has no C runtime, so this is called from _start below.
extern "C" int dspVectorsMain()
{
    TEST1("signed_saturate_rshift_16_0", signed_saturate_rshift((int32_t)a, 16, 0));
    TEST1("signed_saturate_rshift_16_8", signed_saturate_rshift((int32_t)a, 16, 8));
    TEST1("signed_saturate_rshift_24_4", signed_saturate_rshift((int32_t)a, 24, 4));
    TEST1("saturate16", (uint16_t)saturate16((int32_t)a));
    TEST2("signed_multiply_32x16b", signed_multiply_32x16b((int32_t)a, b));
    TEST2("signed_multiply_32x16t", signed_multiply_32x16t((int32_t)a, b));
    TEST2("multiply_32x32_rshift32", multiply_32x32_rshift32((int32_t)a, (int32_t)b));
    TEST2("multiply_32x32_rshift32_rounded",
          multiply_32x32_rshift32_rounded((int32_t)a, (int32_t)b));
    TEST3("multiply_accumulate_32x32_rshift32_rounded",
          multiply_accumulate_32x32_rshift32_rounded((int32_t)a, (int32_t)b, (int32_t)c));
    TEST3("multiply_subtract_32x32_rshift32_rounded",
          multiply_subtract_32x32_rshift32_rounded((int32_t)a, (int32_t)b, (int32_t)c));
    TEST2("pack_16t_16t", pack_16t_16t((int32_t)a, (int32_t)b));
    TEST2("pack_16t_16b", pack_16t_16b((int32_t)a, (int32_t)b));
    TEST2("pack_16b_16b", pack_16b_16b((int32_t)a, (int32_t)b));
    TEST2("signed_add_16_and_16", signed_add_16_and_16(a, b));
    TEST2("signed_subtract_16_and_16", signed_subtract_16_and_16((int32_t)a, (int32_t)b));
    TEST2("signed_halving_add_16_and_16", signed_halving_add_16_and_16((int32_t)a, (int32_t)b));
    TEST2("signed_halving_subtract_16_and_16",
          signed_halving_subtract_16_and_16((int32_t)a, (int32_t)b));
    TEST3("signed_multiply_accumulate_32x16b",
          signed_multiply_accumulate_32x16b((int32_t)a, (int32_t)b, c));
    TEST3("signed_multiply_accumulate_32x16t",
          signed_multiply_accumulate_32x16t((int32_t)a, (int32_t)b, c));
    TEST2("logical_and", logical_and(a, b));
    TEST2("multiply_16tx16t_add_16bx16b", multiply_16tx16t_add_16bx16b(a, b));
    TEST2("multiply_16tx16b_add_16bx16t", multiply_16tx16b_add_16bx16t(a, b));
    TEST64("multiply_accumulate_16tx16t_add_16bx16b",
           multiply_accumulate_16tx16t_add_16bx16b(sum, a, b));
    TEST64("multiply_accumulate_16tx16b_add_16bx16t",
           multiply_accumulate_16tx16b_add_16bx16t(sum, a, b));
    TEST2("multiply_16bx16b", multiply_16bx16b(a, b));
    TEST2("multiply_16bx16t", multiply_16bx16t(a, b));
    TEST2("multiply_16tx16b", multiply_16tx16b(a, b));
    TEST2("multiply_16tx16t", multiply_16tx16t(a, b));
    TEST2("substract_32_saturate", substract_32_saturate(a, b));
    TEST2("multiply_32x32_rshift_15", multiply_32x32_rshift((int32_t)a, (int32_t)b, 15));
    TEST2("multiply_32x32_rshift_16", multiply_32x32_rshift((int32_t)a, (int32_t)b, 16));
    TEST1("Clip16", (uint16_t)Clip16((int32_t)a));

    // sin_q15 over every input value rather than a random sample.
    {
        Hash h;
        for (int32_t x = -32768; x <= 32767; x++)
        {
            h.add((uint16_t)referenceSin((int16_t)x));
        }
        emit("sin_q15_all", h.value);
    }
    {
        Hash h;
        for (int32_t x = 0; x <= 32767; x++)
        {
            h.add((uint16_t)referenceSin((int16_t)x));
        }
        emit("sin_q15_nonnegative", h.value);
    }
    return 0;
}

#ifdef DSP_VECTORS_ARM
extern "C" __attribute__((noreturn, used)) void _start()
{
    int status = dspVectorsMain();
    linuxSyscall3(1 /* exit */, status, 0, 0);
    for (;;)
    {
    }
}
#else
int main() { return dspVectorsMain(); }
#endif
