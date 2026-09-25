# SPDX-License-Identifier: AGPL-3.0-only

cmake_minimum_required(VERSION 3.25)

project(NexoAIVision LANGUAGES CXX)

option(NEXOAI_BUILD_RUNTIME "Build the Windows native runtime" ON)
option(NEXOAI_BUILD_TESTS "Build pure CPU unit tests" ON)
option(NEXOAI_BUILD_PYTHON_BINDINGS "Build the development/QA pybind11 module" OFF)
option(NEXOAI_ENABLE_TENSORRT "Enable the optional TensorRT/CUDA backend" ON)
option(NEXOAI_ENABLE_TRT_INTEGRATION_TESTS "Build opt-in TensorRT integration tests" OFF)
option(NEXOAI_ENABLE_ONNX_CUDA_INTEGRATION_TESTS "Build opt-in real ONNX Runtime CUDA integration tests" OFF)
option(NEXOAI_ENABLE_INTERNAL_DIAGNOSTICS "Compile non-public diagnostic seams" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Product executable names. Keep in sync with installer/native scripts.
set(NEXOAI_PRODUCT_EXE "NexoAIVision")
set(NEXOAI_PRODUCT_LAUNCHER_EXE "NexoAIVisionLauncher")
set(NEXOAI_FILE_VERSION "" CACHE STRING
    "Launcher numeric version; must match installer -FileVersion. Empty means development build")
set(NEXOAI_PRODUCT_VERSION "" CACHE STRING
    "Launcher product label shown in About; may include a prerelease suffix")
set(NEXOAI_PACKAGE_VERSION "" CACHE STRING
    "Canonical package version (major.minor.patch); derived from product version when empty")
include(cmake/LauncherVersion.cmake)

if(NEXOAI_PRODUCT_VERSION STREQUAL "")
    set(NEXOAI_VERSION_DISPLAY "0.0.0.0-dev")
else()
    set(NEXOAI_VERSION_DISPLAY "${NEXOAI_PRODUCT_VERSION}")
endif()

add_library(nexoai_platform STATIC
    src/platform/platform_paths.cpp
    src/platform/platform_secrets.cpp
    src/platform/platform_process.cpp)
target_include_directories(nexoai_platform PUBLIC src/platform)
target_compile_features(nexoai_platform PUBLIC cxx_std_20)
if(WIN32)
    target_sources(nexoai_platform PRIVATE
        src/platform/windows/platform_paths.cpp
        src/platform/windows/platform_secrets.cpp
        src/platform/windows/platform_process.cpp)
    target_link_libraries(nexoai_platform PUBLIC advapi32 ole32 shell32)
else()
    target_sources(nexoai_platform PRIVATE
        src/platform/linux/platform_paths.cpp
        src/platform/linux/platform_secrets.cpp
        src/platform/linux/platform_process.cpp)
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui)

add_library(nexoai_launcher_support
    src/launcher/launcher_support.cpp)
target_include_directories(nexoai_launcher_support PUBLIC include)
target_link_libraries(nexoai_launcher_support PUBLIC nexoai_platform)
target_compile_features(nexoai_launcher_support PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(nexoai_launcher_support PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(nexoai_launcher_support PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
    target_sources(nexoai_launcher_support PRIVATE src/launcher/launcher_version.cpp)
    target_link_libraries(nexoai_launcher_support PRIVATE version)
endif()

if(WIN32)
    # Canonical product icon. Replace this one file to update every Windows
    # surface (launcher, runtime resource and MSI) on the next build.
    set(NEXOAI_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/assets/icon.ico")
    set(NEXOAI_PRODUCT_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/product_icon.rc")
    if(NOT EXISTS "${NEXOAI_PRODUCT_ICON}")
        message(FATAL_ERROR "Product icon is missing: ${NEXOAI_PRODUCT_ICON}")
    endif()
    configure_file(resources/launcher.rc.in "${NEXOAI_PRODUCT_RESOURCE}" @ONLY)
    # NOTE: do not add per-source COMPILE_FLAGS "/c 65001" here. The template
    # starts with `#pragma code_page(65001)`, which already forces rc.exe to
    # read UTF-8. Extra RC flags are forwarded by cmcldeps to cl.exe during
    # Ninja dependency scanning, where the bare `65001` token fails the build
    # (c1 C1083).
endif()

set(NEXOAI_QT_LAUNCHER_SOURCES
    src/launcher-qt/main.cpp
    src/launcher-qt/launcher_window.cpp
    src/launcher-qt/launcher_window.hpp)
if(WIN32)
    set(launcher_version_resource "${CMAKE_CURRENT_BINARY_DIR}/launcher_version.rc")
    nexoai_launcher_version("${NEXOAI_FILE_VERSION}" "${NEXOAI_PRODUCT_VERSION}"
        "${launcher_version_resource}")
    list(APPEND NEXOAI_QT_LAUNCHER_SOURCES
        "${NEXOAI_PRODUCT_RESOURCE}" "${launcher_version_resource}")
endif()
add_executable(nexoai_qt_launcher WIN32 ${NEXOAI_QT_LAUNCHER_SOURCES})
target_link_libraries(nexoai_qt_launcher PRIVATE
    Qt6::Widgets nexoai_launcher_support nexoai_platform)
target_compile_features(nexoai_qt_launcher PRIVATE cxx_std_20)
target_compile_definitions(nexoai_qt_launcher PRIVATE
    NEXOAI_QT_VERSION="${NEXOAI_VERSION_DISPLAY}")
if(MSVC)
    target_compile_options(nexoai_qt_launcher PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(nexoai_qt_launcher PRIVATE -Wall -Wextra -Wpedantic)
endif()
set_target_properties(nexoai_qt_launcher PROPERTIES
    OUTPUT_NAME "${NEXOAI_PRODUCT_LAUNCHER_EXE}")
if(UNIX)
    set_target_properties(nexoai_qt_launcher PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
endif()

if(UNIX AND NEXOAI_ENABLE_TENSORRT AND NOT TENSORRT_ROOT)
    # Keep ordinary Linux developer/CI configures CPU-only. A distribution
    # build opts into TensorRT by supplying TENSORRT_ROOT explicitly.
    message(STATUS
        "Linux: TensorRT backend disabled because TENSORRT_ROOT is empty; "
        "set TENSORRT_ROOT to enable the optional GPU build")
    set(NEXOAI_ENABLE_TENSORRT OFF)
endif()

if(NEXOAI_BUILD_RUNTIME AND NEXOAI_ENABLE_TENSORRT)
    set(TENSORRT_ROOT "" CACHE PATH "TensorRT 11 C++ SDK root containing include/ and lib/")
    if(NOT TENSORRT_ROOT OR TENSORRT_ROOT STREQUAL "")
        message(FATAL_ERROR
            "TENSORRT_ROOT is required and must point to a TensorRT 11 SDK root")
    endif()
endif()

# Ubuntu 24.04 apt ships OpenCV 4.6 via libopencv-dev; the core/imgproc API
# surface used by the portable libraries is stable across 4.6+.
set(NEXOAI_OPENCV_MIN_VERSION "4.8")
if(UNIX)
    set(NEXOAI_OPENCV_MIN_VERSION "4.6")
endif()
find_package(OpenCV ${NEXOAI_OPENCV_MIN_VERSION} REQUIRED COMPONENTS core imgproc)

set(NEXOAI_DEPENDENCY_LOCK
    "${CMAKE_CURRENT_SOURCE_DIR}/../installer/linux/dependency-lock.env")
if(NOT EXISTS "${NEXOAI_DEPENDENCY_LOCK}")
    message(FATAL_ERROR "Native dependency lock is missing: ${NEXOAI_DEPENDENCY_LOCK}")
endif()
file(STRINGS "${NEXOAI_DEPENDENCY_LOCK}" NEXOAI_DEPENDENCY_LOCK_LINES
    REGEX "^[A-Z0-9_]+=")
foreach(NEXOAI_LOCK_LINE IN LISTS NEXOAI_DEPENDENCY_LOCK_LINES)
    string(REGEX MATCH "^([A-Z0-9_]+)=(.*)$" NEXOAI_LOCK_MATCH
        "${NEXOAI_LOCK_LINE}")
    if(NEXOAI_LOCK_MATCH)
        set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()
foreach(NEXOAI_REQUIRED_LOCK_VALUE
    BYTE_TRACK_COMMIT BYTE_TRACK_SHA256 EIGEN_COMMIT EIGEN_SHA256
    ONNX_RUNTIME_VERSION)
    if(NOT DEFINED ${NEXOAI_REQUIRED_LOCK_VALUE}
        OR "${${NEXOAI_REQUIRED_LOCK_VALUE}}" STREQUAL "")
        message(FATAL_ERROR
            "Native dependency lock value is missing: ${NEXOAI_REQUIRED_LOCK_VALUE}")
    endif()
endforeach()
set(NEXOAI_BYTE_TRACK_COMMIT "${BYTE_TRACK_COMMIT}")
set(NEXOAI_EIGEN_COMMIT "${EIGEN_COMMIT}")
set(NEXOAI_BYTE_TRACK_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/native/dependencies/byte-track-eigen-${NEXOAI_BYTE_TRACK_COMMIT}"
    CACHE PATH "Verified patched ByteTrack-Eigen 2.1.0 source root")
set(NEXOAI_EIGEN_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../.tools/native/dependencies/eigen-${NEXOAI_EIGEN_COMMIT}"
    CACHE PATH "Verified Eigen 3.4.0 source root")
if(NOT EXISTS "${NEXOAI_BYTE_TRACK_ROOT}/include/BYTETracker.h"
    OR NOT EXISTS "${NEXOAI_BYTE_TRACK_ROOT}/.nexoai-source-receipt.json"
    OR NOT EXISTS "${NEXOAI_EIGEN_ROOT}/Eigen/Dense"
    OR NOT EXISTS "${NEXOAI_EIGEN_ROOT}/.nexoai-source-receipt.json")
    message(FATAL_ERROR
        "Pinned ByteTrack-Eigen/Eigen sources are missing. Run installer/linux/provision-deps.sh first")
endif()
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/third_party/byte-track-eigen-nexoai.patch"
    NEXOAI_BYTE_TRACK_PATCH_SHA256)
function(nexoai_validate_source_receipt receipt_path expected_archive_sha256)
    file(READ "${receipt_path}" receipt_json)
    string(JSON archive_sha256 GET "${receipt_json}" archive_sha256)
    string(JSON patch_sha256 GET "${receipt_json}" patch_sha256)
    if(NOT archive_sha256 STREQUAL expected_archive_sha256
        OR NOT patch_sha256 STREQUAL NEXOAI_BYTE_TRACK_PATCH_SHA256)
        message(FATAL_ERROR
            "Native tracking dependency receipt does not match approved source and patch hashes: ${receipt_path}")
    endif()
endfunction()
nexoai_validate_source_receipt(
    "${NEXOAI_BYTE_TRACK_ROOT}/.nexoai-source-receipt.json"
    "${BYTE_TRACK_SHA256}")
nexoai_validate_source_receipt(
    "${NEXOAI_EIGEN_ROOT}/.nexoai-source-receipt.json"
    "${EIGEN_SHA256}")

add_library(nexoai_byte_track_upstream STATIC
    "${NEXOAI_BYTE_TRACK_ROOT}/src/BaseTrack.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/BoundingBoxIoUMatching.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/BoundingBoxTrackUtils.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/BYTETracker.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/HungarianAlgorithmEigen.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/KalmanBBoxTrack.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/KalmanFilter.cpp"
    "${NEXOAI_BYTE_TRACK_ROOT}/src/LinearAssignment.cpp"
)
target_include_directories(nexoai_byte_track_upstream SYSTEM PUBLIC
    "${NEXOAI_BYTE_TRACK_ROOT}/include"
    "${NEXOAI_EIGEN_ROOT}")
target_compile_definitions(nexoai_byte_track_upstream
    PUBLIC EIGEN_MPL2_ONLY BYTE_TRACK_EIGEN_STATIC)
target_compile_features(nexoai_byte_track_upstream PUBLIC cxx_std_20)

add_library(nexoai_byte_track_adapter STATIC src/analytics/byte_tracker.cpp)
target_include_directories(nexoai_byte_track_adapter PUBLIC include)
target_link_libraries(nexoai_byte_track_adapter PRIVATE nexoai_byte_track_upstream)
target_compile_features(nexoai_byte_track_adapter PUBLIC cxx_std_20)

add_library(nexoai_compute
    src/foundation/compute.cpp
)
target_include_directories(nexoai_compute PUBLIC include)
target_compile_features(nexoai_compute PUBLIC cxx_std_20)
if(WIN32)
    target_link_libraries(nexoai_compute PRIVATE dxgi advapi32)
else()
    # dlopen/dlsym for the Linux CUDA Driver API probe.
    target_link_libraries(nexoai_compute PRIVATE ${CMAKE_DL_LIBS})
endif()

add_library(nexoai_contracts
    src/foundation/contracts.cpp
)
target_include_directories(nexoai_contracts PUBLIC include)
target_compile_features(nexoai_contracts PUBLIC cxx_std_20)

add_library(nexoai_analytics
    src/analytics/analytics_pipeline.cpp
    src/analytics/fall_analytics.cpp
    src/analytics/letterbox.cpp
    src/analytics/ppe_analytics.cpp
    src/foundation/types.cpp
)
target_include_directories(nexoai_analytics PUBLIC include)
target_link_libraries(nexoai_analytics
    PUBLIC nexoai_contracts
    PRIVATE nexoai_byte_track_adapter)
target_compile_features(nexoai_analytics PUBLIC cxx_std_20)

add_library(nexoai_inference
    src/inference/engine_reader.cpp
    src/inference/preprocess.cpp
    src/inference/yolo_decode.cpp
)
target_include_directories(nexoai_inference PUBLIC include)
target_link_libraries(nexoai_inference PUBLIC nexoai_analytics opencv_core opencv_imgproc)
target_compile_features(nexoai_inference PUBLIC cxx_std_20)

add_library(nexoai_core
    src/app/cli.cpp
    src/app/runtime_execution_plan.cpp
    src/foundation/performance_telemetry.cpp
)
target_include_directories(nexoai_core PUBLIC include)
target_link_libraries(nexoai_core PUBLIC nexoai_contracts nexoai_analytics nexoai_inference nexoai_compute)
target_compile_features(nexoai_core PUBLIC cxx_std_20)
if(MSVC)
    foreach(target nexoai_compute nexoai_contracts nexoai_byte_track_adapter nexoai_analytics nexoai_inference nexoai_core)
        target_compile_options(${target} PRIVATE /W4 /permissive- /EHsc)
    endforeach()
else()
    foreach(target nexoai_compute nexoai_contracts nexoai_byte_track_adapter nexoai_analytics nexoai_inference nexoai_core)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()

if(NEXOAI_BUILD_PYTHON_BINDINGS)
    set(NEXOAI_PYBIND11_ROOT "" CACHE PATH
        "Pinned pybind11 3.0.4 CMake package root from a local Python environment")
    set(NEXOAI_PYTHON_OUTPUT_DIR "${CMAKE_BINARY_DIR}/python" CACHE PATH
        "Output directory for the development/QA Python extension")
    if(NOT NEXOAI_PYBIND11_ROOT)
        message(FATAL_ERROR
            "NEXOAI_PYBIND11_ROOT is required for bindings; use the pinned pybind11 3.0.4 package for Python 3.12")
    endif()
    find_package(Python 3.12 EXACT COMPONENTS Interpreter Development.Module REQUIRED)
    set(PYBIND11_FINDPYTHON ON)
    find_package(pybind11 3.0.4 EXACT CONFIG REQUIRED
        PATHS "${NEXOAI_PYBIND11_ROOT}" NO_DEFAULT_PATH)
    pybind11_add_module(nexoai_python MODULE src/bindings/python_bindings.cpp)
    target_link_libraries(nexoai_python PRIVATE nexoai_analytics)
    target_compile_features(nexoai_python PRIVATE cxx_std_20)
    set_target_properties(nexoai_python PROPERTIES
        OUTPUT_NAME nexoai_native
        LIBRARY_OUTPUT_DIRECTORY "${NEXOAI_PYTHON_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${NEXOAI_PYTHON_OUTPUT_DIR}")
    if(MSVC)
        target_compile_options(nexoai_python PRIVATE /W4 /permissive- /EHsc)
    endif()
endif()

if(NEXOAI_BUILD_TESTS)
    enable_testing()
    add_executable(nexoai_cpu_tests tests/cpu_tests.cpp)
    target_link_libraries(nexoai_cpu_tests PRIVATE nexoai_core)
    add_test(NAME nexoai_cpu_tests COMMAND nexoai_cpu_tests)
    if(WIN32)
        set_tests_properties(nexoai_cpu_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
    endif()
    if(TARGET nexoai_qt_launcher)
        add_executable(nexoai_launcher_tests tests/launcher_tests.cpp)
        if(WIN32)
            target_sources(nexoai_launcher_tests PRIVATE tests/launcher_version_fixture.rc)
        endif()
        target_link_libraries(nexoai_launcher_tests PRIVATE nexoai_launcher_support)
        if(MSVC)
            target_compile_options(nexoai_launcher_tests PRIVATE /W4 /permissive- /EHsc)
        endif()
        add_test(NAME nexoai_launcher_tests COMMAND nexoai_launcher_tests)
        add_test(NAME nexoai_version_resource_tests COMMAND "${CMAKE_COMMAND}"
            "-DTEST_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/version-resource-tests"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/version_resource_tests.cmake")
        if(WIN32)
            add_executable(nexoai_launcher_numeric_version_fixture
                tests/version_resource_fixture.cpp tests/launcher_numeric_version_fixture.rc)
            add_test(NAME nexoai_launcher_numeric_version_fixture
                COMMAND nexoai_launcher_tests --numeric-fixture
                    "$<TARGET_FILE:nexoai_launcher_numeric_version_fixture>")
            add_dependencies(nexoai_launcher_tests nexoai_qt_launcher)
            add_dependencies(nexoai_launcher_tests nexoai_launcher_numeric_version_fixture)
            add_test(NAME nexoai_built_launcher_version COMMAND nexoai_launcher_tests
                --launcher-version "$<TARGET_FILE:nexoai_qt_launcher>" "${NEXOAI_VERSION_DISPLAY}")
            add_test(NAME nexoai_installer_version_policy COMMAND powershell -NoProfile
                -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/test-version-policy.ps1"
                -LauncherExecutable "$<TARGET_FILE:nexoai_qt_launcher>" -ExpectedVersion "${NEXOAI_VERSION_IDENTITY}")
        endif()
    endif()
endif()

if(NEXOAI_BUILD_RUNTIME)
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
        file(GLOB NEXOAI_ORT_LINUX_SO "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so*")
        if(NOT NEXOAI_ORT_LINUX_SO)
            message(FATAL_ERROR
                "ONNXRUNTIME_ROOT must contain the Linux ONNX Runtime 1.25.0 lib/libonnxruntime.so files")
        endif()
        add_library(nexoai_onnxruntime SHARED IMPORTED)
        set_target_properties(nexoai_onnxruntime PROPERTIES
            IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so"
            INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include")
        # Build-tree RPATH so the CLI and tests find the ORT .so without an
        # install step or LD_LIBRARY_PATH.
        set(NEXOAI_ORT_RPATH "${ONNXRUNTIME_ROOT}/lib")
    else()
    if(NOT EXISTS "${ONNXRUNTIME_ROOT}/include/onnxruntime_cxx_api.h"
        OR NOT EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        OR NOT EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
        message(FATAL_ERROR
            "ONNXRUNTIME_ROOT must contain ONNX Runtime 1.25.0 include/ and lib/ files")
    endif()
    add_library(nexoai_onnxruntime SHARED IMPORTED)
    set_target_properties(nexoai_onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${ONNXRUNTIME_ROOT}/lib/onnxruntime.lib"
        IMPORTED_LOCATION "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
        INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_ROOT}/include")
    endif()

    if(NEXOAI_ENABLE_TENSORRT)
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
            add_library(nexoai_cuda_runtime SHARED IMPORTED)
            set_target_properties(nexoai_cuda_runtime PROPERTIES
                IMPORTED_IMPLIB "${CUDA_CUDART_LIBRARY}"
                IMPORTED_LOCATION "${CUDA_CUDART_DLL}"
                INTERFACE_INCLUDE_DIRECTORIES "${CUDA_RUNTIME_INCLUDE_DIRS}")
            set(NEXOAI_CUDART_TARGET nexoai_cuda_runtime)
        else()
            find_package(CUDAToolkit 12 REQUIRED)
            set(NEXOAI_CUDART_TARGET CUDA::cudart)
        endif()
    endif()
    # Qt6 is the sole GUI backend; no legacy display backend is linked.
    find_package(OpenCV ${NEXOAI_OPENCV_MIN_VERSION} REQUIRED COMPONENTS
        core imgproc imgcodecs videoio)

    add_library(nexoai_runtime
        src/runtime/capture.cpp
        src/runtime/engine_pipeline.cpp
        src/runtime/evidence.cpp
        src/runtime/model_manifest.cpp
        src/runtime/onnx_session.cpp
    )
    target_include_directories(nexoai_runtime PUBLIC include)
    target_link_libraries(nexoai_runtime
        PUBLIC nexoai_core
        PRIVATE nexoai_onnxruntime
            opencv_imgcodecs opencv_videoio)
    if(WIN32)
        target_link_libraries(nexoai_runtime PRIVATE bcrypt ws2_32)
    else()
        # dlopen/dlsym for the Linux CUDA Driver API probe (compute.cpp).
        target_link_libraries(nexoai_runtime PRIVATE ${CMAKE_DL_LIBS})
    endif()
    if(NEXOAI_ENABLE_TENSORRT)
        target_sources(nexoai_runtime PRIVATE src/runtime/tensorrt_runtime.cpp)
        target_include_directories(nexoai_runtime PRIVATE "${TENSORRT_INCLUDE_DIR}")
        target_link_libraries(nexoai_runtime PUBLIC ${NEXOAI_CUDART_TARGET})
        target_link_libraries(nexoai_runtime PRIVATE "${TENSORRT_NVINFER_LIBRARY}")
        target_compile_definitions(nexoai_runtime PRIVATE NEXOAI_WITH_TENSORRT=1)
    endif()
    if(MSVC)
        target_compile_options(nexoai_runtime PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(nexoai_runtime PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(NEXOAI_ENABLE_INTERNAL_DIAGNOSTICS)
        target_compile_definitions(nexoai_runtime PUBLIC NEXOAI_INTERNAL_DIAGNOSTICS=1)
    endif()

    if(TARGET nexoai_python)
        target_compile_definitions(nexoai_python PRIVATE NEXOAI_PYTHON_WITH_ENGINE_RUNTIME=1)
        target_link_libraries(nexoai_python PRIVATE nexoai_runtime)
        if(WIN32)
        add_custom_command(TARGET nexoai_python POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nexoai_onnxruntime>
                $<TARGET_FILE_DIR:nexoai_python>)
        if(NEXOAI_ENABLE_TENSORRT)
            target_link_libraries(nexoai_python PRIVATE delayimp)
            target_link_options(nexoai_python PRIVATE
                "/DELAYLOAD:cudart64_12.dll"
                "/DELAYLOAD:nvinfer_11.dll")
        endif()
        endif()
    endif()

    add_executable(nexoai_native
        src/app/main.cpp
        src/app/qt_mosaic_viewer.cpp
        src/app/qt_mosaic_viewer.hpp)
    set_target_properties(nexoai_native PROPERTIES
        BUILD_RPATH "${NEXOAI_ORT_RPATH}"
        INSTALL_RPATH "$ORIGIN/../lib")
    target_link_libraries(nexoai_native PRIVATE nexoai_runtime)
    target_link_libraries(nexoai_native PRIVATE Qt6::Widgets Qt6::Gui)
    if(MSVC)
        target_compile_options(nexoai_native PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(nexoai_native PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    set_target_properties(nexoai_native PROPERTIES
        OUTPUT_NAME "${NEXOAI_PRODUCT_EXE}")
    if(WIN32)
    # Embed the same canonical icon in the Qt inference viewer itself. The
    # launcher has this resource too, but it is a separate executable.
    target_sources(nexoai_native PRIVATE "${NEXOAI_PRODUCT_RESOURCE}")
    add_custom_command(TARGET nexoai_native POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:nexoai_onnxruntime>
            $<TARGET_FILE_DIR:nexoai_native>)
    # Double-click support without activate-native.ps1: the Windows loader
    # resolves DLLs next to the executable, so stage the OpenCV runtime DLLs
    # app-local beside NexoAIVision.exe (the launcher sibling shares the same
    # directory, so its CreateProcessW child inherits this closure). The MSI
    # already stages its own bin\ closure in installer/native/build-installer.ps1;
    # this only affects dev build output directories, never installer payloads.
    get_filename_component(_NEXOAI_OPENCV_BIN "${OpenCV_DIR}/../bin" ABSOLUTE)
    file(GLOB _NEXOAI_OPENCV_WORLD_DLLS "${_NEXOAI_OPENCV_BIN}/opencv_world*.dll")
    file(GLOB _NEXOAI_OPENCV_FFMPEG_DLLS "${_NEXOAI_OPENCV_BIN}/opencv_videoio_ffmpeg*_64.dll")
    foreach(_NEXOAI_OPENCV_DLL IN LISTS _NEXOAI_OPENCV_WORLD_DLLS _NEXOAI_OPENCV_FFMPEG_DLLS)
        # Skip debug-suffixed variants (opencv_world4120d.dll); presets build Release.
        if(_NEXOAI_OPENCV_DLL MATCHES "d\\.dll$")
            continue()
        endif()
        add_custom_command(TARGET nexoai_native POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_NEXOAI_OPENCV_DLL}"
                $<TARGET_FILE_DIR:nexoai_native>)
    endforeach()
    unset(_NEXOAI_OPENCV_BIN)
    unset(_NEXOAI_OPENCV_WORLD_DLLS)
    unset(_NEXOAI_OPENCV_FFMPEG_DLLS)
    unset(_NEXOAI_OPENCV_DLL)
    if(NEXOAI_ENABLE_TENSORRT)
        target_link_libraries(nexoai_native PRIVATE delayimp)
        target_link_options(nexoai_native PRIVATE
            "/DELAYLOAD:cudart64_12.dll"
            "/DELAYLOAD:nvinfer_11.dll")
    endif()
    endif()

    if(WIN32)
    add_library(nexoai_installer_ca SHARED
        src/foundation/compute.cpp
        src/installer/installer_custom_action.cpp
    )
    target_include_directories(nexoai_installer_ca PRIVATE include)
    target_link_libraries(nexoai_installer_ca PRIVATE dxgi advapi32 msi)
    target_compile_options(nexoai_installer_ca PRIVATE /W4 /permissive- /EHsc)
    set_target_properties(nexoai_installer_ca PROPERTIES
        OUTPUT_NAME NexoAIHardwareProbeCA
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    if(NEXOAI_BUILD_TESTS)
        if(MSVC)
            set(NEXOAI_TEST_COMPILE_OPTIONS /W4 /permissive- /EHsc)
        else()
            set(NEXOAI_TEST_COMPILE_OPTIONS -Wall -Wextra -Wpedantic)
        endif()
        add_executable(nexoai_evidence_tests tests/evidence_tests.cpp)
        target_link_libraries(nexoai_evidence_tests PRIVATE nexoai_runtime)
        target_compile_options(nexoai_evidence_tests PRIVATE ${NEXOAI_TEST_COMPILE_OPTIONS})
        add_test(NAME nexoai_evidence_tests COMMAND nexoai_evidence_tests)
        if(WIN32)
        set_tests_properties(nexoai_evidence_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>;PATH=path_list_prepend:${ONNXRUNTIME_ROOT}/lib")
        else()
            # Linux Fase 1: resolve the ORT .so via build-tree RPATH.
            set_target_properties(nexoai_evidence_tests PROPERTIES
                BUILD_RPATH "${NEXOAI_ORT_RPATH}")
        endif()

        add_executable(nexoai_capture_diagnostics_tests tests/capture_diagnostics_tests.cpp)
        target_link_libraries(nexoai_capture_diagnostics_tests PRIVATE nexoai_runtime)
        if(WIN32)
            target_link_libraries(nexoai_capture_diagnostics_tests PRIVATE ws2_32)
        endif()
        target_compile_options(nexoai_capture_diagnostics_tests PRIVATE ${NEXOAI_TEST_COMPILE_OPTIONS})
        add_test(NAME nexoai_capture_diagnostics_tests COMMAND nexoai_capture_diagnostics_tests)
        if(WIN32)
        set_tests_properties(nexoai_capture_diagnostics_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>;PATH=path_list_prepend:${ONNXRUNTIME_ROOT}/lib")
        else()
            set_target_properties(nexoai_capture_diagnostics_tests PROPERTIES
                BUILD_RPATH "${NEXOAI_ORT_RPATH}")
        endif()

        add_executable(nexoai_onnx_tests tests/onnx_tests.cpp)
        target_link_libraries(nexoai_onnx_tests PRIVATE nexoai_runtime)
        target_compile_options(nexoai_onnx_tests PRIVATE ${NEXOAI_TEST_COMPILE_OPTIONS})
        if(WIN32)
        add_custom_command(TARGET nexoai_onnx_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nexoai_onnxruntime>
                $<TARGET_FILE_DIR:nexoai_onnx_tests>)
        add_test(NAME nexoai_onnx_tests COMMAND nexoai_onnx_tests)
        set_tests_properties(nexoai_onnx_tests PROPERTIES
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>;PATH=path_list_prepend:${ONNXRUNTIME_ROOT}/lib")
        else()
            set_target_properties(nexoai_onnx_tests PROPERTIES
                BUILD_RPATH "${NEXOAI_ORT_RPATH}")
            add_test(NAME nexoai_onnx_tests COMMAND nexoai_onnx_tests)
        endif()
    endif()

    # Opt-in GPU integration tests stay Windows-only until the Fase 3 Linux
    # CUDA/TensorRT backends (dumpbin closure copy, DELAYLOAD, TRT headers).
    if(NOT UNIX)
    if(NEXOAI_ENABLE_TRT_INTEGRATION_TESTS AND NEXOAI_ENABLE_TENSORRT)
        set(NEXOAI_TEST_ENGINE "" CACHE FILEPATH "Engine used by the opt-in TensorRT smoke test")
        if(NOT EXISTS "${NEXOAI_TEST_ENGINE}")
            message(FATAL_ERROR "NEXOAI_TEST_ENGINE must point to a compatible engine")
        endif()
        add_executable(nexoai_trt_smoke tests/trt_smoke.cpp)
        target_link_libraries(nexoai_trt_smoke PRIVATE nexoai_runtime)
        target_include_directories(nexoai_trt_smoke PRIVATE "${TENSORRT_INCLUDE_DIR}")
        add_test(NAME nexoai_trt_smoke COMMAND nexoai_trt_smoke "${NEXOAI_TEST_ENGINE}")

        set(NEXOAI_TEST_POSE_ENGINE "" CACHE FILEPATH "Pose engine used by the opt-in TensorRT pipeline parity test")
        set(NEXOAI_TEST_PERSON_IMAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/person_regression.jpg"
            CACHE FILEPATH "Offline person image used by the opt-in TensorRT pipeline parity test")
        if(NOT EXISTS "${NEXOAI_TEST_POSE_ENGINE}")
            message(FATAL_ERROR "NEXOAI_TEST_POSE_ENGINE must point to a compatible pose engine")
        endif()
        add_executable(nexoai_trt_pipeline_parity tests/trt_pipeline_parity.cpp)
        target_link_libraries(nexoai_trt_pipeline_parity PRIVATE nexoai_runtime)
        target_include_directories(nexoai_trt_pipeline_parity SYSTEM PRIVATE
            "${NEXOAI_BYTE_TRACK_ROOT}/include"
            "${NEXOAI_EIGEN_ROOT}")
        target_include_directories(nexoai_trt_pipeline_parity PRIVATE
            "${TENSORRT_INCLUDE_DIR}")
        add_test(NAME nexoai_trt_pipeline_parity
            COMMAND nexoai_trt_pipeline_parity
                "${NEXOAI_TEST_ENGINE}" "${NEXOAI_TEST_POSE_ENGINE}" "${NEXOAI_TEST_PERSON_IMAGE}")
    endif()
    endif()

    if(NOT UNIX)
    if(NEXOAI_ENABLE_ONNX_CUDA_INTEGRATION_TESTS)
        unset(NEXOAI_ONNX_CUDA_RUNTIME_DIR CACHE)
        set(NEXOAI_ONNX_CUDA_DEPENDENCY_ROOTS "" CACHE STRING
            "Semicolon-separated trusted local roots for the ONNX Runtime CUDA DLL closure")
        set(NEXOAI_ONNX_CUDA_PPE_MODEL "" CACHE FILEPATH
            "Verified PPE ONNX model used by the opt-in CUDA pipeline integration test")
        set(NEXOAI_ONNX_CUDA_POSE_MODEL "" CACHE FILEPATH
            "Verified pose ONNX model used by the opt-in CUDA pipeline integration test")
        set(NEXOAI_ONNX_REAL_PERSON_IMAGE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/person_regression.jpg"
            CACHE FILEPATH "Offline person image used by the native real-model regression")
        foreach(required_file
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_cuda.dll"
                "${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                "${NEXOAI_ONNX_CUDA_PPE_MODEL}"
                "${NEXOAI_ONNX_CUDA_PPE_MODEL}.manifest.json"
                "${NEXOAI_ONNX_CUDA_POSE_MODEL}"
                "${NEXOAI_ONNX_CUDA_POSE_MODEL}.manifest.json"
                "${NEXOAI_ONNX_REAL_PERSON_IMAGE}")
            if(NOT EXISTS "${required_file}")
                message(FATAL_ERROR
                    "ONNX CUDA integration prerequisite is missing: ${required_file}")
            endif()
        endforeach()
        if(NOT NEXOAI_ONNX_CUDA_DEPENDENCY_ROOTS)
            message(FATAL_ERROR
                "NEXOAI_ONNX_CUDA_DEPENDENCY_ROOTS must list trusted local CUDA dependency roots")
        endif()
        find_program(NEXOAI_DUMPBIN_EXECUTABLE dumpbin
            HINTS "$ENV{VCToolsInstallDir}/bin/Hostx64/x64"
            NO_DEFAULT_PATH)
        if(NOT NEXOAI_DUMPBIN_EXECUTABLE)
            message(FATAL_ERROR "MSVC dumpbin is required to resolve the local ONNX CUDA runtime closure")
        endif()
        function(nexoai_copy_onnx_cuda_runtime_closure target)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                    "-DDESTINATION_DIR=$<TARGET_FILE_DIR:${target}>"
                    "-DDUMPBIN=${NEXOAI_DUMPBIN_EXECUTABLE}"
                    "-DSEEDS=${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll;${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_cuda.dll;${ONNXRUNTIME_ROOT}/lib/onnxruntime_providers_shared.dll"
                    "-DSEARCH_ROOTS=${NEXOAI_ONNX_CUDA_DEPENDENCY_ROOTS}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CopyOnnxCudaRuntimeClosure.cmake")
        endfunction()
        add_executable(nexoai_onnx_cuda_integration_tests tests/onnx_cuda_integration_tests.cpp)
        target_link_libraries(nexoai_onnx_cuda_integration_tests PRIVATE nexoai_runtime nexoai_onnxruntime)
        target_include_directories(nexoai_onnx_cuda_integration_tests PRIVATE
            "${NEXOAI_BYTE_TRACK_ROOT}/include")
        target_compile_options(nexoai_onnx_cuda_integration_tests PRIVATE /W4 /permissive- /EHsc)
        nexoai_copy_onnx_cuda_runtime_closure(nexoai_onnx_cuda_integration_tests)
        nexoai_copy_onnx_cuda_runtime_closure(nexoai_native)
        add_test(NAME nexoai_onnx_pose_cpu_standalone_tests
            COMMAND nexoai_onnx_cuda_integration_tests
                standalone-pose-cpu "${NEXOAI_ONNX_CUDA_POSE_MODEL}")
        add_test(NAME nexoai_onnx_cuda_ppe_only_tests
            COMMAND nexoai_onnx_cuda_integration_tests
                ppe-only "${NEXOAI_ONNX_CUDA_PPE_MODEL}")
        add_test(NAME nexoai_onnx_cuda_hybrid_pipeline_tests
            COMMAND nexoai_onnx_cuda_integration_tests pipeline
                "${NEXOAI_ONNX_CUDA_PPE_MODEL}" "${NEXOAI_ONNX_CUDA_POSE_MODEL}"
                "${NEXOAI_ONNX_REAL_PERSON_IMAGE}")
        set_tests_properties(
            nexoai_onnx_pose_cpu_standalone_tests
            nexoai_onnx_cuda_ppe_only_tests
            nexoai_onnx_cuda_hybrid_pipeline_tests
            PROPERTIES
            ENVIRONMENT "NEXOAI_ONNX_CUDA_RUNTIME_DIR=$<TARGET_FILE_DIR:nexoai_onnx_cuda_integration_tests>"
            ENVIRONMENT_MODIFICATION
                "PATH=path_list_prepend:$<TARGET_FILE_DIR:opencv_core>")
        set_tests_properties(nexoai_onnx_pose_cpu_standalone_tests PROPERTIES
            LABELS "onnx;acceptance;pose-cpu;hybrid-rationale-ort-cuda-1.25-gtx1650ti")
        set_tests_properties(
            nexoai_onnx_cuda_ppe_only_tests
            nexoai_onnx_cuda_hybrid_pipeline_tests
            PROPERTIES
            SKIP_RETURN_CODE 77)
        set_tests_properties(nexoai_onnx_cuda_ppe_only_tests PROPERTIES
            LABELS "gpu;onnx;acceptance;ppe-cuda")
        set_tests_properties(nexoai_onnx_cuda_hybrid_pipeline_tests PROPERTIES
            LABELS "gpu;onnx;acceptance;hybrid;ppe-cuda;pose-cpu;known-pose-cuda-access-violation")
    endif()
    endif()
endif()

include(cmake/Packaging.cmake)
