# DRM Headers in Android NDK

## The Issue

When trying to compile native code that accesses DRM/KMS to read EDID, we encountered:
```
fatal error: 'linux/drm.h' file not found
```

## Why This Happens

**Android NDK provides:**
- ✅ `drm/drm.h` - Basic DRM ioctl definitions
- ✅ `drm/drm_mode.h` - DRM mode structures
- ❌ **NOT** `linux/drm.h` - This is a Linux kernel header path
- ❌ **NOT** `libdrm` library - The userspace library (xf86drm, xf86drmMode functions)

**The NDK structure:**
```
NDK/sysroot/usr/include/
  ├── drm/
  │   ├── drm.h          ✅ Available
  │   ├── drm_mode.h     ✅ Available
  │   └── ...
  └── linux/
      └── (no drm.h)     ❌ Not available
```

## Why NDK Doesn't Include libdrm

1. **libdrm is a userspace library** - Not part of the kernel or NDK
2. **Device-specific** - Different Android devices may have different DRM implementations
3. **Not standard Android API** - Android doesn't expose DRM directly to apps
4. **Security** - Direct DRM access requires root or special permissions

## Solutions

### Option 1: Use NDK DRM Headers with ioctl() (Implemented)
- Use `drm/drm.h` and `drm/drm_mode.h` from NDK
- Use `ioctl()` system calls directly (no libdrm needed)
- More complex but works without external libraries
- **Status**: ✅ Implemented with hybrid approach
  - Tries libdrm functions first (via `dlsym()`) if `libdrm.so` is available at runtime
  - Falls back to direct ioctl() calls if libdrm is not available
  - No build-time dependency on libdrm

### Option 2: Cross-compile libdrm for Android
- Build libdrm as a static library for Android
- Include it in the app
- Provides convenient `drmModeGetConnector()` functions
- **Complexity**: High (requires cross-compilation setup)

### Option 3: Use System Properties (Fallback)
- Query Android system properties for EDID
- Vendor-specific, not guaranteed
- **Status**: ✅ Implemented

### Option 4: Use Android Display API (Current Fallback)
- Only provides current mode, not all modes
- **Status**: ✅ Implemented

## Current Implementation

The code uses a hybrid approach with fallback chain:
1. **Try DRM access via libdrm** (if `libdrm.so` available at runtime) - ✅ Implemented
   - Uses `dlsym()` to load libdrm functions dynamically
   - Cleaner API when available
2. **Try DRM access via ioctl()** (direct system calls) - ✅ Implemented
   - Uses NDK's `drm/drm.h` and `drm/drm_mode.h` headers
   - Works without libdrm library
   - May require root permissions on some devices
3. **Try system properties** - ✅ Working
   - Vendor-specific, not guaranteed
4. **Fallback to Display API** - ✅ Working (current mode only)
   - Always available but limited data

## Implementation Details

The DRM access code:
- Defines libdrm structs manually (since libdrm headers aren't in NDK)
- Tries to use libdrm functions first (cleaner code)
- Falls back to ioctl() if libdrm not available (more complex but works)
- Gracefully handles failures (may require root, but tries anyway)

## Recommendation

The **hybrid approach** is now implemented:
- Best of both worlds: cleaner code when libdrm is available, working fallback when it's not
- No build-time dependency on libdrm
- Works on devices with or without libdrm
- Falls back gracefully through the chain: libdrm → ioctl() → system properties → Display API

