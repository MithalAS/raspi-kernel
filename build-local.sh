#!/bin/bash
# Local Kernel Build Script (arm32 cross-compile)
# Usage: ./build-local.sh

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${YELLOW}=== Remora Kernel Build (Local ARM32)${NC}"

# Check dependencies
echo -e "${YELLOW}Checking dependencies...${NC}"
for cmd in arm-linux-gnueabihf-gcc git make; do
  if ! command -v $cmd &> /dev/null; then
    echo -e "${RED}ERROR: $cmd not found${NC}"
    echo "Install with: sudo apt-get install gcc-arm-linux-gnueabihf build-essential"
    exit 1
  fi
done

# Setup
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR="${REPO_ROOT}/build"
INSTALL_DIR="${REPO_ROOT}/install"
FRAGMENT="${REPO_ROOT}/remora_fragment.config"

echo -e "${YELLOW}Repository root: $REPO_ROOT${NC}"
echo -e "${YELLOW}Build directory: $BUILD_DIR${NC}"
echo -e "${YELLOW}Install directory: $INSTALL_DIR${NC}"

# Clean if requested
if [[ "$1" == "clean" ]]; then
  echo -e "${YELLOW}Cleaning build...${NC}"
  rm -rf "$BUILD_DIR" "$INSTALL_DIR"
fi

# Create directories
mkdir -p "$BUILD_DIR" "$INSTALL_DIR/boot/overlays"

# Set build version
SHORT_SHA=$(git -C "$REPO_ROOT" rev-parse --short HEAD)
echo "-rem-${SHORT_SHA}" > "$REPO_ROOT/localversion-rem"
echo -e "${GREEN}Build version: -rem-${SHORT_SHA}${NC}"

# Configure
echo -e "${YELLOW}Configuring kernel...${NC}"
export ARCH=arm
export CROSS_COMPILE=arm-linux-gnueabihf-
export DTS_SUBDIR=.
export IMAGE=zImage
export KCONFIG_CONFIG="$BUILD_DIR/.config"
KERNEL_NAME=kernel7l

cd "$REPO_ROOT"
make O="$BUILD_DIR" bcm2711_defconfig

# Apply Remora fragment with merge_config.sh so overridden defconfig values are reported
scripts/kconfig/merge_config.sh -m -O "$BUILD_DIR" \
  "$BUILD_DIR/.config" "$FRAGMENT"

# NOTE: CONFIG_WERROR does not exist in Linux 5.10 (it was added in 5.15), so
# setting it here would be silently dropped by olddefconfig. Use WERROR=1 to
# opt in to warnings-as-errors via KCFLAGS instead.
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
time make O="$BUILD_DIR" -j "$(nproc)" "${MAKE_FLAGS[@]}" "$IMAGE" modules dtbs

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
else
  echo -e "${YELLOW}Note: Driver configured as module (not static)${NC}"
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


# Install modules and boot files (mirrors CI layout)
echo -e "${YELLOW}Installing modules and boot files...${NC}"
make O="$BUILD_DIR" INSTALL_MOD_PATH="$INSTALL_DIR" modules_install

cp "$BUILD_DIR/arch/${ARCH}/boot/dts/${DTS_SUBDIR}"/*.dtb "$INSTALL_DIR/boot/"
cp "$BUILD_DIR/arch/${ARCH}/boot/dts/overlays"/*.dtb* "$INSTALL_DIR/boot/overlays/"
cp "$REPO_ROOT/arch/${ARCH}/boot/dts/overlays/README" "$INSTALL_DIR/boot/overlays/"
cp "$BUILD_DIR/arch/${ARCH}/boot/$IMAGE" "$INSTALL_DIR/boot/${KERNEL_NAME}.img"

# Package
echo -e "${YELLOW}Packaging artifacts...${NC}"
TARBALL="$REPO_ROOT/kernel-bcm2711-arm-${SHORT_SHA}.tar.gz"
CHECKSUM="$REPO_ROOT/kernel-bcm2711-arm-${SHORT_SHA}.sha256"

cd "$INSTALL_DIR"
tar czf "$TARBALL" boot lib
cd "$REPO_ROOT"
sha256sum "$(basename "$TARBALL")" > "$CHECKSUM"

echo -e "${GREEN}✓ Build complete!${NC}"
echo -e "${GREEN}Artifacts:${NC}"
ls -lh "$TARBALL" "$CHECKSUM"
echo ""
echo -e "${GREEN}Config:${NC}"
ls -lh "$BUILD_DIR/.config"
echo ""
echo -e "${YELLOW}Next steps:${NC}"
echo "  1. Check build output above for any warnings/errors"
echo "  2. If successful, artifacts are ready in: $REPO_ROOT"
echo "  3. Verify checksums: sha256sum -c $CHECKSUM"
echo "  4. Extract to test: tar tzf $TARBALL | head"
