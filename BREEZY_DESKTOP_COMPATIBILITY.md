# Breezy Desktop Compatibility with XR_MODES

## Backward Compatibility ✅

**Our changes are fully backward compatible.** Here's why:

1. **Fallback Behavior**: The `drmmode_xr_virtual_set_modes()` function checks if `vout->modes` is set:
   - **If `vout->modes` is set**: Uses the custom modes from `XR_MODES` property
   - **If `vout->modes` is NULL**: Falls back to the original behavior (fixed common modes: 1920x1080, 2560x1440, 3840x2160)

2. **breezy-desktop Current Behavior**:
   - Creates virtual outputs via `CREATE_XR_OUTPUT` with format: `"NAME:WIDTH:HEIGHT:REFRESH"`
   - Only sets a single refresh rate (e.g., `"XR-0:1920:1080:60"`)
   - Does **not** set `XR_MODES` property
   - Result: `vout->modes` remains `NULL`, so it uses the fallback (original behavior)

3. **Conclusion**: breezy-desktop will continue to work exactly as before - no changes needed for backward compatibility.

## Enhancement Opportunity: Multiple Refresh Rates for VR/AR Glasses

### Current Limitation

breezy-desktop currently only creates virtual outputs with a single refresh rate. However, VR/AR glasses typically support **2-3 different refresh rates** (e.g., 60Hz, 72Hz, 90Hz, 120Hz).

### Recommended Enhancement

breezy-desktop should be enhanced to:

1. **Query physical XR connector** for supported refresh rates (via EDID or DRM properties)
2. **Set `XR_MODES` property** after creating the virtual output to include all supported refresh rates

### Example Implementation

```python
def create_virtual_display(self, width: int, height: int, framerate: int = 60,
                           name: str = "XR-0", refresh_rates: List[int] = None) -> Optional[str]:
    """
    Create a virtual XR display using XR-Manager.

    Args:
        width: Display width in pixels
        height: Display height in pixels
        framerate: Display framerate (default: 60) - used for initial creation
        name: Virtual output name (default: "XR-0")
        refresh_rates: List of supported refresh rates (e.g., [60, 72, 90])
                      If None, only uses framerate

    Returns:
        Display ID (output name) if successful, None otherwise
    """
    if not self.is_available():
        logger.error("XR-Manager not available, cannot create virtual display")
        return None

    try:
        # Create virtual output via XR-Manager CREATE_XR_OUTPUT property
        # Format: "NAME:WIDTH:HEIGHT:REFRESH"
        create_cmd = f"{name}:{width}:{height}:{framerate}"
        result = subprocess.run(
            ['xrandr', '--output', 'XR-Manager',
             '--set', 'CREATE_XR_OUTPUT', create_cmd],
            capture_output=True, text=True, check=True
        )

        # Verify the output was created
        xrandr_output = subprocess.run(['xrandr', '--listoutputs'],
                                      capture_output=True, text=True, check=True)
        if name not in xrandr_output.stdout:
            logger.error(f"Virtual output {name} was not created")
            return None

        # Set multiple refresh rates if provided
        if refresh_rates and len(refresh_rates) > 1:
            # Build modes string: "WIDTH:HEIGHT:REFRESH|WIDTH:HEIGHT:REFRESH|..."
            modes_str = "|".join([f"{width}:{height}:{rate}" for rate in refresh_rates])

            # Set XR_MODES property on the virtual output
            subprocess.run(
                ['xrandr', '--output', name,
                 '--set', 'XR_MODES', modes_str],
                capture_output=True, text=True, check=True
            )
            logger.info(f"Set {len(refresh_rates)} refresh rates for {name}: {refresh_rates}")

        # Store display info
        self.virtual_displays[name] = {
            'id': name,
            'width': width,
            'height': height,
            'framerate': framerate,
            'refresh_rates': refresh_rates or [framerate]
        }

        logger.info(f"Created virtual XR display {name}: {width}x{height}@{framerate}Hz")
        return name

    except subprocess.CalledProcessError as e:
        logger.error(f"Failed to create virtual display: {e}")
        if e.stderr:
            logger.error(f"Error output: {e.stderr}")
        return None
    except Exception as e:
        logger.error(f"Error creating virtual display: {e}")
        return None
```

### Querying Physical XR Connector Refresh Rates

breezy-desktop could query the physical XR connector's EDID to get supported refresh rates:

```python
def get_xr_connector_refresh_rates(self, connector_name: str) -> List[int]:
    """
    Query physical XR connector for supported refresh rates.

    Args:
        connector_name: Name of physical XR connector (e.g., "DisplayPort-0")

    Returns:
        List of supported refresh rates in Hz
    """
    try:
        # Query connector modes via xrandr
        result = subprocess.run(
            ['xrandr', '--output', connector_name, '--query'],
            capture_output=True, text=True, check=True
        )

        # Parse output to extract refresh rates
        refresh_rates = []
        for line in result.stdout.splitlines():
            # Look for mode lines like "   1920x1080     60.00*+  75.00    50.00"
            if 'x' in line and 'Hz' in line:
                # Extract refresh rates (numbers followed by Hz or *)
                import re
                rates = re.findall(r'(\d+\.?\d*)\s*\*?\+?', line)
                for rate_str in rates:
                    try:
                        rate = int(float(rate_str))
                        if rate not in refresh_rates:
                            refresh_rates.append(rate)
                    except ValueError:
                        pass

        return sorted(refresh_rates) if refresh_rates else [60]  # Default to 60Hz

    except Exception as e:
        logger.warning(f"Failed to query refresh rates for {connector_name}: {e}")
        return [60]  # Default fallback
```

### Usage Example

```python
# Query physical XR connector for supported refresh rates
physical_connector = "DisplayPort-0"  # or detect automatically
refresh_rates = backend.get_xr_connector_refresh_rates(physical_connector)
# Returns: [60, 72, 90] for example

# Create virtual display with all supported refresh rates
display_id = backend.create_virtual_display(
    width=1920,
    height=1080,
    framerate=90,  # Use highest as preferred
    name="XR-0",
    refresh_rates=refresh_rates  # All supported rates
)

# Now xrandr will show:
# XR-0 connected 1920x1080+0+0
#   1920x1080@60Hz
#   1920x1080@72Hz
#   1920x1080@90Hz (preferred)
```

## Summary

- ✅ **Backward Compatible**: breezy-desktop works as-is, no changes required
- 💡 **Enhancement Opportunity**: breezy-desktop can be enhanced to support multiple refresh rates by:
  1. Querying physical XR connector for supported refresh rates
  2. Setting `XR_MODES` property after creating virtual output
  3. This allows users to switch between refresh rates via xrandr or display settings

