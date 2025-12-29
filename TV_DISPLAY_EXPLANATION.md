# TV Display Support Explanation

## Current Implementation

**What we have:**
- Single `SurfaceView` in the main activity (on the phone)
- When TV is detected: Hide the `SurfaceView` (set `visibility = GONE`)
- Frames are still rendered to the phone's `SurfaceView` (just hidden)

**The Problem:**
1. **Mirror Mode**: If the TV mirrors the phone screen, hiding the `SurfaceView` hides it on both displays
2. **Extended Mode**: If the TV is a separate display, the hidden `SurfaceView` won't appear on the TV at all
3. **No Direct TV Rendering**: We're not actually rendering to the TV display, just hiding the phone display

## What "Proper TV Display Support" Means

Using Android's **Presentation API** would provide:

### Presentation API Benefits

1. **Separate Window on TV**: Creates a dedicated window/dialog that appears on the secondary (TV) display
2. **Direct TV Rendering**: Has its own `SurfaceView` that's physically on the TV display
3. **Works in Both Modes**:
   - **Mirror Mode**: Presentation appears on TV (and can be hidden on phone)
   - **Extended Mode**: Presentation appears only on TV (phone can show UI)
4. **Proper Display Targeting**: Android handles routing the window to the correct display

### How It Would Work

```java
// When TV is detected:
Presentation presentation = new Presentation(context, tvDisplay);
presentation.setContentView(R.layout.presentation_layout); // Contains SurfaceView
SurfaceView tvSurfaceView = presentation.findViewById(R.id.tvSurfaceView);
presentation.show();

// Use tvSurfaceView.getHolder() for FrameReceiver
frameReceiver = new FrameReceiver(socket, tvSurfaceView.getHolder(), ...);
```

### Current vs Proper Implementation

| Aspect | Current (Hidden SurfaceView) | Proper (Presentation API) |
|--------|------------------------------|---------------------------|
| **TV Mirror Mode** | ❌ Content hidden on both | ✅ Content on TV, hidden on phone |
| **TV Extended Mode** | ❌ Content doesn't appear on TV | ✅ Content appears on TV |
| **Phone Display** | ✅ Hidden when TV connected | ✅ Can show UI separately |
| **Display Targeting** | ❌ Not actually targeting TV | ✅ Directly targets TV display |

## Why This Matters

**Current behavior:**
- User connects TV via HDMI
- App detects TV and hides phone SurfaceView
- But frames are still being drawn to the hidden phone SurfaceView
- Result: Nothing appears on TV (unless in mirror mode, then nothing appears anywhere)

**With Presentation API:**
- User connects TV via HDMI
- App detects TV and creates Presentation window on TV
- Frames are drawn to the TV's SurfaceView
- Result: Content appears on TV, phone can show UI or be blank

## Implementation Complexity

**Current approach**: Simple (just hide/show SurfaceView)
**Presentation API**: Medium complexity
- Need to create Presentation class
- Manage Presentation lifecycle (show/hide when TV connects/disconnects)
- Handle SurfaceView in Presentation
- Coordinate between phone UI and TV Presentation

## Recommendation

For now, the current implementation works if:
- TV is in **mirror mode** AND you want content on both (don't hide SurfaceView)
- OR you're testing and don't need TV display yet

For production use with TV, implement Presentation API to ensure content actually appears on the TV display.

