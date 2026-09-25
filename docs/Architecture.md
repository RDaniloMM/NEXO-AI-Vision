# Nexo AI Vision architecture

## Shared core

`nexoai_core`, `nexoai_runtime`, analytics, inference, capture, evidence,
model validation, and `launcher_support` contain the product rules shared by
Windows and Linux. `launcher_support` owns profiles, preferences, validation,
model resolution, and the launch plan; it does not create windows or processes.

## One Qt6 UI

`native/src/launcher-qt` is the primary launcher UI on both supported desktop
platforms. It uses `QStandardPaths` for the platform path roots and `QProcess`
for runtime supervision. The UI contract is the same on Windows and Linux:
profiles, local video, output, validation/start/stop, logs, and the menu
dialogs. Qt6 is required: there is no separate Win32 launcher target or
fallback product UI.

The live mosaic follows the same rule. `qt_mosaic_viewer` is the only live
viewer on both platforms. Headless runtime execution does not construct a Qt
application.

## Platform adapters

`native/src/platform` exposes small neutral APIs for paths, UTF conversion,
atomic file replacement, secrets, and process-stop policy. Only
`platform/windows` and `platform/linux` know OS APIs such as Windows Known
Folders/Credential Manager or Linux XDG and `/proc` details. The shared support
and Qt code consumes those APIs rather than embedding drive letters, system
directories, or OS process primitives.

Linux secrets are session-only unless a future libsecret adapter is enabled;
the UI warns the operator. Windows uses Credential Manager. Passwords are
never serialized into profile files.

## Packaging

`native/cmake/Packaging.cmake` is the single install and CPack contract. It
supports TGZ on Windows and Linux and DEB on UNIX. `installer/linux/build-dist.sh`
and `installer/native/build-installer.ps1` are OS-native wrappers that select
toolchains, provision dependencies, and invoke the common CMake contract; they
do not define separate product layouts. Windows MSI UpgradeCode/version policy
remains owned by the existing WiX wrapper.
