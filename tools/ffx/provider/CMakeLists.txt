cmake_minimum_required(VERSION 3.22)

project(mkw_fsr_fg_vk LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

foreach(required
    MKW_FFX_SDK_ROOT
    MKW_FFX_MINIZ_ROOT
    MKW_FFX_GENERATED
    MKW_FFX_VULKAN_INCLUDE
    MKW_FFX_VULKAN_LIBRARY)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

set(FFX "${MKW_FFX_SDK_ROOT}/Kits/FidelityFX")

add_library(mkw_fsr_fg_vk SHARED
    "${FFX}/api/internal/ffx_api.cpp"
    "${FFX}/api/internal/ffx_assert.cpp"
    "${FFX}/api/internal/ffx_message.cpp"
    "${FFX}/api/internal/ffx_object_management.cpp"
    "${FFX}/api/internal/ffx_query_fallback.cpp"
    "${FFX}/backend/vk/ffx_backends_vk.cpp"
    "${FFX}/backend/vk/ffx_vk.cpp"
    "${FFX}/framegeneration/fsr3/internal/ffx_frameinterpolation.cpp"
    "${FFX}/framegeneration/fsr3/internal/ffx_opticalflow.cpp"
    "${FFX}/framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp"
    "${MKW_FFX_GENERATED}"
    "${CMAKE_CURRENT_LIST_DIR}/provider_glue.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/swapchain_stub.cpp"
    "${MKW_FFX_MINIZ_ROOT}/miniz.c"
)

set_target_properties(mkw_fsr_fg_vk PROPERTIES
    OUTPUT_NAME "mkw_fsr_fg_vk"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/out/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/out/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/out/lib"
    ARCHIVE_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_LIST_DIR}/out/lib"
)

target_compile_definitions(mkw_fsr_fg_vk PRIVATE
    FFX_BACKEND_VK=1
    FFX_FRAMEGENERATION=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_include_directories(mkw_fsr_fg_vk PRIVATE
    "${MKW_FFX_SDK_ROOT}"
    "${FFX}/api/include"
    "${FFX}/api/internal"
    "${FFX}/backend/vk"
    "${FFX}/framegeneration/include"
    "${FFX}/framegeneration/fsr3/include"
    "${MKW_FFX_MINIZ_ROOT}"
    "${MKW_FFX_VULKAN_INCLUDE}"
)

target_link_libraries(mkw_fsr_fg_vk PRIVATE
    "${MKW_FFX_VULKAN_LIBRARY}"
    version
    user32
)

if(MSVC)
    target_compile_options(mkw_fsr_fg_vk PRIVATE
        /W3
        /permissive-
        /Zc:__cplusplus
        /EHsc
        /Brepro
    )
    target_link_options(mkw_fsr_fg_vk PRIVATE /Brepro)
endif()
