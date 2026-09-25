# SPDX-License-Identifier: AGPL-3.0-only

# One install manifest for Windows and UNIX. Native package scripts are thin
# wrappers around this contract: they select a generator and provide verified
# platform dependencies, but do not assemble a second product layout.

set(NEXOAI_INSTALL_ROOT "opt/nexoai-vision")
set(NEXOAI_INSTALL_BIN "${NEXOAI_INSTALL_ROOT}/bin")
set(NEXOAI_INSTALL_LIB "${NEXOAI_INSTALL_ROOT}/lib")
set(NEXOAI_INSTALL_MODELS "${NEXOAI_INSTALL_ROOT}/models")

if(TARGET nexoai_native)
    install(TARGETS nexoai_native RUNTIME DESTINATION "${NEXOAI_INSTALL_BIN}")
endif()
if(TARGET nexoai_qt_launcher)
    install(TARGETS nexoai_qt_launcher RUNTIME DESTINATION "${NEXOAI_INSTALL_BIN}")
endif()

if(UNIX AND ONNXRUNTIME_ROOT)
    file(GLOB _NEXOAI_ORT_RUNTIME_FILES LIST_DIRECTORIES FALSE
        "${ONNXRUNTIME_ROOT}/lib/libonnxruntime.so*")
    if(_NEXOAI_ORT_RUNTIME_FILES)
        install(FILES ${_NEXOAI_ORT_RUNTIME_FILES} DESTINATION "${NEXOAI_INSTALL_LIB}")
    endif()
elseif(WIN32 AND ONNXRUNTIME_ROOT)
    if(EXISTS "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll")
        install(FILES "${ONNXRUNTIME_ROOT}/lib/onnxruntime.dll"
            DESTINATION "${NEXOAI_INSTALL_BIN}")
    endif()
endif()

if(UNIX AND NEXOAI_ENABLE_TENSORRT)
    # Never ship libcuda.so or NVIDIA driver libraries; those belong to the
    # target host. TensorRT and CUDA user-space libraries are package inputs.
    install(DIRECTORY "${TENSORRT_ROOT}/lib/"
        DESTINATION "${NEXOAI_INSTALL_LIB}" FILES_MATCHING
        PATTERN "lib*.so*" PATTERN "libcuda.so*" EXCLUDE
        PATTERN "libnvidia*.so*" EXCLUDE)
    if(TARGET CUDA::cudart)
        get_target_property(_NEXOAI_CUDART_LOCATION CUDA::cudart IMPORTED_LOCATION)
        if(NOT _NEXOAI_CUDART_LOCATION OR _NEXOAI_CUDART_LOCATION MATCHES "NOTFOUND")
            set(_NEXOAI_CUDART_LOCATION "${CUDA_CUDART_LIBRARY}")
        endif()
        if(_NEXOAI_CUDART_LOCATION AND NOT _NEXOAI_CUDART_LOCATION MATCHES "NOTFOUND")
            get_filename_component(_NEXOAI_CUDART_DIR
                "${_NEXOAI_CUDART_LOCATION}" DIRECTORY)
            install(DIRECTORY "${_NEXOAI_CUDART_DIR}/"
                DESTINATION "${NEXOAI_INSTALL_LIB}" FILES_MATCHING
                PATTERN "lib*.so*" PATTERN "libcuda.so*" EXCLUDE
                PATTERN "libnvidia*.so*" EXCLUDE)
        endif()
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../models")
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../models/"
        DESTINATION "${NEXOAI_INSTALL_MODELS}"
        PATTERN "*.engine" EXCLUDE
        PATTERN "*.plan" EXCLUDE
        PATTERN "*.trt" EXCLUDE)
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../docs/Architecture.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/Architecture.md"
        DESTINATION "${NEXOAI_INSTALL_ROOT}")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../docs/LinuxPort.md")
    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../docs/LinuxPort.md"
        DESTINATION "${NEXOAI_INSTALL_ROOT}" RENAME README-Linux.md)
endif()

if(UNIX)
    set(_NEXOAI_INSTALLER_LINUX_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../installer/linux")
    install(PROGRAMS "${_NEXOAI_INSTALLER_LINUX_DIR}/run.sh"
        DESTINATION "${NEXOAI_INSTALL_ROOT}")
    install(FILES "${_NEXOAI_INSTALLER_LINUX_DIR}/config.example"
        DESTINATION "${NEXOAI_INSTALL_ROOT}")
    install(FILES "${_NEXOAI_INSTALLER_LINUX_DIR}/nexoai-vision.desktop"
        DESTINATION "usr/share/applications")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/assets/icon.ico")
        install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../installer/native/assets/icon.ico"
            DESTINATION "usr/share/pixmaps" RENAME "nexoai-vision.ico")
    endif()
    install(FILES "${_NEXOAI_INSTALLER_LINUX_DIR}/config.example"
        DESTINATION "etc/nexoai-vision" RENAME config.example)
endif()

set(CPACK_PACKAGE_NAME "nexoai-vision")
set(CPACK_PACKAGE_VENDOR "Nexo AI Vision")
set(CPACK_PACKAGE_CONTACT "Nexo AI Vision maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Nexo AI Vision PPE and safety analytics")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "nexoai-vision")
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)

if(NEXOAI_PACKAGE_VERSION STREQUAL "")
    if(NEXOAI_PRODUCT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(NEXOAI_PACKAGE_VERSION
            "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    elseif(NEXOAI_FILE_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)")
        set(NEXOAI_PACKAGE_VERSION
            "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    else()
        set(NEXOAI_PACKAGE_VERSION "0.1.0")
    endif()
endif()
if(NOT NEXOAI_PACKAGE_VERSION MATCHES "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$")
    message(FATAL_ERROR
        "NEXOAI_PACKAGE_VERSION must be Debian-compatible major.minor.patch")
endif()
set(CPACK_PACKAGE_VERSION "${NEXOAI_PACKAGE_VERSION}")
if(WIN32)
    set(CPACK_PACKAGE_FILE_NAME "nexoai-vision-${CPACK_PACKAGE_VERSION}-windows-x86_64")
    set(CPACK_GENERATOR "TGZ")
else()
    set(CPACK_PACKAGE_FILE_NAME "nexoai-vision-${CPACK_PACKAGE_VERSION}-linux-x86_64")
    set(CPACK_GENERATOR "DEB;TGZ")
    set(CPACK_DEBIAN_PACKAGE_NAME "nexoai-vision")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Nexo AI Vision maintainers")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
    set(CPACK_DEBIAN_PACKAGE_SECTION "misc")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
    set(_NEXOAI_PACKAGE_DEPENDS "libc6, libgcc-s1, libstdc++6")
    string(APPEND _NEXOAI_PACKAGE_DEPENDS
        ", libqt6core6t64 | libqt6core6, libqt6gui6t64 | libqt6gui6, libqt6widgets6t64 | libqt6widgets6")
    string(APPEND _NEXOAI_PACKAGE_DEPENDS
        ", libopencv-core406t64 | libopencv-core406 | libopencv-core4.5d | libopencv-core410"
        ", libopencv-imgproc406t64 | libopencv-imgproc406 | libopencv-imgproc4.5d | libopencv-imgproc410"
        ", libopencv-imgcodecs406t64 | libopencv-imgcodecs406 | libopencv-imgcodecs4.5d | libopencv-imgcodecs410"
        ", libopencv-videoio406t64 | libopencv-videoio406 | libopencv-videoio4.5d | libopencv-videoio410")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "${_NEXOAI_PACKAGE_DEPENDS}")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_CURRENT_SOURCE_DIR}/../installer/linux/postinst")
endif()

include(CPack)
