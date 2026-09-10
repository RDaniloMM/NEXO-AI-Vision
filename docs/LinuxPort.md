# SPDX-License-Identifier: AGPL-3.0-only

# Linux Port — Fase 0 (build base, no logic ported)

Target platform: **DGX OS / Ubuntu 22.04+** (CI pins `ubuntu-24.04`, GCC 13).
GUI direction: **Qt6** (replaces the Win32 launcher in Fase 2).

## What Fase 0 does

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

CI (`.github/workflows/linux-build.yml`, job `linux-fase0` on
`ubuntu-24.04`): installs apt deps, provisions the pinned ByteTrack/Eigen
sources (same hashes and `git apply` patch as
`native/Provision-TrackingDependencies.ps1`), then runs the three commands
above.

## Roadmap

- **Fase 1 — POSIX runtime port**: `capture` (BSD sockets reachability),
  `evidence`/`model_manifest` (replace Win32/BCrypt with portable crypto +
  filesystem), Linux ONNX Runtime package, enable `evidence_tests`,
  `onnx_tests`, `capture_diagnostics_tests` on UNIX.
- **Fase 2 — Qt6 GUI + packaging**: replace Win32 launcher
  (`cuajone_launcher`, support lib, `.rc`, `version.lib`) with a Qt6 app;
  flip `CUAJONE_BUILD_LAUNCHER` on for Linux; Linux installer/packaging.
- **Fase 3 — GPU**: CUDA/TensorRT backends on Linux, TRT integration tests.
