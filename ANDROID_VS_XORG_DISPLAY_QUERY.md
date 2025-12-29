# Android vs Xorg Display Query Capabilities

## Xorg's Approach

**Xorg has direct EDID access:**
- Reads raw EDID (Extended Display Identification Data) from display via I2C/DDC
- Parses EDID to extract:
  - **All supported resolutions** (detailed timings, standard timings, established timings)
  - **All supported refresh rates** for each resolution
  - **Preferred/native mode**
  - **Physical dimensions** (mm)
  - **Color depth capabilities**
  - **Display name/manufacturer** from EDID
  - **CEA extension modes** (HDMI-specific modes)
- Uses DRM/KMS to query connector modes
- Provides comprehensive mode list via `xrandr`

**Example Xorg query:**
```c
// Xorg can do this:
drmModeConnectorPtr connector = drmModeGetConnector(fd, connector_id);
// Get EDID blob
drmModePropertyBlobPtr edid_blob = get_prop_blob(connector, "EDID");
// Parse EDID
xf86MonPtr mon = xf86InterpretEDID(scrnIndex, edid_blob->data);
// Get all modes from EDID + connector
DisplayModePtr modes = xf86OutputGetEDIDModes(output);
```

## Android's Limitations

**Android's public Display API provides:**
- ✅ Display name (from EDID, but may be limited)
- ✅ Current resolution (`getRealSize()`)
- ✅ Current refresh rate (`getRefreshRate()`)
- ❌ **NO comprehensive list of supported modes**
- ❌ **NO access to raw EDID data**
- ❌ **NO way to query all supported resolutions/refresh rates**

**What Android provides:**
```java
Display display = displayManager.getDisplays()[0];
String name = display.getName();              // ✅ Available
Point size = new Point();
display.getRealSize(size);                    // ✅ Current size only
float refreshRate = display.getRefreshRate(); // ✅ Current refresh rate only
// ❌ No API for: getAllSupportedModes(), getEDID(), etc.
```

## Why Android is Limited

1. **Abstraction Layer**: Android abstracts display management - apps don't need low-level EDID access
2. **Security**: Direct hardware access could be a security risk
3. **Fragmentation**: Different vendors handle displays differently
4. **Use Case**: Most Android apps don't need detailed mode information

## Potential Workarounds

### Option 1: Use Common Resolutions (Current Approach)
- **Pros**: Simple, works for most TVs
- **Cons**: May include unsupported modes, may miss supported modes
- **Current implementation**: Adds common resolutions (1080p, 720p, 4K) based on current display size

### Option 2: Query System Properties (Vendor-Specific)
```java
// Some vendors expose EDID via system properties (not reliable)
String edid = android.os.SystemProperties.get("ro.hdmi.edid");
// Requires system permissions, vendor-specific, not guaranteed
```
- **Pros**: Could get EDID if vendor exposes it
- **Cons**: Not standard, requires system permissions, vendor-specific

### Option 3: Use Hidden/Private APIs (Not Recommended)
- Android has internal EDID parsing, but it's not public
- Could use reflection to access private APIs
- **Cons**: May break with Android updates, not guaranteed to work

### Option 4: Native Code (JNI) to Access EDID
- Write native code to access DRM/KMS directly (like Xorg does)
- **Pros**: Full EDID access
- **Cons**:
  - Requires root or special permissions
  - Complex implementation
  - May not work on all devices
  - Vendor-specific (different DRM implementations)

### Option 5: Let X11 Streamer Handle Mode Switching
- Android reports current mode + common resolutions
- X11 streamer creates virtual display with those modes
- User can use `xrandr` to switch modes
- If mode is unsupported, TV will reject it (Android will handle gracefully)
- **Pros**: Simple, leverages X11's mode management
- **Cons**: Some trial-and-error for unsupported modes

## Recommendation

**For this use case, the current approach is reasonable:**

1. **Query TV's current mode** (resolution + refresh rate) - ✅ Android provides this
2. **Add common resolutions** that TVs typically support - ✅ We do this
3. **Let X11 streamer create virtual display** with those modes
4. **User can use `xrandr`** to switch modes
5. **If mode is unsupported**, TV will handle it (may show "unsupported mode" or scale)

**Why this works:**
- Most TVs support standard resolutions (720p, 1080p, 4K)
- The current mode tells us the TV's native/preferred resolution
- X11's virtual display can be configured with multiple modes
- If a mode doesn't work, user can switch to another via `xrandr`

**Future enhancement:**
- If needed, could implement native EDID reading via JNI
- Would require root or special permissions
- More complex but would provide complete mode list

## Comparison Table

| Feature | Xorg | Android (Public API) | Android (Native/JNI) |
|---------|------|---------------------|---------------------|
| Display Name | ✅ Full EDID name | ✅ Available | ✅ Available |
| Current Resolution | ✅ | ✅ | ✅ |
| Current Refresh Rate | ✅ | ✅ | ✅ |
| All Supported Resolutions | ✅ From EDID | ❌ No | ⚠️ Possible (root) |
| All Supported Refresh Rates | ✅ From EDID | ❌ No | ⚠️ Possible (root) |
| Preferred Mode | ✅ From EDID | ⚠️ Current mode | ⚠️ Possible (root) |
| Physical Dimensions | ✅ From EDID | ❌ No | ⚠️ Possible (root) |
| Raw EDID Access | ✅ Direct | ❌ No | ⚠️ Possible (root) |

## Conclusion

**Android cannot query the TV in the same way Xorg can** through the public API. However, the current approach of:
- Querying current mode (which Android provides)
- Adding common resolutions (educated guess)
- Letting X11 manage mode switching

...is a reasonable compromise that works for most use cases without requiring root or complex native code.

