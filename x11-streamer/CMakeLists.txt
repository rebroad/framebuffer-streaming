cmake_minimum_required(VERSION 3.10)
project(x11-framebuffer-server C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(X11 REQUIRED x11)
pkg_check_modules(XRANDR REQUIRED xrandr)
pkg_check_modules(DRM REQUIRED libdrm)
pkg_check_modules(PULSE REQUIRED libpulse libpulse-simple)

# x264 (optional - check if available)
find_library(X264_LIB x264 PATHS /usr/lib/x86_64-linux-gnu)
if(X264_LIB)
    set(X264_FOUND TRUE)
    set(X264_LIBRARIES ${X264_LIB})
    set(X264_INCLUDE_DIRS /usr/include)
    message(STATUS "Found x264: ${X264_LIB}")
else()
    set(X264_FOUND FALSE)
    message(WARNING "x264 not found - H.264 encoding will be disabled")
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)
include_directories(${X11_INCLUDE_DIRS})
include_directories(${XRANDR_INCLUDE_DIRS})
include_directories(${DRM_INCLUDE_DIRS})
include_directories(${PULSE_INCLUDE_DIRS})

# Source files
set(STREAMER_SOURCES
    src/main.c
    src/x11_streamer.c
    src/x11_output.c
    src/drm_fb.c
    src/protocol.c
    src/audio_capture.c
    src/dirty_rect.c
    src/encoding_metrics.c
)

# Add H.264 encoder if available
if(X264_FOUND)
    list(APPEND STREAMER_SOURCES src/h264_encoder.c)
    add_definitions(-DHAVE_X264)
endif()

# Executable
add_executable(x11-fb-streamer ${STREAMER_SOURCES})

# Link libraries
target_link_libraries(x11-fb-streamer
    ${X11_LIBRARIES}
    ${XRANDR_LIBRARIES}
    ${DRM_LIBRARIES}
    ${PULSE_LIBRARIES}
)

# Link x264 if available
if(X264_FOUND)
    target_link_libraries(x11-fb-streamer ${X264_LIBRARIES})
    target_include_directories(x11-fb-streamer PRIVATE ${X264_INCLUDE_DIRS})
endif()

target_compile_options(x11-fb-streamer PRIVATE
    ${X11_CFLAGS_OTHER}
    ${XRANDR_CFLAGS_OTHER}
    ${DRM_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Werror
    -Wno-format-truncation  # Allow format truncation warnings (safe for our use case)
)

