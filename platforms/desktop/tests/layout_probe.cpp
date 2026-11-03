// Layout probe for the structs the firmware writes to files and EEPROM as raw bytes.
//
// The desktop build must lay these out exactly as the ARM firmware does, or projects,
// instruments, patterns and config written by one cannot be read by the other.
//
// The same list is compiled twice:
//   - for the host, linked into layout_probe, which prints "expression value" lines;
//   - for the firmware target (arm-none-eabi, firmware flags) with LAYOUT_PROBE_OBJECT_ONLY,
//     compiled to an object file only. The values land in the .layout_probe section as
//     little-endian uint32_t; tools/check_layout.sh extracts them and compares.

#include "fileManagerDefs.h"
#include "mtEnvelopeGenerator.h"
#include "mtSequencer.h"
#include "mtStructs.h"
#include "wavHeaderReader.h"

#include <cstddef>
#include <cstdint>

// clang-format off
#define LAYOUT_ITEMS(X)                                                       \
    X(sizeof(strProjectFileHeader))                                           \
    X(sizeof(strInstrumentFile))                                              \
    X(offsetof(strInstrumentFile, crc))                                       \
    X(sizeof(strProjectFile))                                                 \
    X(offsetof(strProjectFile, crc))                                          \
    X(sizeof(strInstrument))                                                  \
    X(sizeof(strInstrument::strSampleBankSlot))                               \
    X(offsetof(strInstrument::strSampleBankSlot, address))                    \
    X(offsetof(strInstrument::strSampleBankSlot, length))                     \
    X(offsetof(strInstrument, name))                                          \
    X(offsetof(strInstrument, granular))                                      \
    X(sizeof(envelopeGenerator::strEnv))                                   \
    X(sizeof(SynthPartSettings))                                              \
    X(sizeof(strMtValues))                                                    \
    X(offsetof(strMtValues, reverb))                                          \
    X(offsetof(strMtValues, synthPart))                                       \
    X(sizeof(strSong))                                                        \
    X(sizeof(strMtProjectRemote))                                             \
    X(offsetof(strMtProjectRemote, values))                                   \
    X(offsetof(strMtProjectRemote, projectName))                              \
    X(offsetof(strMtProjectRemote, projectPath))                              \
    X(sizeof(strMtConfig))                                                    \
    X(offsetof(strMtConfig, header))                                          \
    X(offsetof(strMtConfig, audioCodecConfig))                                \
    X(offsetof(strMtConfig, firmware))                                        \
    X(offsetof(strMtConfig, midi))                                            \
    X(offsetof(strMtConfig, values))                                          \
    X(offsetof(strMtConfig, general))                                         \
    X(offsetof(strMtConfig, interface))                                       \
    X(offsetof(strMtConfig, metronome))                                       \
    X(offsetof(strMtConfig, debug))                                           \
    X(offsetof(strMtConfig, common))                                          \
    X(sizeof(strWavFileHeader))                                               \
    X(sizeof(Sequencer::strPattern))                                          \
    X(sizeof(Sequencer::strPattern::strFileHeader))                           \
    X(sizeof(Sequencer::strPattern::strTrack))                                \
    X(sizeof(Sequencer::strPattern::strTrack::strStep))                       \
    X(sizeof(Sequencer::strPattern::strTrack::strStep::strFx))                \
    X(offsetof(Sequencer::strPattern, track))
// clang-format on

#define LAYOUT_VALUE(expr) static_cast<uint32_t>(expr),

#ifdef LAYOUT_PROBE_OBJECT_ONLY
#define LAYOUT_PROBE_SECTION __attribute__((used, section(".layout_probe")))
#else
#define LAYOUT_PROBE_SECTION
#endif

extern "C" LAYOUT_PROBE_SECTION const uint32_t layoutProbeValues[] = {
    LAYOUT_ITEMS(LAYOUT_VALUE)
};

#ifndef LAYOUT_PROBE_OBJECT_ONLY
#include <cstdio>

#define LAYOUT_NAME(expr) #expr,

static const char* const layoutProbeNames[] = { LAYOUT_ITEMS(LAYOUT_NAME) };

int main()
{
    for (size_t i = 0; i < sizeof(layoutProbeValues) / sizeof(layoutProbeValues[0]); i++)
    {
        std::printf("%s %u\n", layoutProbeNames[i], (unsigned)layoutProbeValues[i]);
    }
    return 0;
}
#endif
