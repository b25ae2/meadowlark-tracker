#pragma once

// core's FileSystem on a host directory that stands in for the SD card.
//
// Behaves like the IMX implementation (FatFs through SD.cpp) where core can tell:
// - names are matched case-insensitively, like FAT; new names keep the case given;
// - paths are relative to the card root with or without a leading '/';
// - mkdir creates one level and succeeds if it exists, remove only deletes files and empty
//   directories, rename fails if the target exists;
// - the FAT "hidden" attribute is kept in a ".hidden" file per directory (the GNOME file
//   manager convention), and dot files count as hidden;
// - listing functions are ports of SdDir's, including their limits. Entries are returned in
//   case-insensitive name order, where FatFs returns directory order.
//
// Sample streaming reads raw sectors through the cluster map from enableFastSeek. Each file
// gets one contiguous run of made-up clusters, and readSectorsAsync maps sectors back to the
// file. Reads complete immediately.
//
// Host mounts (addHostMount) show other host directories as folders in the card's root, so
// the firmware's file browsers can load samples from anywhere. They are read-only: every
// write, rename and delete inside them fails, and deleting card contents never follows a
// symlink out of the card. They appear in browser listings (createFilesList, countContainedFiles) but not
// where the firmware walks the card itself, and a real folder of the same name wins.

#include "Platform.h"

#include <cstdint>
#include <string>
#include <vector>

class DesktopFileSystem : public FileSystem
{
  public:
    // Root used when none is given: $MEADOWLARK_SD_ROOT, else ~/.local/share/meadowlark/sdcard
    // ($XDG_DATA_HOME respected) on Linux and ~/Library/Application Support/Meadowlark/sdcard
    // on macOS.
    static std::string defaultRoot();

    // Creates the directory if needed. Empty string means no card.
    bool setRoot(const std::string& path);
    const std::string& root() const { return rootPath; }

    // Shows hostPath as a read-only folder called name in the card's root.
    void addHostMount(const std::string& name, const std::string& hostPath);
    // Home (the user's home directory) and Computer (/).
    void addDefaultHostMounts();

    bool isSdCardInserted() override { return !rootPath.empty(); }
    void setMountEventCallback(CardStateChangeCallback funct) override { mountCallback = funct; }

    bool exists(const char* path) override;
    bool rename(const char* oldPath, const char* newPath) override;
    bool mkdir(uint8_t hidden, const char* path, bool pFlag = true) override;
    bool remove(const char* path) override;
    bool removeDirWithFiles(const char* path) override;
    bool removeOnlyDirFiles(const char* path) override;

    size_t getFileSize(const char* path) override;
    size_t getFileSize(FileHandle fileHandle) override;
    size_t getFileAvailable(FileHandle fileHandle) override;

    std::tuple<bool, FileHandle> openFile(const char* filepath,
                                          FileOpenMode openMode = kRead,
                                          FileHandle* fileHandle = nullptr) override;
    size_t readFile(void* destBuf, size_t bufLen, FileHandle file) override;
    size_t writeFile(const void* data, size_t dataSize, FileHandle fileHandle) override;
    bool seekFileCur(int offset, FileHandle fileHandle) override;
    bool seekFile(uint32_t pos, FileHandle fileHandle) override;
    void closeFile(FileHandle file, bool deleteOnClose = true) override;
    void endFile(FileHandle file) override;
    bool isFileOpen(FileHandle file) override;

    bool enableFastSeek(FileHandle fileHandle,
                        uint32_t* clmtBuffer,
                        uint32_t clmtElementCount,
                        uint32_t* outDatabaseSector = nullptr,
                        uint32_t* outSectorsPerCluster = nullptr) override;
    bool readSectorsAsync(uint8_t* buffer, uint32_t startSector, uint32_t sectorCount) override;
    bool pollAsyncRead(bool* outSuccess = nullptr) override;
    bool waitAsyncRead() override;
    bool isAsyncReadBusy() override { return false; }

    int createInstrumentsList(const char* path, char** list, uint8_t list_length, uint32_t* sizeOut) override;
    int countContainedFiles(const char* path) override;
    int createFilesList(const char* path,
                        uint8_t start_line,
                        char** list,
                        uint8_t list_length,
                        uint16_t max_used_memory,
                        enListFileFilter filter = enListFileFilter_all,
                        uint16_t skip_count = 0,
                        uint16_t max_files_in_subdir = 0,
                        enListFileFilter folderSizeFilter = enListFileFilter_all) override;
    void listDirContents(const char* dirpath,
                         DirContentType contentType,
                         std::function<bool(const char* name)> nameFilter,
                         char* namesBuffer,
                         int namesBufferSize,
                         int maxNameLen,
                         int& outNamesCount,
                         char** fileList) override;

    int readNbFiles(const char* romPath) override;
    bool nesDir_open(const char* path) override;
    bool nesDir_readItem(char* path, uint8_t* isDir, uint8_t* isHid, uint32_t* size) override;
    void nesDir_close() override;

    // Sector geometry of the made-up volume used for streaming.
    static constexpr uint32_t kSectorSize = 512;
    static constexpr uint32_t kSectorsPerCluster = 64;
    static constexpr uint32_t kDatabaseSector = 0x800;

  private:
    struct Entry
    {
        std::string name;
        bool isDir = false;
        bool hidden = false;
        uint64_t size = 0;
    };

    struct OpenFile
    {
        int fd = -1;
        std::string hostPath;
    };

    struct Extent
    {
        std::string hostPath;
        uint32_t firstCluster = 0;
        uint32_t clusterCount = 0;
    };

    struct Mount
    {
        std::string name;
        std::string hostPath;
    };

    // Host path for a card path. With mustExist false the last component may be missing
    // (its parent must exist); it then keeps the given spelling. inMount reports whether the
    // path lies in a host mount (and is therefore read-only).
    bool resolve(const char* path, std::string& hostPath, bool mustExist = true, bool* inMount = nullptr) const;
    // Resolves a path that is about to be changed: fails for paths in host mounts.
    bool resolveWritable(const char* path, std::string& hostPath, bool mustExist = true) const;
    bool readDirectory(const std::string& hostDir, std::vector<Entry>& entries) const;
    // readDirectory plus the host mounts when hostDir is the card's root; for browsers only.
    bool readListing(const std::string& hostDir, std::vector<Entry>& entries) const;
    bool filterFileEntry(const Entry& entry, enListFileFilter filter, const char* dirPath) const;
    uint16_t countFilesInSubDirectory(const char* dirPath, const char* dirName, enListFileFilter filter);
    bool deleteTree(const std::string& hostPath, bool removeSelf);
    void setHidden(const std::string& hostPath, bool hidden);

    std::string rootPath;
    std::vector<Mount> mounts;
    CardStateChangeCallback mountCallback = nullptr;

    std::vector<Entry> nesEntries;
    size_t nesIndex = 0;
    bool nesOpen = false;

    std::vector<Extent> extents;
    uint32_t nextCluster = 2;
    bool asyncResult = false;
    bool asyncPending = false;
};
