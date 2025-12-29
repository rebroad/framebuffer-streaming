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

# Note: We use ioctl() directly with NDK's drm/drm.h headers
# No need for libdrm library - we implement the DRM access ourselves
message(STATUS "Using direct ioctl() for DRM access (no libdrm dependency)")

