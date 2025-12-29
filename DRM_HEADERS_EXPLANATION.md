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

### Option 1: Use NDK DRM Headers with ioctl() (Current Approach)
- Use `drm/drm.h` and `drm/drm_mode.h` from NDK
- Use `ioctl()` system calls directly (no libdrm needed)
- More complex but works without external libraries
- **Status**: ✅ Implemented (but currently disabled because HAVE_LIBDRM=0)

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

The code is structured to:
1. **Try DRM access** (if libdrm available) - Currently disabled (HAVE_LIBDRM=0)
2. **Try system properties** - ✅ Working
3. **Fallback to Display API** - ✅ Working (current mode only)

## Why DRM Access is Disabled

The NDK doesn't provide `libdrm`, so `HAVE_LIBDRM=0` is set. The DRM code is conditionally compiled out. To enable it, we would need to:
1. Cross-compile libdrm for Android
2. Or rewrite to use ioctl() directly with NDK's `drm/drm.h` headers

## Recommendation

For now, the **system properties approach (Option 2)** is the most practical:
- Works on many Android devices
- No need for root
- No complex cross-compilation
- Falls back gracefully to Display API if EDID not available

If full EDID access is critical, we could implement Option 1 (ioctl() with NDK headers), but it's more complex and may require root permissions on the device.

