#include "DesktopFileSystem.h"

#include "util_string.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
constexpr char kHiddenListName[] = ".hidden";

// MEADOWLARK_FS_TRACE=1 logs file system calls that core makes.
bool traceEnabled()
{
    static const bool enabled = std::getenv("MEADOWLARK_FS_TRACE") != nullptr;
    return enabled;
}

#define FS_TRACE(...)                                                                              \
    do                                                                                             \
    {                                                                                              \
        if (traceEnabled())                                                                        \
        {                                                                                          \
            std::fprintf(stderr, "fs: " __VA_ARGS__);                                              \
        }                                                                                          \
    } while (0)

bool isDirectory(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool pathExists(const std::string& path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0;
}

std::string parentOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string(".") : (slash == 0 ? std::string("/") : path.substr(0, slash));
}

std::string nameOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::vector<std::string> readHiddenList(const std::string& dir)
{
    std::vector<std::string> names;
    std::ifstream in(dir + "/" + kHiddenListName);
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty())
        {
            names.push_back(line);
        }
    }
    return names;
}

bool makeDirectories(const std::string& path)
{
    if (path.empty() || isDirectory(path))
    {
        return true;
    }
    if (!makeDirectories(parentOf(path)))
    {
        return false;
    }
    return ::mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
}

// "name.wav" style check, case-insensitive on the extension letters, like SD.cpp.
bool hasExtension(const std::string& name, const char* extension)
{
    const size_t n = name.size(), e = std::strlen(extension);
    if (n < e + 2) // at least one character before the dot
    {
        return false;
    }
    if (name[n - e - 1] != '.')
    {
        return false;
    }
    return strncasecmp(name.c_str() + n - e, extension, e) == 0;
}

// FatFs drops trailing spaces and dots from each name in a path ("Kicks " opens "Kicks"),
// and the firmware relies on it: folder ranges like "/Kicks (1-100)" are opened as "Kicks ".
std::string fatName(const std::string& name)
{
    size_t end = name.size();
    while (end > 0 && (name[end - 1] == ' ' || name[end - 1] == '.'))
    {
        end--;
    }
    return name.substr(0, end);
}

// Natural order, case-insensitive: "hit 2" before "hit 10". Directory order on FAT is the
// order files were written; this is what that usually looks like, and it keeps the
// firmware's numbered folder ranges ("/Kicks (1-100)") holding files 1 to 100.
bool naturalLess(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size())
    {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[j]);
        if (std::isdigit(ca) && std::isdigit(cb))
        {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])))
                ei++;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])))
                ej++;
            size_t si = i, sj = j; // skip leading zeros
            while (si + 1 < ei && a[si] == '0')
                si++;
            while (sj + 1 < ej && b[sj] == '0')
                sj++;
            if (ei - si != ej - sj)
                return ei - si < ej - sj;
            const int c = a.compare(si, ei - si, b, sj, ej - sj);
            if (c != 0)
                return c < 0;
            i = ei;
            j = ej;
            continue;
        }
        const int la = std::tolower(ca), lb = std::tolower(cb);
        if (la != lb)
            return la < lb;
        i++;
        j++;
    }
    if (a.size() - i != b.size() - j)
        return a.size() - i < b.size() - j;
    return a < b; // a strict order even for names differing only in case or zeros
}

// Whether dir holds an entry whose name matches case-insensitively.
bool hasEntryNamed(const std::string& dir, const std::string& name)
{
    bool found = false;
    if (DIR* handle = ::opendir(dir.c_str()))
    {
        while (dirent* entry = ::readdir(handle))
        {
            if (strcasecmp(entry->d_name, name.c_str()) == 0)
            {
                found = true;
                break;
            }
        }
        ::closedir(handle);
    }
    return found;
}

bool isMacMetaFile(const std::string& name) { return name.size() >= 2 && name[0] == '.' && name[1] == '_'; }
bool isMacMetaFolder(const std::string& name) { return name == "__MACOSX"; }
} // namespace

//------------------------------------------------------------------------------------
// Root and path resolution

std::string DesktopFileSystem::defaultRoot()
{
    if (const char* env = std::getenv("MEADOWLARK_SD_ROOT"); env != nullptr && *env != 0)
    {
        return env;
    }
    const char* home = std::getenv("HOME");
    const std::string homeDir = home ? home : ".";
#ifdef __APPLE__
    return homeDir + "/Library/Application Support/Meadowlark/sdcard";
#else
    const char* dataHome = std::getenv("XDG_DATA_HOME");
    const std::string base = (dataHome && *dataHome) ? std::string(dataHome) : homeDir + "/.local/share";
    return base + "/meadowlark/sdcard";
#endif
}

bool DesktopFileSystem::setRoot(const std::string& path)
{
    if (path.empty())
    {
        rootPath.clear();
        return true;
    }
    std::string clean = path;
    while (clean.size() > 1 && clean.back() == '/')
    {
        clean.pop_back();
    }
    if (!makeDirectories(clean))
    {
        rootPath.clear();
        return false;
    }
    rootPath = clean;
    return true;
}

void DesktopFileSystem::addHostMount(const std::string& name, const std::string& hostPath)
{
    if (name.empty() || name.find('/') != std::string::npos || !isDirectory(hostPath))
    {
        return;
    }
    std::string clean = hostPath;
    while (clean.size() > 1 && clean.back() == '/')
    {
        clean.pop_back();
    }
    mounts.push_back({ name, clean });
}

void DesktopFileSystem::addDefaultHostMounts()
{
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != 0)
    {
        addHostMount("Home", home);
    }
    addHostMount("Computer", "/");
}

bool DesktopFileSystem::resolveWritable(const char* path, std::string& hostPath, bool mustExist) const
{
    bool inMount = false;
    if (!resolve(path, hostPath, mustExist, &inMount))
    {
        return false;
    }
    if (inMount)
    {
        FS_TRACE("write refused, read-only host folder: %s\n", path);
        return false;
    }
    return true;
}

bool DesktopFileSystem::resolve(const char* path, std::string& hostPath, bool mustExist, bool* inMount) const
{
    if (inMount != nullptr)
    {
        *inMount = false;
    }
    if (rootPath.empty() || path == nullptr)
    {
        return false;
    }

    std::vector<std::string> parts;
    for (const char* p = path; *p != 0;)
    {
        while (*p == '/')
        {
            p++;
        }
        const char* start = p;
        while (*p != 0 && *p != '/')
        {
            p++;
        }
        if (p > start)
        {
            std::string part(start, p);
            if (part == "..")
            {
                if (!parts.empty())
                {
                    parts.pop_back();
                }
            }
            else if (part != ".")
            {
                part = fatName(part);
                if (part.empty())
                {
                    return false; // all spaces or dots: FR_INVALID_NAME
                }
                parts.push_back(part);
            }
        }
    }

    std::string current = rootPath;
    size_t first = 0;
    // A host mount, unless the card has a real entry of that name.
    if (!parts.empty())
    {
        for (const Mount& mount : mounts)
        {
            if (strcasecmp(parts[0].c_str(), mount.name.c_str()) == 0 && !hasEntryNamed(rootPath, parts[0]))
            {
                current = mount.hostPath == "/" ? "" : mount.hostPath;
                first = 1;
                if (inMount != nullptr)
                {
                    *inMount = true;
                }
                break;
            }
        }
    }
    if (first == 1 && parts.size() == 1)
    {
        hostPath = current.empty() ? "/" : current;
        return true;
    }
    for (size_t i = first; i < parts.size(); i++)
    {
        const bool last = i + 1 == parts.size();
        std::string candidate = current + "/" + parts[i];
        if (!pathExists(candidate))
        {
            // FAT is case-insensitive and ignores trailing spaces and dots: look for the name
            // spelled another way (host files may really end in a space or dot).
            bool found = false;
            if (DIR* dir = ::opendir(current.empty() ? "/" : current.c_str()))
            {
                while (dirent* entry = ::readdir(dir))
                {
                    if (strcasecmp(fatName(entry->d_name).c_str(), parts[i].c_str()) == 0)
                    {
                        candidate = current + "/" + entry->d_name;
                        found = true;
                        break;
                    }
                }
                ::closedir(dir);
            }
            if (!found && (mustExist || !last))
            {
                return false;
            }
        }
        if (!last && !isDirectory(candidate))
        {
            return false;
        }
        current = candidate;
    }
    hostPath = current;
    return true;
}

bool DesktopFileSystem::readDirectory(const std::string& hostDir, std::vector<Entry>& entries) const
{
    entries.clear();
    DIR* dir = ::opendir(hostDir.c_str());
    if (dir == nullptr)
    {
        return false;
    }
    const std::vector<std::string> hiddenNames = readHiddenList(hostDir);
    while (dirent* item = ::readdir(dir))
    {
        const std::string name = item->d_name;
        if (name == "." || name == "..")
        {
            continue;
        }
        struct stat st;
        if (::stat((hostDir + "/" + name).c_str(), &st) != 0)
        {
            continue;
        }
        Entry entry;
        entry.name = name;
        entry.isDir = S_ISDIR(st.st_mode);
        entry.size = entry.isDir ? 0 : static_cast<uint64_t>(st.st_size);
        entry.hidden = name[0] == '.' ||
                       std::find(hiddenNames.begin(), hiddenNames.end(), name) != hiddenNames.end();
        entries.push_back(entry);
    }
    ::closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return naturalLess(a.name, b.name); });
    return true;
}

bool DesktopFileSystem::readListing(const std::string& hostDir, std::vector<Entry>& entries) const
{
    if (!readDirectory(hostDir, entries))
    {
        return false;
    }
    if (hostDir != rootPath)
    {
        return true;
    }
    for (const Mount& mount : mounts)
    {
        const bool shadowed = std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
            return strcasecmp(entry.name.c_str(), mount.name.c_str()) == 0;
        });
        if (!shadowed)
        {
            Entry entry;
            entry.name = mount.name;
            entry.isDir = true;
            entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return naturalLess(a.name, b.name); });
    return true;
}

void DesktopFileSystem::setHidden(const std::string& hostPath, bool hidden)
{
    const std::string dir = parentOf(hostPath);
    const std::string name = nameOf(hostPath);
    std::vector<std::string> names = readHiddenList(dir);
    const auto it = std::find(names.begin(), names.end(), name);
    if (hidden == (it != names.end()))
    {
        return;
    }
    if (hidden)
    {
        names.push_back(name);
    }
    else
    {
        names.erase(it);
    }
    std::ofstream out(dir + "/" + kHiddenListName, std::ios::trunc);
    for (const std::string& n : names)
    {
        out << n << '\n';
    }
}

//------------------------------------------------------------------------------------
// Files and directories

bool DesktopFileSystem::exists(const char* path)
{
    std::string host;
    const bool found = resolve(path, host);
    FS_TRACE("exists %s -> %d\n", path, found);
    return found;
}

bool DesktopFileSystem::rename(const char* oldPath, const char* newPath)
{
    std::string from, to;
    if (!resolveWritable(oldPath, from) || !resolveWritable(newPath, to, false))
    {
        return false;
    }
    if (pathExists(to) && strcasecmp(from.c_str(), to.c_str()) != 0)
    {
        return false; // FR_EXIST
    }
    const bool wasHidden = [&] {
        const std::vector<std::string> names = readHiddenList(parentOf(from));
        return std::find(names.begin(), names.end(), nameOf(from)) != names.end();
    }();
    if (::rename(from.c_str(), to.c_str()) != 0)
    {
        return false;
    }
    if (wasHidden)
    {
        setHidden(from, false);
        setHidden(to, true);
    }
    return true;
}

bool DesktopFileSystem::mkdir(uint8_t hidden, const char* path, bool)
{
    std::string host;
    FS_TRACE("mkdir %s hidden %d\n", path, hidden);
    if (!resolveWritable(path, host, false))
    {
        return false;
    }
    if (!pathExists(host) && ::mkdir(host.c_str(), 0755) != 0)
    {
        return false;
    }
    if (hidden)
    {
        setHidden(host, true);
    }
    return true;
}

bool DesktopFileSystem::remove(const char* path)
{
    FS_TRACE("remove %s\n", path);
    std::string host;
    if (!resolveWritable(path, host) || host == rootPath)
    {
        return false;
    }
    if (isDirectory(host))
    {
        if (::rmdir(host.c_str()) != 0)
        {
            return false;
        }
    }
    else if (::unlink(host.c_str()) != 0)
    {
        return false;
    }
    setHidden(host, false);
    return true;
}

bool DesktopFileSystem::deleteTree(const std::string& hostPath, bool removeSelf)
{
    std::vector<Entry> entries;
    if (!readDirectory(hostPath, entries))
    {
        return false;
    }
    for (const Entry& entry : entries)
    {
        const std::string child = hostPath + "/" + entry.name;
        // A symlink goes, its target stays: never follow one out of the card.
        struct stat link;
        const bool isLink = ::lstat(child.c_str(), &link) == 0 && S_ISLNK(link.st_mode);
        if (entry.isDir && !isLink ? !deleteTree(child, true) : ::unlink(child.c_str()) != 0)
        {
            return false;
        }
    }
    if (removeSelf)
    {
        if (::rmdir(hostPath.c_str()) != 0)
        {
            return false;
        }
        setHidden(hostPath, false);
    }
    return true;
}

bool DesktopFileSystem::removeDirWithFiles(const char* path)
{
    std::string host;
    return resolveWritable(path, host) && host != rootPath && isDirectory(host) && deleteTree(host, true);
}

bool DesktopFileSystem::removeOnlyDirFiles(const char* path)
{
    std::string host;
    return resolveWritable(path, host) && isDirectory(host) && deleteTree(host, false);
}

size_t DesktopFileSystem::getFileSize(const char* path)
{
    std::string host;
    struct stat st;
    if (!resolve(path, host) || ::stat(host.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    {
        return 0;
    }
    return static_cast<size_t>(st.st_size);
}

size_t DesktopFileSystem::getFileSize(FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    struct stat st;
    if (file == nullptr || file->fd < 0 || ::fstat(file->fd, &st) != 0)
    {
        return 0;
    }
    return static_cast<size_t>(st.st_size);
}

size_t DesktopFileSystem::getFileAvailable(FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0)
    {
        return 0;
    }
    const off_t position = ::lseek(file->fd, 0, SEEK_CUR);
    const size_t size = getFileSize(fileHandle);
    return position >= 0 && static_cast<size_t>(position) < size ? size - static_cast<size_t>(position) : 0;
}

std::tuple<bool, FileSystem::FileHandle> DesktopFileSystem::openFile(const char* filepath,
                                                                    FileOpenMode openMode,
                                                                    FileHandle* fileHandle)
{
    OpenFile* file;
    if (fileHandle == nullptr)
    {
        file = new OpenFile;
    }
    else if (*fileHandle == nullptr)
    {
        file = new OpenFile;
        *fileHandle = file;
    }
    else
    {
        file = static_cast<OpenFile*>(*fileHandle);
    }

    // Like SdFile::open: a reused handle is closed first.
    if (file->fd >= 0)
    {
        ::close(file->fd);
        file->fd = -1;
    }

    auto fail = [&]() -> std::tuple<bool, FileHandle> {
        if (fileHandle == nullptr)
        {
            delete file;
            return { false, nullptr };
        }
        return { false, file };
    };

    std::string host;
    const bool resolved = openMode == kRead ? resolve(filepath, host) : resolveWritable(filepath, host, false);
    if (!resolved || isDirectory(host))
    {
        FS_TRACE("open %s mode %d: not found\n", filepath, static_cast<int>(openMode));
        return fail();
    }

    int flags = O_RDONLY;
    switch (openMode)
    {
        case kWriteOpen: flags = O_RDWR | O_CREAT; break;
        case kWriteAppend: flags = O_RDWR | O_CREAT | O_APPEND; break;
        case kWriteCreateNew: flags = O_RDWR | O_CREAT | O_TRUNC; break;
        default: break;
    }
    const int fd = ::open(host.c_str(), flags, 0644);
    FS_TRACE("open %s mode %d -> %s\n", filepath, static_cast<int>(openMode), fd < 0 ? std::strerror(errno) : "ok");
    if (fd < 0)
    {
        return fail();
    }
    if (openMode == kWriteAppend)
    {
        ::lseek(fd, 0, SEEK_END);
    }
    file->fd = fd;
    file->hostPath = host;
    return { true, file };
}

size_t DesktopFileSystem::readFile(void* destBuf, size_t bufLen, FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0)
    {
        return static_cast<size_t>(-1);
    }
    size_t total = 0;
    auto* out = static_cast<uint8_t*>(destBuf);
    while (total < bufLen)
    {
        const ssize_t n = ::read(file->fd, out + total, bufLen - total);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return static_cast<size_t>(-1);
        }
        if (n == 0)
        {
            break;
        }
        total += static_cast<size_t>(n);
    }
    return total;
}

size_t DesktopFileSystem::writeFile(const void* data, size_t dataSize, FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0)
    {
        return static_cast<size_t>(-1);
    }
    size_t total = 0;
    const auto* in = static_cast<const uint8_t*>(data);
    while (total < dataSize)
    {
        const ssize_t n = ::write(file->fd, in + total, dataSize - total);
        if (n < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return static_cast<size_t>(-1);
        }
        total += static_cast<size_t>(n);
    }
    return total;
}

// FatFs clamps seeks past the end in read mode and grows the file in write mode.
bool DesktopFileSystem::seekFile(uint32_t pos, FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0)
    {
        return false;
    }
    const size_t size = getFileSize(fileHandle);
    const int accessMode = ::fcntl(file->fd, F_GETFL) & O_ACCMODE;
    if (pos > size)
    {
        if (accessMode == O_RDONLY)
        {
            pos = static_cast<uint32_t>(size);
        }
        else if (::ftruncate(file->fd, pos) != 0)
        {
            return false;
        }
    }
    return ::lseek(file->fd, pos, SEEK_SET) >= 0;
}

bool DesktopFileSystem::seekFileCur(int offset, FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0)
    {
        return false;
    }
    if (offset == 0)
    {
        return true;
    }
    const off_t position = ::lseek(file->fd, 0, SEEK_CUR);
    // FatFs takes the sum as an unsigned file offset.
    return seekFile(static_cast<uint32_t>(position + offset), fileHandle);
}

void DesktopFileSystem::closeFile(FileHandle fileHandle, bool deleteOnClose)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr)
    {
        return;
    }
    if (file->fd >= 0)
    {
        ::close(file->fd);
        file->fd = -1;
    }
    if (deleteOnClose)
    {
        delete file;
    }
}

void DesktopFileSystem::endFile(FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0)
    {
        return;
    }
    const off_t position = ::lseek(file->fd, 0, SEEK_CUR);
    if (position >= 0)
    {
        [[maybe_unused]] const int result = ::ftruncate(file->fd, position);
    }
}

bool DesktopFileSystem::isFileOpen(FileHandle fileHandle)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    return file != nullptr && file->fd >= 0;
}

//------------------------------------------------------------------------------------
// Raw sector access for sample streaming

bool DesktopFileSystem::enableFastSeek(FileHandle fileHandle,
                                       uint32_t* clmtBuffer,
                                       uint32_t clmtElementCount,
                                       uint32_t* outDatabaseSector,
                                       uint32_t* outSectorsPerCluster)
{
    auto* file = static_cast<OpenFile*>(fileHandle);
    if (file == nullptr || file->fd < 0 || clmtBuffer == nullptr || clmtElementCount < 4)
    {
        return false;
    }
    constexpr uint64_t kClusterBytes = uint64_t(kSectorsPerCluster) * kSectorSize;
    const uint64_t size = getFileSize(fileHandle);
    const uint32_t clusters = static_cast<uint32_t>(std::max<uint64_t>(1, (size + kClusterBytes - 1) / kClusterBytes));

    auto it = std::find_if(extents.begin(), extents.end(), [&](const Extent& e) {
        return e.hostPath == file->hostPath && e.clusterCount >= clusters;
    });
    if (it == extents.end())
    {
        extents.push_back({ file->hostPath, nextCluster, clusters });
        nextCluster += clusters;
        it = extents.end() - 1;
    }

    // Same layout FatFs writes: used size, then (cluster count, first cluster) runs, then 0.
    clmtBuffer[0] = 4;
    clmtBuffer[1] = it->clusterCount;
    clmtBuffer[2] = it->firstCluster;
    clmtBuffer[3] = 0;
    if (outDatabaseSector != nullptr)
    {
        *outDatabaseSector = kDatabaseSector;
    }
    if (outSectorsPerCluster != nullptr)
    {
        *outSectorsPerCluster = kSectorsPerCluster;
    }
    return true;
}

bool DesktopFileSystem::readSectorsAsync(uint8_t* buffer, uint32_t startSector, uint32_t sectorCount)
{
    asyncPending = true;
    asyncResult = false;
    if (startSector < kDatabaseSector)
    {
        return true;
    }
    const uint32_t cluster = (startSector - kDatabaseSector) / kSectorsPerCluster + 2;
    const auto it = std::find_if(extents.begin(), extents.end(), [&](const Extent& e) {
        return cluster >= e.firstCluster && cluster < e.firstCluster + e.clusterCount;
    });
    if (it == extents.end())
    {
        return true;
    }

    const uint64_t sectorInFile =
      uint64_t(cluster - it->firstCluster) * kSectorsPerCluster + (startSector - kDatabaseSector) % kSectorsPerCluster;
    const size_t bytes = size_t(sectorCount) * kSectorSize;
    std::memset(buffer, 0, bytes);

    const int fd = ::open(it->hostPath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        return true;
    }
    const ssize_t n = ::pread(fd, buffer, bytes, static_cast<off_t>(sectorInFile * kSectorSize));
    ::close(fd);
    asyncResult = n >= 0;
    return true;
}

bool DesktopFileSystem::pollAsyncRead(bool* outSuccess)
{
    if (outSuccess != nullptr)
    {
        *outSuccess = asyncResult;
    }
    asyncPending = false;
    return true;
}

bool DesktopFileSystem::waitAsyncRead()
{
    asyncPending = false;
    return asyncResult;
}

//------------------------------------------------------------------------------------
// Listings (ports of SdDir in platforms/imx/sdmmc/sd/SD.cpp)

bool DesktopFileSystem::filterFileEntry(const Entry& entry, enListFileFilter filter, const char* dirPath) const
{
    if (entry.hidden)
    {
        return false;
    }
    if (entry.isDir && filter != enListFileFilter_folder && filter != enListFileFilter_all)
    {
        return false;
    }

    const std::string& name = entry.name;
    switch (filter)
    {
        case enListFileFilter_all: return true;

        case enListFileFilter_folder: return entry.isDir && !isMacMetaFolder(name);

        case enListFileFilter_wave_inst:
        case enListFileFilter_wave:
        {
            if (name.size() < 5)
            {
                return false;
            }
            if (!hasExtension(name, "wav") && !(filter == enListFileFilter_wave_inst && hasExtension(name, "pti")))
            {
                return false;
            }
            if (std::strlen(dirPath) + name.size() > 253)
            {
                return false;
            }
            return !isMacMetaFile(name);
        }

        case enListFileFilter_firmware:
        {
            constexpr char kFirmwareFileNameStart[] = "TrackerMini";
            const size_t prefix = std::strlen(kFirmwareFileNameStart);
            if (name.size() < prefix + 4 || name.compare(name.size() - 4, 4, ".ptf") != 0 ||
                name.compare(0, prefix, kFirmwareFileNameStart) != 0)
            {
                return false;
            }
            return !isMacMetaFile(name);
        }

        case enListFileFilter_mod:
        {
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            return lower.find(".mod") != std::string::npos || lower.find(".it") != std::string::npos;
        }

        case enListFileFilter_game:
        {
            if (isMacMetaFile(name))
            {
                return false;
            }
            std::string lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
            return lower.find(".nes") != std::string::npos;
        }
    }
    return false;
}

uint16_t DesktopFileSystem::countFilesInSubDirectory(const char* dirPath, const char* dirName, enListFileFilter filter)
{
    const std::string path = std::string(dirPath) + "/" + dirName;
    std::string host;
    std::vector<Entry> entries;
    if (!resolve(path.c_str(), host) || !readDirectory(host, entries))
    {
        return 0;
    }
    uint16_t count = 0;
    for (const Entry& entry : entries)
    {
        // SdDir filters with the parent directory's path; keep that.
        if (filterFileEntry(entry, filter, dirPath))
        {
            count++;
        }
    }
    return count;
}

int DesktopFileSystem::createFilesList(const char* path,
                                       uint8_t start_line,
                                       char** list,
                                       uint8_t list_length,
                                       uint16_t max_used_memory,
                                       enListFileFilter filter,
                                       uint16_t skip_count,
                                       uint16_t max_files_in_subdir,
                                       enListFileFilter folderSizeFilter)
{
    std::string host;
    std::vector<Entry> entries;
    if (!resolve(path, host) || !readListing(host, entries))
    {
        FS_TRACE("list %s: cannot open\n", path);
        return kListError;
    }

    for (uint8_t i = 0; i < list_length; i++)
    {
        if (list[i] != nullptr)
        {
            delete[] list[i];
            list[i] = nullptr;
        }
    }

    uint8_t n = start_line;
    if (max_used_memory < 255)
    {
        return n;
    }

    uint16_t memory_used = 0;
    for (const Entry& entry : entries)
    {
        if (n >= list_length)
        {
            break;
        }
        if (memory_used > max_used_memory - 255)
        {
            break;
        }
        if (!filterFileEntry(entry, filter, path))
        {
            continue;
        }
        if (skip_count > 0)
        {
            skip_count--;
            continue;
        }

        uint16_t len = static_cast<uint16_t>(entry.name.size() + (entry.isDir ? 2 : 1));
        list[n] = new char[len];
        memory_used += len;

        if (entry.isDir)
        {
            std::strcpy(list[n], "/");
            std::strcat(list[n], entry.name.c_str());
            int16_t files_count = static_cast<int16_t>(countFilesInSubDirectory(path, entry.name.c_str(), folderSizeFilter));

            if (max_files_in_subdir != 0 && files_count > max_files_in_subdir)
            {
                uint16_t offset = 0;
                len += 4 + 2 * 5;

                while (files_count > 0 && n < list_length - 1)
                {
                    if (list[n] != nullptr)
                    {
                        delete[] list[n];
                        memory_used -= len;
                    }
                    list[n] = new char[len];
                    memory_used += len;
                    std::snprintf(list[n], len, "/%s (%d-%d)", entry.name.c_str(), offset + 1,
                                  offset + max_files_in_subdir);
                    files_count -= max_files_in_subdir;
                    n++;
                    offset += max_files_in_subdir;
                }
                n--;
            }
        }
        else
        {
            std::strcpy(list[n], entry.name.c_str());
        }
        n++;
    }
    return n;
}

int DesktopFileSystem::createInstrumentsList(const char* path, char** list, uint8_t list_length, uint32_t* sizeOut)
{
    std::string host;
    std::vector<Entry> entries;
    if (!resolve(path, host) || !readDirectory(host, entries))
    {
        return kListError;
    }

    uint8_t n = 0;
    for (const Entry& entry : entries)
    {
        if (n >= list_length)
        {
            break;
        }
        if (entry.hidden || entry.isDir || entry.name.size() < 5 || !hasExtension(entry.name, "pti"))
        {
            continue;
        }

        // "<1..48> <name>.pti"
        const char* p = entry.name.c_str();
        int idx1 = -1, idx2 = -1;
        if (*p >= '0' && *p <= '9')
        {
            idx1 = *p - '0';
        }
        else
        {
            continue;
        }
        p++;
        if (*p >= '0' && *p <= '9')
        {
            idx2 = *p - '0';
            p++;
        }
        if (*p != ' ')
        {
            continue;
        }
        p++;
        if (idx2 >= 0)
        {
            idx1 = idx1 * 10 + idx2;
        }
        if (idx1 < 1 || idx1 > 48)
        {
            continue;
        }
        idx1--;
        if (list[idx1] != nullptr)
        {
            continue;
        }

        const int len = static_cast<int>(entry.name.size()) - static_cast<int>(p - entry.name.c_str()) - 4;
        list[idx1] = new char[len + 1];
        std::strncpy(list[idx1], p, len);
        list[idx1][len] = 0;

        *sizeOut += static_cast<uint32_t>(entry.size);
        n++;
    }
    return n;
}

int DesktopFileSystem::countContainedFiles(const char* path)
{
    std::string host;
    std::vector<Entry> entries;
    if (!resolve(path, host) || !readListing(host, entries))
    {
        return 0;
    }
    uint8_t n = 0;
    for (const Entry& entry : entries)
    {
        if (entry.hidden)
        {
            continue;
        }
        if (!entry.isDir)
        {
            if (entry.name.size() < 5 || !hasExtension(entry.name, "wav"))
            {
                continue;
            }
            if (std::strlen(path) + entry.name.size() > 253)
            {
                continue;
            }
        }
        n++;
    }
    return n;
}

void DesktopFileSystem::listDirContents(const char* dirPath,
                                        DirContentType contentType,
                                        std::function<bool(const char* name)> nameFilter,
                                        char* namesBuffer,
                                        int namesBufferSize,
                                        int maxNameLen,
                                        int& count,
                                        char** fileList)
{
    std::string host;
    std::vector<Entry> entries;
    if (!resolve(dirPath, host) || !readDirectory(host, entries))
    {
        count = 0;
        return;
    }

    int listPos = 0;
    for (const Entry& entry : entries)
    {
        if (listPos >= count)
        {
            break;
        }
        if (entry.hidden)
        {
            continue;
        }

        int nameLen;
        if (contentType == kFolders)
        {
            if (!entry.isDir || isMacMetaFolder(entry.name))
            {
                continue;
            }
            if (nameFilter != nullptr && !nameFilter(entry.name.c_str()))
            {
                continue;
            }
            nameLen = static_cast<int>(entry.name.size()) + 2;
        }
        else if (contentType == kFiles)
        {
            if (entry.isDir || isMacMetaFile(entry.name))
            {
                continue;
            }
            if (nameFilter != nullptr && !nameFilter(entry.name.c_str()))
            {
                continue;
            }
            nameLen = static_cast<int>(entry.name.size()) + 1;
        }
        else
        {
            continue;
        }

        if (nameLen > namesBufferSize)
        {
            break;
        }
        if (nameLen > maxNameLen)
        {
            continue;
        }

        fileList[listPos] = namesBuffer;
        if (entry.isDir)
        {
            *namesBuffer = '/';
            namesBuffer++;
        }
        polystrncpy(namesBuffer, entry.name.c_str(), maxNameLen + 1);

        namesBuffer += nameLen;
        namesBufferSize -= nameLen;
        listPos++;
    }
    count = listPos;

    std::sort(fileList, fileList + count, [](const char* a, const char* b) { return compareStrings(a, b); });
}

int DesktopFileSystem::readNbFiles(const char* romPath)
{
    std::string host;
    std::vector<Entry> entries;
    if (!resolve(romPath, host) || !readDirectory(host, entries))
    {
        return 0;
    }
    return static_cast<int>(entries.size());
}

bool DesktopFileSystem::nesDir_open(const char* path)
{
    nesDir_close();
    std::string host;
    if (!resolve(path, host) || !readDirectory(host, nesEntries))
    {
        return false;
    }
    nesOpen = true;
    return true;
}

bool DesktopFileSystem::nesDir_readItem(char* path, uint8_t* isDir, uint8_t* isHid, uint32_t* size)
{
    if (!nesOpen || nesIndex >= nesEntries.size())
    {
        return false;
    }
    const Entry& entry = nesEntries[nesIndex++];
    *isDir = entry.isDir ? 1 : 0;
    *isHid = entry.hidden ? 1 : 0;
    if (size != nullptr)
    {
        *size = static_cast<uint32_t>(entry.size);
    }
    std::strcpy(path, entry.name.c_str());
    return true;
}

void DesktopFileSystem::nesDir_close()
{
    nesEntries.clear();
    nesIndex = 0;
    nesOpen = false;
}
