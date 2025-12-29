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
)

# Executable
add_executable(x11-fb-streamer ${STREAMER_SOURCES})

# Link libraries
target_link_libraries(x11-fb-streamer
    ${X11_LIBRARIES}
    ${XRANDR_LIBRARIES}
    ${DRM_LIBRARIES}
    ${PULSE_LIBRARIES}
)

target_compile_options(x11-fb-streamer PRIVATE
    ${X11_CFLAGS_OTHER}
    ${XRANDR_CFLAGS_OTHER}
    ${DRM_CFLAGS_OTHER}
    -Wall
    -Wextra
    -Werror
    -Wno-format-truncation  # Allow format truncation warnings (safe for our use case)
)

