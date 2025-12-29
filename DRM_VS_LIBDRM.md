# DRM Headers vs libdrm Library

## The Distinction

There are **two different things** with similar names:

### 1. `/usr/include/drm/` - DRM Kernel Interface Headers

**What they are:**
- **Kernel interface headers** that define the low-level ioctl() structures
- Part of the Linux kernel headers package
- Define structures like `struct drm_mode_get_connector`, `struct drm_mode_card_res`
- Define ioctl numbers like `DRM_IOCTL_MODE_GETCONNECTOR`
- **No functions** - just structures and constants

**Files:**
- `/usr/include/drm/drm.h` - Basic DRM ioctl definitions
- `/usr/include/drm/drm_mode.h` - DRM mode structures

**What you use them for:**
- Direct `ioctl()` system calls
- Manual memory management
- Low-level DRM access

**Example:**
```c
#include <drm/drm.h>
#include <drm/drm_mode.h>

struct drm_mode_get_connector conn;
memset(&conn, 0, sizeof(conn));
conn.connector_id = connector_id;
ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn);
// Manual memory allocation, manual cleanup
```

**Available in:**
- ✅ Linux systems (kernel headers)
- ✅ Android NDK (`drm/drm.h`, `drm/drm_mode.h`)
- ✅ All systems with DRM support

### 2. `/usr/include/xf86drm*.h` - libdrm Userspace Library Headers

**What they are:**
- **Userspace library headers** that provide convenient wrapper functions
- Part of the `libdrm` package (separate from kernel)
- Provide functions like `drmModeGetConnector()`, `drmModeGetResources()`
- Handle memory management automatically
- **High-level API** that wraps ioctl() calls

**Files:**
- `/usr/include/xf86drm.h` - Basic libdrm functions
- `/usr/include/xf86drmMode.h` - Mode-related functions (connectors, properties, etc.)

**What you use them for:**
- Convenient wrapper functions
- Automatic memory management
- Cleaner, simpler code

**Example:**
```c
#include <xf86drm.h>
#include <xf86drmMode.h>

drmModeResPtr resources = drmModeGetResources(fd);
drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[0]);
// Automatic memory management, cleaner code
drmModeFreeConnector(connector);
drmModeFreeResources(resources);
```

**Available in:**
- ✅ Linux desktop systems (when libdrm-dev is installed)
- ❌ **NOT in Android NDK** (userspace library, not part of NDK)
- ❌ **NOT on most Android devices** (unless specifically installed)

## The Relationship

**libdrm uses the DRM headers internally:**

```
libdrm library (xf86drmMode.h)
    ↓ (calls)
ioctl() system calls
    ↓ (uses)
DRM kernel headers (drm/drm.h, drm/drm_mode.h)
    ↓ (communicates with)
Linux kernel DRM subsystem
```

**libdrm functions are wrappers around ioctl() calls:**

```c
// What libdrm does internally:
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connectorId) {
    // 1. Allocate memory for struct drm_mode_get_connector
    // 2. Call ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, ...)
    // 3. Allocate memory for properties, modes, etc.
    // 4. Call ioctl() again to get the data
    // 5. Convert to libdrm's drmModeConnector structure
    // 6. Return pointer
}
```

## In Our Code

**What we have:**
- ✅ NDK provides `drm/drm.h` and `drm/drm_mode.h` (kernel headers)
- ❌ NDK does **NOT** provide `xf86drm.h` or `xf86drmMode.h` (libdrm headers)
- ❌ NDK does **NOT** provide `libdrm.so` library

**What we do:**
1. **Try libdrm first** (if `libdrm.so` is available at runtime):
   - Use `dlsym()` to load functions from `libdrm.so`
   - Use libdrm's convenient API
   - Cleaner code

2. **Fall back to ioctl()** (if libdrm not available):
   - Use NDK's `drm/drm.h` and `drm/drm_mode.h` headers
   - Make direct `ioctl()` calls
   - Manual memory management
   - More complex but works without libdrm

## Summary

| Component | Type | Location | In NDK? | What It Provides |
|-----------|------|----------|---------|------------------|
| `drm/drm.h` | Kernel header | `/usr/include/drm/` | ✅ Yes | ioctl() structures, constants |
| `drm/drm_mode.h` | Kernel header | `/usr/include/drm/` | ✅ Yes | Mode structures |
| `xf86drm.h` | libdrm header | `/usr/include/` | ❌ No | libdrm functions |
| `xf86drmMode.h` | libdrm header | `/usr/include/` | ❌ No | Mode-related functions |
| `libdrm.so` | Library | `/usr/lib/` | ❌ No | Runtime library |

**Key Point:** `/usr/include/drm/` contains **kernel interface headers** (for ioctl()), not libdrm headers. libdrm is a separate userspace library that uses these kernel headers internally.

