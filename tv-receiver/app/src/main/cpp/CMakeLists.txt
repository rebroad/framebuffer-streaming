cmake_minimum_required(VERSION 3.22.1)
project(edidparser)

# Source files
add_library(edidparser SHARED
    edid_parser.cpp
)

# Link libraries
# Note: libdrm may not be available on all Android devices
# The code will handle this gracefully at runtime
target_link_libraries(edidparser
    log
)

# Include directories
# For Android NDK, we need to include system headers
target_include_directories(edidparser PRIVATE
    ${CMAKE_SYSROOT}/usr/include
)

# Try to find libdrm, but don't fail if not found
find_library(drm_LIB drm)
if(drm_LIB)
    target_link_libraries(edidparser ${drm_LIB})
    target_compile_definitions(edidparser PRIVATE HAVE_LIBDRM=1)
    message(STATUS "Found libdrm: ${drm_LIB}")
else()
    message(WARNING "libdrm not found - DRM EDID access will not be available")
    target_compile_definitions(edidparser PRIVATE HAVE_LIBDRM=0)
endif()

