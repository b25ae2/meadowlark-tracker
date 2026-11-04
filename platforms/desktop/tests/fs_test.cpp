// Tests for DesktopFileSystem: FatFs-like path and directory semantics, the SdDir listing
// ports, and the made-up sector map used by sample streaming.
//
//   fs_test <scratch directory>      (it and <scratch directory>-host are deleted and recreated)

#include "DesktopFileSystem.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
int failures = 0;

#define CHECK(cond)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(cond))                                                                               \
        {                                                                                          \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);          \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

bool writeWholeFile(DesktopFileSystem& fs, const char* path, const void* data, size_t size)
{
    auto [ok, file] = fs.openFile(path, FileSystem::kWriteCreateNew);
    if (!ok)
    {
        return false;
    }
    const bool written = fs.writeFile(data, size, file) == size;
    fs.closeFile(file);
    return written;
}

std::string readWholeFile(DesktopFileSystem& fs, const char* path)
{
    auto [ok, file] = fs.openFile(path);
    if (!ok)
    {
        return "<missing>";
    }
    std::string out(fs.getFileSize(file), '\0');
    const size_t n = fs.readFile(out.data(), out.size(), file);
    fs.closeFile(file);
    out.resize(n);
    return out;
}

void testPaths(DesktopFileSystem& fs)
{
    CHECK(fs.mkdir(0, "/Projects"));
    CHECK(fs.mkdir(0, "Projects")); // exists: still true, like FR_EXIST
    CHECK(!fs.mkdir(0, "/Missing/Child")); // one level only
    CHECK(writeWholeFile(fs, "/Projects/Song.MT", "abc", 3));

    // Case-insensitive lookup, with and without the leading slash.
    CHECK(fs.exists("/projects/song.mt"));
    CHECK(fs.exists("PROJECTS/SONG.MT"));
    CHECK(fs.getFileSize("projects/song.mt") == 3);
    CHECK(readWholeFile(fs, "/PROJECTS/song.Mt") == "abc");

    // Writing through a differently-cased path reuses the existing file.
    CHECK(writeWholeFile(fs, "/projects/SONG.mt", "defg", 4));
    CHECK(readWholeFile(fs, "/Projects/Song.MT") == "defg");

    // remove: files and empty directories only.
    CHECK(fs.mkdir(0, "/Projects/Sub"));
    CHECK(writeWholeFile(fs, "/Projects/Sub/x.wav", "1", 1));
    CHECK(!fs.remove("/Projects/Sub"));
    CHECK(fs.remove("/Projects/Sub/X.WAV"));
    CHECK(fs.remove("/Projects/Sub"));
    CHECK(!fs.exists("/Projects/Sub"));

    // rename fails onto an existing name, works otherwise.
    CHECK(writeWholeFile(fs, "/Projects/Other.mt", "z", 1));
    CHECK(!fs.rename("/Projects/Other.mt", "/Projects/song.mt"));
    CHECK(fs.rename("/Projects/Other.mt", "/Projects/Third.mt"));
    CHECK(fs.exists("/Projects/third.mt") && !fs.exists("/Projects/Other.mt"));

    // Opening a directory or a missing file for reading fails.
    CHECK(!std::get<0>(fs.openFile("/Projects")));
    CHECK(!std::get<0>(fs.openFile("/Projects/none.mt")));
}

void testFileHandles(DesktopFileSystem& fs)
{
    // Reused handle (deleteOnClose = false), as cFileTransfer does.
    FileSystem::FileHandle handle = nullptr;
    auto [ok1, h1] = fs.openFile("/handle.bin", FileSystem::kWriteOpen, &handle);
    CHECK(ok1 && h1 == handle && handle != nullptr);
    CHECK(fs.writeFile("0123456789", 10, handle) == 10);
    CHECK(fs.seekFile(4, handle));
    CHECK(fs.writeFile("ab", 2, handle) == 2);
    fs.endFile(handle); // truncate at the position: "0123ab"
    fs.closeFile(handle, false);
    CHECK(!fs.isFileOpen(handle));

    auto [ok2, h2] = fs.openFile("/handle.bin", FileSystem::kRead, &handle);
    CHECK(ok2 && h2 == handle);
    CHECK(fs.getFileSize(handle) == 6);
    char buf[16] = {};
    CHECK(fs.readFile(buf, sizeof(buf), handle) == 6);
    CHECK(std::memcmp(buf, "0123ab", 6) == 0);
    CHECK(fs.getFileAvailable(handle) == 0);
    CHECK(fs.seekFile(100, handle)); // read mode: clamps to the end
    CHECK(fs.readFile(buf, 1, handle) == 0);
    CHECK(fs.seekFile(2, handle) && fs.seekFileCur(1, handle));
    CHECK(fs.readFile(buf, 1, handle) == 1 && buf[0] == '3');
    fs.closeFile(handle, true);

    // Append keeps existing content.
    auto [ok3, h3] = fs.openFile("/handle.bin", FileSystem::kWriteAppend);
    CHECK(ok3 && fs.writeFile("Z", 1, h3) == 1);
    fs.closeFile(h3);
    CHECK(readWholeFile(fs, "/handle.bin") == "0123abZ");
}

void testHiddenAndLists(DesktopFileSystem& fs)
{
    CHECK(fs.mkdir(1, "/Workspace"));
    CHECK(fs.mkdir(0, "/Samples"));
    CHECK(fs.mkdir(0, "/Samples/Drums"));
    CHECK(fs.mkdir(0, "/Samples/__MACOSX"));
    for (const char* name : { "/Samples/kick.WAV", "/Samples/snare.wav", "/Samples/notes.txt",
                              "/Samples/._kick.WAV", "/Samples/a.pti", "/Samples/Drums/1.wav",
                              "/Samples/Drums/2.wav", "/Samples/Drums/3.wav" })
    {
        CHECK(writeWholeFile(fs, name, "x", 1));
    }

    // Hidden directories are left out of listings (and the .hidden file itself is a dot file).
    char* list[16] = {};
    int n = fs.createFilesList("/", 0, list, 16, 2000, FileSystem::enListFileFilter_folder);
    std::vector<std::string> folders;
    for (int i = 0; i < n; i++)
    {
        folders.emplace_back(list[i]);
    }
    CHECK(std::find(folders.begin(), folders.end(), "/Workspace") == folders.end());
    CHECK(std::find(folders.begin(), folders.end(), "/Samples") != folders.end());

    // Wave filter: .wav in any case, not mac metadata, not other files; folders excluded.
    n = fs.createFilesList("/Samples", 0, list, 16, 2000, FileSystem::enListFileFilter_wave);
    CHECK(n == 2);
    CHECK(n == 2 && std::strcmp(list[0], "kick.WAV") == 0 && std::strcmp(list[1], "snare.wav") == 0);

    // wave_inst also takes .pti.
    n = fs.createFilesList("/Samples", 0, list, 16, 2000, FileSystem::enListFileFilter_wave_inst);
    CHECK(n == 3);

    // Folders split into ranges when they hold more than max_files_in_subdir files.
    n = fs.createFilesList("/Samples", 0, list, 16, 2000, FileSystem::enListFileFilter_all, 0, 2,
                           FileSystem::enListFileFilter_wave);
    bool sawRange1 = false, sawRange2 = false;
    for (int i = 0; i < n; i++)
    {
        sawRange1 |= std::strcmp(list[i], "/Drums (1-2)") == 0;
        sawRange2 |= std::strcmp(list[i], "/Drums (3-4)") == 0;
    }
    CHECK(sawRange1 && sawRange2);
    // The firmware opens a range as "<name> " (parseFolderName keeps the space before the
    // parenthesis); FAT drops trailing spaces and dots, so that is the folder itself.
    CHECK(fs.exists("/Samples/Drums ") && fs.exists("/Samples/Drums./1.wav") && fs.exists("/samples /drums /1.WAV"));
    CHECK(fs.createFilesList("/Samples/Drums ", 0, list + 8, 8, 2000, FileSystem::enListFileFilter_wave) == 3);
    for (int i = 8; i < 16; i++)
    {
        delete[] list[i];
        list[i] = nullptr;
    }
    CHECK(!fs.exists("/Samples/ "));

    // Natural order, so numbered samples list (and split into ranges) as 1, 2, ... 10.
    CHECK(fs.mkdir(0, "/Numbered"));
    for (const char* name : { "/Numbered/hit 10.wav", "/Numbered/Hit 2.wav", "/Numbered/hit 1.wav", "/Numbered/hit 007.wav" })
    {
        CHECK(writeWholeFile(fs, name, "x", 1));
    }
    n = fs.createFilesList("/Numbered", 0, list + 8, 8, 2000, FileSystem::enListFileFilter_wave);
    CHECK(n == 4);
    CHECK(n == 4 && std::strcmp(list[8], "hit 1.wav") == 0 && std::strcmp(list[9], "Hit 2.wav") == 0 &&
          std::strcmp(list[10], "hit 007.wav") == 0 && std::strcmp(list[11], "hit 10.wav") == 0);
    for (int i = 8; i < 16; i++)
    {
        delete[] list[i];
        list[i] = nullptr;
    }
    CHECK(fs.removeDirWithFiles("/Numbered"));
    for (char*& item : list)
    {
        delete[] item;
        item = nullptr;
    }

    // countContainedFiles: folders plus .wav files, hidden excluded.
    CHECK(fs.countContainedFiles("/Samples") == 4); // Drums, __MACOSX, kick, snare (._kick is a dot file)

    // listDirContents: folders get a leading '/', mac metadata skipped, sorted.
    char names[256];
    char* fileList[8];
    int count = 8;
    fs.listDirContents("/Samples", FileSystem::kFolders, nullptr, names, sizeof(names), 64, count, fileList);
    CHECK(count == 1 && std::strcmp(fileList[0], "/Drums") == 0);
    count = 8;
    fs.listDirContents("/Samples", FileSystem::kFiles,
                       [](const char* name) { return std::strstr(name, ".txt") == nullptr; }, names,
                       sizeof(names), 64, count, fileList);
    CHECK(count == 3); // a.pti, kick.WAV, snare.wav

    // Instrument list: "<index> <name>.pti" into slot index - 1.
    CHECK(fs.mkdir(0, "/Inst"));
    CHECK(writeWholeFile(fs, "/Inst/3 Bass.pti", "1234", 4));
    CHECK(writeWholeFile(fs, "/Inst/12 Lead.PTI", "12", 2));
    CHECK(writeWholeFile(fs, "/Inst/x Bad.pti", "1", 1));
    char* instruments[48] = {};
    uint32_t size = 0;
    CHECK(fs.createInstrumentsList("/Inst", instruments, 48, &size) == 2);
    CHECK(instruments[2] && std::strcmp(instruments[2], "Bass") == 0);
    CHECK(instruments[11] && std::strcmp(instruments[11], "Lead") == 0);
    CHECK(size == 6);
    for (char*& item : instruments)
    {
        delete[] item;
    }

    // removeOnlyDirFiles keeps the directory, removeDirWithFiles removes it.
    CHECK(fs.removeOnlyDirFiles("/Samples"));
    CHECK(fs.exists("/Samples") && !fs.exists("/Samples/Drums") && !fs.exists("/Samples/kick.wav"));
    CHECK(fs.removeDirWithFiles("/Samples"));
    CHECK(!fs.exists("/Samples"));
}

// Same arithmetic as VoiceStreamManager::translateOffsetToLba.
bool translate(const uint32_t* clmt, uint32_t db, uint32_t csize, uint32_t offset, uint32_t& lba, uint32_t& contig)
{
    const uint32_t sector = offset / 512;
    uint32_t cl = sector / csize;
    const uint32_t csect = sector % csize;
    const uint32_t* tbl = &clmt[1];
    for (;;)
    {
        const uint32_t ncl = *tbl++;
        if (ncl == 0)
        {
            return false;
        }
        const uint32_t start = *tbl++;
        if (cl < ncl)
        {
            lba = db + (start + cl - 2) * csize + csect;
            contig = (ncl - cl) * csize - csect;
            return true;
        }
        cl -= ncl;
    }
}

void testSectorMap(DesktopFileSystem& fs)
{
    // Two files, so the second one's clusters do not start at 2.
    std::vector<uint8_t> a(70000), b(100000);
    for (size_t i = 0; i < a.size(); i++)
    {
        a[i] = static_cast<uint8_t>(i * 7);
    }
    for (size_t i = 0; i < b.size(); i++)
    {
        b[i] = static_cast<uint8_t>(i * 13 + 1);
    }
    CHECK(writeWholeFile(fs, "/a.wav", a.data(), a.size()));
    CHECK(writeWholeFile(fs, "/b.wav", b.data(), b.size()));

    for (const auto& [path, data] : { std::pair{ "/a.wav", &a }, std::pair{ "/b.wav", &b } })
    {
        auto [ok, file] = fs.openFile(path);
        CHECK(ok);
        uint32_t clmt[64] = { 64 };
        uint32_t db = 0, csize = 0;
        CHECK(fs.enableFastSeek(file, clmt, 64, &db, &csize));
        CHECK(db != 0 && csize != 0);

        for (uint32_t offset : { 0u, 512u, 40000u, 65536u, static_cast<uint32_t>(data->size() - 100) })
        {
            uint32_t lba = 0, contig = 0;
            CHECK(translate(clmt, db, csize, offset, lba, contig));
            std::vector<uint8_t> sectors(512 * 2);
            CHECK(fs.readSectorsAsync(sectors.data(), lba, 2));
            bool success = false;
            CHECK(fs.pollAsyncRead(&success) && success);
            const uint32_t inSector = offset % 512;
            const size_t compare = std::min<size_t>(256, data->size() - offset);
            CHECK(std::memcmp(&sectors[inSector], data->data() + offset, compare) == 0);
        }
        fs.closeFile(file);
    }
}
bool hostFileExists(const std::string& path)
{
    FILE* file = std::fopen(path.c_str(), "rb");
    if (file)
        std::fclose(file);
    return file != nullptr;
}

// Host folders shown in the card's root: browsable and readable, never writable, and never
// reached by deleting card contents. Runs last: it empties the card.
void testHostMounts(DesktopFileSystem& fs, const std::string& hostDir)
{
    std::system(("mkdir -p '" + hostDir + "/Sub' && printf 'tone' > '" + hostDir + "/Sub/One.wav'").c_str());
    fs.addHostMount("Host", hostDir);
    fs.addHostMount("Computer", "/");
    fs.addHostMount("Missing", hostDir + "/nope"); // ignored: not a directory

    char* list[64] = {};
    int n = fs.createFilesList("/", 0, list, 64, 4000, FileSystem::enListFileFilter_folder);
    std::vector<std::string> folders(list, list + n);
    CHECK(std::count(folders.begin(), folders.end(), "/Host") == 1);
    CHECK(std::count(folders.begin(), folders.end(), "/Computer") == 1);
    CHECK(std::count(folders.begin(), folders.end(), "/Missing") == 0);
    for (char*& item : list)
    {
        delete[] item;
        item = nullptr;
    }

    // Browsing and reading, case-insensitively like the rest of the card.
    CHECK(fs.exists("/Host") && fs.exists("/host/sub/one.WAV"));
    CHECK(readWholeFile(fs, "/Host/Sub/One.wav") == "tone");
    n = fs.createFilesList("/Host/Sub", 0, list, 64, 4000, FileSystem::enListFileFilter_wave);
    CHECK(n == 1 && std::strcmp(list[0], "One.wav") == 0);
    for (char*& item : list)
    {
        delete[] item;
        item = nullptr;
    }
    const std::string viaComputer = "/Computer" + hostDir + "/Sub/One.wav";
    CHECK(readWholeFile(fs, viaComputer.c_str()) == "tone");
    CHECK(fs.getFileSize(viaComputer.c_str()) == 4);

    // Read-only.
    auto [opened, file] = fs.openFile("/Host/Sub/One.wav", FileSystem::kWriteOpen);
    CHECK(!opened);
    CHECK(!writeWholeFile(fs, "/Host/New.wav", "x", 1));
    CHECK(!fs.mkdir(0, "/Host/Dir"));
    CHECK(!fs.remove("/Host/Sub/One.wav"));
    CHECK(!fs.rename("/Host/Sub/One.wav", "/Host/Sub/Two.wav"));
    CHECK(!fs.rename("/Host/Sub/One.wav", "/Stolen.wav"));
    CHECK(!fs.removeOnlyDirFiles("/Host"));
    CHECK(!fs.removeDirWithFiles("/Host/Sub"));
    CHECK(!fs.remove(viaComputer.c_str()));
    CHECK(hostFileExists(hostDir + "/Sub/One.wav") && !hostFileExists(hostDir + "/New.wav"));

    // Emptying the card leaves the mounts alone, and a symlink on the card is removed
    // without touching what it points to.
    std::system(("ln -s '" + hostDir + "' '" + fs.root() + "/Link'").c_str());
    CHECK(fs.removeOnlyDirFiles("/"));
    CHECK(hostFileExists(hostDir + "/Sub/One.wav"));
    CHECK(fs.exists("/Host/Sub/One.wav") && !fs.exists("/Link"));

    // A real folder of the same name takes precedence.
    std::system(("mkdir '" + fs.root() + "/host'").c_str());
    CHECK(!fs.exists("/Host/Sub"));
    CHECK(writeWholeFile(fs, "/Host/Mine.wav", "m", 1));
    CHECK(!hostFileExists(hostDir + "/Mine.wav"));
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <scratch directory>\n", argv[0]);
        return 2;
    }
    const std::string root = argv[1];
    const std::string hostDir = root + "-host";
    std::system(("rm -rf '" + root + "' '" + hostDir + "'").c_str());

    DesktopFileSystem fs;
    if (!fs.setRoot(root))
    {
        std::fprintf(stderr, "cannot create %s\n", root.c_str());
        return 2;
    }

    testPaths(fs);
    testFileHandles(fs);
    testHiddenAndLists(fs);
    testSectorMap(fs);
    testHostMounts(fs, hostDir);

    if (failures == 0)
    {
        std::printf("fs_test: all checks passed\n");
        return 0;
    }
    std::printf("fs_test: %d checks failed\n", failures);
    return 1;
}
