package com.framebuffer.client;

import android.util.Log;
import java.lang.reflect.Method;

public class EdidParser {
    private static final String TAG = "EdidParser";

    // Load native library (optional - will fail gracefully if not available)
    private static boolean nativeLibraryLoaded = false;
    static {
        try {
            System.loadLibrary("edidparser");
            nativeLibraryLoaded = true;
            Log.d(TAG, "Native edidparser library loaded successfully");
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "Failed to load native edidparser library (EDID from DRM will not be available): " + e.getMessage());
            nativeLibraryLoaded = false;
        }
    }

    public static boolean isNativeLibraryAvailable() {
        return nativeLibraryLoaded;
    }

    // Native method to parse EDID data
    // Returns null if native library is not available or parsing fails
    public static EdidInfo parseEdid(byte[] edidData) {
        if (!nativeLibraryLoaded || edidData == null || edidData.length == 0) {
            return null;
        }
        try {
            return parseEdidNative(edidData);
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "Native parse method not available: " + e.getMessage());
            return null;
        }
    }

    private static native EdidInfo parseEdidNative(byte[] edidData);

    // Try to get EDID from system properties (Option 2)
    public static byte[] getEdidFromSystemProperties() {
        try {
            // Use reflection to access SystemProperties (hidden API)
            Class<?> systemPropertiesClass = Class.forName("android.os.SystemProperties");
            Method getMethod = systemPropertiesClass.getMethod("get", String.class);

            // Try common system property names for EDID
            String[] propertyNames = {
                "ro.hdmi.edid",
                "sys.hdmi.edid",
                "persist.vendor.hdmi.edid",
                "vendor.hdmi.edid"
            };

            for (String propName : propertyNames) {
                String edidHex = (String) getMethod.invoke(null, propName);
                if (edidHex != null && !edidHex.isEmpty()) {
                    Log.d(TAG, "Found EDID in system property: " + propName);
                    return hexStringToByteArray(edidHex);
                }
            }
        } catch (Exception e) {
            Log.w(TAG, "Failed to access system properties: " + e.getMessage());
        }
        return null;
    }

    // Native method to get EDID directly from DRM/KMS (Option 4)
    // Returns null if native library is not available or EDID cannot be read
    public static byte[] getEdidFromDrm() {
        if (!nativeLibraryLoaded) {
            Log.d(TAG, "Native library not available, cannot get EDID from DRM");
            return null;
        }
        try {
            return getEdidFromDrmNative();
        } catch (UnsatisfiedLinkError e) {
            Log.w(TAG, "Native method not available: " + e.getMessage());
            return null;
        }
    }

    private static native byte[] getEdidFromDrmNative();

    // Helper to convert hex string to byte array
    private static byte[] hexStringToByteArray(String hex) {
        int len = hex.length();
        byte[] data = new byte[len / 2];
        for (int i = 0; i < len; i += 2) {
            data[i / 2] = (byte) ((Character.digit(hex.charAt(i), 16) << 4)
                                 + Character.digit(hex.charAt(i+1), 16));
        }
        return data;
    }

    // EDID information structure
    public static class EdidInfo {
        public String displayName;
        public Protocol.DisplayMode[] modes;
        public int physicalWidthMm;
        public int physicalHeightMm;
    }
}

