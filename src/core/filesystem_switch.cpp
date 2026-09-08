/**
 * @file        core/filesystem_switch.cpp
 * @brief       Filesystem access on Horizon.
 *
 * libnx mounts the SD card through a devoptab, so the ordinary stdio and dirent
 * calls work and most of this reads like the POSIX file. Three things differ
 * and all three have bitten this port:
 *
 *   Paths carry a device prefix ("sdmc:/switch/skate3"). std::filesystem does
 *   not know what that is: absolute(), canonical() and relative() all mangle
 *   it. Nothing here calls them, and callers elsewhere have to be checked.
 *
 *   There is no pread or pwrite. Reads seek first, which means two calls that
 *   have to stay together - hence the per-handle lock. The engine reads game
 *   data from several threads at once and without it they interleave into each
 *   other's file positions.
 *
 *   FatFs returns short reads. On a desktop a complete read of a regular file
 *   is one syscall and a short count means the end of the file; here it means
 *   nothing at all and must be retried. filesystem_read_loop is on by default
 *   for that reason and turning it off will corrupt game data.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

#include <switch.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/host_buffer_fault_hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(filesystem_read_loop, true, "Filesystem",
                    "Keep reading until the requested length is satisfied or the file ends. "
                    "FatFs on the SD card returns short reads routinely - unlike a desktop "
                    "filesystem, where a short count means end of file - so turning this off "
                    "silently truncates game data.");

namespace rex {

std::string path_to_utf8(const std::filesystem::path& path) {
  return path.string();
}

std::u16string path_to_utf16(const std::filesystem::path& path) {
  return rex::string::to_utf16(path.string());
}

std::filesystem::path to_path(const std::string_view source) {
  return std::filesystem::path(source);
}

std::filesystem::path to_path(const std::u16string_view source) {
  return std::filesystem::path(rex::string::to_utf8(source));
}

namespace filesystem {

namespace {

// The homebrew menu passes the NRO's own path as argv[0]; when it does not,
// this is where the installer puts it.
constexpr const char* kDefaultExecutablePath = "sdmc:/switch/skate3/skate3.nro";
constexpr const char* kAppRoot = "sdmc:/switch/skate3";

std::atomic<HostBufferFaultHook> g_host_buffer_fault_hook{nullptr};
std::atomic<uint64_t> g_io_warn_counter{0};

// First eight, then powers of two: enough to see a pattern, never a flood.
bool ShouldWarn(uint64_t* out_n) {
  const uint64_t n = g_io_warn_counter.fetch_add(1, std::memory_order_relaxed);
  *out_n = n + 1;
  return n < 8 || (n & (n - 1)) == 0;
}

uint64_t TimespecToTimestamp(const struct timespec& ts) {
  // Windows FILETIME epoch, as the rest of the engine expects.
  return uint64_t(ts.tv_sec) * 10000000ULL + uint64_t(ts.tv_nsec) / 100ULL + 116444736000000000ULL;
}

}  // namespace

void SetHostBufferFaultHook(HostBufferFaultHook hook) {
  // Kept for source compatibility with the desktop builds. It cannot fire here:
  // the hook exists to recover a read into a page a debugging watch had made
  // read-only, and Horizon has no page protection to make it read-only with.
  g_host_buffer_fault_hook.store(hook, std::memory_order_release);
}

std::filesystem::path GetExecutablePath() {
  // libnx exposes no argument count outside main(), so the path hbmenu
  // launched us from cannot be recovered here. It would not be useful anyway:
  // over nxlink the NRO runs from a temporary location, and everything that
  // matters resolves against the fixed root below rather than against this.
  return std::filesystem::path(kDefaultExecutablePath);
}

std::filesystem::path GetExecutableFolder() {
  return GetExecutablePath().parent_path();
}

std::filesystem::path GetAppRootFolder() {
  // Not the executable folder. hbmenu can load the NRO from anywhere, including
  // over the network with nxlink, where its path is a temporary one; the game
  // data, settings and logs always live in the same place.
  return std::filesystem::path(kAppRoot);
}

std::filesystem::path GetUserFolder() {
  return std::filesystem::path(kAppRoot);
}

FILE* OpenFile(const std::filesystem::path& path, const std::string_view mode) {
  return fopen(path.c_str(), std::string(mode).c_str());
}

bool Seek(FILE* file, int64_t offset, int origin) {
  return fseek(file, long(offset), origin) == 0;
}

int64_t Tell(FILE* file) {
  return int64_t(ftell(file));
}

bool TruncateStdioFile(FILE* file, uint64_t length) {
  if (fflush(file) != 0) {
    return false;
  }
  if (ftruncate(fileno(file), off_t(length)) != 0) {
    return false;
  }
  // Match the POSIX contract: the file pointer is clamped into the new size.
  if (int64_t(length) < Tell(file) && fseek(file, 0, SEEK_END) != 0) {
    return false;
  }
  return true;
}

bool CreateParentFolder(const std::filesystem::path& path) {
  std::error_code ec;
  const auto parent = path.parent_path();
  if (parent.empty()) {
    return true;
  }
  std::filesystem::create_directories(parent, ec);
  return !ec || std::filesystem::exists(parent);
}

bool CreateEmptyFile(const std::filesystem::path& path) {
  FILE* file = fopen(path.c_str(), "wb");
  if (!file) {
    return false;
  }
  fclose(file);
  return true;
}

namespace {

class SwitchFileHandle : public FileHandle {
 public:
  SwitchFileHandle(std::filesystem::path path, int handle)
      : FileHandle(std::move(path)), handle_(handle) {}

  ~SwitchFileHandle() override {
    if (handle_ >= 0) {
      close(handle_);
      handle_ = -1;
    }
  }

  bool Read(size_t file_offset, void* buffer, size_t buffer_length,
            size_t* out_bytes_read) override {
    const bool loop = REXCVAR_GET(filesystem_read_loop);
    std::lock_guard<std::mutex> guard(lock_);

    // Seek and read are two calls without pread to fuse them, so the lock is
    // what keeps another thread from moving the cursor in between.
    if (lseek(handle_, off_t(file_offset), SEEK_SET) < 0) {
      *out_bytes_read = 0;
      return false;
    }

    size_t done = 0;
    uint32_t retries = 0;
    uint32_t calls = 0;
    while (done < buffer_length) {
      const ssize_t got = read(handle_, static_cast<uint8_t*>(buffer) + done, buffer_length - done);
      ++calls;
      if (got < 0) {
        const int err = errno;
        if (loop && (err == EINTR || err == EAGAIN) && ++retries <= 64) {
          continue;
        }
        uint64_t n = 0;
        if (ShouldWarn(&n)) {
          REXFS_WARN(
              "host read FAILED: '{}' offset {} asked {} got {} errno {} ({}) - the guest is "
              "told END_OF_FILE and its buffer keeps whatever it held (occurrence {})",
              path_.string(), file_offset, buffer_length, done, err, strerror(err), n);
        }
        *out_bytes_read = done;
        return false;
      }
      if (got == 0) {
        break;  // end of file
      }
      done += size_t(got);
      if (!loop) {
        break;
      }
    }

    if (calls > 1 && done == buffer_length) {
      // Expected on this filesystem rather than alarming, so it is logged at
      // the same throttled rate and says so.
      uint64_t n = 0;
      if (ShouldWarn(&n)) {
        REXFS_WARN("host read needed {} reads (short reads recovered by the loop, normal on "
                   "FatFs): '{}' offset {} asked {} (occurrence {})",
                   calls, path_.string(), file_offset, buffer_length, n);
      }
    } else if (!loop && done < buffer_length) {
      uint64_t n = 0;
      if (ShouldWarn(&n)) {
        REXFS_WARN("host read SHORT with the loop disabled: '{}' offset {} asked {} got {} "
                   "(occurrence {})",
                   path_.string(), file_offset, buffer_length, done, n);
      }
    }
    *out_bytes_read = done;
    return true;
  }

  bool Write(size_t file_offset, const void* buffer, size_t buffer_length,
             size_t* out_bytes_written) override {
    const bool loop = REXCVAR_GET(filesystem_read_loop);
    std::lock_guard<std::mutex> guard(lock_);

    if (lseek(handle_, off_t(file_offset), SEEK_SET) < 0) {
      *out_bytes_written = 0;
      return false;
    }

    size_t done = 0;
    uint32_t retries = 0;
    while (done < buffer_length) {
      const ssize_t put =
          write(handle_, static_cast<const uint8_t*>(buffer) + done, buffer_length - done);
      if (put < 0) {
        const int err = errno;
        if (loop && (err == EINTR || err == EAGAIN) && ++retries <= 64) {
          continue;
        }
        uint64_t n = 0;
        if (ShouldWarn(&n)) {
          REXFS_WARN("host write FAILED: '{}' offset {} asked {} wrote {} errno {} ({}) "
                     "(occurrence {})",
                     path_.string(), file_offset, buffer_length, done, err, strerror(err), n);
        }
        *out_bytes_written = done;
        return false;
      }
      if (put == 0) {
        break;
      }
      done += size_t(put);
      if (!loop) {
        break;
      }
    }
    *out_bytes_written = done;
    return true;
  }

  bool SetLength(size_t length) override {
    std::lock_guard<std::mutex> guard(lock_);
    return ftruncate(handle_, off_t(length)) >= 0;
  }

  void Flush() override {
    std::lock_guard<std::mutex> guard(lock_);
    fsync(handle_);
  }

 private:
  int handle_ = -1;
  std::mutex lock_;
};

}  // namespace

std::unique_ptr<FileHandle> FileHandle::OpenExisting(const std::filesystem::path& path,
                                                     uint32_t desired_access) {
  int open_access = 0;
  if (desired_access & FileAccess::kGenericRead) {
    open_access |= O_RDONLY;
  }
  if (desired_access & FileAccess::kGenericWrite) {
    open_access |= O_WRONLY;
  }
  if (desired_access & FileAccess::kGenericExecute) {
    open_access |= O_RDONLY;
  }
  if (desired_access & FileAccess::kGenericAll) {
    open_access |= O_RDWR;
  }
  if (desired_access & FileAccess::kFileReadData) {
    open_access |= O_RDONLY;
  }
  if (desired_access & FileAccess::kFileWriteData) {
    open_access |= O_WRONLY;
  }
  if (desired_access & FileAccess::kFileAppendData) {
    open_access |= O_APPEND;
  }
  // O_RDONLY is zero, so a request for both arrives here as O_WRONLY and would
  // lose the read side.
  if ((open_access & O_WRONLY) && (desired_access & (FileAccess::kGenericRead |
                                                     FileAccess::kFileReadData))) {
    open_access = (open_access & ~O_WRONLY) | O_RDWR;
  }

  const int handle = open(path.c_str(), open_access);
  if (handle < 0) {
    return nullptr;
  }
  return std::make_unique<SwitchFileHandle>(path, handle);
}

bool GetInfo(const std::filesystem::path& path, FileInfo* out_info) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) {
    return false;
  }
  out_info->path = path.parent_path();
  out_info->name = path.filename();
  if (S_ISDIR(st.st_mode)) {
    out_info->type = FileInfo::Type::kDirectory;
    out_info->total_size = 0;
  } else {
    out_info->type = FileInfo::Type::kFile;
    out_info->total_size = size_t(st.st_size);
  }
  out_info->create_timestamp = TimespecToTimestamp(st.st_ctim);
  out_info->access_timestamp = TimespecToTimestamp(st.st_atim);
  out_info->write_timestamp = TimespecToTimestamp(st.st_mtim);
  return true;
}

std::vector<FileInfo> ListFiles(const std::filesystem::path& path) {
  std::vector<FileInfo> result;

  DIR* dir = opendir(path.c_str());
  if (!dir) {
    return result;
  }
  while (auto ent = readdir(dir)) {
    if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
      continue;
    }

    FileInfo info;
    info.name = ent->d_name;
    info.path = path;

    // The devoptab fills d_type, but not for every mount; stat is the fallback
    // and also the only source of the size and the timestamps.
    const auto full = path / ent->d_name;
    struct stat st;
    if (stat(full.c_str(), &st) != 0) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      info.type = FileInfo::Type::kDirectory;
      info.total_size = 0;
    } else {
      info.type = FileInfo::Type::kFile;
      info.total_size = size_t(st.st_size);
    }
    info.create_timestamp = TimespecToTimestamp(st.st_ctim);
    info.access_timestamp = TimespecToTimestamp(st.st_atim);
    info.write_timestamp = TimespecToTimestamp(st.st_mtim);
    result.push_back(std::move(info));
  }
  closedir(dir);
  return result;
}

}  // namespace filesystem
}  // namespace rex
