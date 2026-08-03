#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
RENDER_DEVICE="${RENDER_DEVICE:-/dev/dri/renderD128}"
INSTALL_DEPS=0
CLEAN_BUILD=0
RUN_TEST=1
BACKUP_PATH=""
LAUNCH_CHROME=0
PRINT_CHROME_COMMAND=0
CHROME_WAYLAND=0
CHROME_INTEGRATION=1
CHROME_INTEGRATION_ONLY=0
CHROME_BIN="${CHROME_BIN:-}"
CHROME_ARGS=()
CHROME_ENV=()
CHROME_COMMAND=()

usage() {
    cat <<'EOF'
Usage: ./install.sh [options]

Build and install this nvidia-vaapi-driver checkout.

Options:
  --deps            Install common build dependencies with the detected package manager.
  --clean           Remove the build directory before configuring.
  --build-dir DIR   Use a custom build directory. Default: ./build
  --render-device D Use a custom render node for the vainfo smoke test.
                    Default: /dev/dri/renderD128
  --no-test         Skip the vainfo smoke test after installation.
  --launch-chrome   Launch Chrome with this driver after installation.
  --print-chrome-command
                    Print a reusable Chrome command without building or installing.
  --chrome-bin PATH Use a specific Chrome/Chromium executable.
  --chrome-wayland  Add --ozone-platform=wayland to the Chrome command.
  --no-chrome-integration
                    Do not update the current user's Chrome desktop launcher.
  --chrome-integration-only
                    Update Chrome desktop launchers without building or installing.
  --                Pass all remaining arguments to Chrome.
  -h, --help        Show this help.

Environment:
  BUILD_DIR         Same as --build-dir.
  RENDER_DEVICE    Same as --render-device.
  CHROME_BIN        Same as --chrome-bin.
EOF
}

run_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    else
        sudo "$@"
    fi
}

install_deps() {
    if command -v dnf >/dev/null 2>&1; then
        run_sudo dnf install -y \
            gcc meson ninja-build pkgconf-pkg-config \
            libva-devel libdrm-devel libglvnd-devel \
            gstreamer1-plugins-bad-free-devel nv-codec-headers
    elif command -v apt-get >/dev/null 2>&1; then
        run_sudo apt-get update
        run_sudo apt-get install -y \
            build-essential meson ninja-build pkg-config \
            libva-dev libegl-dev libdrm-dev \
            libgstreamer-plugins-bad1.0-dev libffmpeg-nvenc-dev vainfo
    elif command -v pacman >/dev/null 2>&1; then
        run_sudo pacman -S --needed \
            base-devel meson ninja pkgconf libva libdrm \
            gst-plugins-bad ffnvcodec-headers libglvnd libva-utils
    elif command -v zypper >/dev/null 2>&1; then
        run_sudo zypper install -y \
            gcc meson ninja pkg-config libva-devel libdrm-devel \
            Mesa-libEGL-devel gstreamer-plugins-bad-devel nv-codec-headers
    else
        echo "No supported package manager found. Install the build dependencies manually." >&2
        exit 1
    fi
}

driver_dir() {
    local dir=""
    if command -v pkg-config >/dev/null 2>&1; then
        dir="$(pkg-config --variable=driverdir libva 2>/dev/null || true)"
    fi

    if [ -n "$dir" ]; then
        printf '%s\n' "$dir"
    elif [ -d /usr/lib64/dri ]; then
        printf '%s\n' /usr/lib64/dri
    elif [ -d /usr/lib/x86_64-linux-gnu/dri ]; then
        printf '%s\n' /usr/lib/x86_64-linux-gnu/dri
    else
        printf '%s\n' /usr/lib64/dri
    fi
}

backup_existing_driver() {
    local dir target stamp
    dir="$(driver_dir)"
    target="$dir/nvidia_drv_video.so"
    if [ -e "$target" ]; then
        stamp="$(date +%Y%m%d-%H%M%S)"
        BACKUP_PATH="$target.backup-$stamp"
        run_sudo cp -a "$target" "$BACKUP_PATH"
        echo "Backed up existing driver to $BACKUP_PATH"
    fi
}

resolve_chrome_binary() {
    local candidate resolved

    if [ -n "$CHROME_BIN" ]; then
        if [[ "$CHROME_BIN" == */* ]]; then
            if [ -x "$CHROME_BIN" ]; then
                printf '%s\n' "$CHROME_BIN"
                return 0
            fi
        else
            resolved="$(command -v "$CHROME_BIN" 2>/dev/null || true)"
            if [ -n "$resolved" ]; then
                printf '%s\n' "$resolved"
                return 0
            fi
        fi
        return 1
    fi

    for candidate in google-chrome-stable google-chrome chromium chromium-browser; do
        resolved="$(command -v "$candidate" 2>/dev/null || true)"
        if [ -n "$resolved" ]; then
            printf '%s\n' "$resolved"
            return 0
        fi
    done
    return 1
}

prepare_chrome_command() {
    local chrome
    if ! chrome="$(resolve_chrome_binary)"; then
        echo "Chrome/Chromium was not found. Use --chrome-bin PATH or set CHROME_BIN." >&2
        return 1
    fi

    CHROME_ENV=(
        "LIBVA_DRIVER_NAME=nvidia"
        "LIBVA_DRIVERS_PATH=$(driver_dir)"
        "NVD_BACKEND=direct"
        "NVD_EXPORT_LAYOUT=auto"
    )
    CHROME_COMMAND=(
        "$chrome"
        "--enable-features=AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs"
        "--ignore-gpu-blocklist"
        "--use-gl=angle"
        "--use-angle=gl"
    )
    if [ "$CHROME_WAYLAND" -eq 1 ]; then
        CHROME_COMMAND+=("--ozone-platform=wayland")
    fi
    CHROME_COMMAND+=("${CHROME_ARGS[@]}")
}

print_chrome_command() {
    local value
    printf 'env'
    for value in "${CHROME_ENV[@]}" "${CHROME_COMMAND[@]}"; do
        printf ' %q' "$value"
    done
    printf '\n'
}

launch_chrome() {
    echo "Close existing Chrome processes first; an already-running browser may ignore the new environment."
    echo "Launching Chrome with nvidia-vaapi-driver:"
    print_chrome_command
    exec env "${CHROME_ENV[@]}" "${CHROME_COMMAND[@]}"
}

ensure_exec_environment() {
    local command="$1"
    local name="$2"
    local value="$3"
    local match prefix

    if [[ "$command" =~ (^|[[:space:]])${name}=[^[:space:]]+ ]]; then
        match="${BASH_REMATCH[0]}"
        prefix="${BASH_REMATCH[1]}"
        command="${command/"$match"/"$prefix$name=$value"}"
    elif [[ "$command" == /usr/bin/env\ * ]]; then
        command="/usr/bin/env $name=$value ${command#/usr/bin/env }"
    elif [[ "$command" == env\ * ]]; then
        command="env $name=$value ${command#env }"
    else
        command="/usr/bin/env $name=$value $command"
    fi
    printf '%s\n' "$command"
}

insert_exec_switch() {
    local command="$1"
    local option="$2"
    local option_name="${option%%=*}"
    local existing field

    if [[ "$option" == *=* &&
          "$command" =~ (^|[[:space:]])${option_name}=[^[:space:]]+ ]]; then
        existing="${BASH_REMATCH[0]}"
        command="${command/"$existing"/"${BASH_REMATCH[1]}$option"}"
        printf '%s\n' "$command"
        return
    fi
    if [[ " $command " == *" $option "* ]]; then
        printf '%s\n' "$command"
        return
    fi
    if [[ "$command" =~ %[fFuU] ]]; then
        field="${BASH_REMATCH[0]}"
        command="${command/"$field"/"$option $field"}"
    else
        command="$command $option"
    fi
    printf '%s\n' "$command"
}

ensure_exec_feature() {
    local command="$1"
    local feature="$2"
    local features updated

    if [[ "$command" =~ --enable-features=([^[:space:]]+) ]]; then
        features="${BASH_REMATCH[1]}"
        case ",$features," in
            *",$feature,"*) ;;
            *)
                updated="$features,$feature"
                command="${command/"--enable-features=$features"/"--enable-features=$updated"}"
                ;;
        esac
    else
        command="$(insert_exec_switch "$command" "--enable-features=$feature")"
    fi
    printf '%s\n' "$command"
}

patch_chrome_exec() {
    local command="$1"

    # Apply in reverse display order because each missing assignment is
    # inserted immediately after /usr/bin/env.
    command="$(ensure_exec_environment "$command" NVD_EXPORT_LAYOUT auto)"
    command="$(ensure_exec_environment "$command" NVD_BACKEND direct)"
    command="$(ensure_exec_environment "$command" LIBVA_DRIVERS_PATH "$(driver_dir)")"
    command="$(ensure_exec_environment "$command" LIBVA_DRIVER_NAME nvidia)"
    command="$(ensure_exec_feature "$command" AcceleratedVideoDecodeLinuxGL)"
    command="$(ensure_exec_feature "$command" VaapiOnNvidiaGPUs)"
    command="$(insert_exec_switch "$command" --ignore-gpu-blocklist)"
    command="$(insert_exec_switch "$command" --use-gl=angle)"
    command="$(insert_exec_switch "$command" --use-angle=gl)"
    printf '%s\n' "$command"
}

patch_chrome_desktop_file() {
    local source="$1"
    local target="$2"
    local target_dir temp stamp backup line

    target_dir="$(dirname -- "$target")"
    mkdir -p "$target_dir"
    if [ -e "$target" ] &&
       ! grep -Fq '# Managed by AoTofu nvidia-vaapi-driver install.sh' "$target"; then
        stamp="$(date +%Y%m%d-%H%M%S)"
        backup="$target.nvidia-vaapi-backup-$stamp"
        cp -a "$target" "$backup"
        echo "Backed up existing Chrome launcher to $backup"
    fi

    temp="$(mktemp "$target_dir/.nvidia-vaapi-chrome.XXXXXX")"
    {
        echo '# Managed by AoTofu nvidia-vaapi-driver install.sh'
        while IFS= read -r line || [ -n "$line" ]; do
            if [ "$line" = '# Managed by AoTofu nvidia-vaapi-driver install.sh' ]; then
                continue
            fi
            if [[ "$line" == Exec=* ]]; then
                printf 'Exec=%s\n' "$(patch_chrome_exec "${line#Exec=}")"
            else
                printf '%s\n' "$line"
            fi
        done <"$source"
    } >"$temp"
    chmod 0644 "$temp"
    mv -f "$temp" "$target"
}

install_chrome_integration() {
    local user_data_home target_dir data_dirs candidate source target
    local data_dir installed=0
    local -a chrome_data_dirs
    local candidates=(
        google-chrome.desktop
        google-chrome-stable.desktop
        com.google.Chrome.desktop
        chromium.desktop
        chromium-browser.desktop
    )

    user_data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
    target_dir="$user_data_home/applications"
    data_dirs="${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"

    for candidate in "${candidates[@]}"; do
        target="$target_dir/$candidate"
        source=""
        if [ -f "$target" ]; then
            source="$target"
        else
            IFS=':' read -r -a chrome_data_dirs <<<"$data_dirs"
            for data_dir in "${chrome_data_dirs[@]}"; do
                if [ -f "$data_dir/applications/$candidate" ]; then
                    source="$data_dir/applications/$candidate"
                    break
                fi
            done
        fi
        if [ -z "$source" ]; then
            continue
        fi
        patch_chrome_desktop_file "$source" "$target"
        echo "Enabled nvidia-vaapi-driver in $target"
        installed=$((installed + 1))
    done

    if [ "$installed" -eq 0 ]; then
        echo "No Chrome/Chromium desktop entry was found; launcher integration was skipped."
        return 1
    fi
    if command -v update-desktop-database >/dev/null 2>&1; then
        update-desktop-database "$target_dir" >/dev/null 2>&1 || true
    fi
    echo "Chrome integration installed. Fully restart Chrome to use the new environment."
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --deps)
            INSTALL_DEPS=1
            ;;
        --clean)
            CLEAN_BUILD=1
            ;;
        --build-dir)
            shift
            if [ "$#" -eq 0 ]; then
                echo "--build-dir requires a value" >&2
                exit 2
            fi
            BUILD_DIR="$1"
            ;;
        --render-device)
            shift
            if [ "$#" -eq 0 ]; then
                echo "--render-device requires a value" >&2
                exit 2
            fi
            RENDER_DEVICE="$1"
            ;;
        --no-test)
            RUN_TEST=0
            ;;
        --launch-chrome)
            LAUNCH_CHROME=1
            ;;
        --print-chrome-command)
            PRINT_CHROME_COMMAND=1
            ;;
        --chrome-bin)
            shift
            if [ "$#" -eq 0 ]; then
                echo "--chrome-bin requires a value" >&2
                exit 2
            fi
            CHROME_BIN="$1"
            ;;
        --chrome-wayland)
            CHROME_WAYLAND=1
            ;;
        --no-chrome-integration)
            CHROME_INTEGRATION=0
            ;;
        --chrome-integration-only)
            CHROME_INTEGRATION_ONLY=1
            ;;
        --)
            shift
            CHROME_ARGS=("$@")
            break
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if [ "$LAUNCH_CHROME" -eq 1 ] && [ "$PRINT_CHROME_COMMAND" -eq 1 ]; then
    echo "--launch-chrome and --print-chrome-command cannot be used together" >&2
    exit 2
fi

if [ "$CHROME_INTEGRATION_ONLY" -eq 1 ] &&
   { [ "$LAUNCH_CHROME" -eq 1 ] || [ "$PRINT_CHROME_COMMAND" -eq 1 ]; }; then
    echo "--chrome-integration-only cannot be combined with Chrome print/launch modes" >&2
    exit 2
fi

if [ "$PRINT_CHROME_COMMAND" -eq 1 ]; then
    prepare_chrome_command
    print_chrome_command
    exit 0
fi

if [ "$CHROME_INTEGRATION_ONLY" -eq 1 ]; then
    install_chrome_integration
    exit 0
fi

cd "$ROOT_DIR"

if [ "$INSTALL_DEPS" -eq 1 ]; then
    install_deps
fi

for cmd in meson pkg-config; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing command: $cmd. Run ./install.sh --deps or install dependencies manually." >&2
        exit 1
    fi
done

if [ "$CLEAN_BUILD" -eq 1 ]; then
    rm -rf "$BUILD_DIR"
fi

if [ -d "$BUILD_DIR" ]; then
    meson setup "$BUILD_DIR" --prefix=/usr --buildtype=release --reconfigure
else
    meson setup "$BUILD_DIR" --prefix=/usr --buildtype=release
fi

meson compile -C "$BUILD_DIR"
backup_existing_driver
run_sudo meson install -C "$BUILD_DIR"

if [ "$CHROME_INTEGRATION" -eq 1 ]; then
    install_chrome_integration || true
fi

if [ "$RUN_TEST" -eq 1 ]; then
    if command -v vainfo >/dev/null 2>&1 && [ -e "$RENDER_DEVICE" ]; then
        LIBVA_DRIVER_NAME=nvidia vainfo --display drm --device "$RENDER_DEVICE"
    else
        echo "Skipping vainfo smoke test; install vainfo or set RENDER_DEVICE if needed."
    fi
fi

echo
echo "Installed nvidia-vaapi-driver from $ROOT_DIR"
if [ -n "$BACKUP_PATH" ]; then
    echo "Rollback command:"
    echo "  sudo install -m 0755 '$BACKUP_PATH' '$(driver_dir)/nvidia_drv_video.so'"
fi

if [ "$LAUNCH_CHROME" -eq 1 ]; then
    prepare_chrome_command
    launch_chrome
elif prepare_chrome_command 2>/dev/null; then
    echo
    echo "Chrome launch command (add URLs or profile flags after --):"
    print_chrome_command
else
    echo
    echo "Chrome/Chromium was not found; use --chrome-bin PATH to print or launch a command."
fi
