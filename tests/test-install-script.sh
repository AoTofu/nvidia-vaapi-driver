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

# Keep integration tests from rebuilding the real desktop session's cache.
CACHE_BIN="$TMP_DIR/cache-bin"
mkdir -p "$CACHE_BIN"
export CACHE_CAPTURE="$TMP_DIR/cache-capture"
cat >"$CACHE_BIN/kbuildsycoca6" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$*" >>"$CACHE_CAPTURE"
exit "${CACHE_BUILD_STATUS:-0}"
EOF
chmod +x "$CACHE_BIN/kbuildsycoca6"
export PATH="$CACHE_BIN:$PATH"

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

grep -Fx -- '--noincremental' "$CACHE_CAPTURE"
if CACHE_BUILD_STATUS=1 XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$TMP_DIR/no-system-data" \
    "$ROOT_DIR/install.sh" --chrome-integration-only >"$TMP_DIR/cache-failure" 2>&1; then
    echo "Chrome integration ignored a KDE cache rebuild failure" >&2
    exit 1
fi
grep -F 'cache could not be refreshed' "$TMP_DIR/cache-failure"

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

# Recovery must replace an already-managed failing wrapper, not merge it again.
SYSTEM_DATA="$TMP_DIR/system-data"
SYSTEM_DESKTOP="$SYSTEM_DATA/applications/google-chrome.desktop"
mkdir -p "$SYSTEM_DATA/applications"
cat >"$SYSTEM_DESKTOP" <<'EOF'
[Desktop Entry]
Name=Google Chrome
Exec=/usr/bin/google-chrome-stable %U

[Desktop Action new-window]
Name=New Window
Exec=/usr/bin/google-chrome-stable

[Desktop Action new-private-window]
Name=New Incognito Window
Exec=/usr/bin/google-chrome-stable --incognito
EOF
SYSTEM_HASH="$(sha256sum "$SYSTEM_DESKTOP" | awk '{print $1}')"
cp "$DESKTOP_FILE" "$TMP_DIR/broken-managed.desktop"
cp "$DESKTOP_FILE" "$APPLICATION_DIR/chromium.desktop"

if XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$USER_DATA:$SYSTEM_DATA" \
    PATH="$FAIL_BIN:$PATH" "$ROOT_DIR/install.sh" --chrome-integration-only \
    --restore-chrome-launcher google-chrome.desktop >/dev/null 2>&1; then
    echo "Recovery ignored a managed-launcher backup failure" >&2
    exit 1
fi
cmp "$DESKTOP_FILE" "$TMP_DIR/broken-managed.desktop"

RECOVERY_DRIVER_DIR="$TMP_DIR/custom-driver"
XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$USER_DATA:$SYSTEM_DATA" \
    NVD_DRIVER_DIR="$RECOVERY_DRIVER_DIR" "$ROOT_DIR/install.sh" \
    --chrome-integration-only --restore-chrome-launcher google-chrome.desktop \
    >"$TMP_DIR/recovery-output"

RECOVERY_BACKUP="$(sed -n 's/^Backed up existing Chrome launcher to //p' "$TMP_DIR/recovery-output")"
cmp "$RECOVERY_BACKUP" "$TMP_DIR/broken-managed.desktop"
cmp "$APPLICATION_DIR/chromium.desktop" "$TMP_DIR/broken-managed.desktop"
if grep -Eq 'chrome-hotpatch|AcceleratedVideoEncoder' "$DESKTOP_FILE"; then
    echo "Recovery preserved the failing encode wrapper or its flags" >&2
    exit 1
fi
if [ "$(grep -c '^Exec=.* /usr/bin/google-chrome-stable' "$DESKTOP_FILE")" -ne 3 ]; then
    echo "Recovery did not restore every desktop action" >&2
    exit 1
fi
grep -F "LIBVA_DRIVERS_PATH=$RECOVERY_DRIVER_DIR" "$DESKTOP_FILE"
grep -F 'NVD_EXPORT_LAYOUT=packed' "$DESKTOP_FILE"
grep -F 'VaapiOnNvidiaGPUs' "$DESKTOP_FILE"
grep -F -- '--incognito' "$DESKTOP_FILE"
grep -F -- '%U' "$DESKTOP_FILE"
if [ "$SYSTEM_HASH" != "$(sha256sum "$SYSTEM_DESKTOP" | awk '{print $1}')" ]; then
    echo "Recovery modified the system template" >&2
    exit 1
fi

# Another recovery must create its own backup, even within the same second.
cp "$DESKTOP_FILE" "$TMP_DIR/recovered.desktop"
XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$SYSTEM_DATA" \
    NVD_DRIVER_DIR="$RECOVERY_DRIVER_DIR" "$ROOT_DIR/install.sh" \
    --chrome-integration-only --restore-chrome-launcher google-chrome.desktop \
    >"$TMP_DIR/recovery-repeat-output"
REPEAT_BACKUP="$(sed -n 's/^Backed up existing Chrome launcher to //p' "$TMP_DIR/recovery-repeat-output")"
test "$REPEAT_BACKUP" != "$RECOVERY_BACKUP"
cmp "$RECOVERY_BACKUP" "$TMP_DIR/broken-managed.desktop"
cmp "$REPEAT_BACKUP" "$TMP_DIR/recovered.desktop"
cmp "$DESKTOP_FILE" "$TMP_DIR/recovered.desktop"

# Never mistake the user override for a system template.
if XDG_DATA_HOME="$USER_DATA" XDG_DATA_DIRS="$USER_DATA" \
    "$ROOT_DIR/install.sh" --chrome-integration-only \
    --restore-chrome-launcher google-chrome.desktop >/dev/null 2>&1; then
    echo "Recovery accepted an absent system template" >&2
    exit 1
fi
cmp "$DESKTOP_FILE" "$TMP_DIR/recovered.desktop"

if XDG_DATA_HOME="$USER_DATA" "$ROOT_DIR/install.sh" \
    --restore-chrome-launcher google-chrome.desktop >/dev/null 2>&1; then
    echo "Recovery was accepted without integration-only mode" >&2
    exit 1
fi
if XDG_DATA_HOME="$USER_DATA" "$ROOT_DIR/install.sh" \
    --chrome-integration-only --restore-chrome-launcher ../outside.desktop >/dev/null 2>&1; then
    echo "Recovery accepted an unsupported desktop filename" >&2
    exit 1
fi
if "$ROOT_DIR/install.sh" --chrome-integration-only --restore-chrome-launcher >/dev/null 2>&1; then
    echo "Recovery accepted a missing desktop filename" >&2
    exit 1
fi
if XDG_DATA_HOME="$FLATPAK_USER_DATA" XDG_DATA_DIRS="$FLATPAK_SYSTEM_DATA" \
    "$ROOT_DIR/install.sh" --chrome-integration-only \
    --restore-chrome-launcher com.google.Chrome.desktop >/dev/null 2>&1; then
    echo "Recovery accepted a Flatpak template" >&2
    exit 1
fi
test ! -e "$FLATPAK_TARGET"
