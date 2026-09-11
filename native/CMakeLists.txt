# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.25)

project(NexoAIVision LANGUAGES CXX)

option(CUAJONE_BUILD_RUNTIME "Build the Windows native runtime" ON)
option(CUAJONE_BUILD_TESTS "Build pure CPU unit tests" ON)
option(CUAJONE_BUILD_LAUNCHER "Build the legacy native launcher fallback" ON)
option(CUAJONE_BUILD_QT_LAUNCHER "Build the Qt6 launcher on Windows and UNIX" OFF)
option(CUAJONE_BUILD_QT_VIEWER "Build the Qt6 live analytics viewer on Windows and UNIX" OFF)
option(CUAJONE_USE_QT "Prefer the Qt6 launcher and viewer when Qt6 is available" OFF)
option(CUAJONE_REQUIRE_QT "Fail when an explicitly requested Qt package build cannot find Qt6" OFF)
option(CUAJONE_BUILD_PYTHON_BINDINGS "Build the development/QA pybind11 module" OFF)
option(CUAJONE_ENABLE_TENSORRT "Enable the optional TensorRT/CUDA backend" ON)
option(CUAJONE_ENABLE_TRT_INTEGRATION_TESTS "Build opt-in TensorRT integration tests" OFF)
option(CUAJONE_ENABLE_ONNX_CUDA_INTEGRATION_TESTS "Build opt-in real ONNX Runtime CUDA integration tests" OFF)
option(CUAJONE_ENABLE_INTERNAL_DIAGNOSTICS "Compile non-public diagnostic seams" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Product executable names. Keep in sync with installer/native scripts and
# native/src/launcher/launcher.cpp (kRuntimeExecutable).
set(CUAJONE_PRODUCT_EXE "NexoAIVision")
set(CUAJONE_PRODUCT_LAUNCHER_EXE "NexoAIVisionLauncher")
set(CUAJONE_FILE_VERSION "" CACHE STRING
    "Launcher numeric version; must match installer -FileVersion. Empty means development build")
set(CUAJONE_PRODUCT_VERSION "" CACHE STRING
    "Launcher product label shown in About; may include a prerelease suffix")
set(CUAJONE_PACKAGE_VERSION "" CACHE STRING
    "Canonical package version (major.minor.patch); derived from product version when empty")
include(cmake/LauncherVersion.cmake)

if(CUAJONE_USE_QT)
    set(CUAJONE_BUILD_QT_LAUNCHER ON CACHE BOOL "Build the Qt6 launcher on Windows and UNIX" FORCE)
    set(CUAJONE_BUILD_QT_VIEWER ON CACHE BOOL "Build the Qt6 live analytics viewer on Windows and UNIX" FORCE)
endif()

if(CUAJONE_PRODUCT_VERSION STREQUAL "")
    set(CUAJONE_VERSION_DISPLAY "0.0.0.0-dev")
else()
    set(CUAJONE_VERSION_DISPLAY "${CUAJONE_PRODUCT_VERSION}")
endif()

add_library(cuajone_platform STATIC
    src/platform/platform_paths.cpp
    src/platform/platform_secrets.cpp
    src/platform/platform_process.cpp)
target_include_directories(cuajone_platform PUBLIC src/platform)
target_compile_features(cuajone_platform PUBLIC cxx_std_20)
if(WIN32)
    target_sources(cuajone_platform PRIVATE
        src/platform/windows/platform_paths.cpp
        src/platform/windows/platform_secrets.cpp
        src/platform/windows/platform_process.cpp)
    target_link_libraries(cuajone_platform PUBLIC advapi32 ole32 shell32)
else()
    target_sources(cuajone_platform PRIVATE
        src/platform/linux/platform_paths.cpp
        src/platform/linux/platform_secrets.cpp
        src/platform/linux/platform_process.cpp)
endif()

set(CUAJONE_RESVG_EXE
    "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/native/dependencies/resvg-0.47.0/resvg.exe"
    CACHE FILEPATH "Verified resvg 0.47.0 CLI used to rasterize the product SVG icon")

set(CUAJONE_QT_AVAILABLE OFF)
if(CUAJONE_BUILD_QT_LAUNCHER OR CUAJONE_BUILD_QT_VIEWER)
    find_package(Qt6 QUIET COMPONENTS Widgets Gui)
    if(Qt6_FOUND)
        set(CUAJONE_QT_AVAILABLE ON)
    elseif(CUAJONE_REQUIRE_QT)
        message(FATAL_ERROR "Qt6 Widgets/Gui are required for this Qt build")
    else()
        message(STATUS "Qt6 Widgets/Gui not found; requested Qt targets are disabled")
        set(CUAJONE_BUILD_QT_LAUNCHER OFF)
        set(CUAJONE_BUILD_QT_VIEWER OFF)
    endif()
endif()

if(CUAJONE_BUILD_QT_LAUNCHER)
    if(NOT TARGET cuajone_launcher_support)
        add_library(cuajone_launcher_support
            src/launcher/launcher_support.cpp)
        target_include_directories(cuajone_launcher_support PUBLIC include)
        target_link_libraries(cuajone_launcher_support PUBLIC cuajone_platform)
        target_compile_features(cuajone_launcher_support PUBLIC cxx_std_20)
    endif()
    add_executable(cuajone_qt_launcher WIN32
        src/launcher-qt/main.cpp
        src/launcher-qt/launcher_window.cpp
        src/launcher-qt/launcher_window.hpp)
    target_link_libraries(cuajone_qt_launcher PRIVATE
        Qt6::Widgets cuajone_launcher_support cuajone_platform)
    target_compile_features(cuajone_qt_launcher PRIVATE cxx_std_20)
    target_compile_definitions(cuajone_qt_launcher PRIVATE
        CUAJONE_QT_VERSION="${CUAJONE_VERSION_DISPLAY}")
    if(MSVC)
        target_compile_options(cuajone_qt_launcher PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(cuajone_qt_launcher PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    set_target_properties(cuajone_qt_launcher PROPERTIES
        OUTPUT_NAME "${CUAJONE_PRODUCT_LAUNCHER_EXE}")
    if(UNIX)
        set_target_properties(cuajone_qt_launcher PROPERTIES
            INSTALL_RPATH "$ORIGIN/../lib")
    endif()
endif()

if(CUAJONE_BUILD_QT_VIEWER)
    if(NOT CUAJONE_BUILD_RUNTIME)
        message(STATUS
            "CUAJONE_BUILD_QT_VIEWER requires CUAJONE_BUILD_RUNTIME=ON; "
            "optional Qt6 viewer is disabled")
        set(CUAJONE_BUILD_QT_VIEWER OFF)
    endif()
endif()

if(WIN32 AND NOT CUAJONE_BUILD_QT_LAUNCHER
    AND (CUAJONE_BUILD_RUNTIME OR CUAJONE_BUILD_LAUNCHER))
    set(CUAJONE_PRODUCT_ICON "${CMAKE_CURRENT_BINARY_DIR}/CuajonePPEMonitor.ico")
    set(CUAJONE_PRODUCT_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/product_icon.rc")
    set(CUAJONE_PRODUCT_SVG "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/assets/icon.svg")
    if(EXISTS "${CUAJONE_PRODUCT_SVG}" AND NOT EXISTS "${CUAJONE_RESVG_EXE}")
        message(FATAL_ERROR
            "resvg CLI is missing for SVG icon generation. Run native/Provision-Resvg.ps1 first")
    endif()
    add_custom_command(
        OUTPUT "${CUAJONE_PRODUCT_ICON}"
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File
            "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/generate-icon.ps1"
            -OutputPath "${CUAJONE_PRODUCT_ICON}"
            -SvgPath "${CUAJONE_PRODUCT_SVG}"
            -ResvgPath "${CUAJONE_RESVG_EXE}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/generate-icon.ps1"
            "${CUAJONE_PRODUCT_SVG}"
        VERBATIM)
    configure_file(resources/launcher.rc.in "${CUAJONE_PRODUCT_RESOURCE}" @ONLY)
    # NOTE: do not add per-source COMPILE_FLAGS "/c 65001" here. The template
    # starts with `#pragma code_page(65001)`, which already forces rc.exe to
    # read UTF-8. Extra RC flags are forwarded by cmcldeps to cl.exe during
    # Ninja dependency scanning, where the bare `65001` token fails the build
    # (c1 C1083).
    set_source_files_properties("${CUAJONE_PRODUCT_RESOURCE}" PROPERTIES
        OBJECT_DEPENDS "${CUAJONE_PRODUCT_ICON}")
    add_custom_target(cuajone_product_icon DEPENDS "${CUAJONE_PRODUCT_ICON}")
endif()

if(CUAJONE_BUILD_LAUNCHER OR CUAJONE_BUILD_QT_LAUNCHER)
    if(NOT TARGET cuajone_launcher_support)
        add_library(cuajone_launcher_support
            src/launcher/launcher_support.cpp
        )
        target_include_directories(cuajone_launcher_support PUBLIC include)
        target_link_libraries(cuajone_launcher_support PUBLIC cuajone_platform)
        target_compile_features(cuajone_launcher_support PUBLIC cxx_std_20)
        if(MSVC)
            target_compile_options(cuajone_launcher_support PRIVATE /W4 /permissive- /EHsc /utf-8)
        else()
            target_compile_options(cuajone_launcher_support PRIVATE -Wall -Wextra -Wpedantic)
        endif()
    endif()

    if(WIN32 AND NOT CUAJONE_BUILD_QT_LAUNCHER)
        target_sources(cuajone_launcher_support PRIVATE src/launcher/launcher_version.cpp)
        target_link_libraries(cuajone_launcher_support PRIVATE version)
        set(launcher_version_resource "${CMAKE_CURRENT_BINARY_DIR}/launcher_version.rc")
        cuajone_launcher_version("${CUAJONE_FILE_VERSION}" "${CUAJONE_PRODUCT_VERSION}"
            "${launcher_version_resource}")
        # NOTE: launcher_version.rc carries ASCII only, so no codepage flag is
        # needed (see note above about cmcldeps forwarding RC flags to cl.exe).
        add_executable(cuajone_launcher WIN32 src/launcher/launcher.cpp
            "${CUAJONE_PRODUCT_RESOURCE}" "${launcher_version_resource}")
        add_dependencies(cuajone_launcher cuajone_product_icon)
        target_link_libraries(cuajone_launcher PRIVATE
            cuajone_launcher_support advapi32 comctl32 dwmapi ole32 shell32)
        target_compile_definitions(cuajone_launcher PRIVATE
            UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
        if(MSVC)
            target_compile_options(cuajone_launcher PRIVATE /W4 /permissive- /EHsc /utf-8)
        endif()
        set_target_properties(cuajone_launcher PROPERTIES
            OUTPUT_NAME "${CUAJONE_PRODUCT_LAUNCHER_EXE}")
    endif()
endif()

if(UNIX AND CUAJONE_ENABLE_TENSORRT AND NOT TENSORRT_ROOT)
    # Keep ordinary Linux developer/CI configures CPU-only. A distribution
    # build opts into TensorRT by supplying TENSORRT_ROOT explicitly.
    message(STATUS
        "Linux: TensorRT backend disabled because TENSORRT_ROOT is empty; "
        "set TENSORRT_ROOT to enable the optional GPU build")
    set(CUAJONE_ENABLE_TENSORRT OFF)
endif()

if(CUAJONE_BUILD_RUNTIME AND CUAJONE_ENABLE_TENSORRT)
    set(TENSORRT_ROOT "" CACHE PATH "TensorRT 11 C++ SDK root containing include/ and lib/")
    if(NOT TENSORRT_ROOT OR TENSORRT_ROOT STREQUAL "")
        message(FATAL_ERROR
            "TENSORRT_ROOT is required and must point to a TensorRT 11 SDK root")
    endif()
endif()

# Ubuntu 24.04 apt ships OpenCV 4.6 via libopencv-dev; the core/imgproc API
# surface used by the portable libraries is stable across 4.6+.
set(CUAJONE_OPENCV_MIN_VERSION "4.8")
if(UNIX)
    set(CUAJONE_OPENCV_MIN_VERSION "4.6")
endif()
find_package(OpenCV ${CUAJONE_OPENCV_MIN_VERSION} REQUIRED COMPONENTS core imgproc)

set(CUAJONE_DEPENDENCY_LOCK
    "${CMAKE_CURRENT_SOURCE_DIR}/../installer/linux/dependency-lock.env")
if(NOT EXISTS "${CUAJONE_DEPENDENCY_LOCK}")
    message(FATAL_ERROR "Native dependency lock is missing: ${CUAJONE_DEPENDENCY_LOCK}")
endif()
file(STRINGS "${CUAJONE_DEPENDENCY_LOCK}" CUAJONE_DEPENDENCY_LOCK_LINES
    REGEX "^[A-Z_]+=")
foreach(CUAJONE_LOCK_LINE IN LISTS CUAJONE_DEPENDENCY_LOCK_LINES)
    string(REGEX MATCH "^([A-Z_]+)=(.*)$" CUAJONE_LOCK_MATCH
        "${CUAJONE_LOCK_LINE}")
    if(CUAJONE_LOCK_MATCH)
        set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()
foreach(CUAJONE_REQUIRED_LOCK_VALUE
    BYTE_TRACK_COMMIT BYTE_TRACK_SHA256 EIGEN_COMMIT EIGEN_SHA256
    ONNX_RUNTIME_VERSION)
    if(NOT DEFINED ${CUAJONE_REQUIRED_LOCK_VALUE}
        OR "${${CUAJONE_REQUIRED_LOCK_VALUE}}" STREQUAL "")
        message(FATAL_ERROR
            "Native dependency lock value is missing: ${CUAJONE_REQUIRED_LOCK_VALUE}")
    endif()
endforeach()
set(CUAJONE_BYTE_TRACK_COMMIT "${BYTE_TRACK_COMMIT}")
set(CUAJONE_EIGEN_COMMIT "${EIGEN_COMMIT}")
set(CUAJONE_BYTE_TRACK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/native/dependencies/byte-track-eigen-${CUAJONE_BYTE_TRACK_COMMIT}"
    CACHE PATH "Verified patched ByteTrack-Eigen 2.1.0 source root")
set(CUAJONE_EIGEN_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/native/dependencies/eigen-${CUAJONE_EIGEN_COMMIT}"
    CACHE PATH "Verified Eigen 3.4.0 source root")
if(NOT EXISTS "${CUAJONE_BYTE_TRACK_ROOT}/include/BYTETracker.h"
    OR NOT EXISTS "${CUAJONE_BYTE_TRACK_ROOT}/.cuajone-source-receipt.json"
    OR NOT EXISTS "${CUAJONE_EIGEN_ROOT}/Eigen/Dense"
    OR NOT EXISTS "${CUAJONE_EIGEN_ROOT}/.cuajone-source-receipt.json")
    message(FATAL_ERROR
        "Pinned ByteTrack-Eigen/Eigen sources are missing. Run installer/linux/provision-deps.sh first")
endif()
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/byte-track-eigen-cuajone.patch"
    CUAJONE_BYTE_TRACK_PATCH_SHA256)
function(cuajone_validate_source_receipt receipt_path expected_archive_sha256)
    file(READ "${receipt_path}" receipt_json)
    string(JSON archive_sha256 GET "${receipt_json}" archive_sha256)
    string(JSON patch_sha256 GET "${receipt_json}" patch_sha256)
    if(NOT archive_sha256 STREQUAL expected_archive_sha256
        OR NOT patch_sha256 STREQUAL CUAJONE_BYTE_TRACK_PATCH_SHA256)
        message(FATAL_ERROR
            "Native tracking dependency receipt does not match approved source and patch hashes: ${receipt_path}")
    endif()
endfunction()
cuajone_validate_source_receipt(
    "${CUAJONE_BYTE_TRACK_ROOT}/.cuajone-source-receipt.json"
    "${BYTE_TRACK_SHA256}")
cuajone_validate_source_receipt(
    "${CUAJONE_EIGEN_ROOT}/.cuajone-source-receipt.json"
    "${EIGEN_SHA256}")

add_library(cuajone_byte_track_upstream STATIC
    "${CUAJONE_BYTE_TRACK_ROOT}/src/BaseTrack.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/BoundingBoxIoUMatching.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/BoundingBoxTrackUtils.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/BYTETracker.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/HungarianAlgorithmEigen.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/KalmanBBoxTrack.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/KalmanFilter.cpp"
    "${CUAJONE_BYTE_TRACK_ROOT}/src/LinearAssignment.cpp"
)
target_include_directories(cuajone_byte_track_upstream SYSTEM PUBLIC
    "${CUAJONE_BYTE_TRACK_ROOT}/include"
    "${CUAJONE_EIGEN_ROOT}")
target_compile_definitions(cuajone_byte_track_upstream
    PUBLIC EIGEN_MPL2_ONLY BYTE_TRACK_EIGEN_STATIC)
target_compile_features(cuajone_byte_track_upstream PUBLIC cxx_std_20)

add_library(cuajone_byte_track_adapter STATIC src/analytics/byte_tracker.cpp)
target_include_directories(cuajone_byte_track_adapter PUBLIC include)
target_link_libraries(cuajone_byte_track_adapter PRIVATE cuajone_byte_track_upstream)
target_compile_features(cuajone_byte_track_adapter PUBLIC cxx_std_20)

add_library(cuajone_compute
    src/foundation/compute.cpp
)
target_include_directories(cuajone_compute PUBLIC include)
target_compile_features(cuajone_compute PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(cuajone_compute PRIVATE dxgi advapi32)
else()
    # dlopen/dlsym for the Linux CUDA Driver API probe.
    target_link_libraries(cuajone_compute PRIVATE ${CMAKE_DL_LIBS})
endif()

add_library(cuajone_contracts
    src/foundation/contracts.cpp
)
target_include_directories(cuajone_contracts PUBLIC include)
target_compile_features(cuajone_contracts PUBLIC cxx_std_20)

add_library(cuajone_analytics
    src/analytics/analytics_pipeline.cpp
    src/analytics/fall_analytics.cpp
    src/analytics/letterbox.cpp
    src/analytics/ppe_analytics.cpp
    src/foundation/types.cpp
)
target_include_directories(cuajone_analytics PUBLIC include)
target_link_libraries(cuajone_analytics
    PUBLIC cuajone_contracts
    PRIVATE cuajone_byte_track_adapter)
target_compile_features(cuajone_analytics PUBLIC cxx_std_20)

add_library(cuajone_inference
    src/inference/engine_reader.cpp
    src/inference/preprocess.cpp
    src/inference/yolo_decode.cpp
)
target_include_directories(cuajone_inference PUBLIC include)
target_link_libraries(cuajone_inference PUBLIC cuajone_analytics opencv_core opencv_imgproc)
target_compile_features(cuajone_inference PUBLIC cxx_std_20)

add_library(cuajone_core
    src/app/cli.cpp
    src/app/runtime_execution_plan.cpp
    src/foundation/performance_telemetry.cpp
)
target_include_directories(cuajone_core PUBLIC include)
target_link_libraries(cuajone_core PUBLIC cuajone_contracts cuajone_analytics cuajone_inference cuajone_compute)
target_compile_features(cuajone_core PUBLIC cxx_std_20)
if(MSVC)
    foreach(target cuajone_compute cuajone_contracts cuajone_byte_track_adapter cuajone_analytics cuajone_inference cuajone_core)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    endforeach()
else()
    foreach(target cuajone_compute cuajone_contracts cuajone_byte_track_adapter cuajone_analytics cuajone_inference cuajone_core)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()

if(CUAJONE_BUILD_PYTHON_BINDINGS)
    set(CUAJONE_PYBIND11_ROOT "" CACHE PATH
        "Pinned pybind11 3.0.4 CMake package root from a local Python environment")
    set(CUAJONE_PYTHON_OUTPUT_DIR "${CMAKE_BINARY_DIR}/python" CACHE PATH
        "Output directory for the development/QA Python extension")
    if(NOT CUAJONE_PYBIND11_ROOT)
        message(FATAL_ERROR
            "CUAJONE_PYBIND11_ROOT is required for bindings; use the pinned pybind11 3.0.4 package for Python 3.12")
    endif()
    find_package(Python 3.12 EXACT COMPONENTS Interpreter Development.Module REQUIRED)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 3.0.4 EXACT CONFIG REQUIRED
        PATHS "${CUAJONE_PYBIND11_ROOT}" NO_DEFAULT_PATH)
    pybind11_add_module(cuajone_python MODULE src/bindings/python_bindings.cpp)
    target_link_libraries(cuajone_python PRIVATE cuajone_analytics)
    target_compile_features(cuajone_python PRIVATE cxx_std_20)
    set_target_properties(cuajone_python PROPERTIES
        OUTPUT_NAME cuajone_native
        LIBRARY_OUTPUT_DIRECTORY "${CUAJONE_PYTHON_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${CUAJONE_PYTHON_OUTPUT_DIR}")
    if(MSVC)
        target_compile_options(cuajone_python PRIVATE /W4 /permissive- /EHsc)
    endif()
endif()

if(CUAJONE_BUILD_TESTS)
    enable_testing()
    add_executable(cuajone_cpu_tests tests/cpu_tests.cpp)
    target_link_libraries(cuajone_cpu_tests PRIVATE cuajone_core)
    add_test(NAME cuajone_cpu_tests COMMAND cuajone_cpu_tests)
    if(WIN32)
        set_tests_properties(cuajone_cpu_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
    endif()
    if(UNIX AND CUAJONE_BUILD_LAUNCHER)
        # The legacy Win32 launcher is excluded on UNIX. The optional Qt6
        # replacement is a separate target; see docs/LinuxPort.md.
        message(STATUS
            "UNIX: legacy Win32 launcher excluded (optional Qt6 GUI is Fase 2a); "
            "building portable core libraries and cpu_tests only")
    endif()

    # Linux Fase 1: launcher_tests still need the Win32 GUI and .rc fixtures;
    # they remain Windows-only while the Qt6 GUI has its own smoke coverage.
    # The POSIX runtime port (capture reachability, portable SHA-256, Linux
    # ONNX Runtime package) enables evidence, onnx, and capture_diagnostics
    # tests on UNIX in the runtime block below.
    if(TARGET cuajone_launcher)
        add_executable(cuajone_launcher_tests tests/launcher_tests.cpp)
        if(WIN32)
            target_sources(cuajone_launcher_tests PRIVATE tests/launcher_version_fixture.rc)
        endif()
        target_link_libraries(cuajone_launcher_tests PRIVATE cuajone_launcher_support)
        if(MSVC)
            target_compile_options(cuajone_launcher_tests PRIVATE /W4 /permissive- /EHsc)
        endif()
        add_test(NAME cuajone_launcher_tests COMMAND cuajone_launcher_tests)
        add_test(NAME cuajone_version_resource_tests COMMAND "${CMAKE_COMMAND}"
            "-DTEST_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/version-resource-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_resource_tests.cmake")
        if(WIN32)
            add_executable(cuajone_launcher_numeric_version_fixture
                tests/version_resource_fixture.cpp tests/launcher_numeric_version_fixture.rc)
            add_test(NAME cuajone_launcher_numeric_version_fixture
                COMMAND cuajone_launcher_tests --numeric-fixture
                    "$<TARGET_FILE:cuajone_launcher_numeric_version_fixture>")
            add_dependencies(cuajone_launcher_tests cuajone_launcher)
            add_dependencies(cuajone_launcher_tests cuajone_launcher_numeric_version_fixture)
            add_test(NAME cuajone_built_launcher_version COMMAND cuajone_launcher_tests
                --launcher-version "$<TARGET_FILE:cuajone_launcher>" "${CUAJONE_VERSION_DISPLAY}")
            add_test(NAME cuajone_installer_version_policy COMMAND powershell -NoProfile
                -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/test-version-policy.ps1"
                -LauncherExecutable "$<TARGET_FILE:cuajone_launcher>" -ExpectedVersion "${CUAJONE_VERSION_IDENTITY}")
        endif()
    endif()
endif()

if(UNIX AND CUAJONE_BUILD_RUNTIME)
    # UNIX: the POSIX runtime port (capture BSD-sockets reachability,
    # portable SHA-256, Linux ONNX Runtime package) builds cuajone_runtime and
    # the NexoAIVision CLI below. The Qt6 launcher/viewer are optional Fase
    # 2a/2b targets.
    # Still excluded from this ordinary CPU job: Win32 launcher, installer
    # custom action (msiquery.h), dumpbin closure copy, delayimp/DELAYLOAD;
    # the optional Linux TensorRT package path is documented in docs/LinuxPort.md.
    message(STATUS
        "Linux Fase 1: building POSIX runtime (cuajone_runtime + NexoAIVision CLI); "
         "optional Qt6 launcher/viewer are available on both OSes; CUDA/TensorRT is Fase 3")
endif()

if(CUAJONE_BUILD_RUNTIME)
    if(UNIX)
        if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "The native runtime currently supports 64-bit Linux only")
        endif()
    else()
        if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
            message(FATAL_ERROR "The native runtime currently supports Windows x64 only")
        endif()
        if(NOT MSVC)
            message(FATAL_ERROR "The native runtime requires the MSVC C++ toolchain")
        endif()
    endif()

    set(ONNXRUNTIME_ROOT "" CACHE PATH "ONNX Runtime 1.25.0 package root")
    if(NOT ONNXRUNTIME_ROOT)
        message(FATAL_ERROR "ONNXRUNTIME_ROOT is required for the CPU inference backend")
    endif()
    get_filename_component(ONNXRUNTIME_ROOT "${ONNXRUNTIME_ROOT}" ABSOLUTE)
    if(UNIX)
        # Linux Fase 1: upstream onnxruntime-linux-x64-1.25.0 layout
        # (include/ + lib/libonnxruntime.so); provisioned by
        # .github/workflows/linux-build.yml under .tools/native/.
        if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h")
            message(FATAL_ERROR
                "ONNXRUNTIME_ROOT must contain the Linux ONNX Runtime 1.25.0 include/ files")
        endif()
        file(GLOB CUAJONE_ORT_LINUX_SO "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so*")
        if(NOT CUAJONE_ORT_LINUX_SO)
            message(FATAL_ERROR
                "ONNXRUNTIME_ROOT must contain the Linux ONNX Runtime 1.25.0 lib/libonnxruntime.so files")
        endif()
        add_library(cuajone_onnxruntime SHARED IMPORTED)
        set_target_properties(cuajone_onnxruntime PROPERTIES
            IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so"
            INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include")
        # Build-tree RPATH so the CLI and tests find the ORT .so without an
        # install step or LD_LIBRARY_PATH.
        set(CUAJONE_ORT_RPATH "${ONNXRUNTIME_ROOT}/lib")
    else()
    if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h"
        OR NOT EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        OR NOT EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
        message(FATAL_ERROR
            "ONNXRUNTIME_ROOT must contain ONNX Runtime 1.25.0 include/ and lib/ files")
    endif()
    add_library(cuajone_onnxruntime SHARED IMPORTED)
    set_target_properties(cuajone_onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include")
    endif()

    if(CUAJONE_ENABLE_TENSORRT)
        get_filename_component(TENSORRT_ROOT "${TENSORRT_ROOT}" ABSOLUTE)
        unset(TENSORRT_INCLUDE_DIR CACHE)
        unset(TENSORRT_NVINFER_LIBRARY CACHE)
        find_path(TENSORRT_INCLUDE_DIR NvInfer.h PATHS "${TENSORRT_ROOT}/include" NO_DEFAULT_PATH)
        if(UNIX)
            find_library(TENSORRT_NVINFER_LIBRARY NAMES nvinfer nvinfer_11
                PATHS "${TENSORRT_ROOT}/lib" "${TENSORRT_ROOT}/lib/x64" NO_DEFAULT_PATH)
        else()
            find_library(TENSORRT_NVINFER_LIBRARY NAMES nvinfer_11
            PATHS "${TENSORRT_ROOT}/lib" "${TENSORRT_ROOT}/lib/x64" NO_DEFAULT_PATH)
        endif()
        if(NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_NVINFER_LIBRARY)
            message(FATAL_ERROR "TensorRT 11 headers and nvinfer_11 import library are required")
        endif()
        set(CUDA_RUNTIME_ROOT "" CACHE PATH
            "Optional CUDA runtime-only root containing include/, lib/x64/, and bin/")
        set(CUDA_COMPILER_HEADERS_ROOT "" CACHE PATH "Optional split CUDA compiler headers root")
        set(CUDA_CCCL_HEADERS_ROOT "" CACHE PATH "Optional split CUDA CCCL headers root")
        if(CUDA_RUNTIME_ROOT)
            get_filename_component(CUDA_RUNTIME_ROOT "${CUDA_RUNTIME_ROOT}" ABSOLUTE)
            find_path(CUDA_RUNTIME_INCLUDE_DIR cuda_runtime_api.h PATHS "${CUDA_RUNTIME_ROOT}/include" NO_DEFAULT_PATH)
            find_library(CUDA_CUDART_LIBRARY NAMES cudart PATHS "${CUDA_RUNTIME_ROOT}/lib/x64" NO_DEFAULT_PATH)
            find_file(CUDA_CUDART_DLL NAMES cudart64_12.dll PATHS "${CUDA_RUNTIME_ROOT}/bin" NO_DEFAULT_PATH)
            if(NOT CUDA_RUNTIME_INCLUDE_DIR OR NOT CUDA_CUDART_LIBRARY OR NOT CUDA_CUDART_DLL)
                message(FATAL_ERROR "CUDA_RUNTIME_ROOT is incomplete")
            endif()
            set(CUDA_RUNTIME_INCLUDE_DIRS "${CUDA_RUNTIME_INCLUDE_DIR}")
            if(NOT EXISTS "${CUDA_RUNTIME_INCLUDE_DIR}/crt/host_defines.h")
                if(NOT EXISTS "${CUDA_COMPILER_HEADERS_ROOT}/include/crt/host_defines.h")
                    message(FATAL_ERROR "CUDA_COMPILER_HEADERS_ROOT is required for split runtime headers")
                endif()
                list(APPEND CUDA_RUNTIME_INCLUDE_DIRS "${CUDA_COMPILER_HEADERS_ROOT}/include")
            endif()
            if(NOT EXISTS "${CUDA_RUNTIME_INCLUDE_DIR}/nv/target"
                AND NOT EXISTS "${CUDA_COMPILER_HEADERS_ROOT}/include/nv/target")
                if(NOT EXISTS "${CUDA_CCCL_HEADERS_ROOT}/include/nv/target")
                    message(FATAL_ERROR "CUDA_CCCL_HEADERS_ROOT is required for split runtime headers")
                endif()
                list(APPEND CUDA_RUNTIME_INCLUDE_DIRS "${CUDA_CCCL_HEADERS_ROOT}/include")
            endif()
            add_library(cuajone_cuda_runtime SHARED IMPORTED)
            set_target_properties(cuajone_cuda_runtime PROPERTIES
                IMPORTED_IMPLIB "${CUDA_CUDART_LIBRARY}"
                IMPORTED_LOCATION "${CUDA_CUDART_DLL}"
                INTERFACE_INCLUDE_DIRECTORIES "${CUDA_RUNTIME_INCLUDE_DIRS}")
            set(CUAJONE_CUDART_TARGET cuajone_cuda_runtime)
        else()
            find_package(CUDAToolkit 12 REQUIRED)
            set(CUAJONE_CUDART_TARGET CUDA::cudart)
        endif()
    endif()
    if(CUAJONE_BUILD_QT_VIEWER)
        # The Qt viewer replaces HighGUI on this path on both supported OSes.
        find_package(OpenCV ${CUAJONE_OPENCV_MIN_VERSION} REQUIRED COMPONENTS
            core imgproc imgcodecs videoio)
    else()
        find_package(OpenCV ${CUAJONE_OPENCV_MIN_VERSION} REQUIRED COMPONENTS
            core imgproc imgcodecs videoio highgui)
    endif()

    add_library(cuajone_runtime
        src/runtime/capture.cpp
        src/runtime/engine_pipeline.cpp
        src/runtime/evidence.cpp
        src/runtime/model_manifest.cpp
        src/runtime/onnx_session.cpp
    )
    target_include_directories(cuajone_runtime PUBLIC include)
    target_link_libraries(cuajone_runtime
        PUBLIC cuajone_core
        PRIVATE cuajone_onnxruntime
            opencv_imgcodecs opencv_videoio)
    if(NOT CUAJONE_BUILD_QT_VIEWER)
        target_link_libraries(cuajone_runtime PRIVATE opencv_highgui)
    endif()
    if(WIN32)
        target_link_libraries(cuajone_runtime PRIVATE bcrypt ws2_32)
    else()
        # dlopen/dlsym for the Linux CUDA Driver API probe (compute.cpp).
        target_link_libraries(cuajone_runtime PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(CUAJONE_ENABLE_TENSORRT)
        target_sources(cuajone_runtime PRIVATE src/runtime/tensorrt_runtime.cpp)
        target_include_directories(cuajone_runtime PRIVATE "${TENSORRT_INCLUDE_DIR}")
        target_link_libraries(cuajone_runtime PUBLIC ${CUAJONE_CUDART_TARGET})
        target_link_libraries(cuajone_runtime PRIVATE "${TENSORRT_NVINFER_LIBRARY}")
        target_compile_definitions(cuajone_runtime PRIVATE CUAJONE_WITH_TENSORRT=1)
    endif()
    if(MSVC)
        target_compile_options(cuajone_runtime PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(cuajone_runtime PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(CUAJONE_ENABLE_INTERNAL_DIAGNOSTICS)
        target_compile_definitions(cuajone_runtime PUBLIC CUAJONE_INTERNAL_DIAGNOSTICS=1)
    endif()

    if(TARGET cuajone_python)
        target_compile_definitions(cuajone_python PRIVATE CUAJONE_PYTHON_WITH_ENGINE_RUNTIME=1)
        target_link_libraries(cuajone_python PRIVATE cuajone_runtime)
        if(WIN32)
        add_custom_command(TARGET cuajone_python POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:cuajone_onnxruntime>
                $<TARGET_FILE_DIR:cuajone_python>)
        if(CUAJONE_ENABLE_TENSORRT)
            target_link_libraries(cuajone_python PRIVATE delayimp)
            target_link_options(cuajone_python PRIVATE
                "/DELAYLOAD:cudart64_12.dll"
                "/DELAYLOAD:nvinfer_11.dll")
        endif()
        endif()
    endif()

    if(WIN32 AND NOT (CUAJONE_BUILD_QT_VIEWER OR CUAJONE_BUILD_QT_LAUNCHER))
    add_executable(cuajone_native src/app/main.cpp "${CUAJONE_PRODUCT_RESOURCE}")
    add_dependencies(cuajone_native cuajone_product_icon)
    else()
        # The runtime has no resource file when the Qt viewer is the primary UI.
        add_executable(cuajone_native src/app/main.cpp)
        set_target_properties(cuajone_native PROPERTIES
            BUILD_RPATH "${CUAJONE_ORT_RPATH}"
            INSTALL_RPATH "$ORIGIN/../lib")
    endif()
    target_link_libraries(cuajone_native PRIVATE cuajone_runtime)
    if(CUAJONE_BUILD_QT_VIEWER)
        target_sources(cuajone_native PRIVATE
            src/app/qt_mosaic_viewer.cpp
            src/app/qt_mosaic_viewer.hpp)
        target_compile_definitions(cuajone_native PRIVATE CUAJONE_BUILD_QT_VIEWER=1)
        target_link_libraries(cuajone_native PRIVATE Qt6::Widgets Qt6::Gui)
    endif()
    if(MSVC)
        target_compile_options(cuajone_native PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(cuajone_native PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    set_target_properties(cuajone_native PROPERTIES
        OUTPUT_NAME "${CUAJONE_PRODUCT_EXE}")
    if(WIN32)
    add_custom_command(TARGET cuajone_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:cuajone_onnxruntime>
            $<TARGET_FILE_DIR:cuajone_native>)
    # Double-click support without activate-native.ps1: the Windows loader
    # resolves DLLs next to the executable, so stage the OpenCV runtime DLLs
    # app-local beside NexoAIVision.exe (the launcher sibling shares the same
    # directory, so its CreateProcessW child inherits this closure). The MSI
    # already stages its own bin\ closure in installer/native/build-installer.ps1;
    # this only affects dev build output directories, never installer payloads.
    get_filename_component(_CUAJONE_OPENCV_BIN "${OpenCV_DIR}/../bin" ABSOLUTE)
    file(GLOB _CUAJONE_OPENCV_WORLD_DLLS "${_CUAJONE_OPENCV_BIN}/opencv_world*.dll")
    file(GLOB _CUAJONE_OPENCV_FFMPEG_DLLS "${_CUAJONE_OPENCV_BIN}/opencv_videoio_ffmpeg*_64.dll")
    foreach(_CUAJONE_OPENCV_DLL IN LISTS _CUAJONE_OPENCV_WORLD_DLLS _CUAJONE_OPENCV_FFMPEG_DLLS)
        # Skip debug-suffixed variants (opencv_world4120d.dll); presets build Release.
        if(_CUAJONE_OPENCV_DLL MATCHES "d\\.dll$")
            continue()
        endif()
        add_custom_command(TARGET cuajone_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_CUAJONE_OPENCV_DLL}"
                $<TARGET_FILE_DIR:cuajone_native>)
    endforeach()
    unset(_CUAJONE_OPENCV_BIN)
    unset(_CUAJONE_OPENCV_WORLD_DLLS)
    unset(_CUAJONE_OPENCV_FFMPEG_DLLS)
    unset(_CUAJONE_OPENCV_DLL)
    if(CUAJONE_ENABLE_TENSORRT)
        target_link_libraries(cuajone_native PRIVATE delayimp)
        target_link_options(cuajone_native PRIVATE
            "/DELAYLOAD:cudart64_12.dll"
            "/DELAYLOAD:nvinfer_11.dll")
    endif()
    endif()

    if(WIN32)
    add_library(cuajone_installer_ca SHARED
        src/foundation/compute.cpp
        src/installer/installer_custom_action.cpp
    )
    target_include_directories(cuajone_installer_ca PRIVATE include)
    target_link_libraries(cuajone_installer_ca PRIVATE dxgi advapi32 msi)
    target_compile_options(cuajone_installer_ca PRIVATE /W4 /permissive- /EHsc)
    set_target_properties(cuajone_installer_ca PROPERTIES
        OUTPUT_NAME CuajoneHardwareProbeCA
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    if(CUAJONE_BUILD_TESTS)
        if(MSVC)
            set(CUAJONE_TEST_COMPILE_OPTIONS /W4 /permissive- /EHsc)
        else()
            set(CUAJONE_TEST_COMPILE_OPTIONS -Wall -Wextra -Wpedantic)
        endif()
        add_executable(cuajone_evidence_tests tests/evidence_tests.cpp)
        target_link_libraries(cuajone_evidence_tests PRIVATE cuajone_runtime)
        target_compile_options(cuajone_evidence_tests PRIVATE ${CUAJONE_TEST_COMPILE_OPTIONS})
        add_test(NAME cuajone_evidence_tests COMMAND cuajone_evidence_tests)
        if(WIN32)
        set_tests_properties(cuajone_evidence_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>;PATH=path_list_prepend:${ONNXRUNTIME_ROOT}/lib")
        else()
            # Linux Fase 1: resolve the ORT .so via build-tree RPATH.
            set_target_properties(cuajone_evidence_tests PROPERTIES
                BUILD_RPATH "${CUAJONE_ORT_RPATH}")
        endif()

        add_executable(cuajone_capture_diagnostics_tests tests/capture_diagnostics_tests.cpp)
        target_link_libraries(cuajone_capture_diagnostics_tests PRIVATE cuajone_runtime)
        if(WIN32)
            target_link_libraries(cuajone_capture_diagnostics_tests PRIVATE ws2_32)
        endif()
        target_compile_options(cuajone_capture_diagnostics_tests PRIVATE ${CUAJONE_TEST_COMPILE_OPTIONS})
        add_test(NAME cuajone_capture_diagnostics_tests COMMAND cuajone_capture_diagnostics_tests)
        if(WIN32)
        set_tests_properties(cuajone_capture_diagnostics_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>;PATH=path_list_prepend:${ONNXRUNTIME_ROOT}/lib")
        else()
            set_target_properties(cuajone_capture_diagnostics_tests PROPERTIES
                BUILD_RPATH "${CUAJONE_ORT_RPATH}")
        endif()

        add_executable(cuajone_onnx_tests tests/onnx_tests.cpp)
        target_link_libraries(cuajone_onnx_tests PRIVATE cuajone_runtime)
        target_compile_options(cuajone_onnx_tests PRIVATE ${CUAJONE_TEST_COMPILE_OPTIONS})
        if(WIN32)
        add_custom_command(TARGET cuajone_onnx_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:cuajone_onnxruntime>
                $<TARGET_FILE_DIR:cuajone_onnx_tests>)
        add_test(NAME cuajone_onnx_tests COMMAND cuajone_onnx_tests)
        set_tests_properties(cuajone_onnx_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>;PATH=path_list_prepend:${ONNXRUNTIME_ROOT}/lib")
        else()
            set_target_properties(cuajone_onnx_tests PROPERTIES
                BUILD_RPATH "${CUAJONE_ORT_RPATH}")
            add_test(NAME cuajone_onnx_tests COMMAND cuajone_onnx_tests)
        endif()
    endif()

    # Opt-in GPU integration tests stay Windows-only until the Fase 3 Linux
    # CUDA/TensorRT backends (dumpbin closure copy, DELAYLOAD, TRT headers).
    if(NOT UNIX)
    if(CUAJONE_ENABLE_TRT_INTEGRATION_TESTS AND CUAJONE_ENABLE_TENSORRT)
        set(CUAJONE_TEST_ENGINE "" CACHE FILEPATH "Engine used by the opt-in TensorRT smoke test")
        if(NOT EXISTS "${CUAJONE_TEST_ENGINE}")
            message(FATAL_ERROR "CUAJONE_TEST_ENGINE must point to a compatible engine")
        endif()
        add_executable(cuajone_trt_smoke tests/trt_smoke.cpp)
        target_link_libraries(cuajone_trt_smoke PRIVATE cuajone_runtime)
        target_include_directories(cuajone_trt_smoke PRIVATE "${TENSORRT_INCLUDE_DIR}")
        add_test(NAME cuajone_trt_smoke COMMAND cuajone_trt_smoke "${CUAJONE_TEST_ENGINE}")

        set(CUAJONE_TEST_POSE_ENGINE "" CACHE FILEPATH "Pose engine used by the opt-in TensorRT pipeline parity test")
        set(CUAJONE_TEST_PERSON_IMAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/person_regression.jpg"
            CACHE FILEPATH "Offline person image used by the opt-in TensorRT pipeline parity test")
        if(NOT EXISTS "${CUAJONE_TEST_POSE_ENGINE}")
            message(FATAL_ERROR "CUAJONE_TEST_POSE_ENGINE must point to a compatible pose engine")
        endif()
        add_executable(cuajone_trt_pipeline_parity tests/trt_pipeline_parity.cpp)
        target_link_libraries(cuajone_trt_pipeline_parity PRIVATE cuajone_runtime)
        target_include_directories(cuajone_trt_pipeline_parity SYSTEM PRIVATE
            "${CUAJONE_BYTE_TRACK_ROOT}/include"
            "${CUAJONE_EIGEN_ROOT}")
        target_include_directories(cuajone_trt_pipeline_parity PRIVATE
            "${TENSORRT_INCLUDE_DIR}")
        add_test(NAME cuajone_trt_pipeline_parity
            COMMAND cuajone_trt_pipeline_parity
                "${CUAJONE_TEST_ENGINE}" "${CUAJONE_TEST_POSE_ENGINE}" "${CUAJONE_TEST_PERSON_IMAGE}")
    endif()
    endif()

    if(NOT UNIX)
    if(CUAJONE_ENABLE_ONNX_CUDA_INTEGRATION_TESTS)
        unset(CUAJONE_ONNX_CUDA_RUNTIME_DIR CACHE)
        set(CUAJONE_ONNX_CUDA_DEPENDENCY_ROOTS "" CACHE STRING
            "Semicolon-separated trusted local roots for the ONNX Runtime CUDA DLL closure")
        set(CUAJONE_ONNX_CUDA_PPE_MODEL "" CACHE FILEPATH
            "Verified PPE ONNX model used by the opt-in CUDA pipeline integration test")
        set(CUAJONE_ONNX_CUDA_POSE_MODEL "" CACHE FILEPATH
            "Verified pose ONNX model used by the opt-in CUDA pipeline integration test")
        set(CUAJONE_ONNX_REAL_PERSON_IMAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/person_regression.jpg"
            CACHE FILEPATH "Offline person image used by the native real-model regression")
        foreach(required_file
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_cuda.dll"
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                "${CUAJONE_ONNX_CUDA_PPE_MODEL}"
                "${CUAJONE_ONNX_CUDA_PPE_MODEL}.manifest.json"
                "${CUAJONE_ONNX_CUDA_POSE_MODEL}"
                "${CUAJONE_ONNX_CUDA_POSE_MODEL}.manifest.json"
                "${CUAJONE_ONNX_REAL_PERSON_IMAGE}")
            if(NOT EXISTS "${required_file}")
                message(FATAL_ERROR
                    "ONNX CUDA integration prerequisite is missing: ${required_file}")
            endif()
        endforeach()
        if(NOT CUAJONE_ONNX_CUDA_DEPENDENCY_ROOTS)
            message(FATAL_ERROR
                "CUAJONE_ONNX_CUDA_DEPENDENCY_ROOTS must list trusted local CUDA dependency roots")
        endif()
        find_program(CUAJONE_DUMPBIN_EXECUTABLE dumpbin
            HINTS "$ENV{VCToolsInstallDir}/bin/Hostx64/x64"
            NO_DEFAULT_PATH)
        if(NOT CUAJONE_DUMPBIN_EXECUTABLE)
            message(FATAL_ERROR "MSVC dumpbin is required to resolve the local ONNX CUDA runtime closure")
        endif()
        function(cuajone_copy_onnx_cuda_runtime_closure target)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${target}>"
                    "-DDUMPBIN=${CUAJONE_DUMPBIN_EXECUTABLE}"
                    "-DSEEDS=${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll;${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_cuda.dll;${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                    "-DSEARCH_ROOTS=${CUAJONE_ONNX_CUDA_DEPENDENCY_ROOTS}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyOnnxCudaRuntimeClosure.cmake")
        endfunction()
        add_executable(cuajone_onnx_cuda_integration_tests tests/onnx_cuda_integration_tests.cpp)
        target_link_libraries(cuajone_onnx_cuda_integration_tests PRIVATE cuajone_runtime cuajone_onnxruntime)
        target_include_directories(cuajone_onnx_cuda_integration_tests PRIVATE
            "${CUAJONE_BYTE_TRACK_ROOT}/include")
        target_compile_options(cuajone_onnx_cuda_integration_tests PRIVATE /W4 /permissive- /EHsc)
        cuajone_copy_onnx_cuda_runtime_closure(cuajone_onnx_cuda_integration_tests)
        cuajone_copy_onnx_cuda_runtime_closure(cuajone_native)
        add_test(NAME cuajone_onnx_pose_cpu_standalone_tests
            COMMAND cuajone_onnx_cuda_integration_tests
                standalone-pose-cpu "${CUAJONE_ONNX_CUDA_POSE_MODEL}")
        add_test(NAME cuajone_onnx_cuda_ppe_only_tests
            COMMAND cuajone_onnx_cuda_integration_tests
                ppe-only "${CUAJONE_ONNX_CUDA_PPE_MODEL}")
        add_test(NAME cuajone_onnx_cuda_hybrid_pipeline_tests
            COMMAND cuajone_onnx_cuda_integration_tests pipeline
                "${CUAJONE_ONNX_CUDA_PPE_MODEL}" "${CUAJONE_ONNX_CUDA_POSE_MODEL}"
                "${CUAJONE_ONNX_REAL_PERSON_IMAGE}")
        set_tests_properties(
            cuajone_onnx_pose_cpu_standalone_tests
            cuajone_onnx_cuda_ppe_only_tests
            cuajone_onnx_cuda_hybrid_pipeline_tests
            PROPERTIES
            ENVIRONMENT "CUAJONE_ONNX_CUDA_RUNTIME_DIR=$<TARGET_FILE_DIR:cuajone_onnx_cuda_integration_tests>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
        set_tests_properties(cuajone_onnx_pose_cpu_standalone_tests PROPERTIES
            LABELS "onnx;acceptance;pose-cpu;hybrid-rationale-ort-cuda-1.25-gtx1650ti")
        set_tests_properties(
            cuajone_onnx_cuda_ppe_only_tests
            cuajone_onnx_cuda_hybrid_pipeline_tests
            PROPERTIES
            SKIP_RETURN_CODE 77)
        set_tests_properties(cuajone_onnx_cuda_ppe_only_tests PROPERTIES
            LABELS "gpu;onnx;acceptance;ppe-cuda")
        set_tests_properties(cuajone_onnx_cuda_hybrid_pipeline_tests PROPERTIES
            LABELS "gpu;onnx;acceptance;hybrid;ppe-cuda;pose-cpu;known-pose-cuda-access-violation")
    endif()
    endif()
endif()

include(cmake/Packaging.cmake)
