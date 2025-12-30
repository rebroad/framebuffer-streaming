# Enabling USB Tethering via ADB

This document describes how to enable USB tethering on an Android device connected via ADB (Android Debug Bridge).

## Prerequisites

1. **ADB installed** - Android Debug Bridge must be installed and accessible
2. **Device connected** - Android device must be connected via USB and authorized for debugging
3. **USB debugging enabled** - Developer options and USB debugging must be enabled on the device

## Verification

First, verify your device is connected:

```bash
adb devices
```

You should see your device listed with status "device" (not "unauthorized" or "offline").

## Method 1: Direct USB Function Change (May Require Root)

The most direct method is to change the USB function to RNDIS (Remote Network Driver Interface Specification), which is what USB tethering uses:

```bash
# Check current USB functions
adb shell svc usb getFunctions

# Enable USB tethering (sets USB to RNDIS mode)
adb shell svc usb setFunctions rndis
```

**Note:** This command may fail with exit code 255 on non-rooted devices or devices with restricted permissions. On some devices, this requires root access or system-level permissions.

## Method 2: Settings Configuration (Non-Root Alternative)

For non-rooted devices, you can prepare the settings and then enable tethering through the UI:

```bash
# Disable DUN requirement (may help with some carriers)
adb shell settings put global tether_dun_required 0

# Open tethering settings (user can then manually enable)
adb shell am start -a android.settings.TETHER_AND_HOTSPOT_SETTINGS
```

## Method 3: Root Access (If Available)

If your device is rooted, you can use root commands:

```bash
# Enable USB tethering via root
adb shell su -c 'svc usb setFunctions rndis'

# Or using service calls (varies by Android version)
# Android 7.0.0:
adb shell su -c 'service call connectivity 33 i32 1'

# Android 5.1.0 and 6.0.1:
adb shell su -c 'service call connectivity 30 i32 1'

# Android 4.4.4:
adb shell su -c 'service call connectivity 34 i32 1'
```

## Verification After Enabling

After enabling USB tethering, verify it's active:

```bash
# Check USB functions (should show "rndis")
adb shell svc usb getFunctions

# On Linux host, check for new network interface
ip link show | grep -i usb
# or
ip addr show | grep -i usb
```

You should see a new network interface (typically `usb0` or similar) on your Linux host when tethering is active.

## What Actually Worked

In our testing session:

1. **Initial state**: USB functions showed `mtp,conn_gadget,adb`
2. **Applied settings**: `adb shell settings put global tether_dun_required 0` (succeeded)
3. **Attempted direct change**: `adb shell svc usb setFunctions rndis` (initially failed with exit code 255)
4. **Final state**: USB functions showed `rndis` (tethering active)

The exact mechanism that enabled tethering may have been:
- The settings change combined with manual UI interaction
- A delayed effect of the `svc usb setFunctions` command
- Device-specific behavior that allowed the command to succeed

## Troubleshooting

### Command Fails with Permission Denied

- Ensure USB debugging is enabled and authorized
- Try the UI method (Method 2) instead
- Some devices require root access for direct USB function changes

### No Network Interface Appears

- Check that tethering is actually enabled: `adb shell svc usb getFunctions`
- Verify USB connection is stable: `adb devices`
- Check Linux kernel USB networking support: `lsmod | grep rndis`
- Some distributions may need additional packages for USB tethering support

### Tethering Disables Automatically

- Some devices have power management that disables tethering
- Check device settings for USB tethering timeout/auto-disable options
- Ensure USB connection remains stable

## Android Version Compatibility

- **Android 12 (SDK 31)**: Tested - requires appropriate permissions
- **Android 7.0+**: `svc usb` commands available
- **Older versions**: May require different service call methods

## Related Commands

```bash
# Get Android SDK version
adb shell getprop ro.build.version.sdk

# Check if device is rooted
adb shell su -c 'echo "root available"' || echo "not rooted"

# List all USB functions available
adb shell svc usb getFunctions

# Disable USB tethering (set back to default)
adb shell svc usb setFunctions mtp,adb
```

## References

- [Android USB Tethering via ADB (Stack Exchange)](https://android.stackexchange.com/questions/171744/how-can-i-enable-usb-tethering-through-adb)
- [ADB Command Reference](https://developer.android.com/tools/adb)

