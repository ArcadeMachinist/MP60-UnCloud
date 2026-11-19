#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPENWRT_VERSION="25.12.6"
OPENWRT_COMMIT="64900e756efa667774214f9c6b17c113222e01f7"
OPENWRT_DIR="${OPENWRT_DIR:-$ROOT/.openwrt}"
JOBS="${JOBS:-16}"
IMAGE_NAME="mp60-openwrt-${OPENWRT_VERSION}-r10.bin"
KEY_FILE="$ROOT/package/utils/mp60d/files/etc/dropbear/authorized_keys"

if [ ! -d "$OPENWRT_DIR/.git" ]; then
    if [ -e "$OPENWRT_DIR" ]; then
        echo "OPENWRT_DIR exists but is not a Git checkout: $OPENWRT_DIR" >&2
        exit 1
    fi
    git clone --depth 1 --branch "v${OPENWRT_VERSION}" \
        https://github.com/openwrt/openwrt.git "$OPENWRT_DIR"
fi

ACTUAL_COMMIT="$(git -C "$OPENWRT_DIR" rev-parse HEAD)"
if [ "$ACTUAL_COMMIT" != "$OPENWRT_COMMIT" ]; then
    echo "OpenWrt checkout is at $ACTUAL_COMMIT; expected $OPENWRT_COMMIT" >&2
    echo "Use the v${OPENWRT_VERSION} checkout or set OPENWRT_DIR accordingly." >&2
    exit 1
fi

PATCH="$ROOT/openwrt/patches/openwrt-${OPENWRT_VERSION}-mp60.patch"
if git -C "$OPENWRT_DIR" apply --check "$PATCH" 2>/dev/null; then
    git -C "$OPENWRT_DIR" apply "$PATCH"
elif git -C "$OPENWRT_DIR" apply --reverse --check "$PATCH" 2>/dev/null; then
    echo "MP60 OpenWrt patch is already applied"
else
    echo "OpenWrt patch does not apply cleanly; use a clean pinned checkout." >&2
    exit 1
fi

cp -a "$ROOT/openwrt/overlay/." "$OPENWRT_DIR/"
rm -rf "$OPENWRT_DIR/package/utils/mp60d"
cp -a "$ROOT/package/utils/mp60d" "$OPENWRT_DIR/package/utils/mp60d"
cp "$ROOT/openwrt/config.seed" "$OPENWRT_DIR/.config"

if [ -s "$KEY_FILE" ]; then
    echo "Building with the user-supplied SSH public key."
else
    echo "Building without an SSH authorized key."
fi

cd "$OPENWRT_DIR"
make defconfig
make -j"$JOBS"

IMAGE="$OPENWRT_DIR/bin/targets/ath79/generic/openwrt-ath79-generic-datto_mp60-squashfs-sysupgrade.bin"
mkdir -p "$ROOT/dist"
cp "$IMAGE" "$ROOT/dist/$IMAGE_NAME"
python3 "$ROOT/scripts/verify_image.py" "$ROOT/dist/$IMAGE_NAME" \
    --json "$ROOT/dist/verification.json"
(
    cd "$ROOT/dist"
    sha256sum "$IMAGE_NAME" > SHA256SUMS.txt
)
echo "Built $ROOT/dist/$IMAGE_NAME"
