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

for DANGEROUS_BUILD_DIR in / "$HOME" "$ROOT_DIR"; do
    if BUILD_DIR="$DANGEROUS_BUILD_DIR" \
        "$ROOT_DIR/install.sh" --clean --no-test --no-chrome-integration \
        >/dev/null 2>&1; then
        echo "dangerous clean target was accepted: $DANGEROUS_BUILD_DIR" >&2
        exit 1
    fi
done

UNMARKED_BUILD_DIR="$ROOT_DIR/.install-test-unmarked-$$"
mkdir -p "$UNMARKED_BUILD_DIR"
touch "$UNMARKED_BUILD_DIR/must-survive"
if BUILD_DIR="$UNMARKED_BUILD_DIR" \
    "$ROOT_DIR/install.sh" --clean --no-test --no-chrome-integration \
    >/dev/null 2>&1; then
    echo "unmarked clean target was accepted" >&2
    exit 1
fi
if [ ! -f "$UNMARKED_BUILD_DIR/must-survive" ]; then
    echo "unmarked clean target was modified" >&2
    exit 1
fi
rm -r -- "$UNMARKED_BUILD_DIR"

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
grep -Fx 'NVD_EXPORT_LAYOUT=packed' "$CAPTURE_FILE"
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

USER_DATA="$TMP_DIR/user-data"
APPLICATION_DIR="$USER_DATA/applications"
DESKTOP_FILE="$APPLICATION_DIR/google-chrome.desktop"
mkdir -p "$APPLICATION_DIR"
cat >"$DESKTOP_FILE" <<'EOF'
[Desktop Entry]
Name=Custom Chrome
Exec=/opt/chrome-hotpatch --enable-features=AcceleratedVideoEncoder --ozone-platform-hint=auto %U

[Desktop Action new-private-window]
Name=New Incognito Window
Exec=/opt/chrome-hotpatch --incognito
EOF

XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$TMP_DIR/no-system-data" \
    "$ROOT_DIR/install.sh" --chrome-integration-only >/dev/null

grep -Fx '# Managed by AoTofu nvidia-vaapi-driver install.sh' "$DESKTOP_FILE"
grep -F '/opt/chrome-hotpatch' "$DESKTOP_FILE"
grep -F 'AcceleratedVideoEncoder,AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs' "$DESKTOP_FILE"
grep -F 'LIBVA_DRIVER_NAME=nvidia' "$DESKTOP_FILE"
grep -F 'LIBVA_DRIVERS_PATH=' "$DESKTOP_FILE"
grep -F 'NVD_BACKEND=direct' "$DESKTOP_FILE"
grep -F 'NVD_EXPORT_LAYOUT=packed' "$DESKTOP_FILE"
grep -F -- '--ignore-gpu-blocklist' "$DESKTOP_FILE"
grep -F -- '--use-gl=angle' "$DESKTOP_FILE"
grep -F -- '--use-angle=gl' "$DESKTOP_FILE"
grep -F -- '--incognito' "$DESKTOP_FILE"

BACKUP_COUNT="$(find "$APPLICATION_DIR" -maxdepth 1 \
    -name 'google-chrome.desktop.nvidia-vaapi-backup-*' | wc -l)"
if [ "$BACKUP_COUNT" -ne 1 ]; then
    echo "existing user launcher was not backed up exactly once" >&2
    exit 1
fi

FIRST_HASH="$(sha256sum "$DESKTOP_FILE" | awk '{print $1}')"
XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$TMP_DIR/no-system-data" \
    "$ROOT_DIR/install.sh" --chrome-integration-only >/dev/null
SECOND_HASH="$(sha256sum "$DESKTOP_FILE" | awk '{print $1}')"
if [ "$FIRST_HASH" != "$SECOND_HASH" ]; then
    echo "Chrome integration is not idempotent" >&2
    exit 1
fi

FLATPAK_USER_DATA="$TMP_DIR/flatpak-user-data"
FLATPAK_SYSTEM_DATA="$TMP_DIR/flatpak-system-data"
FLATPAK_APPLICATION_DIR="$FLATPAK_SYSTEM_DATA/applications"
FLATPAK_SOURCE="$FLATPAK_APPLICATION_DIR/com.google.Chrome.desktop"
FLATPAK_TARGET="$FLATPAK_USER_DATA/applications/com.google.Chrome.desktop"
FLATPAK_OUTPUT="$TMP_DIR/flatpak-output"
mkdir -p "$FLATPAK_APPLICATION_DIR"
cat >"$FLATPAK_SOURCE" <<'EOF'
[Desktop Entry]
Name=Google Chrome
Exec=/usr/bin/flatpak run --branch=stable --arch=x86_64 --command=/app/bin/chrome --file-forwarding com.google.Chrome @@u %U @@
EOF

FLATPAK_HASH="$(sha256sum "$FLATPAK_SOURCE" | awk '{print $1}')"
XDG_DATA_HOME="$FLATPAK_USER_DATA" XDG_DATA_DIRS="$FLATPAK_SYSTEM_DATA" \
    "$ROOT_DIR/install.sh" --chrome-integration-only >"$FLATPAK_OUTPUT"
grep -F "Skipping Flatpak Chrome launcher $FLATPAK_SOURCE" "$FLATPAK_OUTPUT"
if [ -e "$FLATPAK_TARGET" ]; then
    echo "Flatpak Chrome launcher was unexpectedly overridden" >&2
    exit 1
fi
if [ "$FLATPAK_HASH" != "$(sha256sum "$FLATPAK_SOURCE" | awk '{print $1}')" ]; then
    echo "Flatpak Chrome launcher was unexpectedly modified" >&2
    exit 1
fi

FAIL_USER_DATA="$TMP_DIR/failure-user-data"
FAIL_APPLICATION_DIR="$FAIL_USER_DATA/applications"
FAIL_DESKTOP_FILE="$FAIL_APPLICATION_DIR/google-chrome.desktop"
FAIL_BIN="$TMP_DIR/failure-bin"
mkdir -p "$FAIL_APPLICATION_DIR" "$FAIL_BIN"
cat >"$FAIL_DESKTOP_FILE" <<'EOF'
[Desktop Entry]
Name=Failure Chrome
Exec=/opt/failure-chrome %U
EOF
cat >"$FAIL_BIN/cp" <<'EOF'
#!/usr/bin/env bash
exit 42
EOF
chmod +x "$FAIL_BIN/cp"

FAIL_HASH="$(sha256sum "$FAIL_DESKTOP_FILE" | awk '{print $1}')"
if PATH="$FAIL_BIN:$PATH" XDG_DATA_HOME="$FAIL_USER_DATA" \
        XDG_DATA_DIRS="$TMP_DIR/no-system-data" \
        "$ROOT_DIR/install.sh" --chrome-integration-only >/dev/null 2>&1; then
    echo "Chrome integration ignored a launcher backup failure" >&2
    exit 1
fi
if [ "$FAIL_HASH" != "$(sha256sum "$FAIL_DESKTOP_FILE" | awk '{print $1}')" ]; then
    echo "Chrome launcher changed after its backup failed" >&2
    exit 1
fi
if grep -Fq '# Managed by AoTofu nvidia-vaapi-driver install.sh' "$FAIL_DESKTOP_FILE"; then
    echo "Chrome launcher was marked managed after its backup failed" >&2
    exit 1
fi
