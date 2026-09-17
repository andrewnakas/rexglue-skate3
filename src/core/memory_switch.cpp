/**
 * @file        core/memory_switch.cpp
 * @brief       Guest address space on Horizon.
 *
 * The guest wants what every other platform gives it with one mmap of a shared
 * object and nine overlapping views: a 4 GB virtual space in which several
 * ranges are the *same* physical memory seen at different addresses. Writing at
 * 0xA0000000 has to be visible at 0xC0000000, 0xE0000000 and 0x100000000,
 * because that is how the Xbox 360 mapped its physical pages and the recompiled
 * code takes it for granted at nearly a million load and store sites.
 *
 * Horizon has no mmap and no shared memory a process can alias to itself, but
 * it does have a pair of syscalls that amount to the same thing:
 *
 *   svcMapProcessCodeMemory  takes ordinary heap and republishes it elsewhere
 *                            in the address space as code memory.
 *   svcMapProcessMemory      maps that code memory at another address - and can
 *                            do so more than once, which is the whole trick.
 *
 * So a commit here is: allocate real memory from the heap, publish it once as
 * code memory (the "alias"), then map that alias into every guest view whose
 * file range covers it. Reads and writes through any of those addresses reach
 * the same pages.
 *
 * Two things are deliberately unlike the POSIX backend:
 *
 *   Nothing is ever unmapped. A decommit clears the accounting and leaves the
 *   pages mapped, because the native renderer reads guest memory from another
 *   thread with no lock and a real unmap would race it into a fault that this
 *   platform cannot recover from. The memory is not returned to the system
 *   until the process exits, which it does by _Exit anyway.
 *
 *   Protect does nothing. Horizon cannot make a mapping read-only after the
 *   fact from userspace, and even if it could there is no way to resume a
 *   thread after a fault, so the write-watch mechanism the emulated renderer
 *   uses to notice CPU writes cannot work. See the shared-memory frame-start
 *   invalidation that stands in for it.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <map>
#include <mutex>
#include <vector>

#include <rex/memory/utils.h>

namespace rex::memory {

namespace {

// Horizon pages are 4 KB and the mapping syscalls take 4 KB-aligned addresses
// and sizes. Reporting 4 KB as the allocation granularity too is what keeps the
// guest's 0xE0000000 view - whose backing sits at a 4 KB offset - exact; a
// coarser granularity would round that offset away and the view would alias the
// wrong pages.
constexpr size_t kPageSize = 0x1000;

// Physical memory is taken in chunks of at least this much, aligned to it. Two
// megabytes is a compromise between two costs that pull in opposite directions:
// every chunk needs one kernel mapping per guest view that covers it, and the
// kernel's table of memory blocks is not unbounded; but a chunk is also the
// granularity at which memory is taken from the pool and never given back.
constexpr size_t kChunkAlign = 2ull * 1024 * 1024;

struct View {
  uint8_t* host = nullptr;
  size_t length = 0;
  uint64_t file_offset = 0;
};

// One heap allocation, published once as code memory and then mapped into every
// view that covers it.
struct Chunk {
  uint64_t file_offset = 0;
  size_t length = 0;
  void* backing = nullptr;
  uint8_t* alias = nullptr;
  VirtmemReservation* alias_reservation = nullptr;
  // One bit per 4 KB page. A page that has never been committed reads as zero,
  // which is what the guest heaps assume after a Reset, so the bitmap decides
  // when a range has to be cleared again.
  std::vector<uint64_t> committed;
};

struct Window {
  std::mutex lock;
  uint8_t* base = nullptr;
  size_t length = 0;
  VirtmemReservation* reservation = nullptr;
  std::vector<View> views;
  std::map<uint64_t, Chunk> chunks;  // keyed by file offset
  size_t mapping_count = 0;
  size_t committed_bytes = 0;
  bool aliasing_failed = false;
};

Window& window() {
  static Window w;
  return w;
}

// Where this process's own code lives, filled in by SwitchInitialize. Guest
// memory is published into the same region of the address space, so every
// mapping is checked against it.
u64 g_image_start = 0;
u64 g_image_end = 0;

constexpr size_t AlignDown(size_t v, size_t a) { return v & ~(a - 1); }
constexpr size_t AlignUp(size_t v, size_t a) { return AlignDown(v + a - 1, a); }

u32 ToHorizonPermission(PageAccess access) {
  switch (access) {
    case PageAccess::kNoAccess:
      return Perm_None;
    case PageAccess::kReadOnly:
    case PageAccess::kExecuteReadOnly:
      return Perm_R;
    default:
      return Perm_Rw;
  }
}

// Guest host address -> offset into the flat backing space the views describe.
// Returns false for an address outside every view, which is a caller bug.
bool HostToFileOffset(const void* address, uint64_t* out_offset) {
  const auto* p = static_cast<const uint8_t*>(address);
  for (const View& v : window().views) {
    if (p >= v.host && p < v.host + v.length) {
      *out_offset = v.file_offset + uint64_t(p - v.host);
      return true;
    }
  }
  return false;
}

// Publishes one chunk's pages at every guest address that should see them.
// Returns the number of mappings made, or 0 if the first one failed.
size_t MapChunkIntoViews(Chunk& chunk) {
  size_t made = 0;
  for (const View& v : window().views) {
    const uint64_t start = std::max(chunk.file_offset, v.file_offset);
    const uint64_t end =
        std::min(chunk.file_offset + chunk.length, v.file_offset + v.length);
    if (start >= end) {
      continue;
    }
    void* dst = v.host + (start - v.file_offset);
    const u64 src = reinterpret_cast<u64>(chunk.alias) + (start - chunk.file_offset);
    const Result rc = svcMapProcessMemory(dst, envGetOwnProcessHandle(), src, end - start);
    if (R_FAILED(rc)) {
      std::fprintf(stderr,
                   "[mem] svcMapProcessMemory failed (rc=0x%x) mapping file offset 0x%llx "
                   "at %p, %llu KB\n",
                   rc, (unsigned long long)start, dst, (unsigned long long)((end - start) >> 10));
      return made;
    }
    ++made;
  }
  return made;
}

// Creates and maps a chunk covering exactly [file_offset, file_offset+length).
// Both are already chunk-aligned and known not to overlap an existing chunk.
bool CreateChunk(uint64_t file_offset, size_t length) {
  Window& w = window();

  Chunk chunk;
  chunk.file_offset = file_offset;
  chunk.length = length;

  // memalign, not malloc: the alias has to start on a page boundary because
  // svcMapProcessCodeMemory rejects anything else. newlib has memalign and not
  // aligned_alloc, and free() releases either.
  chunk.backing = memalign(kPageSize, length);
  if (!chunk.backing) {
    std::fprintf(stderr, "[mem] out of memory reserving %llu KB of guest backing\n",
                 (unsigned long long)(length >> 10));
    return false;
  }
  // Guest pages must read as zero the first time they are touched. The heap
  // this came from may hold anything.
  std::memset(chunk.backing, 0, length);

  virtmemLock();
  chunk.alias = static_cast<uint8_t*>(virtmemFindCodeMemory(length, kPageSize));
  if (chunk.alias) {
    chunk.alias_reservation = virtmemAddReservation(chunk.alias, length);
  }
  virtmemUnlock();
  if (!chunk.alias || !chunk.alias_reservation) {
    std::fprintf(stderr, "[mem] no room in the code region for a %llu KB alias\n",
                 (unsigned long long)(length >> 10));
    std::free(chunk.backing);
    return false;
  }

  // Guest memory is published into the code region, which is where the loader
  // also placed this image. libnx's allocator is supposed to skip what is
  // already mapped, but the consequence of it not doing so is that the game
  // overwrites its own code and then dies somewhere unrelated - so check,
  // rather than trust and debug it later.
  const u64 alias_start = reinterpret_cast<u64>(chunk.alias);
  const u64 alias_end = alias_start + length;
  if (g_image_end && alias_start < g_image_end && alias_end > g_image_start) {
    std::fprintf(stderr,
                 "[mem] REFUSING an alias at %#llx..%#llx: it overlaps this image "
                 "(%#llx..%#llx). Mapping there would overwrite the running code.\n",
                 (unsigned long long)alias_start, (unsigned long long)alias_end,
                 (unsigned long long)g_image_start, (unsigned long long)g_image_end);
    virtmemLock();
    virtmemRemoveReservation(chunk.alias_reservation);
    virtmemUnlock();
    std::free(chunk.backing);
    return false;
  }

  Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(),
                                      reinterpret_cast<u64>(chunk.alias),
                                      reinterpret_cast<u64>(chunk.backing), length);
  if (R_FAILED(rc)) {
    std::fprintf(stderr, "[mem] svcMapProcessCodeMemory failed (rc=0x%x) for %llu KB\n", rc,
                 (unsigned long long)(length >> 10));
    virtmemLock();
    virtmemRemoveReservation(chunk.alias_reservation);
    virtmemUnlock();
    std::free(chunk.backing);
    return false;
  }
  // Code memory arrives read-execute; the guest writes to all of it.
  rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                     reinterpret_cast<u64>(chunk.alias), length, Perm_Rw);
  if (R_FAILED(rc)) {
    std::fprintf(stderr, "[mem] svcSetProcessMemoryPermission failed (rc=0x%x)\n", rc);
    svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<u64>(chunk.alias),
                              reinterpret_cast<u64>(chunk.backing), length);
    virtmemLock();
    virtmemRemoveReservation(chunk.alias_reservation);
    virtmemUnlock();
    std::free(chunk.backing);
    return false;
  }

  // svcMapProcessCodeMemory borrows the source pages: the kernel drops the
  // original mapping's permissions while the alias exists. The source here is
  // 512 MB of the C heap, so report what state it is left in - anything that
  // later allocates inside a range the kernel has made inaccessible would fault
  // far away from here.
  {
    MemoryInfo bi = {};
    u32 bpi = 0;
    if (R_SUCCEEDED(svcQueryMemory(&bi, &bpi, reinterpret_cast<u64>(chunk.backing)))) {
      std::fprintf(stderr,
                   "[mem] backing %#llx +%#llx now type=%u perm=%u attr=%u%s\n",
                   (unsigned long long)bi.addr, (unsigned long long)bi.size,
                   (unsigned)bi.type, (unsigned)bi.perm, (unsigned)bi.attr,
                   bi.perm == Perm_None ? "  <-- source is now INACCESSIBLE" : "");
    }
  }

  chunk.committed.assign((length / kPageSize + 63) / 64, 0);

  const size_t mapped = MapChunkIntoViews(chunk);
  if (mapped == 0) {
    w.aliasing_failed = true;
    svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<u64>(chunk.alias),
                              reinterpret_cast<u64>(chunk.backing), length);
    virtmemLock();
    virtmemRemoveReservation(chunk.alias_reservation);
    virtmemUnlock();
    std::free(chunk.backing);
    return false;
  }

  w.mapping_count += mapped;
  w.committed_bytes += length;

  // The first commit is the one that matters: it is the first proof on real
  // hardware that heap can be published as code memory and then mapped at
  // several guest addresses at once, which is what the whole design rests on.
  // After that, a line every 256 MB is enough to follow the shape of a boot
  // without burying the log.
  static bool first_commit_reported = false;
  static size_t last_reported_mb = 0;
  const size_t committed_mb = w.committed_bytes >> 20;
  if (!first_commit_reported) {
    first_commit_reported = true;
    std::fprintf(stderr,
                 "[mem] first guest commit ok: %zu KB at file offset 0x%llx, "
                 "alias %#llx, mapped into %zu view(s)\n",
                 length >> 10, (unsigned long long)file_offset,
                 (unsigned long long)alias_start, mapped);
    SwitchVerifyOwnCode("after first commit");
  } else if (committed_mb >= last_reported_mb + 256) {
    last_reported_mb = committed_mb;
    std::fprintf(stderr, "[mem] committed %zu MB across %zu chunks, %zu mappings\n",
                 committed_mb, w.chunks.size() + 1, w.mapping_count);
    SwitchVerifyOwnCode("after 256 MB");
  }

  w.chunks.emplace(file_offset, std::move(chunk));
  return true;
}

// Ensures physical memory exists behind every page of [offset, offset+length),
// creating chunks over whatever parts are not covered yet.
bool EnsureCommitted(uint64_t offset, size_t length) {
  Window& w = window();
  const uint64_t begin = AlignDown(offset, kChunkAlign);
  const uint64_t end = AlignUp(offset + length, kChunkAlign);

  uint64_t cursor = begin;
  while (cursor < end) {
    // The last chunk starting at or before the cursor is the only one that can
    // contain it.
    auto it = w.chunks.upper_bound(cursor);
    if (it != w.chunks.begin()) {
      auto prev = std::prev(it);
      const uint64_t prev_end = prev->second.file_offset + prev->second.length;
      if (prev_end > cursor) {
        cursor = prev_end;  // already covered
        continue;
      }
    }
    // Uncovered from the cursor up to the next chunk, or to the end.
    uint64_t gap_end = end;
    if (it != w.chunks.end() && it->second.file_offset < gap_end) {
      gap_end = it->second.file_offset;
    }
    if (!CreateChunk(cursor, size_t(gap_end - cursor))) {
      return false;
    }
    cursor = gap_end;
  }
  return true;
}

// Marks pages committed, zeroing any that were decommitted since last time.
void MarkCommitted(uint64_t offset, size_t length) {
  Window& w = window();
  const uint64_t begin = AlignDown(offset, kPageSize);
  const uint64_t end = AlignUp(offset + length, kPageSize);

  for (auto& [key, chunk] : w.chunks) {
    const uint64_t start = std::max(begin, chunk.file_offset);
    const uint64_t stop = std::min(end, chunk.file_offset + chunk.length);
    if (start >= stop) {
      continue;
    }
    for (uint64_t a = start; a < stop; a += kPageSize) {
      const size_t page = size_t((a - chunk.file_offset) / kPageSize);
      uint64_t& word = chunk.committed[page / 64];
      const uint64_t bit = 1ull << (page % 64);
      if (!(word & bit)) {
        word |= bit;
      }
    }
  }
}

void MarkDecommitted(uint64_t offset, size_t length) {
  Window& w = window();
  const uint64_t begin = AlignDown(offset, kPageSize);
  const uint64_t end = AlignUp(offset + length, kPageSize);

  for (auto& [key, chunk] : w.chunks) {
    const uint64_t start = std::max(begin, chunk.file_offset);
    const uint64_t stop = std::min(end, chunk.file_offset + chunk.length);
    if (start >= stop) {
      continue;
    }
    // Clear the memory now rather than on the next commit: the guest heap can
    // hand the same pages back out at a different address in the same frame,
    // and it expects them empty.
    void* host = nullptr;
    for (const View& v : w.views) {
      if (start >= v.file_offset && start < v.file_offset + v.length) {
        host = v.host + (start - v.file_offset);
        break;
      }
    }
    if (host) {
      std::memset(host, 0, size_t(stop - start));
    }
    for (uint64_t a = start; a < stop; a += kPageSize) {
      const size_t page = size_t((a - chunk.file_offset) / kPageSize);
      chunk.committed[page / 64] &= ~(1ull << (page % 64));
    }
  }
}

}  // namespace

size_t page_size() { return kPageSize; }

size_t allocation_granularity() { return kPageSize; }

void SwitchInitialize() {
  // Ask the kernel where this image is. Guest memory is published as code
  // memory, which comes from the same region of the address space the loader
  // put the game in - so "did I just map over myself" is a question with a
  // definite answer, and it is worth being able to ask it.
  MemoryInfo info = {};
  u32 pageinfo = 0;
  const u64 probe = reinterpret_cast<u64>(&SwitchInitialize);
  if (R_SUCCEEDED(svcQueryMemory(&info, &pageinfo, probe))) {
    g_image_start = info.addr;
    g_image_end = info.addr + info.size;
    std::fprintf(stderr, "[mem] own code occupies %#llx..%#llx\n",
                 (unsigned long long)g_image_start, (unsigned long long)g_image_end);
  }
}

void SwitchShutdown() {}

uint8_t* SwitchGuestWindowBase() { return window().base; }

size_t SwitchGuestCommittedBytes() { return window().committed_bytes; }

size_t SwitchGuestMappingCount() { return window().mapping_count; }

void SwitchVerifyOwnCode(const char* when) {
  if (!g_image_start) {
    return;
  }
  MemoryInfo info = {};
  u32 pageinfo = 0;
  // The far end of the image is what the crashes call into, so check there
  // rather than at the start: a mapping that has been broken part-way through
  // still looks intact from the front.
  const u64 probe = g_image_end - 0x2000;
  if (R_FAILED(svcQueryMemory(&info, &pageinfo, probe))) {
    std::fprintf(stderr, "[mem] %s: cannot query our own code at %#llx\n", when,
                 (unsigned long long)probe);
    return;
  }
  // The pool figure is not the interesting one: __nx_heap_size = 0 hands the
  // whole pool to the heap at startup, so the kernel calls almost all of it
  // "used" before a single allocation happens. What matters is how much the
  // allocator still has to give out.
  const struct mallinfo mi = mallinfo();
  std::fprintf(stderr, "[mem] %s: heap %zu MB in use, %zu MB free, arena %zu MB\n", when,
               (size_t)mi.uordblks >> 20, (size_t)mi.fordblks >> 20, (size_t)mi.arena >> 20);

  const bool executable = info.perm == Perm_Rx;
  const bool covers = probe >= info.addr && probe < info.addr + info.size;
  std::fprintf(stderr, "[mem] %s: own code %#llx +%#llx perm=%u%s\n", when,
               (unsigned long long)info.addr, (unsigned long long)info.size,
               (unsigned)info.perm,
               (executable && covers) ? " (still executable)" : "  <-- NO LONGER EXECUTABLE");
}

FileMappingHandle CreateFileMappingHandle(const std::filesystem::path& /*path*/, size_t length,
                                          PageAccess /*access*/, bool /*commit*/) {
  Window& w = window();
  std::lock_guard<std::mutex> guard(w.lock);
  if (w.base) {
    // The guest only ever asks for one of these.
    return FileMappingHandle(1);
  }

  const size_t reserve = AlignUp(length, kChunkAlign);
  virtmemLock();
  w.base = static_cast<uint8_t*>(virtmemFindAslr(reserve, kChunkAlign));
  if (w.base) {
    w.reservation = virtmemAddReservation(w.base, reserve);
  }
  virtmemUnlock();
  if (!w.base || !w.reservation) {
    std::fprintf(stderr, "[mem] could not reserve %llu MB of address space for the guest\n",
                 (unsigned long long)(reserve >> 20));
    w.base = nullptr;
    return kFileMappingHandleInvalid;
  }
  w.length = reserve;
  std::fprintf(stderr, "[mem] guest window reserved: %llu MB at %p\n",
               (unsigned long long)(reserve >> 20), w.base);
  return FileMappingHandle(1);
}

void CloseFileMappingHandle(FileMappingHandle /*handle*/, const std::filesystem::path& /*path*/) {
  Window& w = window();
  std::lock_guard<std::mutex> guard(w.lock);
  for (auto& [key, chunk] : w.chunks) {
    for (const View& v : w.views) {
      const uint64_t start = std::max(chunk.file_offset, v.file_offset);
      const uint64_t end = std::min(chunk.file_offset + chunk.length, v.file_offset + v.length);
      if (start < end) {
        svcUnmapProcessMemory(v.host + (start - v.file_offset), envGetOwnProcessHandle(),
                              reinterpret_cast<u64>(chunk.alias) + (start - chunk.file_offset),
                              end - start);
      }
    }
    svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), reinterpret_cast<u64>(chunk.alias),
                              reinterpret_cast<u64>(chunk.backing), chunk.length);
    virtmemLock();
    virtmemRemoveReservation(chunk.alias_reservation);
    virtmemUnlock();
    std::free(chunk.backing);
  }
  w.chunks.clear();
  w.views.clear();
  w.mapping_count = 0;
  w.committed_bytes = 0;
  if (w.reservation) {
    virtmemLock();
    virtmemRemoveReservation(w.reservation);
    virtmemUnlock();
    w.reservation = nullptr;
  }
  w.base = nullptr;
  w.length = 0;
}

void* MapFileView(FileMappingHandle /*handle*/, void* base_address, size_t length,
                  PageAccess /*access*/, size_t file_offset) {
  // Nothing is mapped here. A view is a promise about which addresses see which
  // part of the backing space; the mapping happens when memory is committed,
  // because on this platform a mapping and a physical page are the same act.
  Window& w = window();
  std::lock_guard<std::mutex> guard(w.lock);
  if (!base_address) {
    return nullptr;
  }
  w.views.push_back(View{static_cast<uint8_t*>(base_address), length, file_offset});
  return base_address;
}

bool UnmapFileView(FileMappingHandle /*handle*/, void* /*base_address*/, size_t /*length*/) {
  return true;
}

void* AllocFixed(void* base_address, size_t length, AllocationType allocation_type,
                 PageAccess access) {
  if (!base_address || !length) {
    return nullptr;
  }
  (void)access;

  Window& w = window();
  std::lock_guard<std::mutex> guard(w.lock);

  uint64_t offset = 0;
  if (!HostToFileOffset(base_address, &offset)) {
    std::fprintf(stderr, "[mem] AllocFixed at %p is outside the guest window\n", base_address);
    return nullptr;
  }

  // A reservation on its own costs nothing: the whole window is already
  // reserved, and no physical memory is owed until a commit.
  if (!(static_cast<uint32_t>(allocation_type) & static_cast<uint32_t>(AllocationType::kCommit))) {
    return base_address;
  }

  if (!EnsureCommitted(offset, length)) {
    return nullptr;
  }
  MarkCommitted(offset, length);
  return base_address;
}

bool DeallocFixed(void* base_address, size_t length, DeallocationType deallocation_type) {
  if (!base_address) {
    return false;
  }
  Window& w = window();
  std::lock_guard<std::mutex> guard(w.lock);

  uint64_t offset = 0;
  if (!HostToFileOffset(base_address, &offset)) {
    return false;
  }
  // A release names the whole region with a length of zero; there is nothing to
  // give back either way, since chunks live until the process exits.
  if (static_cast<uint32_t>(deallocation_type) &
      static_cast<uint32_t>(DeallocationType::kDecommit)) {
    MarkDecommitted(offset, length);
  }
  return true;
}

bool Protect(void* base_address, size_t length, PageAccess access, PageAccess* out_old_access) {
  (void)base_address;
  (void)length;
  (void)ToHorizonPermission(access);
  // Horizon offers no way to change the protection of an existing mapping from
  // userspace, and no way to resume a thread that faulted on one, so there is
  // nothing useful to do and nothing to report. The guest heaps keep their own
  // record of what they intended, which is what every query goes through.
  if (out_old_access) {
    *out_old_access = PageAccess::kReadWrite;
  }
  return true;
}

bool QueryProtect(void* /*base_address*/, size_t& /*length*/, PageAccess& /*access_out*/) {
  // Only reached from the fault handler on platforms that have one.
  return false;
}

}  // namespace rex::memory
