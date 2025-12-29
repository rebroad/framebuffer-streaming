# Mode Capabilities Flow Analysis

## Current Flow

### 1. TV Receiver → X11 Streamer ✅

**TV Receiver sends:**
- Display name (from EDID or fallback)
- **Array of ALL modes** from EDID (or single current mode if EDID unavailable)
- Each mode includes: width, height, refresh_rate (Hz * 100)

**Location:** `tv-receiver/app/src/main/java/com/framebuffer/client/MainActivity.java:237-238`
```java
Protocol.sendHello(clientSocket.getOutputStream(), displayName, modes);
```

**Protocol:** `tv-receiver/app/src/main/java/com/framebuffer/client/Protocol.java:82-109`
- Sends `num_modes` (number of modes)
- Sends array of `DisplayMode` structures (width, height, refreshRate)

### 2. X11 Streamer Receives ✅

**X11 Streamer receives:**
- Parses HELLO message
- Extracts display name
- **Extracts ALL modes** from the message

**Location:** `x11-streamer/src/x11_streamer.c:67-97`
```c
hello_message_t *hello_msg = (hello_message_t *)payload;
display_mode_t *modes = NULL;
// ... parses all modes from payload
```

### 3. X11 Streamer Creates Virtual Output ⚠️ **ISSUE HERE**

**Problem:** X11 streamer only uses the **FIRST mode** to create the virtual output

**Location:** `x11-streamer/src/x11_streamer.c:114-125`
```c
if (modes && hello_msg->num_modes > 0) {
    // Use the first/preferred mode to create virtual output
    display_mode_t *preferred_mode = &modes[0];  // ⚠️ Only uses first mode!

    virtual_output_id = x11_context_create_virtual_output(
        streamer->x11_ctx,
        tv_display_name,
        preferred_mode->width,      // Only first mode's width
        preferred_mode->height,      // Only first mode's height
        preferred_mode->refresh_rate / 100  // Only first mode's refresh
    );
}
```

**What's missing:**
- All other modes are **discarded**
- Only width, height, refresh of first mode are passed to xserver

### 4. X11 Streamer → X Server ⚠️ **ISSUE HERE**

**X11 Streamer calls:**
- `x11_context_create_virtual_output()` with only first mode's parameters

**Location:** `x11-streamer/src/x11_output.c:259-331`
```c
RROutput x11_context_create_virtual_output(x11_context_t *ctx, const char *name,
                                            int width, int height, int refresh)
{
    // Creates virtual output via CREATE_XR_OUTPUT property
    // Format: "NAME:WIDTH:HEIGHT:REFRESH"
    snprintf(create_cmd, sizeof(create_cmd), "%s:%d:%d:%d", name, width, height, refresh);
    // Only passes ONE mode (the first/preferred one)
}
```

**What's missing:**
- No way to pass multiple modes to xserver
- CREATE_XR_OUTPUT only accepts single width/height/refresh

### 5. X Server Creates Virtual Output ⚠️ **ISSUE HERE**

**X Server receives:**
- Only the first mode's parameters (width, height, refresh)

**Location:** `xserver/hw/xfree86/drivers/video/modesetting/drmmode_xr_virtual.c:650-675`
```c
// CREATE_XR_OUTPUT format: "NAME:WIDTH:HEIGHT:REFRESH"
// Only receives single mode parameters
```

**X Server creates modes:**
- **FIXED set of common modes** - NOT the modes from TV receiver!

**Location:** `xserver/hw/xfree86/drivers/video/modesetting/drmmode_xr_virtual.c:207-243`
```c
static void
drmmode_xr_virtual_set_modes(xf86OutputPtr output, int width, int height, int refresh)
{
    // Create multiple common modes for virtual outputs
    int common_widths[] = {1920, 2560, 3840, 0};
    int common_heights[] = {1080, 1440, 2160, 0};

    // Creates modes like: 1920x1080, 1920x1440, 1920x2160, 2560x1080, etc.
    // These are HARDCODED, not from the TV receiver!
}
```

**Problem:**
- xserver creates a **fixed set of common resolutions** (1920x1080, 2560x1440, 3840x2160)
- It does **NOT** use the actual modes sent by the TV receiver
- The preferred mode (first mode) is marked, but all other modes are ignored

### 6. xrandr Query ❌ **WON'T SHOW TV MODES**

**What xrandr will show:**
```
Output: "Samsung Electric Company 55\""
  Modes:
    - 1920x1080 (preferred)  ← Only if it matches the fixed set
    - 2560x1440              ← Fixed mode, may not be supported by TV
    - 3840x2160              ← Fixed mode, may not be supported by TV
```

**What xrandr should show:**
```
Output: "Samsung Electric Company 55\""
  Modes:
    - 1920x1080@60Hz (preferred)  ← From TV EDID
    - 1920x1080@50Hz              ← From TV EDID
    - 1920x1080@30Hz              ← From TV EDID
    - 1280x720@60Hz               ← From TV EDID
    - 3840x2160@60Hz              ← From TV EDID
    - ... (all modes from TV EDID)
```

## Summary

| Step | Status | Issue |
|------|--------|-------|
| 1. TV receiver sends modes | ✅ Works | Sends all modes from EDID |
| 2. X11 streamer receives modes | ✅ Works | Receives all modes |
| 3. X11 streamer uses modes | ❌ **Broken** | Only uses first mode, discards others |
| 4. X11 streamer → xserver | ❌ **Broken** | CREATE_XR_OUTPUT only accepts single mode |
| 5. xserver creates modes | ❌ **Broken** | Creates fixed common modes, ignores TV modes |
| 6. xrandr shows modes | ❌ **Broken** | Shows fixed modes, not actual TV modes |

## The Problem

**Current behavior:**
- TV receiver correctly sends all mode capabilities
- X11 streamer correctly receives all mode capabilities
- **BUT**: X11 streamer only uses the first mode
- **AND**: xserver creates a fixed set of common modes
- **RESULT**: xrandr shows fixed modes, not the actual TV capabilities

**What needs to be fixed:**

1. **X11 Streamer** needs to pass ALL modes to xserver (not just first)
2. **CREATE_XR_OUTPUT** needs to accept multiple modes (or a new property/API)
3. **xserver's `drmmode_xr_virtual_set_modes`** needs to use the actual modes from TV receiver instead of fixed common modes

## Recommendation

To fix this, we need to:

1. **Extend CREATE_XR_OUTPUT** to accept multiple modes, OR
2. **Add a new property** (e.g., `XR_MODES`) to set modes after creation, OR
3. **Modify xserver** to accept modes via a different mechanism

The cleanest approach would be to extend the CREATE_XR_OUTPUT format to include all modes, or add a separate property to set modes after the output is created.

