# nvidia-vaapi-driver

This is an VA-API implementation that uses NVDEC as a backend. This implementation is specifically designed to be used by Firefox for accelerated decode of web content, and may not operate correctly in other applications.

# Table of contents

- [nvidia-vaapi-driver](#nvidia-vaapi-driver)
- [Table of contents](#table-of-contents)
- [Codec Support](#codec-support)
- [Installation](#installation)
  - [Quick install from this fork](#quick-install-from-this-fork)
  - [Packaging status](#packaging-status)
  - [Building](#building)
  - [Removal](#removal)
- [Configuration](#configuration)
  - [Upstream regressions](#upstream-regressions)
  - [Kernel parameters](#kernel-parameters)
  - [Environment Variables](#environment-variables)
  - [Firefox](#firefox)
  - [Chrome](#chrome)
  - [MPV](#mpv)
  - [Direct Backend](#direct-backend)
- [Testing](#testing)

# Codec Support

Hardware decoding only, encoding is [not supported](/../../issues/116).

| Codec | Supported | Comments |
|---|---|---|
|AV1|:heavy_check_mark:|Firefox 98+ is required.|
|H.264|:heavy_check_mark:||
|HEVC|:heavy_check_mark:|Some distros are shipping Firefox and/or FFMPEG with HEVC support disabled due to patent concerns.|
|VP8|:heavy_check_mark:||
|VP9|:heavy_check_mark:|Requires being compiled with `gstreamer-codecparsers-1.0`|
|MPEG-2|:heavy_check_mark:||
|VC-1|:heavy_check_mark:||
|MPEG-4|:x:|VA-API does not supply enough of the original bitstream to allow NVDEC to decode it.|
|JPEG|:x:|This is unlikely to ever work, the two APIs are too different.|

YUV444 is supported but requires:

* \>= Turing (20XX/16XX)
* HEVC
* Direct backend

To view which codecs your card is capable of decoding you can use the `vainfo` command with this driver installed, or visit the NVIDIA website [here](https://developer.nvidia.com/video-encode-and-decode-gpu-support-matrix-new#geforce).

# Installation

To install and use `nvidia-vaapi-driver`, follow the steps in installation and configuration. It is recommended to follow testing as well to verify hardware acceleration is working as intended.

**Requirements**

* NVIDIA driver series 470 or 500+

## Quick install from this fork

This fork's `main` branch is intended to match the locally tested AoTofu driver build. If the repository is private, clone it with a GitHub account that has access:

```sh
git clone git@github.com:AoTofu/nvidia-vaapi-driver.git
cd nvidia-vaapi-driver
./install.sh --deps --clean
```

The installer builds the driver, backs up any existing `nvidia_drv_video.so`, installs the new driver into libva's driver directory, runs a `vainfo` smoke test when possible, and automatically enables the driver in installed Chrome/Chromium desktop launchers for the current user. Existing custom launchers are preserved and backed up before the required environment and flags are merged. Fully close and reopen Chrome after installation. To skip dependency installation:

```sh
./install.sh --clean
```

The installer prints a rollback command if it replaced an existing driver. No additional Chrome command is required after the normal quick-install steps. Use `--no-chrome-integration` if the installer should leave desktop launchers unchanged. When Chrome or Chromium is installed, the installer also prints a reusable command that selects this VA-API driver. To print that command without rebuilding or installing anything:

```sh
./install.sh --print-chrome-command
```

## Packaging status

<p align="top"><a href="https://repology.org/project/nvidia-vaapi-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/nvidia-vaapi-driver.svg" alt="repology"><a href="https://repology.org/project/libva-nvidia-driver/versions"><img src="https://repology.org/badge/vertical-allrepos/libva-nvidia-driver.svg" alt="repology" align="top" width="%"></p>

[pkgs.org/nvidia-vaapi-driver](https://pkgs.org/search/?q=nvidia-vaapi-driver) [pkgs.org/libva-nvidia-driver](https://pkgs.org/search/?q=libva-nvidia-driver)

openSUSE: [1](https://software.opensuse.org/package/nvidia-vaapi-driver), [2](https://software.opensuse.org/package/libva-nvidia-driver).

Feel free to add your distributions package in an issue/PR, if it isn't on these websites.

## Building

You'll need `meson`, the `gstreamer-plugins-bad` library, and [`nv-codec-headers`](https://git.videolan.org/?p=ffmpeg/nv-codec-headers.git) installed.

| Package manager | Packages                                        | Optional packages for additional codec support |
|-----------------|-------------------------------------------------|------------------------------------------------|
| pacman          | meson gst-plugins-bad ffnvcodec-headers         |                                                |
| apt             | meson gstreamer1.0-plugins-bad libffmpeg-nvenc-dev libva-dev libegl-dev libdrm-dev | libgstreamer-plugins-bad1.0-dev   |
| yum/dnf         | meson libva-devel gstreamer1-plugins-bad-freeworld nv-codec-headers libdrm-devel | gstreamer1-plugins-bad-free-devel |

Then run the following commands:

```sh
meson setup build --buildtype=release
meson compile -C build
sudo meson install -C build
```

## Removal

By default the driver installs itself as `/usr/lib64/dri/nvidia_drv_video.so` (this might be `/usr/lib/x86_64-linux-gnu/dri/nvidia_drv_video.so` on some distros). To uninstall the driver, simply remove this file. In addition, this file is usually symlinked to `/usr/lib64/dri/vdpau_drv_video.so` (or `/usr/lib/x86_64-linux-gnu/dri/vdpau_drv_video.so`) if the VDPAU to VA-API driver is installed, so this symlink will need to be restored for that driver to work normally again.

# Configuration

## Upstream regressions

The EGL backend is broken on driver versions 525 or later due to a regression. Users running these drivers should use the [direct backend](#direct-backend) instead.

For more information read the [upstream bug report](https://forums.developer.nvidia.com/t/cueglstreamproducerconnect-returns-error-801-on-525-53-driver/233610) or [issue #126](/../../issues/126).

## Kernel parameters

This library requires that the `nvidia_drm` kernel module is [configured with the parameter](https://wiki.archlinux.org/title/Kernel_parameters) `nvidia-drm.modeset=1`

## Environment Variables

Environment variables used to control the behavior of this library.

| Variable | Purpose |
|---|---|
| `NVD_LOG` | Used to control logging. `1` to log to stdout, anything else to append to the given file. |
| `NVD_MAX_INSTANCES` | Controls the maximum concurrent instances of the driver will be allowed per-process. This option is only really useful for older GPUs with not much VRAM, especially with Firefox on video heavy websites. |
| `NVD_BACKEND` | Controls which backend this library uses. Either `egl`, or `direct` (default). See [direct backend](#direct-backend) for more details. |
| `NVD_EXPORT_LAYOUT` | Direct-backend dma-buf layout: `auto` (default), `per-plane-natural`, `per-plane-shared-modifier`, or `packed`. `auto` uses shared-modifier per-plane objects for Chromium-family GPU processes and natural per-plane modifiers for other clients. `packed` remains available for older importers. |
| `NVD_SINGLE_BUFFER` | Legacy compatibility override. `1` selects `packed`; `0` selects `per-plane-natural`. `NVD_EXPORT_LAYOUT` takes precedence. |
| `NVD_MAX_DETACHED_BACKING_IMAGE_BYTES` | Upper bound (in bytes) on the size of the detached backing-image cache used by the direct backend to recycle decode surfaces across stream switches. Lower this on low-VRAM GPUs to reduce memory usage at the cost of more re-allocation when streams change. Set to `0` to disable detached caching. Default: scales with the GPU — total VRAM / 64 (~1.6%), clamped to 64 MiB–512 MiB; falls back to `134217728` (128 MiB) if the VRAM size cannot be queried. |
| `NVD_MAX_DETACHED_BACKING_IMAGES` | Upper bound on the number of cached detached backing images. Set to `0` to disable detached caching. Default: `16`. |
| `NVD_MEMORY_BUDGET_BYTES` | Optional shared GPU-memory budget for detached backing images and VideoProc GPU scratch. The driver prunes reclaimable cache entries before optional scratch growth; essential active decode surfaces are never evicted. `0` disables the shared budget. Default: `0`. |
| `NVD_DECODE_SURFACES` | Decode-surface selection mode. `auto` derives the count from codec reference requirements and client render targets; a positive integer forces a count, subject to the min/max bounds below. Unset keeps the compatibility value of 32 while statistics report the automatic candidate for validation. |
| `NVD_DECODE_SURFACES_MIN` | Lower bound for automatic or forced decode-surface selection. Default: `2`. |
| `NVD_DECODE_SURFACES_MAX` | Upper bound for decode-surface selection. NVIDIA picture indices are limited to 32, so larger values are clamped. Default: `32`. |
| `NVD_VIDEOPROC_SCRATCH_MAX_BYTES` | Upper bound, separately, for fallback VideoProc GPU and CPU scratch buffers. Direct CUDA-array conversion does not allocate these buffers. Set to `0` to disable scratch-backed fallbacks. Default: `268435456` (256 MiB). Idle fallback scratch is released after 120 consecutive CUDA-processed frames. |
| `NVD_BUFFER_POOL_MAX_BYTES` | Upper bound for reusable VA buffer/image host allocations. Six size classes from 4 KiB through 4 MiB are retained; oversize allocations are freed immediately. Set to `0` to disable retention. Default: `67108864` (64 MiB). |
| `NVD_HOST_BUFFER_TRIM_THRESHOLD_BYTES` | Capacity above which an underused per-context codec bitstream/slice buffer becomes eligible for shrinking. `0` disables trimming. Default: `8388608` (8 MiB). |
| `NVD_HOST_BUFFER_TRIM_FRAMES` | Consecutive frames using at most 25% of an oversized codec buffer before it is shrunk to twice the current need (with conservative floor capacity). Default: `120`. |
| `NVD_STATS` | Enables performance counters. `final` logs only at context/driver teardown; `1` logs every 120 decoded pictures; a larger integer selects that interval. A final snapshot is always emitted when the driver terminates. Leave unset for benchmark runs that do not need instrumentation. |
| `NVD_STATS_LOG` | Appends `NVD_STATS` snapshots to this file instead of the normal log output. Statistics include copy bytes, host fallbacks, resolve-queue pressure, backing allocation time/cache usage, GPU-side security-clear bytes/fallbacks, current and peak backing/scratch memory, codec host-buffer trims, VideoProc time, object lookup cost, and codec copy volume. |

## Firefox

Due to license, Firefox on Linux does not support HEVC till now.
To use the driver with firefox you will need at least Firefox 96, `ffmpeg` compiled with vaapi support (`ffmpeg -hwaccels` output should include vaapi), and the following config options need to be set in the `about:config` page:

| Option | Value | Reason |
|---|---|---|
| media.ffmpeg.vaapi.enabled | true | Required until Firefox 137, enables the use of VA-API. |
| media.hardware-video-decoding.force-enabled | true | Required since Firefox 137, enables hardware acceleration. |
| media.rdd-ffmpeg.enabled | true | Required, default on FF97. Forces ffmpeg usage into the RDD process, rather than the content process. |
| media.av1.enabled | false | Optional, disables AV1. If your GPU doesn't support AV1, this will prevent sites using it and falling back to software decoding. |
| gfx.x11-egl.force-enabled | true | Required, this driver requires that Firefox use the EGL backend. It may be enabled by default. It is recommended to test it with the `MOZ_X11_EGL=1` environment variable before enabling it in the Firefox configuration. |
| widget.dmabuf.force-enabled | true | Required on NVIDIA 470 series drivers. Note that Firefox isn't coded to allow DMA-BUF support without GBM support, so it may not function completely correctly when it's forced on. |

In addition the following environment variables need to be set. For permanent configuration `/etc/environment` may suffice.

| Variable | Value | Reason |
|---|---|---|
| MOZ_DISABLE_RDD_SANDBOX | 1 | Disables the sandbox for the RDD process that the decoder runs in. |
| LIBVA_DRIVER_NAME | nvidia | Required for libva 2.20+, forces libva to load this driver. |
| __EGL_VENDOR_LIBRARY_FILENAMES | /usr/share/glvnd/egl_vendor.d/10_nvidia.json | Required for the 470 driver series only. It overrides the list of drivers the glvnd library can use to prevent Firefox from using the MESA driver by mistake. |
| CUDA_DISABLE_PERF_BOOST | 1 | Optional. Requires NVIDIA driver >= 580.105.08. Disables the forced power boost the GPU gets when CUDA is activated. This should reduce the power usage when decoding video. This setting is the equivilent of the 'CUDA Force P2' NVIDIA Profile Inspector setting on Windows. |

When libva is used it will log out some information, which can be excessive when Firefox initalises it multiple times per page. This logging can be suppressed by adding the following line to the `/etc/libva.conf` file:
```
LIBVA_MESSAGING_LEVEL=1
```

If you're using the Snap version of Firefox, it will be unable to access the host version of the driver that is installed.

## Chrome

The installer explicitly selects `NVD_EXPORT_LAYOUT=packed` for Chrome to work around split frames during repeated seeking on tested NVIDIA Wayland configurations. The library's unset/`auto` policy still selects shared-modifier per-plane objects for Chromium-family processes and natural per-plane modifiers for other clients. Close all existing Chrome processes before relaunching, or use a separate `--user-data-dir`, so the new environment reaches the GPU process. With `NVD_LOG=1`, startup logs identify the loaded driver path and selected layout.

For buffers exported before decoding or imported from the client, the driver
finishes the frame copy before returning from `vaEndPicture`. Chrome can reuse
those buffers without another `vaSyncSurface` or export call, and CUDA writes
do not publish an implicit DMA-BUF fence. Waiting only during export can show
stale or partially written frames after seeking. Private decode surfaces keep
their asynchronous resolve path. Failed resolves are reported to the client
instead of exporting the preceding image as a successful frame.

Start the browser with flags similar to:

```sh
LIBVA_DRIVER_NAME=nvidia NVD_BACKEND=direct NVD_EXPORT_LAYOUT=packed google-chrome \
  --enable-features=AcceleratedVideoDecodeLinuxGL,VaapiOnNvidiaGPUs \
  --ignore-gpu-blocklist \
  --use-gl=angle --use-angle=gl
```

On Wayland, also try `--ozone-platform=wayland` or `--ozone-platform-hint=auto`.

The installer can detect `google-chrome-stable`, `google-chrome`, `chromium`, or `chromium-browser` and generate the same command with the direct backend and packed export layout enabled:

```sh
# Print a copy-and-paste command without building or installing.
./install.sh --print-chrome-command --chrome-wayland -- https://example.com/

# Install the driver, run the normal smoke test, then launch Chrome.
./install.sh --launch-chrome --chrome-wayland -- https://example.com/
```

Use `--chrome-bin /path/to/chrome` (or the `CHROME_BIN` environment variable) for a custom or unpacked browser. Arguments after `--`, including URLs and `--user-data-dir`, are passed to Chrome without re-parsing. The generated environment explicitly sets `LIBVA_DRIVER_NAME=nvidia`, the installed `LIBVA_DRIVERS_PATH`, `NVD_BACKEND=direct`, and `NVD_EXPORT_LAYOUT=packed`. Normal installation creates current-user desktop overrides with the same settings; it does not change system desktop files or global browser policy. Fully close existing Chrome processes before using the command, or provide a separate `--user-data-dir`; an already-running browser may reuse its old environment.

Chrome and Chromium receive one dma-buf object per plane with a shared DRM modifier, as required by Chromium's `vaapi_wrapper`. Other VA clients continue to receive separate plane objects with their natural per-plane modifiers; this avoids changing the block-height behavior needed by per-plane importers.

### Recovering a launcher after a Chrome update

Normal integration preserves custom launch commands. If a separate Chrome
binary hotpatcher becomes incompatible after a browser update, this also
preserves the failing wrapper. This driver's hardware **decode** support does
not require the H.264 **encode** hotpatch from `chrome-vaapi-hotpatch`.

To replace that wrapper with the installed browser's launch command:

```sh
./install.sh --chrome-integration-only --restore-chrome-launcher google-chrome.desktop
```

Recovery uses the matching desktop file from `XDG_DATA_DIRS`, backs up the
existing user entry (even when already managed), and reapplies the NVIDIA
decode settings. It restores all actions, including incognito, from that
template. Custom commands, flags and other launcher customizations are replaced;
browser profiles and other desktop entries are untouched. A missing template
or backup failure leaves the current entry unchanged. Flatpak templates are
unsupported. Restore the printed backup file to roll back.

Distribution updates can also replace a manually installed driver at the
package-owned path. To use a previously built driver without reinstalling it,
prefix the recovery command with `NVD_DRIVER_DIR=/absolute/path/to/build`.
Keep that directory available: Chrome will load `nvidia_drv_video.so` from it.
This changes only the selected browser launcher, not the system-wide driver.

## MPV

Currently this only works with a recent MPV version (at least 0.36.0).

There's no real reason to run it with mpv except for testing, as mpv already supports using nvdec directly. The `test.sh` script will run mpv with the file provided and various environment variables set to use the newly built driver

## Direct Backend

The direct backend is a experimental backend that accesses the NVIDIA kernel driver directly, rather than using EGL to share the buffers. This allows us
a greater degree of control over buffer allocation and freeing.

The direct backend has been tested on a variety of hardware from the Kepler to Lovelace generations, and seems to be working fine. If you find any compatibility issues, please leave a comment [here](/../../issues/126).

Given this backend accesses the NVIDIA driver directly, via NVIDIA's unstable API, this module is likely to break often with new versions of the kernel driver. If you encounter issues using this backend raise an issue and including logs generated by `NVD_LOG=1`.

This backend uses headers files from the NVIDIA [open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules)
project. The `extract_headers.sh` script, along with the `headers.in` file list which files we need, and will copy them from a checked out version of the NVIDIA project to the `nvidia-include` directory. This is done to prevent everyone needing to checkout that project.

# Performance diagnostics

Set `NVD_STATS=final NVD_STATS_LOG=/tmp/nvd-stats.log` for a diagnostic run.
The final `Timing` records contain fixed 32-bin logarithmic histograms of CPU
wall time, starting at 1 microsecond and doubling each upper boundary. The last
bin is unbounded (`UINT64_MAX` for a percentile in that bin). `p50_upper_ns`,
`p95_upper_ns`, and `p99_upper_ns` are bucket upper bounds; inspect `bins` and
sample counts before comparing them. Records are cumulative approximate
snapshots under concurrency, not independent frame samples. Context records
identify the process, driver instance, context, decoder format/bit depth, and shared/private status at submission;
late exports do not retroactively reclassify a job. No per-frame trace is stored.

Decode submission, queue residence, map, export (including allocation/copy),
unmap, shared EndPicture wait, copy completion wait, lifetime-lock acquisition,
security clear, and AV1 tile registration/validation are measured separately.
Queue residence starts at insertion **after** producer backpressure. The legacy
queue wait counter measures producer backpressure; the legacy depth/high-water
counters aggregate the driver, while `ContextStats` reports a single queue's
high water and capacity. CPU waits can overlap GPU work and each other. Map time
includes NVDEC waiting; these durations are not GPU execution timings and must
not be added to infer total frame latency. Statistics-disabled paths perform no
timing clock reads. Numeric `NVD_STATS` retains synchronous periodic logging,
which can affect results; use `final` for diagnostics and leave all statistics
disabled for the performance comparison.

`MemoryStats` separates logical backing views, unique driver-owned backing
allocations, borrowed views, and external import views. External import views exclude borrowed wrappers and are
not a deduplicated physical-memory gauge. Owned GPU bytes also include VPP and
security-clear scratch. Host bytes include live VA-buffer capacities, retained
pool blocks, VPP scratch, temporary clear staging, and context appendable-buffer
capacities observed at VA calls/teardown (not transient realloc peaks). Legacy
`tracked_vram_equivalent_bytes` remains a logical-view estimate for compatibility;
`tracked_ram_equivalent_bytes` now covers the listed host allocations. Driver
objects, codec-private allocations, allocator overhead, NVDEC/CUDA internals,
and process-wide physical VRAM/RSS are not covered. Internal allocations are
reported as `unknown`. `memory_budget_bytes` is a reclaimable-cache budget;
it is not a hard cap on total VRAM. Quarantined resources remain owned.

`NVD_BENCH_DIAGNOSTIC=1` explicitly enables final statistics in `bench-seek.mjs`
and records their path and measurement mode in the JSON. Use separate diagnostic
runs; the default benchmark still removes logging/statistics from child Chrome.

The direct backend generates 16-bit YUV clear patterns with optional
[`cuMemsetD16Async`](https://docs.nvidia.com/cuda/cuda-driver-api/cuda_driver_api/group__CUDA__MEM.html)
from the already loaded CUDA library; no newer ffnvcodec loader is required.
Missing functions or submission failures retain host staging after a successful
drain. Failed completion checks quarantine resources. Multi-plane GPU clears
reserve the largest scratch chunk first, issue every plane on one protected
stream, and finish once per image (`security_clear_syncs` also counts scratch
resize drains). Clear scratch can retain up to 8 MiB for supported surface sizes.
RGB allocation follows its existing `alloc_image` initialization and does not
use this YUV clear path.

AV1 tile intervals use a lazy, reusable AVL index bounded by 4096 nodes. It
preserves immediate overlap errors, coordinate checks, empty/range validation,
and NVDEC tile ordering. The index stores original bitstream coordinates;
compaction changes only the decoder offset view after all tiles are registered.
EndPicture requires a complete tile set before calling NVDEC. The index adds a
small host allocation (256 bytes for small tile sets, approximately 73 KiB at
4096 tiles with the current growth policy); this is counted in context buffers.
The 4096-node cap is an internal validation bound, not a claim about playable
4096-tile videos. Benchmark validation in isolation with
`build-perf/bench-av1-tiles TILE_COUNT FRAME_COUNT`. GPU clear benchmarks use
`build-perf/test-security-clear --bench p010 60 3840 2160 [THREADS]` with
`NVD_RUN_GPU_TESTS=1`, the selected `LIBVA_DRIVERS_PATH`, and an explicit layout.
Their JSON reports fresh-allocation/export time and checks every visible pixel;
this is not steady-state video latency or an energy measurement.

# Testing

Run the CPU regressions and opt-in GPU state checks with:

```sh
meson setup build-test --buildtype=debugoptimized -Dgpu_tests=true
meson test -C build-test --print-errorlogs
```

The Chrome seek regression uses Node 22+, ffmpeg and a running Wayland session.
The fixture generator validates every encoded frame in software and writes a
hash-bound JSON manifest with its frame rate and marker positions. Use `.mp4`
for `--codec h264`, or `.webm` for `vp9` / `av1`. Existing files are not replaced.

```sh
python3 tests/generate-seek-video.py /tmp/seek-4k60.webm --width 3840 --height 2160 --fps 60
NVD_BENCH_MODE=burst NVD_BENCH_REQUIRE_CORRECT=1 \
  node tests/bench-seek.mjs build-test /tmp/seek-4k60.webm /tmp/seek-result.json 150
```

`NVD_BENCH_MODE` selects paused random seeks (default `paused`), random seeks
during playback (`playing`), actual CDP arrow-key input (`arrows`), or four
arrow presses spaced 10 ms apart (`burst`). Arrow keys seek by five seconds
and wrap at the fixture ends. Each run warms up with ten seeks/bursts, then
checks the requested count. Markers repeat down the image to detect interior
splits as well as top/bottom mismatches. `NVD_BENCH_REQUIRE_CORRECT=1` fails on
an incorrect frame, missing keyboard events, software fallback or the wrong
loaded driver. `NVD_BENCH_SOFTWARE=1` selects a software control run.

Results include individual frame IDs, callback times, decoder identity and
the mapped driver path/hash. Latency measures the frame callback after the
first seek/key request; it includes the whole key burst. These synthetic
fixtures and canvas readbacks do not measure physical display tearing or
exercise a streaming site's adaptive quality switches.

To verify that the driver is being used to decode video, you can use nvidia-settings or nvidia-smi.

- nvidia-settings

  By selecting the relevant GPU on the left of the nvidia-settings window, it will show `Video Engine Utilization` on the right. While playing a video this value should be non-zero.

- nvidia-smi

  Running `nvidia-smi` while decoding a video should show a Firefox process with `C` in the `Type` column. In addition `nvidia-smi pmon` will show the usage of the decode engine per-process, and `nvidia-smi dmon` will show the usage per-GPU. When using nvidia open gpu kernel modules, the usage of the decode engine may not be displayed correctly.
