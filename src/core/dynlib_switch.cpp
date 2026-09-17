/**
 * Dynamic library loading on Horizon: there is none.
 *
 * An NRO is a single statically linked image with no dynamic linker behind it.
 * Everything the desktop builds dlopen at runtime - the Vulkan loader,
 * RenderDoc, SPIRV-Tools - is either linked in (NVK, whose one entry point is
 * referenced directly) or simply absent. Load always fails, and every caller
 * already handles that, because it is also what happens on a desktop machine
 * that does not have the library installed.
 */

#include <rex/platform.h>
#include <rex/platform/dynlib.h>

static_assert(REX_PLATFORM_SWITCH, "This file is Horizon-only");

namespace rex::platform {

DynamicLibrary::~DynamicLibrary() {
  Close();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept : handle_(other.handle_) {
  other.handle_ = nullptr;
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept {
  if (this != &other) {
    Close();
    handle_ = other.handle_;
    other.handle_ = nullptr;
  }
  return *this;
}

bool DynamicLibrary::Load(const std::filesystem::path& /*path*/) {
  Close();
  return false;
}

void DynamicLibrary::Close() {
  handle_ = nullptr;
}

void* DynamicLibrary::GetRawSymbol(const char* /*name*/) const {
  return nullptr;
}

}  // namespace rex::platform
