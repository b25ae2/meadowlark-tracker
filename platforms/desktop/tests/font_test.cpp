// Font checks: .ptfont files built from a TTF/OTF hold what the firmware's loader expects
// (loadCustomFontFromSD), fonts.toml is read, and the fonts appear in the card's /Fonts
// as read-only virtual files.
//
//   font_test <scratch dir> <font file>

#include "DesktopFileSystem.h"
#include "fonts/FontBuilder.h"
#include "fonts/FontConfig.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        if (!(cond))                                                      \
        {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            failures++;                                                   \
        }                                                                 \
    } while (0)

uint32_t word(const std::vector<uint8_t>& data, size_t at)
{
    return data[at] | (data[at + 1] << 8) | (data[at + 2] << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
}

struct Slot
{
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t stride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// Parses and checks the layout; returns the five slots.
std::vector<Slot> checkLayout(const std::vector<uint8_t>& data)
{
    std::vector<Slot> slots;
    CHECK(data.size() > 16 + 5 * 12);
    if (data.size() <= 16 + 5 * 12)
        return slots;
    CHECK(std::memcmp(data.data(), "PTFN", 4) == 0);
    CHECK((data[4] | (data[5] << 8)) == 1);
    CHECK((data[6] | (data[7] << 8)) == 5);

    uint32_t address = 0;
    for (int i = 0; i < 5; i++)
    {
        Slot slot;
        slot.offset = word(data, 16 + 12 * i);
        slot.size = word(data, 16 + 12 * i + 4);
        CHECK(slot.offset + slot.size <= data.size());
        CHECK(slot.size <= fonts::kMaxFontBytes);
        if (slot.offset + slot.size > data.size())
            return {};
        const uint8_t* block = &data[slot.offset];

        // What loadCustomFontFromSD derives: characters with a width, contiguous from the
        // first one, and the size of the cells behind the metric block.
        int count = 0;
        int first = -1;
        int last = -1;
        for (int ch = 0; ch < 128; ch++)
        {
            if (block[ch])
            {
                count++;
                first = first < 0 ? ch : first;
                last = ch;
            }
        }
        // bold17 starts at the transport icons.
        const int expectedFirst = i == 3 ? 0x18 : 32;
        CHECK(first == expectedFirst && last == 126 && count == 127 - expectedFirst);
        slot.stride = word(data, slot.offset + 132);
        slot.width = word(data, slot.offset + 136);
        slot.height = word(data, slot.offset + 140);
        CHECK(word(data, slot.offset + 128) == 2); // L4
        CHECK(slot.stride == (slot.width + 1) / 2);
        CHECK(slot.size == 148 + static_cast<uint32_t>(count) * slot.stride * slot.height);
        CHECK(word(data, slot.offset + 144) == address + 148);
        address = (address + slot.size + 3) & ~3u;
        slots.push_back(slot);
    }
    CHECK(address <= fonts::kMaxTotalFontBytes);
    return slots;
}

// Sum of the 4-bit levels in one character's cell.
int ink(const std::vector<uint8_t>& data, const Slot& slot, char ch, int firstChar = 32)
{
    const size_t cellBytes = slot.stride * slot.height;
    const uint8_t* cell = &data[slot.offset + 148 + static_cast<size_t>(ch - firstChar) * cellBytes];
    int sum = 0;
    for (size_t i = 0; i < cellBytes; i++)
        sum += (cell[i] >> 4) + (cell[i] & 15);
    return sum;
}

void checkBuild(const std::string& fontFile)
{
    fonts::FontSpec spec;
    spec.name = "Test";
    spec.regular.file = fontFile;
    std::vector<uint8_t> data;
    std::string error;
    fonts::SlotInfo info[fonts::kSlotCount];
    CHECK(fonts::buildPtFont(spec, data, error, info));
    if (!error.empty())
        std::printf("build error: %s\n", error.c_str());
    const std::vector<Slot> slots = checkLayout(data);
    CHECK(slots.size() == 5);
    if (slots.size() != 5)
        return;
    for (int i = 0; i < 5; i++)
        std::printf("  %-9s %2d px: cell %2ux%2u, %5u bytes\n",
                    fonts::kSlotNames[i], info[i].pixelSize, slots[i].width, slots[i].height, slots[i].size);

    CHECK(slots[1].height > slots[0].height);  // 17 px is taller than 14
    CHECK(slots[4].height < slots[0].height);  // 12 px is shorter
    CHECK(ink(data, slots[0], 'M') > 0);
    CHECK(ink(data, slots[0], ' ') == 0);
    // Without a bold face the bold sizes are emboldened: same size, more ink.
    CHECK(ink(data, slots[2], 'M') > ink(data, slots[0], 'M'));
    // The pause icon's two halves, and text after the icons.
    CHECK(ink(data, slots[3], '\x18', 0x18) > 0 && ink(data, slots[3], '\x19', 0x18) > 0);
    CHECK(ink(data, slots[3], 'M', 0x18) > 0);
    CHECK(slots[3].width >= 12);

    fonts::FontSpec bigger = spec;
    bigger.sizes[0] = 20;
    std::vector<uint8_t> big;
    CHECK(fonts::buildPtFont(bigger, big, error));
    const std::vector<Slot> bigSlots = checkLayout(big);
    if (bigSlots.size() == 5)
        CHECK(bigSlots[0].height > slots[1].height);

    // Past the firmware's load buffer.
    bigger.sizes[0] = 40;
    CHECK(!fonts::buildPtFont(bigger, big, error));
    CHECK(error.find("too big") != std::string::npos);

    fonts::FontSpec missing = spec;
    missing.regular.file = "/no/such/font.ttf";
    CHECK(!fonts::buildPtFont(missing, data, error));
    CHECK(error.find("cannot open") != std::string::npos);

    fonts::FontSpec wrongStyle = spec;
    wrongStyle.regular.style = "No Such Style";
    CHECK(!fonts::buildPtFont(wrongStyle, data, error));
    CHECK(error.find("it has:") != std::string::npos);
}

bool writeFile(const std::string& path, const std::string& text)
{
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file)
        return false;
    std::fputs(text.c_str(), file);
    return std::fclose(file) == 0;
}

std::vector<std::string> listFonts(DesktopFileSystem& fs)
{
    char buffer[2048];
    char* list[32];
    int count = 32;
    fs.listDirContents("Fonts", FileSystem::kFiles, nullptr, buffer, sizeof(buffer), 32, count, list);
    return std::vector<std::string>(list, list + count);
}

void checkConfigAndCard(const std::string& scratch, const std::string& fontFile)
{
    const std::string configPath = scratch + "/fonts.toml";
    CHECK(writeFile(configPath,
                    "[[font]]\n"
                    "name = \"Good\"\n"
                    "regular = \"" + fontFile + "\"\n"
                    "sizes = { regular14 = 13, bold17 = 18, huge = 3 }\n"
                    "[[font]]\n"
                    "name = \"Broken\"\n"
                    "regular = { file = \"/no/such/font.ttf\" }\n"
                    "[[font]]\n"
                    "regular = \"" + fontFile + "\"\n"));
    std::printf("(the next three warnings are expected)\n");
    std::fflush(stdout);
    fonts::FontConfig config;
    CHECK(fonts::loadFontConfig(configPath, config));
    CHECK(config.fonts.size() == 2); // the nameless one is skipped
    if (config.fonts.size() != 2)
        return;
    CHECK(config.fonts[0].name == "Good" && config.fonts[0].regular.file == fontFile);
    CHECK(config.fonts[0].sizes[0] == 13 && config.fonts[0].sizes[3] == 18 && config.fonts[0].sizes[1] == 17);
    CHECK(config.fonts[1].bold.file.empty());

    fonts::FontConfig none;
    CHECK(fonts::loadFontConfig(scratch + "/missing.toml", none) && none.fonts.empty());

    // On the card: listed in /Fonts, readable, not writable; a real file wins.
    const std::string card = scratch + "/card";
    DesktopFileSystem fs;
    CHECK(fs.setRoot(card));
    CHECK(fs.mkdir(0, "Fonts"));
    CHECK(fonts::installFonts(config, scratch + "/cache/fonts", fs) == 1);
    const std::vector<std::string> listed = listFonts(fs);
    CHECK(listed.size() == 1 && listed[0] == "Good.ptfont");
    CHECK(fs.exists("/Fonts/good.ptfont"));
    CHECK(fs.getFileSize("Fonts/Good.ptfont") > 1000);

    auto [isOpen, file] = fs.openFile("Fonts/Good.ptfont", FileSystem::kRead);
    CHECK(isOpen);
    if (isOpen)
    {
        char magic[4] = {};
        CHECK(fs.readFile(magic, 4, file) == 4 && std::memcmp(magic, "PTFN", 4) == 0);
        fs.closeFile(file);
    }
    auto [canWrite, writeHandle] = fs.openFile("Fonts/Good.ptfont", FileSystem::kWriteOpen);
    CHECK(!canWrite);
    if (canWrite)
        fs.closeFile(writeHandle);
    CHECK(!fs.remove("Fonts/Good.ptfont"));

    CHECK(writeFile(card + "/Fonts/Good.ptfont", "real"));
    CHECK(fs.getFileSize("Fonts/Good.ptfont") == 4);
    CHECK(listFonts(fs).size() == 1);
    CHECK(fs.remove("Fonts/Good.ptfont"));
    CHECK(fs.getFileSize("Fonts/Good.ptfont") > 1000); // the virtual one again
}

void removeTree(const std::string& path)
{
    std::system(("rm -rf '" + path + "'").c_str());
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: font_test <scratch dir> <font file>\n");
        return 2;
    }
    const std::string scratch = argv[1];
    removeTree(scratch);
    mkdir(scratch.c_str(), 0755);

    checkBuild(argv[2]);
    checkConfigAndCard(scratch, argv[2]);

    if (failures)
    {
        std::printf("%d font check(s) failed\n", failures);
        return 1;
    }
    std::printf("all font checks passed\n");
    return 0;
}
