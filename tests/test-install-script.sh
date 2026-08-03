#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="${1:?repository root is required}"
TMP_DIR="$(mktemp -d /tmp/nvvaapi-install-test-XXXXXX)"
trap 'rm -rf -- "$TMP_DIR"' EXIT

FAKE_DIR="$TMP_DIR/Chrome Test's"
FAKE_CHROME="$FAKE_DIR/chrome"
CAPTURE_FILE="$TMP_DIR/capture"
mkdir -p "$FAKE_DIR"

cat >"$FAKE_CHROME" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
{
    printf 'LIBVA_DRIVER_NAME=%s\n' "${LIBVA_DRIVER_NAME:-}"
    printf 'LIBVA_DRIVERS_PATH=%s\n' "${LIBVA_DRIVERS_PATH:-}"
    printf 'NVD_BACKEND=%s\n' "${NVD_BACKEND:-}"
    printf 'NVD_EXPORT_LAYOUT=%s\n' "${NVD_EXPORT_LAYOUT:-}"
    printf 'ARG=%s\n' "$@"
} >"$CAPTURE_FILE"
EOF
chmod +x "$FAKE_CHROME"
export CAPTURE_FILE

PROFILE_ARG="--user-data-dir=$TMP_DIR/Profile Test"
URL_ARG='https://example.test/watch?v=1&codec=h264'
COMMAND="$({
    BUILD_DIR="$TMP_DIR/build-must-not-exist" \
    CHROME_BIN="$FAKE_CHROME" \
        "$ROOT_DIR/install.sh" --print-chrome-command --chrome-wayland -- \
        "$PROFILE_ARG" "$URL_ARG"
})"

if [ -e "$TMP_DIR/build-must-not-exist" ]; then
    echo "--print-chrome-command unexpectedly configured a build" >&2
    exit 1
fi

eval "$COMMAND"

grep -Fx 'LIBVA_DRIVER_NAME=nvidia' "$CAPTURE_FILE"
grep -E '^LIBVA_DRIVERS_PATH=/.+' "$CAPTURE_FILE"
grep -Fx 'NVD_BACKEND=direct' "$CAPTURE_FILE"
grep -Fx 'NVD_EXPORT_LAYOUT=auto' "$CAPTURE_FILE"
grep -Fx 'ARG=--enable-features=AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs' "$CAPTURE_FILE"
grep -Fx 'ARG=--ignore-gpu-blocklist' "$CAPTURE_FILE"
grep -Fx 'ARG=--use-gl=angle' "$CAPTURE_FILE"
grep -Fx 'ARG=--use-angle=gl' "$CAPTURE_FILE"
grep -Fx 'ARG=--ozone-platform=wayland' "$CAPTURE_FILE"
grep -Fx "ARG=$PROFILE_ARG" "$CAPTURE_FILE"
grep -Fx "ARG=$URL_ARG" "$CAPTURE_FILE"

if CHROME_BIN="$FAKE_CHROME" "$ROOT_DIR/install.sh" \
        --print-chrome-command --launch-chrome >/dev/null 2>&1; then
    echo "conflicting Chrome modes were accepted" >&2
    exit 1
fi

if "$ROOT_DIR/install.sh" --chrome-bin >/dev/null 2>&1; then
    echo "missing --chrome-bin value was accepted" >&2
    exit 1
fi
