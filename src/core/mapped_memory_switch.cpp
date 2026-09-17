/**
 * @file        core/mapped_memory_switch.cpp
 * @brief       File mapping on Horizon, which has none.
 *
 * There is no mmap here, so a "mapping" is a buffer that was read once and is
 * written back on close if the caller asked for write access. That is a fair
 * trade for what actually uses this at runtime: the XEX loader reads the game
 * executable (about twenty megabytes), and the STFS probe reads four bytes.
 *
 * It is not a fair trade for a disc image, which is several gigabytes and which
 * the desktop builds map lazily. This port does not mount one - the game is
 * installed as an extracted folder - so a request that large is refused with an
 * explanation rather than an allocation failure somewhere further down.
 *
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

#include <rex/logging.h>
#include <rex/memory/mapped_memory.h>

namespace rex::memory {

namespace {

// Comfortably above the largest XEX and far below a disc image, so the message
// below can tell the two cases apart.
constexpr size_t kMaxMappedBytes = 256ull * 1024 * 1024;

class SwitchMappedMemory : public MappedMemory {
 public:
  SwitchMappedMemory(std::vector<uint8_t> buffer, std::filesystem::path path, size_t offset,
                     Mode mode)
      : buffer_(std::move(buffer)), path_(std::move(path)), offset_(offset), mode_(mode) {
    data_ = buffer_.empty() ? nullptr : buffer_.data();
    size_ = buffer_.size();
  }

  ~SwitchMappedMemory() override { Close(); }

  void Close(uint64_t truncate_size = 0) override {
    if (closed_) {
      return;
    }
    closed_ = true;
    if (mode_ == Mode::kReadWrite && !buffer_.empty()) {
      Flush();
      if (truncate_size) {
        // No ftruncate on a devoptab file; reopening for write and stopping
        // short has the same effect for the one caller that asks.
        if (std::FILE* f = std::fopen(path_.c_str(), "r+b")) {
          std::fclose(f);
        }
      }
    }
    buffer_.clear();
    buffer_.shrink_to_fit();
    data_ = nullptr;
    size_ = 0;
  }

  void Flush() override {
    if (mode_ != Mode::kReadWrite || buffer_.empty()) {
      return;
    }
    std::FILE* f = std::fopen(path_.c_str(), "r+b");
    if (!f) {
      return;
    }
    std::fseek(f, long(offset_), SEEK_SET);
    std::fwrite(buffer_.data(), 1, buffer_.size(), f);
    std::fclose(f);
  }

  bool Remap(size_t offset, size_t length) override {
    auto remapped = Read(path_, offset, length);
    if (!remapped) {
      return false;
    }
    Flush();
    buffer_ = std::move(*remapped);
    offset_ = offset;
    data_ = buffer_.empty() ? nullptr : buffer_.data();
    size_ = buffer_.size();
    return true;
  }

  static std::optional<std::vector<uint8_t>> Read(const std::filesystem::path& path, size_t offset,
                                                  size_t length) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      return std::nullopt;
    }
    std::fseek(f, 0, SEEK_END);
    const long file_size = std::ftell(f);
    if (file_size < 0) {
      std::fclose(f);
      return std::nullopt;
    }
    if (offset > size_t(file_size)) {
      std::fclose(f);
      return std::nullopt;
    }
    size_t want = length ? length : size_t(file_size) - offset;
    want = std::min(want, size_t(file_size) - offset);

    if (want > kMaxMappedBytes) {
      std::fclose(f);
      REXLOG_ERROR(
          "Cannot map {} MB of '{}': Horizon has no file mapping, so a mapped file is read into "
          "memory in full. This size means a disc image, which this build does not mount - "
          "install the game as an extracted folder instead.",
          want / (1024 * 1024), path.string());
      return std::nullopt;
    }

    std::vector<uint8_t> buffer;
    buffer.resize(want);
    std::fseek(f, long(offset), SEEK_SET);
    // fatfs returns short reads; loop until the buffer is full or the file ends.
    size_t got = 0;
    while (got < want) {
      const size_t n = std::fread(buffer.data() + got, 1, want - got, f);
      if (n == 0) {
        break;
      }
      got += n;
    }
    std::fclose(f);
    buffer.resize(got);
    return buffer;
  }

 private:
  std::vector<uint8_t> buffer_;
  std::filesystem::path path_;
  size_t offset_ = 0;
  Mode mode_ = Mode::kRead;
  bool closed_ = false;
};

}  // namespace

std::unique_ptr<MappedMemory> MappedMemory::Open(const std::filesystem::path& path, Mode mode,
                                                 size_t offset, size_t length) {
  auto buffer = SwitchMappedMemory::Read(path, offset, length);
  if (!buffer) {
    return nullptr;
  }
  return std::make_unique<SwitchMappedMemory>(std::move(*buffer), path, offset, mode);
}

std::unique_ptr<ChunkedMappedMemoryWriter> ChunkedMappedMemoryWriter::Open(
    const std::filesystem::path& /*path*/, size_t /*chunk_size*/, bool /*low_address_space*/) {
  // Only the GPU trace writer asks for this, and tracing is a desktop tool.
  return nullptr;
}

}  // namespace rex::memory
