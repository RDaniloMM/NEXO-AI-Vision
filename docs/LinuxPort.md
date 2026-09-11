# SPDX-License-Identifier: AGPL-3.0-only

# Linux Port — common Windows/Linux architecture

Target platform: **DGX OS / Ubuntu 22.04+** (CI pins `ubuntu-24.04`, GCC 13).
GUI direction: **Qt6** (the same launcher and live viewer on Windows and UNIX).

The shared-core boundary and the small OS adapters are documented in
[`Architecture.md`](Architecture.md). This file keeps the Linux build and
distribution details.

## Quick install (Ubuntu/DGX OS x86_64)

From the repository checkout, run the public installer:

```bash
sudo ./installer/linux/install.sh --yes --version 0.1.0
```

It detects a compatible Ubuntu/DGX OS host, asks before installing the required
APT packages (unless `--yes`/`-y` is used), provisions the pinned native
dependencies, builds the Qt6 `.deb`, and installs it. The default version is
`0.1.0`; use `--version X.Y.Z` to override it. The `.deb` installs the
application under `/opt/nexoai-vision`; start it from the applications menu or
run `nexoai-vision`. An existing package can be installed without compiling:

```bash
sudo ./installer/linux/install.sh --package ./NexoAIVision.deb
```

Use `--no-install` to build only. GPU drivers are never installed or replaced;
TensorRT/CUDA packaging requires the explicit `--with-tensorrt` option and a
target-compatible SDK.

## What Fase 2b does

When `CUAJONE_BUILD_QT_VIEWER=ON`, `NexoAIVision` uses one Qt6 Widgets window
for live analytics on both Windows and Linux. HighGUI remains only when the Qt
viewer option is off, and the headless path does not construct a Qt application.

- keeps sequential batch-1 inference, capture/reconnect behavior, telemetry,
  evidence output, local video, RTSP, and contract 1.0.0 unchanged;
- composes the existing 1280x720 canvas and passes it to Qt as a copied
  `QImage`, so OpenCV frames cannot be mutated while Qt paints;
- preserves pixel-perfect tile rendering: frames are uniformly downscaled
  when needed, never upscaled to fill a tile, and are centered on black;
- keeps detections, skeletons, PPE labels, performance metrics, reconnect
  banners, camera hover names, and the keyboard help strip at canvas
  resolution before Qt presents the image;
- handles focus, `+`/`-`, `[`/`]`, `0`, divider dragging, Q/Escape, and the
  two-second hover-name timeout through Qt mouse and keyboard events;
- pumps `QApplication::processEvents()` from the existing monitor loop rather
  than starting a second event loop. This keeps inference sequential and avoids
  a viewer thread touching OpenCV matrices during `paintEvent`.

The tradeoff is that event delivery is polled at the same bounded display
cadence as composition instead of using a Qt signal/slot worker. This is
deliberate: it minimizes concurrency changes in the inference/evidence path.
The Qt viewer is optional; without Qt6 the build retains the legacy HighGUI
fallback for compatibility.

## What Fase 2a does

The optional `cuajone_qt_launcher` target (output `NexoAIVisionLauncher`) is a
Qt6 Widgets application for Windows, DGX OS, and Ubuntu 22.04/24.04. It is enabled with
`-DCUAJONE_BUILD_QT_LAUNCHER=ON`; missing Qt6 is reported as a status message
unless `CUAJONE_REQUIRE_QT=ON`, in which case configuration fails clearly.

- manages multi-selected camera profiles with New/Edit/Delete/Select all;
- accepts an optional local MP4, AVI, MOV, or MKV file and an output folder;
- provides Validate, Start, Stop, runtime status, log path, and Open log;
- keeps PPE thresholds and advanced compute, image-size, stream, telemetry,
  and acceleration controls under the `Menú` menu;
- launches the sibling `NexoAIVision` CLI through `QProcess`, capturing and
  redacting stdout/stderr in an append-only session log;
- resolves config/data/log roots through Qt `QStandardPaths` and the platform
  path adapter; no Windows or Linux system path is embedded in the UI;
- uses Windows Credential Manager or Linux session-only secret storage and
  never writes camera passwords to profile files.

The Qt launcher reuses `launcher_support` and `buildLaunchPlan`; the Win32
interface in `native/src/launcher/launcher.cpp` remains a Windows-only
fallback target and is not built when the Qt launcher is selected.

## What Fase 1 does

Builds the POSIX runtime on UNIX via `native/CMakeLists.txt`:

- `cuajone_runtime` (`capture`, `engine_pipeline`, `evidence`,
  `model_manifest`, `onnx_session`) against the Linux ONNX Runtime 1.25.0
  package (`onnxruntime-linux-x64`, `ONNXRUNTIME_ROOT`), with build-tree
  RPATH so the CLI and tests find `libonnxruntime.so` without an install
  step;
- the `NexoAIVision` CLI (`cuajone_native`, no Win32 icon resource, no
  launcher);
- runtime tests on UNIX: `cuajone_evidence_tests`,
  `cuajone_capture_diagnostics_tests`, `cuajone_onnx_tests`
  (plus the portable `cuajone_cpu_tests` from Fase 0).

Still excluded on UNIX: the Win32 launcher UI, installer custom action
(`msiquery.h`), Windows `dumpbin` closure copy, `delayimp`/`DELAYLOAD`, and the
opt-in GPU integration tests. The shared launcher support library is now
portable. Linux TensorRT is an explicit distribution-build option; ordinary
CPU/CI configures remain CPU-only unless `TENSORRT_ROOT` is supplied.

### Porting notes by unit

- `compute.cpp`: Linux probes `libcuda.so.1` with `dlopen`/`dlsym`
  (`cuInit`, `cuDriverGetVersion`, `cuDeviceGetCount`, `cuDeviceGetName`,
  `cuDeviceComputeCapability`) — same thresholds (driver ≥ 12.9, SM ≥ 7.5)
  and status semantics as Windows, no DXGI adapter enumeration, no NVML.
  `installedComputeBackend` reads `NEXOAI_COMPUTE_MODE` (wins when set) or
  the `ComputeMode=` line of `/etc/nexoai-vision/config`; absent means
  unmanaged (`nullopt`).
- `capture.cpp`: RTSP reachability via `getaddrinfo` + non-blocking
  `connect` + `poll` + `SO_ERROR`, mapping `ENETUNREACH`/`ETIMEDOUT`/
  `ECONNREFUSED` exactly like the Winsock branch (`getaddrinfo` itself has
  no timeout; the TCP phase honors the preflight deadline). New
  `--video-acceleration vaapi` maps to `VIDEO_ACCELERATION_VAAPI` (present
  since OpenCV 4.5, so apt 4.6 is fine); a failed VAAPI open retries
  software decoding via the existing fallback.
- `main.cpp`: no isolated CUDA-warmup child on Linux — the warmup runs
  in-process. The child process existed to contain an ORT-CUDA 1.25 access
  violation specific to the GTX 1650 Ti on Windows; porting `fork`+`exec`
  isolation is deferred until a Linux crash needs containing. `getenv`
  replaces `_dupenv_s`, `gmtime_r` replaces `gmtime_s`, the window icon is
  a Windows-only no-op, signals stay `SIGINT`/`SIGTERM`.
- `model_manifest.cpp`: SHA-256 uses a minimal self-contained FIPS 180-4
  implementation on Linux (zero new dependencies — no OpenSSL); Windows
  keeps BCrypt. Pinned by FIPS known-answer vectors in `onnx_tests.cpp`.
- `evidence.cpp` needed no changes: its `<windows.h>` include and the
  Win32 flush/append paths were already `#ifdef _WIN32`-guarded with a
  POSIX fallback.

## What Fase 0 did

Provides a Linux build base that compiles everything already portable, with
zero changes to ported logic. On UNIX, `native/CMakeLists.txt`:

- builds the portable libraries `cuajone_compute`, `cuajone_contracts`,
  `cuajone_byte_track_upstream`, `cuajone_byte_track_adapter`,
  `cuajone_analytics`, `cuajone_inference`, `cuajone_core`;
- builds and registers `cuajone_cpu_tests` (links only `cuajone_core`;
  needs OpenCV `core`/`imgproc`, no ONNX Runtime);
- skips the Windows-only graph with a clear `message(STATUS)`: Win32
  launcher + support lib (`<windows.h>`, `.rc`, `version.lib`), native
  runtime (`capture.cpp` needs `<winsock2.h>`, `evidence.cpp` needs
  `<windows.h>`, `model_manifest.cpp` needs `<windows.h>`/`<bcrypt.h>`),
  installer custom action (`msiquery.h`), `dumpbin` closure copy,
  `delayimp`/`DELAYLOAD` link options;
- keeps TensorRT disabled unless a build explicitly supplies `TENSORRT_ROOT`;
- accepts OpenCV **4.6+** (apt `libopencv-dev` on Ubuntu 24.04 ships 4.6;
  Windows keeps requiring 4.8);
- emits no `FATAL_ERROR` for the skipped scope; missing real dependencies
  (OpenCV, pinned ByteTrack/Eigen sources) still fail with actionable
  messages.

The Fase 0 exception for `evidence_tests`, `onnx_tests`, and
`capture_diagnostics_tests` was removed by the Fase 1 POSIX runtime port.
`launcher_tests` and the `.rc` fixtures remain Win32-only; the Qt6 launcher
is covered separately by the Fase 2a smoke build.

There is no `CUAJONE_ENABLE_ONNX`-style flag in `native/CMakeLists.txt`
(verified): the Fase 0 CI job simply builds the targets that compile
without ORT.

## Empty preset cache variables

The `linux-release` preset passes `ONNXRUNTIME_ROOT=""`,
`OpenCV_DIR=""`, `TENSORRT_ROOT=""`:

- `OpenCV_DIR=""` → CMake system search finds apt `libopencv-dev`
  (`/usr/lib/x86_64-linux-gnu/cmake/opencv4`). Set it only for a custom
  OpenCV build.
- `ONNXRUNTIME_ROOT=""` → points at the Linux ORT package in Fase 1 (default
  in `activate-native.sh`:
  `.tools/native/linux-onnxruntime-1.25.0`).
- `TENSORRT_ROOT=""` → keeps ordinary Linux builds CPU-only. A GPU package
  uses `--with-tensorrt` and defaults to `/opt/tensorrt`.

## Advanced: manual native build

The commands below are for development and troubleshooting. They are not the
normal installation path; use the quick installer above for an operator install.

```bash
source native/activate-native.sh   # prints apt prerequisites + env summary
cmake --preset linux-release       # run from native/
cmake --build --preset linux-release
ctest --preset linux-release --output-on-failure
```

For the optional Qt6 launcher, install `qt6-base-dev` and configure a
launcher-only tree (the runtime CLI remains the sibling executable when the
two trees are used together):

```bash
sudo apt-get install -y qt6-base-dev
cmake -S native -B .tools/native/build/presets/linux-qt -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCUAJONE_BUILD_RUNTIME=OFF \
  -DCUAJONE_BUILD_TESTS=OFF -DCUAJONE_BUILD_LAUNCHER=OFF \
  -DCUAJONE_BUILD_QT_LAUNCHER=ON
cmake --build .tools/native/build/presets/linux-qt --target cuajone_qt_launcher
QT_QPA_PLATFORM=offscreen .tools/native/build/presets/linux-qt/NexoAIVisionLauncher --help
```

For the Qt6 live viewer, use a runtime tree. `ONNXRUNTIME_ROOT` must point to
the provisioned Linux ONNX Runtime 1.25.0 package; the command below keeps
tests and the Windows launcher disabled:

```bash
source native/activate-native.sh
sudo apt-get install -y qt6-base-dev
cmake -S native -B .tools/native/build/presets/linux-qt-viewer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCUAJONE_BUILD_RUNTIME=ON -DCUAJONE_BUILD_TESTS=OFF \
  -DCUAJONE_BUILD_LAUNCHER=OFF -DCUAJONE_BUILD_QT_VIEWER=ON \
  -DONNXRUNTIME_ROOT="$ONNXRUNTIME_ROOT"
cmake --build .tools/native/build/presets/linux-qt-viewer --target cuajone_native
```

Run the installed runtime normally on Ubuntu/DGX OS with `--show` and
the same model/source arguments used by the CLI. For a display-less smoke
check, use `QT_QPA_PLATFORM=offscreen NexoAIVision --help`; `--help` exits
before model loading and does not exercise live capture.

CI (`.github/workflows/linux-build.yml`) uses
`installer/linux/provision-deps.sh` on `ubuntu-24.04` for the pinned
ByteTrack/Eigen sources and Linux ONNX Runtime 1.25.0 package, then builds the
exact CLI/viewer targets and runs the offscreen `--help` smoke checks. The
separate `linux-qt` job covers the launcher-only target. `linux-package` builds
the Qt6 launcher/viewer, runs CPack DEB and creates the tarball, then uploads
only workflow artifacts. The Windows provisioning script consumes the same
dependency lock file without changing the Windows installer flow.

## Fase 3 distribution

The Linux distribution uses the same private layout for DEB and tarball:

```text
/opt/nexoai-vision/
  bin/NexoAIVision
  bin/NexoAIVisionLauncher
  lib/libonnxruntime.so*
  lib/                         # optional CUDA/TensorRT user-space libraries
  models/                      # copied only when the repository has portable models/
  run.sh
  README-Linux.md
```

The DEB also installs the desktop entry under `/usr/share/applications`, the
SVG icon under `/usr/share/icons/hicolor/scalable/apps`, and an example under
`/etc/nexoai-vision/config.example`. Its `postinst` creates
`/etc/nexoai-vision/config` only when absent, with `ComputeMode=auto`. It does
not compile engines, install drivers, or store credentials. The optional probe
can be requested explicitly with `NEXOAI_VALIDATE_HARDWARE=1`; installation
does not run a GPU build automatically. Any `*.engine`, `*.plan`, or `*.trt`
files under a repository `models/` directory are excluded from the package.

### Exact local package commands (advanced)

Install the CPU/GUI build prerequisites on Ubuntu 22.04/24.04 or DGX OS:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config \
  dpkg-dev libopencv-dev qt6-base-dev git patch unzip curl ca-certificates
```

Provision the pinned ByteTrack/Eigen sources and Linux ONNX Runtime 1.25.0 with
the shared helper, then run:

```bash
installer/linux/provision-deps.sh
installer/linux/build-dist.sh \
  --build-dir .tools/native/build/linux-package \
  --output-dir .tools/native/dist \
  --version 0.1.0 \
  --with-qt
```

The technical wrapper enables the Qt6 launcher and viewer by default, uses
`CUAJONE_CMAKE_BIN` when that variable points to CMake or its directory, and
does not install apt packages itself. It provisions its local dependencies
automatically and outputs:

```text
.tools/native/dist/nexoai-vision-0.1.0-linux-x86_64.deb
.tools/native/dist/nexoai-vision-0.1.0-linux-x86_64.tar.gz
```

For a direct development build without Qt, configure with
`-DCUAJONE_BUILD_QT_LAUNCHER=OFF -DCUAJONE_BUILD_QT_VIEWER=OFF`; HighGUI is a
development fallback, not the default distribution GUI.

Install and run the DEB:

```bash
sudo apt-get install -y ./.tools/native/dist/nexoai-vision-0.1.0-linux-x86_64.deb
sudo env NEXOAI_VALIDATE_HARDWARE=1 /opt/nexoai-vision/bin/NexoAIVision --hardware-probe-json
/opt/nexoai-vision/bin/NexoAIVisionLauncher
```

The tarball needs no root access:

```bash
tar -xzf .tools/native/dist/nexoai-vision-0.1.0-linux-x86_64.tar.gz
./opt/nexoai-vision/run.sh
```

`run.sh` exports `LD_LIBRARY_PATH` for the private `lib` directory and
`XDG_CONFIG_HOME`, `XDG_DATA_HOME`, and `XDG_STATE_HOME` inside the extracted
tree. It also defaults `NEXOAI_COMPUTE_MODE=auto`; override it in the shell for
an operator-selected mode. The DEB uses the same `/opt/nexoai-vision` tree.
Do not put passwords or tokens in these files.

### GPU package and DGX caveats

The optional GPU package is built on the target-compatible Linux toolchain:

```bash
TENSORRT_ROOT=/opt/tensorrt CUDA_ROOT=/usr/local/cuda \
  installer/linux/build-dist.sh \
    --build-dir .tools/native/build/linux-gpu-package \
    --output-dir .tools/native/dist-gpu \
    --version 0.1.0 \
    --with-qt --with-tensorrt
```

`--with-tensorrt` packages TensorRT and CUDA runtime user-space libraries when
the configured build uses them, but never packages `libcuda.so.1`: the NVIDIA
driver is a host responsibility. The target must expose a compatible
`libcuda.so.1`, driver/API level, GPU architecture, and display stack. TensorRT
engines are hardware-, TensorRT-, CUDA-, and often OS-specific; do not copy a
prebuilt engine from another GPU or operating system. Re-export or rebuild
engines on the target DGX GPU and validate their manifests before use. The
repository contains no production engine bundle, so the package remains
functional for CLI/help/hardware-probe without one.

System dependencies expected by the DEB are libc/libstdc++, Qt6 when the Qt
launcher is present, and the OpenCV core/imgproc/imgcodecs/videoio libraries
(plus HighGUI when the fallback is built). ONNX Runtime is installed in the
private package `lib` directory. CUDA/TensorRT packages do not replace the
host NVIDIA driver.

Package versioning is resolved once by CMake through `CUAJONE_PACKAGE_VERSION`.
The Linux wrapper passes the same `X.Y.Z` to `CUAJONE_FILE_VERSION` and
`CUAJONE_PRODUCT_VERSION`; the Windows MSI wrapper continues to own the
four-component `version-state.json` revision policy. A Windows four-component
file version is reduced to its first three components by the common package
contract when CPack is used.

Package checks available on any host are limited to shell syntax and static
inspection. A Windows checkout cannot claim Linux compilation or execution;
the authoritative package configure/build and smoke checks run on Ubuntu/DGX.

## Roadmap

- **Fase 1 — POSIX runtime port** (done): `capture` (BSD sockets
  reachability + VAAPI), `compute` (`dlopen` CUDA probe + config-file
  backend), `evidence`/`model_manifest` (portable crypto + filesystem),
  Linux ONNX Runtime package, `evidence_tests`, `onnx_tests`,
  `capture_diagnostics_tests` on UNIX.
- **Fase 2a — Qt6 launcher** (done): `NexoAIVisionLauncher`, local video
  input, profile/PPE/advanced dialogs, XDG configuration, safe password
  handling, QProcess runtime control, and Linux CI smoke coverage.
- **Fase 2b — Qt6 mosaic viewer** (done): optional Qt6 presentation in
  `NexoAIVision`, canvas-resolution overlays, pixel-perfect tiles, Qt input
  events, and Linux CI build/help smoke coverage.
- **Fase 3 — Linux distribution** (implemented): reproducible Qt6 DEB/tarball
  packaging, private RPATH-aware libraries, desktop integration, config
  bootstrap, optional target-built CUDA/TensorRT user-space runtime, and
  Ubuntu 24.04 artifact CI. Full DGX engine validation remains a target-host
  operation.
