#!/usr/bin/env bash
# Assemble a binary-only, local-evaluation staging bundle.  This script never
# contacts or mutates a board and never publishes the bundle.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SYSROOT=${SYSROOT:-/opt/zero2w-bookworm-arm64}
CPP_BUILD=${CPP_BUILD:-/opt/zero2w-cpp-build}
CP_NATIVE="$ROOT/target/aarch64-unknown-linux-musl/release/cp-native"
MVP_SERVER="$CPP_BUILD/mvp_server"
CATPLAY="$HERE/artifacts/catplay_c2a-aarch64-bookworm"
STAMP=${STAMP:-$(date -u +%Y%m%dT%H%M%SZ)}
BUNDLE="$HERE/board-staging/local-evaluation-arm64-$STAMP"
MANIFEST="$BUNDLE/manifest.txt"
READELF=${READELF:-aarch64-linux-gnu-readelf}

for path in "$CP_NATIVE" "$MVP_SERVER" "$CATPLAY" "$CPP_BUILD/mvp_server.link.map"; do
  [ -f "$path" ] || { echo "required build input is missing: $path" >&2; exit 2; }
done
# The canonical C2A artifact is strict-gated upstream.  Reject any replacement
# that does not retain the Bookworm ARM64 dynamic-linking boundary.
"$READELF" -h "$CATPLAY" | grep -Eq 'Machine: +AArch64'
"$READELF" -l "$CATPLAY" | grep -Fq '/lib/ld-linux-aarch64.so.1'
if find "$HERE/artifacts" -maxdepth 1 -type f -name '*INCOMPATIBLE-GLIBC239*' | grep -q .; then
  : # Quarantined files may exist beside the canonical artifact but are never copied.
fi

mkdir -p "$BUNDLE"
cp "$CP_NATIVE" "$BUNDLE/cp-native"
cp "$MVP_SERVER" "$BUNDLE/mvp_server"
cp "$CATPLAY" "$BUNDLE/catplay_c2a"
chmod 755 "$BUNDLE/cp-native" "$BUNDLE/mvp_server" "$BUNDLE/catplay_c2a"

version_max() {
  local prefix=$1 bin=$2 versions
  versions=$("$READELF" --version-info "$bin" 2>/dev/null | grep -oE "${prefix}_[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -Vu || true)
  if [ -n "$versions" ]; then
    printf '%s\n' "$versions" | tail -n 1
  else
    printf 'none\n'
  fi
}
version_list() {
  local prefix=$1 bin=$2 versions
  versions=$("$READELF" --version-info "$bin" 2>/dev/null | grep -oE "${prefix}_[0-9]+\.[0-9]+(\.[0-9]+)?" | sort -Vu || true)
  printf '%s\n' "${versions:-none}"
}
require_glibc_at_most_bookworm() {
  local label=$1 bin=$2 max
  max=$(version_max GLIBC "$bin")
  [ "$max" != none ] || { echo "$label has no GLIBC version metadata" >&2; exit 3; }
  max=${max#GLIBC_}
  [ "$(printf '%s\n%s\n' "$max" 2.36 | sort -V | head -n 1)" = "$max" ] || {
    echo "$label requires GLIBC_$max, exceeding Bookworm GLIBC_2.36" >&2
    exit 3
  }
}
require_arm64() {
  local label=$1 bin=$2
  "$READELF" -h "$bin" | grep -Eq 'Machine: +AArch64' || {
    echo "$label is not ARM64" >&2
    exit 3
  }
}
record_binary() {
  local name=$1 bin=$2 interpreter needed
  interpreter=$("$READELF" -l "$bin" | sed -n 's/.*Requesting program interpreter: \([^]]*\).*/\1/p')
  needed=$("$READELF" -d "$bin" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | paste -sd ',' - || true)
  cat >> "$MANIFEST" <<EOF

[$name]
sha256=$(sha256sum "$bin" | awk '{print $1}')
size_bytes=$(stat -c '%s' "$bin")
file=$(file -b "$bin")
elf_class=$("$READELF" -h "$bin" | awk -F: '/Class:/{gsub(/^ +/, "", $2); print $2}')
elf_data=$("$READELF" -h "$bin" | awk -F: '/Data:/{gsub(/^ +/, "", $2); print $2}')
elf_type=$("$READELF" -h "$bin" | awk -F: '/Type:/{gsub(/^ +/, "", $2); print $2}')
elf_machine=$("$READELF" -h "$bin" | awk -F: '/Machine:/{gsub(/^ +/, "", $2); print $2}')
interpreter=${interpreter:-none-static}
needed=${needed:-none-static}
glibc_versions=$(version_list GLIBC "$bin" | paste -sd ',' -)
max_glibc=$(version_max GLIBC "$bin")
glibcxx_versions=$(version_list GLIBCXX "$bin" | paste -sd ',' -)
max_glibcxx=$(version_max GLIBCXX "$bin")
EOF
}

require_arm64 cp-native "$BUNDLE/cp-native"
require_arm64 mvp_server "$BUNDLE/mvp_server"
require_arm64 catplay_c2a "$BUNDLE/catplay_c2a"
require_glibc_at_most_bookworm mvp_server "$BUNDLE/mvp_server"
require_glibc_at_most_bookworm catplay_c2a "$BUNDLE/catplay_c2a"
# Reconfirm the C++ linker map selected the sysroot libc and libstdc++, never
# the GCC 14 cross-runtime copies.
if grep -E 'LOAD .*/(libc(\.so|_nonshared)|libstdc\+\+|libm\.so|ld-linux|Scrt1|crti|crtn)' \
  "$CPP_BUILD/mvp_server.link.map" | grep -vF "$SYSROOT/"; then
  echo 'C++ linker map contains non-Bookworm libc/libstdc++ input' >&2
  exit 3
fi

cat > "$MANIFEST" <<EOF
LOCAL-EVALUATION ARM64 staging bundle for Orange Pi Zero 2W
bundle_path=$BUNDLE
created_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)
contents=binary-only; local staging only; not published or deployed
source_and_license_material=not included and not claimed by this bundle
catplay_quarantine=catplay_c2a-aarch64-bookworm.INCOMPATIBLE-GLIBC239 was excluded
cpp_linker_validation=Bookworm sysroot libc/libstdc++ map inputs only
EOF
record_binary cp-native "$BUNDLE/cp-native"
record_binary mvp_server "$BUNDLE/mvp_server"
record_binary catplay_c2a "$BUNDLE/catplay_c2a"

# The bundle is intentionally limited to executable artifacts plus this manifest.
[ "$(find "$BUNDLE" -maxdepth 1 -type f | wc -l)" -eq 4 ] || {
  echo 'unexpected bundle contents' >&2
  exit 4
}
printf '%s\n' "$BUNDLE" > "$HERE/board-staging/latest-bundle-path.txt"
printf 'bundle: %s\n' "$BUNDLE"
sha256sum "$BUNDLE/cp-native" "$BUNDLE/mvp_server" "$BUNDLE/catplay_c2a"
