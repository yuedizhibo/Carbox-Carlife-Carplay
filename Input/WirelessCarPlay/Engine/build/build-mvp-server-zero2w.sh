#!/usr/bin/env bash
# Cross-build only mvp_server against the Debian 12 ARM64 sysroot.
# The wrapper deliberately suppresses GCC's default target runtime search so
# Bookworm libc and libstdc++ are selected instead of host cross-GCC copies.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
SOURCE_ROOT=$(cd "$HERE/../../.." && pwd)
SYSROOT=${SYSROOT:-/opt/zero2w-bookworm-arm64}
BUILD_DIR=${BUILD_DIR:-/opt/zero2w-cpp-build}
LIBDIR="$SYSROOT/usr/lib/aarch64-linux-gnu"
WRAPPER="$BUILD_DIR/bookworm-aarch64-g++"
TOOLCHAIN="$BUILD_DIR/bookworm-aarch64-toolchain.cmake"
MAP="$BUILD_DIR/mvp_server.link.map"
GCC_INCLUDE="$(dirname "$(aarch64-linux-gnu-g++ -print-file-name=crtbeginS.o)")/include"
CROSS_UAPI_INCLUDE=/usr/aarch64-linux-gnu/include

ensure_bookworm_cxx_headers() {
  local cache sources package
  [ -d "$SYSROOT/usr/include/c++/12" ] && return
  cache="$SYSROOT/.zero2w-cpp-build-inputs"
  sources="$cache/sources.list"
  mkdir -p "$cache"
  printf '%s\n' 'deb http://deb.debian.org/debian bookworm main' > "$sources"
  (
    cd "$cache"
    apt-get -o Dir::Etc::sourcelist="$sources" -o Dir::Etc::sourceparts=- \
      -o APT::Architecture=arm64 -o APT::Architectures::=arm64 update
    apt-get -o Dir::Etc::sourcelist="$sources" -o Dir::Etc::sourceparts=- \
      -o APT::Architecture=arm64 -o APT::Architectures::=arm64 \
      download libstdc++-12-dev:arm64
  )
  package=$(find "$cache" -maxdepth 1 -name 'libstdc++-12-dev_*_arm64.deb' -print -quit)
  [ -n "$package" ] || { echo "Bookworm libstdc++-12-dev download failed" >&2; exit 2; }
  dpkg-deb -x "$package" "$SYSROOT"
}

ensure_bookworm_cxx_headers
CXX_INCLUDE="$SYSROOT/usr/include/c++/12"
CXX_TARGET_INCLUDE="$SYSROOT/usr/include/aarch64-linux-gnu/c++/12"

for path in "$SYSROOT" "$LIBDIR/libc.so.6" "$LIBDIR/libstdc++.so.6" \
            "$LIBDIR/libm.so.6" "$LIBDIR/ld-linux-aarch64.so.1" \
            "$LIBDIR/Scrt1.o" "$LIBDIR/crti.o" "$LIBDIR/crtn.o"; do
  [ -e "$path" ] || { echo "missing Bookworm sysroot input: $path" >&2; exit 2; }
done
command -v aarch64-linux-gnu-g++ >/dev/null
for path in "$CXX_INCLUDE" "$CXX_TARGET_INCLUDE" "$GCC_INCLUDE" "$CROSS_UAPI_INCLUDE"; do
  [ -d "$path" ] || { echo "missing GCC 14 C++ compiler header directory: $path" >&2; exit 2; }
done
mkdir -p "$BUILD_DIR"

cat > "$WRAPPER" <<EOF
#!/usr/bin/env bash
set -euo pipefail
sysroot='$SYSROOT'
libdir='${LIBDIR}'
cxx=/usr/bin/aarch64-linux-gnu-g++
gccdir="\$(dirname "\$(\$cxx -print-file-name=crtbeginS.o)")"
for arg in "\$@"; do
  [ "\$arg" = -c ] && exec "\$cxx" "\$@"
done
args=()
for arg in "\$@"; do
  case "\$arg" in
    -lstdc++) args+=('-l:libstdc++.so.6') ;;
    -lc|-lpthread|-lpthreads|-ldl|-lrt|-lutil|-lanl) args+=('-l:libc.so.6') ;;
    -lm) args+=('-l:libm.so.6') ;;
    *) args+=("\$arg") ;;
  esac
done
exec "\$cxx" -nostdlib --sysroot="\$sysroot" -L"\$gccdir" -L"\$libdir" \
  "\$libdir/Scrt1.o" "\$libdir/crti.o" "\$gccdir/crtbeginS.o" \
  "\${args[@]}" -l:libstdc++.so.6 -l:libm.so.6 -lgcc -lgcc_s \
  -l:libc.so.6 -l:ld-linux-aarch64.so.1 "\$gccdir/crtendS.o" "\$libdir/crtn.o"
EOF
chmod 755 "$WRAPPER"

cat > "$TOOLCHAIN" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_SYSROOT "$SYSROOT")
set(CMAKE_CXX_COMPILER "$WRAPPER")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# GCC's cross C headers are newer than Bookworm.  Keep only its compiler
# intrinsics and kernel UAPI headers; libc and C++ headers come from Bookworm.
set(CMAKE_CXX_FLAGS "-nostdinc -isystem $CXX_INCLUDE -isystem $CXX_TARGET_INCLUDE -isystem $GCC_INCLUDE -isystem $SYSROOT/usr/include/aarch64-linux-gnu -isystem $SYSROOT/usr/include -isystem $CROSS_UAPI_INCLUDE" CACHE STRING "" FORCE)
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-rpath-link,$LIBDIR -Wl,-Map,$MAP" CACHE STRING "" FORCE)
set(CMAKE_FIND_ROOT_PATH "$SYSROOT")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

rm -f "$MAP"
cmake -S "$SOURCE_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DBUILD_MFI_AUTH3_NATIVE=OFF
cmake --build "$BUILD_DIR" --target mvp_server --parallel
[ -f "$BUILD_DIR/mvp_server" ] || { echo "mvp_server was not produced" >&2; exit 3; }
[ -f "$MAP" ] || { echo "mvp_server linker map was not produced" >&2; exit 3; }
# All libc-family and libstdc++ inputs must resolve inside the Bookworm sysroot.
if grep -E 'LOAD .*/(libc(\.so|_nonshared)|libstdc\+\+|libm\.so|ld-linux|Scrt1|crti|crtn)' "$MAP" \
  | grep -vF "$SYSROOT/"; then
  echo "host libc/libstdc++ linker-map contamination" >&2
  exit 4
fi
file "$BUILD_DIR/mvp_server"
aarch64-linux-gnu-readelf -d "$BUILD_DIR/mvp_server" | grep NEEDED
