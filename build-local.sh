#!/bin/bash
# Remora kernel build script (cross-compile).
#
# Invoked by both local builds and CI (.github/workflows/*.yml) so that the
# two cannot drift apart.
#
# Usage:
#   ./build-local.sh [clean] [deb|bindeb-pkg]
#   ./build-local.sh toolchain-pkg
#
# Environment:
#   TARGET=arm|arm64        Target to build (default: arm)
#   BUILD_DEB=1             Build Debian packages via bindeb-pkg (or pass 'deb' / 'bindeb-pkg')
#   KDEB_PKGVERSION=<ver>   Override Debian package version (defaults to <kernelver>-rem-<tag>-1)
#   KDEB_COMPRESS=<type>    Debian package compression: gzip, xz, etc. (default: gzip)
#   JOBS=<n>                Parallel jobs (default: nproc)
#   WERROR=1                Build with -Werror (see note below)

set -euo pipefail

# Colors (disabled when not attached to a tty, e.g. in CI logs)
if [[ -t 1 ]]; then
  RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
else
  RED=''; GREEN=''; YELLOW=''; NC=''
fi

TARGET="${TARGET:-arm}"

# Per-target settings. These are the single source of truth for the CI matrix.
case "$TARGET" in
  arm)
    NAME="bcm2711-arm"
    ARCH=arm
    CROSS_COMPILE=arm-linux-gnueabihf-
    DEFCONFIG=bcm2711_defconfig
    DTS_SUBDIR=.
    IMAGE=zImage
    KERNEL_NAME=kernel7l
    TOOLCHAIN_PKG=gcc-arm-linux-gnueabihf
    ;;
  arm64)
    NAME="bcm2711-arm64"
    ARCH=arm64
    CROSS_COMPILE=aarch64-linux-gnu-
    DEFCONFIG=bcm2711_defconfig
    DTS_SUBDIR=broadcom
    IMAGE=Image.gz
    KERNEL_NAME=kernel8
    TOOLCHAIN_PKG=gcc-aarch64-linux-gnu
    ;;
  *)
    echo "ERROR: unknown TARGET '$TARGET' (expected 'arm' or 'arm64')" >&2
    exit 1
    ;;
esac

BUILD_DEB="${BUILD_DEB:-${DEB:-0}}"
DO_CLEAN=0

for arg in "$@"; do
  case "$arg" in
    toolchain-pkg)
      echo "$TOOLCHAIN_PKG"
      exit 0
      ;;
    clean)
      DO_CLEAN=1
      ;;
    deb|bindeb-pkg)
      BUILD_DEB=1
      ;;
    *)
      echo "ERROR: unknown argument '$arg' (expected 'clean', 'deb', 'bindeb-pkg', or 'toolchain-pkg')" >&2
      exit 1
      ;;
  esac
done

echo -e "${YELLOW}=== Remora Kernel Build (${NAME})${NC}"

# Check dependencies
echo -e "${YELLOW}Checking dependencies...${NC}"
DEPS=("${CROSS_COMPILE}gcc" git make bc bison flex)
if [[ "$BUILD_DEB" == "1" ]]; then
  DEPS+=(dpkg-buildpackage dpkg-deb fakeroot rsync kmod cpio)
fi

for cmd in "${DEPS[@]}"; do
  if ! command -v "$cmd" &> /dev/null; then
    echo -e "${RED}ERROR: $cmd not found${NC}"
    echo "Install with: sudo apt-get install $TOOLCHAIN_PKG build-essential bc bison flex libssl-dev dpkg-dev fakeroot rsync kmod cpio"
    exit 1
  fi
done

# Setup
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# Per-target directories so switching TARGET cannot reuse a stale build tree.
BUILD_DIR="${REPO_ROOT}/build/${TARGET}"
INSTALL_DIR="${REPO_ROOT}/install/${TARGET}"
FRAGMENT="${REPO_ROOT}/remora_fragment.config"
JOBS="${JOBS:-$(nproc)}"

echo -e "${YELLOW}Repository root: $REPO_ROOT${NC}"
echo -e "${YELLOW}Build directory: $BUILD_DIR${NC}"
echo -e "${YELLOW}Install directory: $INSTALL_DIR${NC}"
echo -e "${YELLOW}Parallel jobs: $JOBS${NC}"
if [[ "$BUILD_DEB" == "1" ]]; then
  echo -e "${YELLOW}Debian packages: enabled (bindeb-pkg)${NC}"
fi

# Set build version
SHORT_SHA=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
echo "-rem-${SHORT_SHA}" > "$REPO_ROOT/localversion-rem"
echo -e "${GREEN}Build version: -rem-${SHORT_SHA}${NC}"

PKG_TAG="${CUSTOM_TAG:-$(git -C "$REPO_ROOT" describe --tags --exact-match 2>/dev/null || echo "$SHORT_SHA")}"
PKG_TAG="${PKG_TAG#v}"

# Debian packaging settings (bindeb-pkg)
if [[ "$BUILD_DEB" == "1" ]]; then
  export KDEB_COMPRESS="${KDEB_COMPRESS:-gzip}"
  KERNEL_VER=$(make -s kernelversion)
  export KDEB_PKGVERSION="${KDEB_PKGVERSION:-${KERNEL_VER}-rem-${PKG_TAG}-1}"
  echo -e "${GREEN}Debian package version: ${KDEB_PKGVERSION} (compression: ${KDEB_COMPRESS})${NC}"
fi

# Clean if requested
if [[ "$DO_CLEAN" == "1" ]]; then
  echo -e "${YELLOW}Cleaning build...${NC}"
  rm -rf "$BUILD_DIR" "$INSTALL_DIR"
  rm -f "$REPO_ROOT/build"/*"${SHORT_SHA}"*.deb \
        "$REPO_ROOT/build"/*"${SHORT_SHA}"*.changes \
        "$REPO_ROOT/build"/*"${SHORT_SHA}"*.buildinfo 2>/dev/null || true
fi

# Create directories
mkdir -p "$BUILD_DIR" "$INSTALL_DIR/boot/overlays"

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "name=${NAME}" >> "$GITHUB_OUTPUT"
  echo "short_sha=${SHORT_SHA}" >> "$GITHUB_OUTPUT"
fi

# Configure
echo -e "${YELLOW}Configuring kernel...${NC}"
export ARCH CROSS_COMPILE DTS_SUBDIR IMAGE
export KCONFIG_CONFIG="$BUILD_DIR/.config"

cd "$REPO_ROOT"
make O="$BUILD_DIR" "$DEFCONFIG"

# Apply the Remora fragment with merge_config.sh rather than appending it to
# .config: merge_config.sh logs every defconfig value the fragment overrides,
# so a silently dropped customization is visible in the build log.
scripts/kconfig/merge_config.sh -m -O "$BUILD_DIR" \
  "$BUILD_DIR/.config" "$FRAGMENT"

# NOTE: CONFIG_WERROR does not exist in Linux 5.10 (it was added in 5.15), so
# setting it in .config is silently discarded by olddefconfig. Use WERROR=1.
MAKE_FLAGS=()
if [[ "${WERROR:-0}" == "1" ]]; then
  echo -e "${YELLOW}WERROR=1: building with -Werror${NC}"
  MAKE_FLAGS+=(KCFLAGS=-Werror)
fi

# Resolve the merged config: new/dependent symbols get their defaults
make O="$BUILD_DIR" olddefconfig

echo -e "${GREEN}Configuration complete${NC}"

# Verify every symbol requested by the fragment survived olddefconfig.
# merge_config.sh only performs this check when it runs make itself (not with -m).
echo -e "${YELLOW}Verifying Remora fragment was applied...${NC}"
FRAGMENT_OK=1
while IFS= read -r line; do
  [[ "$line" =~ ^CONFIG_[A-Za-z0-9_]+= ]] || continue
  if ! grep -qxF "$line" "$BUILD_DIR/.config"; then
    SYM="${line%%=*}"
    ACTUAL=$(grep -E "^($SYM=| *# $SYM is not set)" "$BUILD_DIR/.config" || echo "<absent>")
    echo -e "${RED}ERROR: fragment requested '$line' but config has '$ACTUAL'${NC}"
    FRAGMENT_OK=0
  fi
done < "$FRAGMENT"
if [[ "$FRAGMENT_OK" -ne 1 ]]; then
  echo -e "${RED}ERROR: remora_fragment.config was not fully applied${NC}"
  exit 1
fi
echo -e "${GREEN}Fragment validation passed${NC}"

echo -e "${YELLOW}Final config:${NC}"
grep "CONFIG_LOCALVERSION\|CONFIG_SERIAL_XR20M117X\|CONFIG_OVERLAY_FS" "$BUILD_DIR/.config"

# Build
echo -e "${YELLOW}Building kernel...${NC}"
time make O="$BUILD_DIR" -j "$JOBS" "${MAKE_FLAGS[@]}" "$IMAGE" modules dtbs

# Validate driver
echo -e "${YELLOW}Validating XR20M117X driver...${NC}"
CONFIG_FILE="$BUILD_DIR/.config"
BUILD_DIR_FULL="$BUILD_DIR"

if ! grep -q "CONFIG_SERIAL_XR20M117X[^_]" "$CONFIG_FILE"; then
  echo -e "${RED}ERROR: CONFIG_SERIAL_XR20M117X not found${NC}"
  exit 1
fi

CORE_CONFIG=$(grep "^CONFIG_SERIAL_XR20M117X_CORE=" "$CONFIG_FILE" || echo "")
if [[ -z "$CORE_CONFIG" ]]; then
  echo -e "${RED}ERROR: CONFIG_SERIAL_XR20M117X_CORE not found${NC}"
  exit 1
fi

echo -e "${GREEN}Config validation passed:${NC}"
grep "^CONFIG_SERIAL_XR20M117X" "$CONFIG_FILE"

if [[ "$CORE_CONFIG" == "CONFIG_SERIAL_XR20M117X_CORE=y" ]]; then
  OBJ_FILE=$(find "$BUILD_DIR_FULL" -name "xrm117x.o" 2>/dev/null)
  if [[ -z "$OBJ_FILE" ]]; then
    echo -e "${RED}ERROR: XR20M117X object file not found${NC}"
    exit 1
  fi
  echo -e "${GREEN}Static linking validation passed: $OBJ_FILE${NC}"
  ls -lh "$OBJ_FILE"
elif [[ "$CORE_CONFIG" == "CONFIG_SERIAL_XR20M117X_CORE=m" ]]; then
  KO_FILE=$(find "$BUILD_DIR_FULL" -name "xrm117x.ko" 2>/dev/null)
  if [[ -z "$KO_FILE" ]]; then
    echo -e "${RED}ERROR: XR20M117X module (xrm117x.ko) not found${NC}"
    exit 1
  fi
  echo -e "${GREEN}Module validation passed: $KO_FILE${NC}"
  ls -lh "$KO_FILE"
else
  echo -e "${RED}ERROR: Unexpected CONFIG_SERIAL_XR20M117X_CORE value: $CORE_CONFIG${NC}"
  exit 1
fi

# Validate overlayfs (required for Docker storage-driver=overlay2)
echo -e "${YELLOW}Validating overlayfs support...${NC}"
if ! grep -q "^CONFIG_OVERLAY_FS=y" "$CONFIG_FILE"; then
  echo -e "${RED}ERROR: CONFIG_OVERLAY_FS is not built in (required for Docker overlay2)${NC}"
  grep -E "^(CONFIG_OVERLAY_FS=| *# CONFIG_OVERLAY_FS is not set)" "$CONFIG_FILE" || true
  exit 1
fi
for OVL_OPT in CONFIG_OVERLAY_FS_REDIRECT_DIR CONFIG_OVERLAY_FS_METACOPY; do
  if ! grep -q "^${OVL_OPT}=y" "$CONFIG_FILE"; then
    echo -e "${RED}ERROR: ${OVL_OPT} is not enabled${NC}"
    exit 1
  fi
done
echo -e "${GREEN}Overlayfs validation passed:${NC}"
grep "^CONFIG_OVERLAY_FS" "$CONFIG_FILE"

# overlayfs must be built in, not a module, so "overlay" is present in
# /proc/filesystems at boot without relying on module loading on the target.
if grep -q "overlayfs/overlay\.ko" "$BUILD_DIR/modules.order" 2>/dev/null; then
  echo -e "${RED}ERROR: overlay is built as a module (expected built-in)${NC}"
  exit 1
fi


# Install modules and boot files (mirrors CI layout)
echo -e "${YELLOW}Installing modules and boot files...${NC}"
make O="$BUILD_DIR" INSTALL_MOD_PATH="$INSTALL_DIR" modules_install

cp "$BUILD_DIR/arch/${ARCH}/boot/dts/${DTS_SUBDIR}"/*.dtb "$INSTALL_DIR/boot/"
cp "$BUILD_DIR/arch/${ARCH}/boot/dts/overlays"/*.dtb* "$INSTALL_DIR/boot/overlays/"
cp "$REPO_ROOT/arch/${ARCH}/boot/dts/overlays/README" "$INSTALL_DIR/boot/overlays/"
cp "$BUILD_DIR/arch/${ARCH}/boot/$IMAGE" "$INSTALL_DIR/boot/${KERNEL_NAME}.img"

# Package
echo -e "${YELLOW}Packaging artifacts...${NC}"
TARBALL="$REPO_ROOT/kernel-${NAME}-${SHORT_SHA}.tar.gz"
CHECKSUM="$REPO_ROOT/kernel-${NAME}-${SHORT_SHA}.sha256"

cd "$INSTALL_DIR"
tar czf "$TARBALL" boot lib
cd "$REPO_ROOT"
sha256sum "$(basename "$TARBALL")" > "$CHECKSUM"

# Build Debian packages if requested
if [[ "$BUILD_DEB" == "1" ]]; then
  echo -e "${YELLOW}Building Debian packages (bindeb-pkg)...${NC}"
  time make O="$BUILD_DIR" -j "$JOBS" "${MAKE_FLAGS[@]}" bindeb-pkg
  echo -e "${GREEN}✓ Debian packaging complete!${NC}"
fi

if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
  echo "tarball=${TARBALL}" >> "$GITHUB_OUTPUT"
  echo "checksum=${CHECKSUM}" >> "$GITHUB_OUTPUT"
  if [[ "$BUILD_DEB" == "1" ]]; then
    IMAGE_DEB=$(ls -1 "$REPO_ROOT/build"/linux-image-*.deb 2>/dev/null | head -n 1 || true)
    if [[ -n "$IMAGE_DEB" ]]; then
      echo "image_deb=${IMAGE_DEB}" >> "$GITHUB_OUTPUT"
    fi
  fi
fi

echo -e "${GREEN}✓ Build complete!${NC}"
echo -e "${GREEN}Artifacts:${NC}"
ls -lh "$TARBALL" "$CHECKSUM"
if [[ "$BUILD_DEB" == "1" ]]; then
  echo -e "${GREEN}Debian packages:${NC}"
  ls -lh "$REPO_ROOT/build"/*"${PKG_TAG}"*.deb 2>/dev/null || ls -lh "$REPO_ROOT/build"/*.deb 2>/dev/null || true
fi
echo ""
echo -e "${GREEN}Config:${NC}"
ls -lh "$BUILD_DIR/.config"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Check build output above for any warnings/errors"
echo "  2. If successful, artifacts are ready in: $REPO_ROOT"
echo "  3. Verify checksums: sha256sum -c $CHECKSUM"
echo "  4. Extract to test: tar tzf $TARBALL | head"
