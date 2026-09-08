#!/usr/bin/env bash
# Install the Horizon backend for glslang's OS layer.
#
# glslang is a submodule, so these files cannot be committed inside it. They
# live here and are copied in, the same arrangement the FFmpeg configuration
# uses. Idempotent; re-run after re-provisioning that submodule.
#
#     bash thirdparty/glslang-switch/apply.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
dest="$here/../glslang/glslang/OSDependent/Switch"

if [ ! -d "$here/../glslang/glslang/OSDependent" ]; then
  echo "glslang is not provisioned at $here/../glslang" >&2
  exit 1
fi

mkdir -p "$dest"
cp "$here/ossource.cpp" "$dest/ossource.cpp"
cp "$here/CMakeLists.txt" "$dest/CMakeLists.txt"
echo "installed the Horizon OS backend into $dest"

# And the one-line dispatch that selects it.
disp="$here/../glslang/glslang/CMakeLists.txt"
if grep -q "OSDependent/Switch" "$disp"; then
  echo "dispatch already patched"
  exit 0
fi
python3 - "$disp" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1]); s = p.read_text()
old = """if(WIN32)
    add_subdirectory(OSDependent/Windows)
elseif(UNIX)"""
new = """if(WIN32)
    add_subdirectory(OSDependent/Windows)
elseif(REX_SWITCH OR CMAKE_CXX_FLAGS MATCHES "__SWITCH__")
    # Horizon: CMake does not call it UNIX, and the Unix backend is written
    # against pthreads and thread cancellation, which libnx does not have.
    add_subdirectory(OSDependent/Switch)
elseif(UNIX)"""
assert old in s, "glslang dispatch not in the expected shape"
p.write_text(s.replace(old, new, 1))
print("patched the platform dispatch")
PY
