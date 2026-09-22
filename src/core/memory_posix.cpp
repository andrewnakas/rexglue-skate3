/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/utils.h>
#include <rex/platform.h>
#include <rex/string.h>

REXCVAR_DECLARE(bool, guest_backing_file);

#if REX_PLATFORM_ANDROID
#include <string.h>

#include <dlfcn.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>

#include <linux/ashmem.h>

#include <rex/main_android.h>
#endif

#if REX_PLATFORM_MAC
#define ftruncate64 ftruncate
#define mmap64 mmap
#endif

namespace rex {
namespace memory {

namespace {

#if REX_PLATFORM_MAC
void AlignHostPageRange(void*& base_address, size_t& length) {
  const uintptr_t page_mask = uintptr_t(page_size() - 1);
  const uintptr_t start = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = start + length;
  const uintptr_t aligned_start = start & ~page_mask;
  const uintptr_t aligned_end = (end + page_mask) & ~page_mask;
  base_address = reinterpret_cast<void*>(aligned_start);
  length = static_cast<size_t>(aligned_end - aligned_start);
}
#endif

}  // namespace

// Convert filesystem path to valid shm_open name (must start with /, no other slashes)
static std::string MakeShmName(const std::filesystem::path& path) {
  std::string name = path.string();
  for (char& c : name) {
    if (c == '/')
      c = '_';
  }
  if (name.empty() || name[0] != '/') {
    name.insert(name.begin(), '/');
  }
  return name;
}

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* libandroid_;
// API 26+.
static int (*android_ASharedMemory_create_)(const char* name, size_t size);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    libandroid_ = dlopen("libandroid.so", RTLD_NOW);
    assert_not_null(libandroid_);
    if (libandroid_) {
      android_ASharedMemory_create_ = reinterpret_cast<decltype(android_ASharedMemory_create_)>(
          dlsym(libandroid_, "ASharedMemory_create"));
      assert_not_null(android_ASharedMemory_create_);
    }
  }
}

void AndroidShutdown() {
  android_ASharedMemory_create_ = nullptr;
  if (libandroid_) {
    dlclose(libandroid_);
    libandroid_ = nullptr;
  }
}
#endif

size_t page_size() {
  return getpagesize();
}
size_t allocation_granularity() {
  return page_size();
}

uint32_t ToPosixProtectFlags(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return PROT_NONE;
    case PageAccess::kReadOnly:
      return PROT_READ;
    case PageAccess::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case PageAccess::kExecuteReadOnly:
      // Statically recompiled guest code executes from the host image, never
      // from guest pages, so execute requests are served without PROT_EXEC.
      // This keeps the process W^X: no path through this layer can create
      // executable memory.
      return PROT_READ;
    case PageAccess::kExecuteReadWrite:
      return PROT_READ | PROT_WRITE;
    default:
      assert_unhandled_case(access);
      return PROT_NONE;
  }
}

// TODO(tomc): this needs to go somewhere else. we should utilize the platform namespace more.
#if REX_PLATFORM_LINUX
namespace {

struct LinuxMapEntry {
  uintptr_t start = 0;
  uintptr_t end = 0;
  char perms[5] = {};
};

// Parse a line from /proc/self/maps into a LinuxMapEntry
static bool ParseProcMapsLine(const std::string& line, LinuxMapEntry& out) {
  out = LinuxMapEntry{};
  unsigned long long start = 0, end = 0;
  char perms[5] = {};
  const int matched = std::sscanf(line.c_str(), "%llx-%llx %4s", &start, &end, perms);
  if (matched < 3)
    return false;
  out.start = static_cast<uintptr_t>(start);
  out.end = static_cast<uintptr_t>(end);
  std::memcpy(out.perms, perms, sizeof(out.perms));
  return out.start < out.end;
}

// Find the mapping entry in /proc/self/maps that contains the given address
static bool FindEntryForAddress(void* address, LinuxMapEntry& out_entry) {
  const uintptr_t addr = reinterpret_cast<uintptr_t>(address);
  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (addr >= e.start && addr < e.end) {
      out_entry = e;
      return true;
    }
  }
  return false;
}

// Check if [base, base+length) is fully covered by existing mappings (no gaps)
static bool IsRangeFullyMapped(void* base_address, size_t length) {
  if (!base_address || length == 0)
    return false;

  const uintptr_t begin = reinterpret_cast<uintptr_t>(base_address);
  const uintptr_t end = begin + length;
  if (end < begin) {  // overflow check
    return false;
  }

  std::ifstream maps("/proc/self/maps");
  if (!maps.is_open())
    return false;

  uintptr_t cursor = begin;
  std::string line;
  while (std::getline(maps, line)) {
    LinuxMapEntry e;
    if (!ParseProcMapsLine(line, e))
      continue;
    if (e.end <= cursor)
      continue;
    if (e.start > cursor)
      return false;  // gap found
    cursor = e.end;
    if (cursor >= end)
      return true;
  }
  return cursor >= end;
}

// Convert /proc/self/maps permission chars to PageAccess
static PageAccess PermsToPageAccess(const char perms[5]) {
  const bool r = perms[0] == 'r';
  const bool w = perms[1] == 'w';
  const bool x = perms[2] == 'x';

  if (!r && !w && !x)
    return PageAccess::kNoAccess;
  if (x)
    return w ? PageAccess::kExecuteReadWrite : PageAccess::kExecuteReadOnly;
  return w ? PageAccess::kReadWrite : PageAccess::kReadOnly;
}

}  // namespace
#endif  // REX_PLATFORM_LINUX

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  // Emulates Windows VirtualAlloc behavior:
  // - Reserve: create PROT_NONE mapping to hold address space
  // - Commit on existing reservation: mprotect to enable access (EEXIST path)
  // - New allocation: mmap with MAP_FIXED_NOREPLACE (never silently replace)
  const uint32_t prot_requested = ToPosixProtectFlags(access);

  // Determine initial protection based on allocation type
  int prot_initial = 0;
  switch (allocation_type) {
    case AllocationType::kReserve:
      prot_initial = PROT_NONE;
      break;
    case AllocationType::kCommit:
    case AllocationType::kReserveCommit:
    default:
      prot_initial = static_cast<int>(prot_requested);
      break;
  }

#if REX_PLATFORM_MAC
  if (base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    void* protect_base = base_address;
    size_t protect_length = length;
    AlignHostPageRange(protect_base, protect_length);
    if (mprotect(protect_base, protect_length, static_cast<int>(prot_requested)) == 0) {
      return base_address;
    }
  }
#endif

  // Build flags - always use MAP_FIXED_NOREPLACE for fixed addresses
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_FIXED_NOREPLACE)
  if (base_address) {
    flags |= MAP_FIXED_NOREPLACE;
  }
#else
  if (base_address) {
    flags |= MAP_FIXED;
  }
#endif

  void* result = mmap(base_address, length, prot_initial, flags, -1, 0);
  if (result != MAP_FAILED) {
    return result;
  }
#if defined(MAP_FIXED_NOREPLACE) && REX_PLATFORM_LINUX
  // Handle EEXIST: address already has a mapping (e.g., from prior Reserve)
  // This is the "commit on existing reservation" path
  if (errno == EEXIST && base_address &&
      (allocation_type == AllocationType::kCommit ||
       allocation_type == AllocationType::kReserveCommit)) {
    // Verify the entire range is mapped before using mprotect
    if (IsRangeFullyMapped(base_address, length)) {
      if (mprotect(base_address, length, static_cast<int>(prot_requested)) == 0) {
        return base_address;
      }
    }
  }
#endif

  return nullptr;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  switch (deallocation_type) {
    case DeallocationType::kDecommit: {
      // Decommit: remove access first, then release physical pages
#if REX_PLATFORM_MAC
      AlignHostPageRange(base_address, length);
#endif
      if (mprotect(base_address, length, PROT_NONE) != 0) {
        return false;
      }
#if defined(MADV_DONTNEED)
      (void)madvise(base_address, length, MADV_DONTNEED);
#endif
      return true;
    }
    case DeallocationType::kRelease: {
      return munmap(base_address, length) == 0;
    }
    default:
      // how we get here? :(
      assert_always();
      return false;
  }
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  if (out_old_access) {
    *out_old_access = PageAccess::kNoAccess;
  }

#if REX_PLATFORM_LINUX
  // NOTE(tomc): we may want to look at doing this differently. it should work for now
  //             but there is a TOCTOU window between reading and changing.
  //             This really shouldn't be an issue since VirtualProtect on Windows isn't truly
  //             atomic in a mutli-threaded process either, but it's something to be aware of.
  // Query old access before changing, if the caller needs it
  if (out_old_access) {
    LinuxMapEntry e;
    if (FindEntryForAddress(base_address, e)) {
      *out_old_access = PermsToPageAccess(e.perms);
    }
  }
#endif

  uint32_t prot = ToPosixProtectFlags(access);
#if REX_PLATFORM_MAC
  AlignHostPageRange(base_address, length);
#endif
  return mprotect(base_address, length, prot) == 0;
}

bool QueryProtect(void* base_address, size_t& length, PageAccess& access_out) {
#if !REX_PLATFORM_LINUX
  access_out = PageAccess::kNoAccess;
  length = 0;
  return false;
#else
  access_out = PageAccess::kNoAccess;
  length = 0;

  LinuxMapEntry e;
  if (!FindEntryForAddress(base_address, e)) {
    return false;
  }

  const uintptr_t addr = reinterpret_cast<uintptr_t>(base_address);
  length = static_cast<size_t>(e.end - addr);
  access_out = PermsToPageAccess(e.perms);

  return true;
#endif
}

#if REX_PLATFORM_IOS || REX_PLATFORM_ANDROID
// Opens the sparse file that backs the whole guest address space.
//
// The file is unlinked the moment it exists: the descriptor keeps it alive for
// as long as the process needs it, and a crash therefore leaves no 4.5 GB
// corpse behind for the player to find. ftruncate leaves it sparse on APFS and
// on ext4/f2fs alike, so the reservation costs no disk until the guest actually
// touches a page.
//
// Returns -1 on any failure; every caller has a fallback.
static int OpenGuestBackingFile(const std::filesystem::path& directory,
                                const std::filesystem::path& name, size_t length) {
  if (directory.empty()) {
    return -1;
  }
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  const std::filesystem::path backing_path = directory / name;
  int fd = open(backing_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    return -1;
  }
  unlink(backing_path.c_str());
  if (ftruncate(fd, static_cast<off_t>(length)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}
#endif

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& path, size_t length,
                                          PageAccess access, bool commit) {
#if REX_PLATFORM_ANDROID
  // Prefer a real file over ASharedMemory, because ashmem is the reason this
  // app is the first thing Android reclaims.
  //
  // Ashmem pages are RAM and nothing else: they cannot be written back and they
  // cannot be dropped, so a guest heap of a few hundred megabytes to three
  // gigabytes counts against the process in full, permanently, and makes it the
  // fattest target on the device. A MAP_SHARED file mapping is different in
  // kind - the kernel can write dirty pages out and drop clean ones under
  // pressure - so the same guest memory stops looking like unreclaimable RAM.
  //
  // Internal storage deliberately, not getExternalFilesDir(): the external path
  // is FUSE-backed, and MAP_SHARED on FUSE is exactly the case to avoid. Both
  // live on /data, so this costs nothing in space budget.
  //
  // Everything here is best-effort. A device that cannot spare the space still
  // boots, on ashmem, exactly as it did before.
  const std::filesystem::path& backing_dir = GetFileMappingDirectory();
  if (REXCVAR_GET(guest_backing_file) && !backing_dir.empty()) {
    bool have_room = true;
    struct statvfs vfs = {};
    if (statvfs(backing_dir.c_str(), &vfs) == 0) {
      const uint64_t available = uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
      // The guest never dirties the whole 4.5 GB - the reservation is sparse -
      // but refuse to start down this road without room for a realistic
      // working set plus headroom for the rest of the system.
      constexpr uint64_t kRequiredFreeBytes = 2048ull * 1024 * 1024;
      have_room = available >= kRequiredFreeBytes;
      if (!have_room) {
        REXLOG_WARN(
            "guest backing file: only {} MB free at {}, staying on shared memory",
            available / (1024 * 1024), backing_dir.string());
      }
    }
    if (have_room) {
      int fd = OpenGuestBackingFile(backing_dir, path.filename(), length);
      if (fd >= 0) {
        REXLOG_INFO("guest backing file: {} MB reserved under {}", length / (1024 * 1024),
                    backing_dir.string());
        return static_cast<FileMappingHandle>(fd);
      }
      REXLOG_WARN("guest backing file could not be created under {}; falling back to shared memory",
                  backing_dir.string());
    }
  }

  // TODO(Triang3l): Check if memfd can be used instead on API 30+.
  if (android_ASharedMemory_create_) {
    int sharedmem_fd = android_ASharedMemory_create_(path.c_str(), length);
    return sharedmem_fd >= 0 ? static_cast<FileMappingHandle>(sharedmem_fd)
                             : kFileMappingHandleInvalid;
  }

  // Use /dev/ashmem on API versions below 26, which added ASharedMemory.
  // /dev/ashmem was disabled on API 29 for apps targeting it.
  // https://chromium.googlesource.com/chromium/src/+/master/third_party/ashmem/ashmem-dev.c
  int ashmem_fd = open("/" ASHMEM_NAME_DEF, O_RDWR);
  if (ashmem_fd < 0) {
    return kFileMappingHandleInvalid;
  }
  char ashmem_name[ASHMEM_NAME_LEN];
  strlcpy(ashmem_name, path.c_str(), rex::countof(ashmem_name));
  if (ioctl(ashmem_fd, ASHMEM_SET_NAME, ashmem_name) < 0 ||
      ioctl(ashmem_fd, ASHMEM_SET_SIZE, length) < 0) {
    close(ashmem_fd);
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ashmem_fd);
#elif REX_PLATFORM_IOS
  // The iOS sandbox denies shm_open outright, which is why this is a plain
  // file. The guest views alias it through MAP_SHARED exactly as they would a
  // shm object; see OpenGuestBackingFile for the sparse/unlink handling.
  //
  // TMPDIR remains the fallback for any path that reaches here before the app
  // has said where it may write.
  (void)commit;
  std::filesystem::path backing_dir = GetFileMappingDirectory();
  if (backing_dir.empty()) {
    const char* tmp_dir = std::getenv("TMPDIR");
    backing_dir = std::filesystem::path(tmp_dir ? tmp_dir : "/tmp");
  }
  int fd = OpenGuestBackingFile(backing_dir, path.filename(), length);
  if (fd < 0) {
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(fd);
#else
  int oflag;
  switch (access) {
    case PageAccess::kNoAccess:
      oflag = 0;
      break;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      oflag = O_RDONLY;
      break;
    case PageAccess::kReadWrite:
    case PageAccess::kExecuteReadWrite:
      oflag = O_RDWR;
      break;
    default:
      assert_always();
      return kFileMappingHandleInvalid;
  }
  oflag |= O_CREAT;
  auto full_path = MakeShmName(path);
  int ret = shm_open(full_path.c_str(), oflag, 0777);
  if (ret < 0) {
    return kFileMappingHandleInvalid;
  }
  if (ftruncate64(ret, static_cast<off_t>(length)) != 0) {
    close(ret);
    shm_unlink(full_path.c_str());
    return kFileMappingHandleInvalid;
  }
  return static_cast<FileMappingHandle>(ret);
#endif
}

void CloseFileMappingHandle(FileMappingHandle handle, const std::filesystem::path& path) {
  close(static_cast<int>(handle));
  // Android's shared-memory fd and the iOS backing file (already unlinked at
  // creation) both need nothing beyond the close.
#if !REX_PLATFORM_ANDROID && !REX_PLATFORM_IOS
  auto full_path = MakeShmName(path);
  shm_unlink(full_path.c_str());
#endif
}

void* MapFileView(FileMappingHandle handle, void* base_address, size_t length, PageAccess access,
                  size_t file_offset) {
  // file_offset must be page-aligned
  const size_t page = page_size();
  if (file_offset % page != 0) {
    return nullptr;
  }

  int flags = MAP_SHARED;

  // For file views, we need MAP_FIXED to replace existing reservations.
  // The emulator reserves address space first, then maps file views into it.
  // MAP_FIXED_NOREPLACE would fail with EEXIST in this case.
  if (base_address) {
    flags |= MAP_FIXED;
  }

  uint32_t prot = ToPosixProtectFlags(access);
  void* result = mmap64(base_address, length, prot, flags, static_cast<int>(handle),
                        static_cast<off_t>(file_offset));
  if (result == MAP_FAILED) {
    return nullptr;
  }

  // Verify we got the address we asked for
  if (base_address && result != base_address) {
    munmap(result, length);
    return nullptr;
  }

  return result;
}

bool UnmapFileView(FileMappingHandle handle, void* base_address, size_t length) {
  return munmap(base_address, length) == 0;
}

}  // namespace memory
}  // namespace rex
