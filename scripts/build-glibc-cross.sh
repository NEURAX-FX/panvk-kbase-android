#!/bin/sh
# build-glibc-cross.sh — cross-build a debug AArch64 glibc PanVK ICD, or with
# --with-egl-zink the Wayland EGL/GLES2 Zink runtime that drives it.
# Usage: scripts/build-glibc-cross.sh --mesa PATH --sysroot PATH \
#          --host-tools PATH --build-dir PATH --out PATH [--jobs 6] \
#          [--with-egl-zink]
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
MESA="${MESA:-$ROOT/work/mesa}"
SYSROOT="${SYSROOT:-$ROOT/sysroot-aarch64}"
HOST_TOOLS="${HOST_TOOLS:-}"
BUILD_DIR="${BDIR:-$ROOT/build/glibc-aarch64-cross}"
OUT_DIR="${DDIR:-$ROOT/dist/glibc-aarch64-cross}"
JOBS=6
WITH_EGL_ZINK=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --mesa) MESA="$2"; shift 2 ;;
    --sysroot) SYSROOT="$2"; shift 2 ;;
    --host-tools) HOST_TOOLS="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --out) OUT_DIR="$2"; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --with-egl-zink) WITH_EGL_ZINK=1; shift ;;
    --help|-h)
      sed -n '2,5p' "$0"
      exit 0
      ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

case "$JOBS" in
  ''|*[!0-9]*|0) echo "--jobs must be a positive integer" >&2; exit 2 ;;
esac

[ -f "$MESA/meson.build" ] || { echo "Mesa source tree missing: $MESA" >&2; exit 1; }
[ -d "$SYSROOT/usr/include" ] || { echo "AArch64 sysroot missing: $SYSROOT" >&2; exit 1; }
MESA="$(CDPATH= cd -- "$MESA" && pwd)"
SYSROOT="$(CDPATH= cd -- "$SYSROOT" && pwd)"
mkdir -p "$BUILD_DIR" "$OUT_DIR"
BUILD_DIR="$(CDPATH= cd -- "$BUILD_DIR" && pwd)"
OUT_DIR="$(CDPATH= cd -- "$OUT_DIR" && pwd)"

CC="${CC:-aarch64-linux-gnu-gcc}"
CXX="${CXX:-aarch64-linux-gnu-g++}"
AR="${AR:-aarch64-linux-gnu-ar}"
STRIP="${STRIP:-aarch64-linux-gnu-strip}"
PKG_CONFIG="${PKG_CONFIG:-pkg-config}"
WAYLAND_SCANNER="${WAYLAND_SCANNER:-$(command -v wayland-scanner || true)}"
[ -n "$WAYLAND_SCANNER" ] || { echo "host wayland-scanner not found" >&2; exit 1; }

for tool in "$CC" "$CXX" "$AR" "$STRIP" "$PKG_CONFIG" meson ninja python3 file readelf; do
  command -v "$tool" >/dev/null 2>&1 || { echo "required host tool not found: $tool" >&2; exit 1; }
done

if [ -z "$HOST_TOOLS" ]; then
  mesa_name="$(basename -- "$MESA")"
  HOST_TOOLS_BUILD="$ROOT/build/host-tools${mesa_name#mesa}"
  if [ ! -x "$HOST_TOOLS_BUILD/bin/mesa_clc" ] ||
     [ ! -x "$HOST_TOOLS_BUILD/bin/vtn_bindgen2" ] ||
     [ ! -x "$HOST_TOOLS_BUILD/bin/panfrost_compile" ]; then
    "$ROOT/scripts/bootstrap-host-tools.sh" --mesa "$MESA" --out "$HOST_TOOLS_BUILD"
  fi
  HOST_TOOLS="$HOST_TOOLS_BUILD/bin"
fi
for tool in mesa_clc vtn_bindgen2 panfrost_compile; do
  [ -x "$HOST_TOOLS/$tool" ] || {
    echo "host Mesa codegen tool missing: $HOST_TOOLS/$tool" >&2
    exit 1
  }
done

# Restrict all package discovery to target ARM64 .pc files.  Treat the sysroot
# top-level include/lib directories as system paths so pkg-config does not emit
# -I/usr/include (which breaks GCC/glibc include_next handling) or host paths.
export PATH="$HOST_TOOLS:$PATH"
export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/usr/lib64/pkgconfig:$SYSROOT/usr/share/pkgconfig"
export PKG_CONFIG_SYSTEM_INCLUDE_PATH="$SYSROOT/usr/include"
export PKG_CONFIG_SYSTEM_LIBRARY_PATH="$SYSROOT/usr/lib64"
unset PKG_CONFIG_PATH
"$PKG_CONFIG" --exists libdrm wayland-client wayland-server wayland-protocols libudev \
  xcb xcb-randr x11-xcb xcb-dri3 xcb-present xcb-shm xcb-sync xshmfence xcb-xfixes || {
  echo "target ARM64 sysroot is missing libdrm/Wayland/libudev/X11 pkg-config dependencies" >&2
  exit 1
}
# The GLES2/X11 platform needs the Xrandr/Xext/Xrender development metadata that
# the Vulkan-only ICD build does not link against.
if [ "$WITH_EGL_ZINK" = 1 ]; then
  "$PKG_CONFIG" --exists xrandr xext xrender || {
    echo "target ARM64 sysroot is missing xrandr/xext/xrender pkg-config dependencies" >&2
    exit 1
  }
fi
for header in wayland-client.h libudev.h; do
  [ -f "$SYSROOT/usr/include/$header" ] || {
    echo "target ARM64 header missing: $SYSROOT/usr/include/$header" >&2
    exit 1
  }
done

CXX_VERSION="$("$CXX" -dumpversion)"
CXX_MAJOR="${CXX_VERSION%%.*}"
CXX_INCLUDE="$SYSROOT/usr/include/c++/$CXX_MAJOR"
[ -f "$CXX_INCLUDE/thread" ] || {
  echo "target libstdc++ headers missing; install/extract libstdc++-devel.aarch64 into $SYSROOT" >&2
  exit 1
}
CXX_ARCH_INCLUDE=""
for dir in "$CXX_INCLUDE"/*; do
  if [ -f "$dir/bits/c++config.h" ]; then
    CXX_ARCH_INCLUDE="$dir"
    break
  fi
done
[ -n "$CXX_ARCH_INCLUDE" ] || {
  echo "target libstdc++ architecture headers missing under $CXX_INCLUDE" >&2
  exit 1
}
TARGET_LIBSTDCPP="$SYSROOT/usr/lib64/libstdc++.so.6"
[ -f "$TARGET_LIBSTDCPP" ] || {
  echo "target libstdc++ runtime missing: $TARGET_LIBSTDCPP" >&2
  exit 1
}
CXX_LINK_DIR="${CXX_LINK_DIR:-$BUILD_DIR/target-cxx-link}"
mkdir -p "$CXX_LINK_DIR"
CXX_LINK_DIR="$(CDPATH= cd -- "$CXX_LINK_DIR" && pwd)"
if [ -e "$CXX_LINK_DIR/libstdc++.so" ] || [ -L "$CXX_LINK_DIR/libstdc++.so" ]; then
  [ "$(readlink -f "$CXX_LINK_DIR/libstdc++.so")" = "$(readlink -f "$TARGET_LIBSTDCPP")" ] || {
    echo "refusing to replace unrelated file: $CXX_LINK_DIR/libstdc++.so" >&2
    exit 1
  }
else
  ln -s "$TARGET_LIBSTDCPP" "$CXX_LINK_DIR/libstdc++.so"
fi

# Fail early on a broken sysroot before starting Mesa's large build.
printf '#include <limits.h>\n#ifndef PATH_MAX\n#error target PATH_MAX missing\n#endif\n' |
  "$CC" --sysroot="$SYSROOT" -isystem "$SYSROOT/usr/include" -x c -fsyntax-only - || {
    echo "target C/glibc header smoke check failed" >&2
    exit 1
  }
printf '#include <thread>\nint main() { std::thread t([]{}); t.join(); }\n' |
  "$CXX" --sysroot="$SYSROOT" -isystem "$CXX_INCLUDE" \
    -isystem "$CXX_ARCH_INCLUDE" -isystem "$CXX_INCLUDE/backward" \
    -isystem "$SYSROOT/usr/include" -L "$CXX_LINK_DIR" \
    -std=c++17 -pthread -x c++ - -o "$BUILD_DIR/.cross-cxx-smoke"
rm -f "$BUILD_DIR/.cross-cxx-smoke"

CROSS_FILE="$BUILD_DIR/aarch64-glibc-cross.ini"
CROSS_TMP="$CROSS_FILE.tmp"
cat > "$CROSS_TMP" <<EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkg-config = '$PKG_CONFIG'
wayland-scanner = '$WAYLAND_SCANNER'

[host_machine]
system = 'linux'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'

[properties]
sys_root = '$SYSROOT'
needs_exe_wrapper = true
pkg_config_libdir = ['$SYSROOT/usr/lib64/pkgconfig', '$SYSROOT/usr/share/pkgconfig']

[built-in options]
c_args = ['--sysroot=$SYSROOT', '-isystem$SYSROOT/usr/include']
c_link_args = ['--sysroot=$SYSROOT']
cpp_args = ['--sysroot=$SYSROOT', '-isystem$CXX_INCLUDE', '-isystem$CXX_ARCH_INCLUDE', '-isystem$CXX_INCLUDE/backward', '-isystem$SYSROOT/usr/include']
cpp_link_args = ['--sysroot=$SYSROOT', '-L$CXX_LINK_DIR']
EOF
if [ -f "$BUILD_DIR/build.ninja" ] && [ -f "$CROSS_FILE" ] &&
   ! cmp -s "$CROSS_TMP" "$CROSS_FILE"; then
  rm -f "$CROSS_TMP"
  echo "cross settings changed for existing build; use a new --build-dir" >&2
  exit 1
fi
mv -f "$CROSS_TMP" "$CROSS_FILE"

RECONF=""
[ ! -f "$BUILD_DIR/build.ninja" ] || RECONF=--reconfigure
# Vulkan-only builds skip Gallium entirely; the EGL profile builds Zink (the GL
# implementation) plus softpipe (the software DRI frontend EGL falls back to
# when kbase exposes no DRM render node).
if [ "$WITH_EGL_ZINK" = 1 ]; then
  GL_OPTIONS="-Dgallium-drivers=zink,softpipe -Degl=enabled -Dgles1=disabled \
-Dgles2=enabled -Dopengl=true -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled"
else
  GL_OPTIONS="-Dgallium-drivers= -Degl=disabled -Dgles1=disabled -Dgles2=disabled \
-Dopengl=false -Dglx=disabled -Dgbm=disabled"
fi
# shellcheck disable=SC2086
meson setup $RECONF "$BUILD_DIR" "$MESA" --cross-file "$CROSS_FILE" \
  --prefix=/usr --libdir=lib64 -Dbuildtype=release \
  -Dplatforms=x11,wayland $GL_OPTIONS -Dvulkan-drivers=panfrost \
  -Dpanfrost-kmds=kbase -Dmesa-clc=system -Dprecomp-compiler=system \
  -Dxlib-lease=disabled -Dlibunwind=disabled -Dllvm=disabled -Dzstd=disabled \
  > "$BUILD_DIR/meson-cross.log" 2>&1 || {
    echo "BUILD-FAIL: Meson setup; see $BUILD_DIR/meson-cross.log" >&2
    exit 1
  }

# Verify the build was configured for the intended target and sysroot before
# invoking Ninja.  Meson does not reread a changed cross file on reconfigure.
python3 - "$BUILD_DIR" "$SYSROOT" "$CXX_INCLUDE" "$CXX_ARCH_INCLUDE" "$CXX_LINK_DIR" <<'PY'
import json, pathlib, re, sys
build, sysroot, cxx_include, cxx_arch, cxx_link = map(pathlib.Path, sys.argv[1:])
machines = json.loads((build / "meson-info/intro-machines.json").read_text())
if machines["host"]["cpu_family"] != "aarch64" or machines["build"]["cpu_family"] != "x86_64":
    raise SystemExit("configured Meson build is not x86_64 -> AArch64 cross compilation")
commands = json.loads((build / "compile_commands.json").read_text())
def command_for(suffix):
    return next((item["command"] for item in commands if item["file"].endswith(suffix)), "")
c = command_for("fossilize_db.c")
cpp = command_for("u_qsort.cpp")
wayland = command_for("wsi_common_wayland.c")
for required in (str(sysroot), "-isystem" + str(sysroot / "usr/include")):
    if required not in c:
        raise SystemExit(f"C compiler command missing target sysroot flag: {required}")
for required in (str(sysroot), str(cxx_include), str(cxx_arch)):
    if required not in cpp:
        raise SystemExit(f"C++ compiler command missing target sysroot path: {required}")
if str(cxx_link) not in (build / "build.ninja").read_text(errors="replace"):
    raise SystemExit(f"link rules missing target C++ library path: {cxx_link}")
if not wayland or "HAVE_WAYLAND_PLATFORM" not in wayland:
    raise SystemExit("Wayland WSI source is not enabled in the Meson build")
log = (build / "meson-logs/meson-log.txt").read_text(errors="replace")
for dep in ("libdrm", "libudev", "wayland-client"):
    pattern = re.compile(rf"\bdependency\s+{re.escape(dep)}\b.*\bfound:\s*yes", re.I)
    if not any(pattern.search(line) for line in log.splitlines()):
        raise SystemExit(f"target dependency was not detected: {dep}")
PY

ninja -C "$BUILD_DIR" -j"$JOBS" || {
  echo "BUILD-FAIL: Ninja ($JOBS jobs) in $BUILD_DIR" >&2
  exit 1
}

SO="$BUILD_DIR/src/panfrost/vulkan/libvulkan_panfrost.so"
ICD="$BUILD_DIR/src/panfrost/vulkan/panfrost_icd.aarch64.json"
[ -s "$SO" ] || { echo "BUILD-FAIL: ICD library missing: $SO" >&2; exit 1; }
[ -s "$ICD" ] || { echo "BUILD-FAIL: ICD JSON missing: $ICD" >&2; exit 1; }
[ -f "$BUILD_DIR/src/vulkan/wsi/libvulkan_wsi.a.p/wsi_common_wayland.c.o" ] || {
  echo "BUILD-FAIL: Wayland WSI object missing" >&2; exit 1;
}
[ -f "$BUILD_DIR/src/panfrost/vulkan/libpanvk_v12.a" ] || {
  echo "BUILD-FAIL: PanVK v12 CSF object library missing" >&2; exit 1;
}
strings "$SO" | grep -q 'kbase_kmod' || {
  echo "BUILD-FAIL: kbase path not present in ICD" >&2; exit 1;
}

mkdir -p "$OUT_DIR"
cp "$SO" "$OUT_DIR/libvulkan_panfrost.so"
python3 - "$ICD" "$OUT_DIR/panvk-kbase-debug.json" <<'PY'
import json, pathlib, sys
source, output = map(pathlib.Path, sys.argv[1:])
data = json.loads(source.read_text())
data["ICD"]["library_path"] = "./libvulkan_panfrost.so"
output.write_text(json.dumps(data, indent=2) + "\n")
PY
"$ROOT/scripts/validate-binary.sh" --abi glibc "$OUT_DIR/libvulkan_panfrost.so"
readelf --version-info "$OUT_DIR/libvulkan_panfrost.so" | grep -q 'GLIBC_' || {
  echo "BUILD-FAIL: glibc symbol versions missing" >&2; exit 1;
}
readelf -d "$OUT_DIR/libvulkan_panfrost.so" > "$OUT_DIR/dynamic-section.txt"
python3 - "$OUT_DIR/libvulkan_panfrost.so" "$OUT_DIR/panvk-kbase-debug.json" <<'PY'
import json, pathlib, re, subprocess, sys
so, manifest = map(pathlib.Path, sys.argv[1:])
dynamic = subprocess.check_output(["readelf", "-d", str(so)], text=True)
needed = re.findall(r"\[(lib[^\]]+)\]", dynamic)
if "libc.so.6" not in needed:
    raise SystemExit(f"ICD does not depend on glibc libc.so.6: {needed}")
if any(name in needed for name in ("libandroid.so", "liblog.so", "libc.so")):
    raise SystemExit(f"Bionic dependency found: {needed}")
if "libudev.so.1" not in needed:
    raise SystemExit(f"target libudev was not linked: {needed}")
data = json.loads(manifest.read_text())
if data["ICD"]["library_path"] != "./libvulkan_panfrost.so":
    raise SystemExit("debug ICD manifest does not use its adjacent library")
PY
file "$OUT_DIR/libvulkan_panfrost.so"
readelf -h "$OUT_DIR/libvulkan_panfrost.so"
readelf -d "$OUT_DIR/libvulkan_panfrost.so"
readelf --version-info "$OUT_DIR/libvulkan_panfrost.so"

if [ "$WITH_EGL_ZINK" = 1 ]; then
  EGL_SO="$BUILD_DIR/src/egl/libEGL.so.1.0.0"
  GLES_SO="$BUILD_DIR/src/mesa/glapi/es2api/libGLESv2.so.2.0.0"
  ZINK_DRI="$BUILD_DIR/src/gallium/targets/dril/zink_dri.so"
  SWRAST_DRI="$BUILD_DIR/src/gallium/targets/dril/swrast_dri.so"
  GALLIUM_SO="$(ls "$BUILD_DIR"/src/gallium/targets/dri/libgallium-*.so 2>/dev/null | head -1)"
  for artifact in "$EGL_SO" "$GLES_SO" "$ZINK_DRI" "$SWRAST_DRI" "$GALLIUM_SO"; do
    [ -s "$artifact" ] || { echo "BUILD-FAIL: EGL/Zink artifact missing: $artifact" >&2; exit 1; }
  done
  [ -f "$BUILD_DIR/src/egl/libEGL.so.1.0.0.p/main_eglapi.c.o" ] || {
    echo "BUILD-FAIL: EGL API object missing" >&2; exit 1;
  }
  for artifact in "$EGL_SO" "$GLES_SO" "$ZINK_DRI" "$SWRAST_DRI" "$GALLIUM_SO"; do
    file "$artifact" | grep -q 'ELF 64-bit.*ARM aarch64' || {
      echo "BUILD-FAIL: not AArch64 ELF: $artifact" >&2; exit 1;
    }
    "$ROOT/scripts/validate-binary.sh" --abi glibc "$artifact"
    readelf --version-info "$artifact" | grep -q 'GLIBC_' || {
      echo "BUILD-FAIL: glibc symbol versions missing: $artifact" >&2; exit 1;
    }
  done
  STAGE="$OUT_DIR/stage"
  rm -rf "$STAGE"
  DESTDIR="$STAGE" meson install -C "$BUILD_DIR" > "$OUT_DIR/egl-zink-install.log" 2>&1 || {
    echo "BUILD-FAIL: meson install; see $OUT_DIR/egl-zink-install.log" >&2; exit 1;
  }
  for staged in usr/lib64/libEGL.so.1.0.0 usr/lib64/libGLESv2.so.2.0.0 \
                usr/lib64/dri/zink_dri.so usr/lib64/dri/swrast_dri.so \
                usr/lib64/dri/libdril_dri.so usr/lib64/libvulkan_panfrost.so; do
    [ -s "$STAGE/$staged" ] || { echo "BUILD-FAIL: staged file missing: $staged" >&2; exit 1; }
  done
  [ -n "$(ls "$STAGE"/usr/lib64/libgallium-*.so 2>/dev/null)" ] || {
    echo "BUILD-FAIL: staged libgallium missing" >&2; exit 1;
  }
  ( cd "$STAGE" && find usr/lib64 -maxdepth 2 \( -type f -o -type l \) \
      \( -name 'libEGL*' -o -name 'libGLESv2*' -o -name 'libgallium*.so*' \
         -o -name 'zink_dri.so' -o -name 'swrast_dri.so' \
         -o -name 'libdril_dri.so' \) ) | sort > "$OUT_DIR/stage-file-list.txt"
  echo "OK: AArch64 glibc Wayland EGL/GLES2 Zink runtime staged in $STAGE"
fi

echo "OK: AArch64 glibc PanVK debug ICD and ICD JSON in $OUT_DIR"