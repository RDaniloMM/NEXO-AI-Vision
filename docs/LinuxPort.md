# SPDX-License-Identifier: AGPL-3.0-only

# Linux Port — Fase 1 (POSIX runtime CLI)

Target platform: **DGX OS / Ubuntu 22.04+** (CI pins `ubuntu-24.04`, GCC 13).
GUI direction: **Qt6** (replaces the Win32 launcher in Fase 2).

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

Still excluded on UNIX: Win32 launcher + support lib (Fase 2 Qt6 GUI),
installer custom action (`msiquery.h`), TensorRT/CUDA backends
(`CUAJONE_ENABLE_TENSORRT` forced OFF until Fase 3), `dumpbin` closure
copy, `delayimp`/`DELAYLOAD`, and the opt-in GPU integration tests.

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
- forces `CUAJONE_ENABLE_TENSORRT=OFF` (no CUDA/TRT toolchain yet);
- accepts OpenCV **4.6+** (apt `libopencv-dev` on Ubuntu 24.04 ships 4.6;
  Windows keeps requiring 4.8);
- emits no `FATAL_ERROR` for the skipped scope; missing real dependencies
  (OpenCV, pinned ByteTrack/Eigen sources) still fail with actionable
  messages.

Deliberate deviation from the first scoping note: `evidence_tests` and
`onnx_tests` are **not** portable "as-is" — both link `cuajone_runtime`,
whose translation units include `<windows.h>`/`<bcrypt.h>`/`<winsock2.h>`
unconditionally. They are excluded until the Fase 1 runtime port, as is
`capture_diagnostics_tests` (includes `<winsock2.h>` directly; needs the
POSIX reachability port). `launcher_tests` and the `.rc` fixtures stay
Win32-only until the Fase 2 Qt6 GUI.

There is no `CUAJONE_ENABLE_ONNX`-style flag in `native/CMakeLists.txt`
(verified): the Fase 0 CI job simply builds the targets that compile
without ORT.

## Empty preset cache variables

The `linux-release` preset passes `ONNXRUNTIME_ROOT=""`,
`OpenCV_DIR=""`, `TENSORRT_ROOT=""`:

- `OpenCV_DIR=""` → CMake system search finds apt `libopencv-dev`
  (`/usr/lib/x86_64-linux-gnu/cmake/opencv4`). Set it only for a custom
  OpenCV build.
- `ONNXRUNTIME_ROOT=""` → unused in Fase 0; Fase 1 will point it at the
  Linux ORT package (default in `activate-native.sh`:
  `.tools/native/linux-onnxruntime-1.25.0`).
- `TENSORRT_ROOT=""` → unused until Fase 3 (`/opt/tensorrt` default).

## Usage

```bash
source native/activate-native.sh   # prints apt prerequisites + env summary
cmake --preset linux-release       # run from native/
cmake --build --preset linux-release
ctest --preset linux-release --output-on-failure
```

CI (`.github/workflows/linux-build.yml`, job `linux-fase1` on
`ubuntu-24.04`): installs apt deps, provisions the pinned ByteTrack/Eigen
sources (same hashes and `git apply` patch as
`native/Provision-TrackingDependencies.ps1`) and the Linux ONNX Runtime
1.25.0 package (layout-checked), then runs the three commands above.

## Roadmap

- **Fase 1 — POSIX runtime port** (done): `capture` (BSD sockets
  reachability + VAAPI), `compute` (`dlopen` CUDA probe + config-file
  backend), `evidence`/`model_manifest` (portable crypto + filesystem),
  Linux ONNX Runtime package, `evidence_tests`, `onnx_tests`,
  `capture_diagnostics_tests` on UNIX.
- **Fase 2 — Qt6 GUI + packaging**: replace Win32 launcher
  (`cuajone_launcher`, support lib, `.rc`, `version.lib`) with a Qt6 app;
  flip `CUAJONE_BUILD_LAUNCHER` on for Linux; Linux installer/packaging.
- **Fase 3 — GPU**: CUDA/TensorRT backends on Linux, TRT integration tests.
