#include <jni.h>
#include <string>
#include <vector>
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <cstring>

#if defined(HAVE_LIBDRM) && HAVE_LIBDRM
// Note: NDK provides drm/drm.h and drm/drm_mode.h (not linux/drm.h)
// However, libdrm library functions (drmModeGetConnector, etc.) are NOT in the NDK
// They would need to be:
//   a) Cross-compiled as a static library, OR
//   b) Available on the device at runtime (unlikely without root)
// For now, this code is disabled (HAVE_LIBDRM=0) because libdrm is not found
// We could rewrite to use ioctl() directly, but that's more complex
#include <drm/drm.h>
#include <drm/drm_mode.h>
// These would be needed if we had libdrm:
// #include <xf86drm.h>
// #include <xf86drmMode.h>
#endif

#define LOG_TAG "EdidParser"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)

// Simple EDID parsing - extract basic information
struct EdidMode {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_rate;  // Hz * 100
    bool preferred;
};

// Parse EDID to extract display name and modes
static bool parseEdidBasic(const uint8_t* edid, size_t edidLen,
                           std::string& displayName,
                           std::vector<EdidMode>& modes) {
    if (!edid || edidLen < 128) {
        return false;
    }

    // Check EDID header
    if (edid[0] != 0x00 || edid[1] != 0xFF || edid[2] != 0xFF ||
        edid[3] != 0xFF || edid[4] != 0xFF || edid[5] != 0xFF ||
        edid[6] != 0xFF || edid[7] != 0x00) {
        ALOGW("Invalid EDID header");
        return false;
    }

    // Extract manufacturer name (bytes 8-9)
    uint16_t mfgId = (edid[8] << 8) | edid[9];
    char mfg[4] = {
        (char)(((mfgId >> 10) & 0x1F) + 'A' - 1),
        (char)(((mfgId >> 5) & 0x1F) + 'A' - 1),
        (char)((mfgId & 0x1F) + 'A' - 1),
        '\0'
    };

    // Extract product code (bytes 10-11)
    uint16_t productCode = (edid[11] << 8) | edid[10];

    // Extract serial number (bytes 12-15)
    uint32_t serial = edid[12] | (edid[13] << 8) | (edid[14] << 16) | (edid[15] << 24);

    // Build display name
    char name[64];
    snprintf(name, sizeof(name), "%s %04X", mfg, productCode);
    displayName = name;

    // Parse detailed timing blocks (bytes 54-125, each 18 bytes)
    for (int i = 0; i < 4 && (54 + i * 18 + 17) < (int)edidLen; i++) {
        int offset = 54 + i * 18;

        // Check if this is a detailed timing descriptor (not other descriptor type)
        if (edid[offset] == 0 && edid[offset + 1] == 0) {
            // Detailed timing descriptor
            uint32_t pixelClock = (edid[offset + 2] | (edid[offset + 3] << 8)) * 10; // kHz -> 10kHz

            uint32_t hActive = edid[offset + 2] | ((edid[offset + 4] & 0xF0) << 4);
            uint32_t hBlanking = edid[offset + 3] | ((edid[offset + 4] & 0x0F) << 8);

            uint32_t vActive = edid[offset + 5] | ((edid[offset + 7] & 0xF0) << 4);
            uint32_t vBlanking = edid[offset + 6] | ((edid[offset + 7] & 0x0F) << 8);

            // Calculate refresh rate
            uint32_t totalPixels = (hActive + hBlanking) * (vActive + vBlanking);
            uint32_t refreshRate = (pixelClock * 10000) / totalPixels; // Hz * 100

            EdidMode mode;
            mode.width = hActive;
            mode.height = vActive;
            mode.refresh_rate = refreshRate;
            mode.preferred = (i == 0); // First detailed timing is usually preferred

            modes.push_back(mode);
        }
    }

    // Parse standard timings (bytes 38-53)
    for (int i = 0; i < 8; i++) {
        int offset = 38 + i * 2;
        if (offset + 1 >= (int)edidLen) break;

        if (edid[offset] != 0x01 && edid[offset + 1] != 0x01) {
            uint32_t hRes = (edid[offset] + 31) * 8;
            uint32_t aspect = (edid[offset + 1] >> 6) & 0x3;
            uint32_t vRefresh = (edid[offset + 1] & 0x3F) + 60;

            uint32_t vRes;
            if (aspect == 0) vRes = hRes * 10 / 16; // 16:10
            else if (aspect == 1) vRes = hRes * 3 / 4; // 4:3
            else if (aspect == 2) vRes = hRes * 9 / 16; // 16:9
            else vRes = hRes; // 1:1

            EdidMode mode;
            mode.width = hRes;
            mode.height = vRes;
            mode.refresh_rate = vRefresh * 100;
            mode.preferred = false;

            modes.push_back(mode);
        }
    }

    return true;
}

// Get EDID from DRM/KMS (Option 4)
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_framebuffer_client_EdidParser_getEdidFromDrmNative(JNIEnv *env, jclass clazz) {
#if defined(HAVE_LIBDRM) && HAVE_LIBDRM
    ALOGI("Attempting to get EDID from DRM/KMS");

    // Try common DRM device paths
    const char* drmPaths[] = {
        "/dev/dri/card0",
        "/dev/dri/card1",
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
    };

    for (const char* drmPath : drmPaths) {
        int fd = open(drmPath, O_RDWR);
        if (fd < 0) {
            continue;
        }

        ALOGI("Opened DRM device: %s", drmPath);

        // Get resources
        drmModeResPtr resources = drmModeGetResources(fd);
        if (!resources) {
            close(fd);
            continue;
        }

        // Find first connected connector
        for (int i = 0; i < resources->count_connectors; i++) {
            drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[i]);
            if (!connector) continue;

            if (connector->connection == DRM_MODE_CONNECTED) {
                ALOGI("Found connected connector: %d", connector->connector_id);

                // Get properties
                for (int j = 0; j < connector->count_props; j++) {
                    drmModePropertyPtr prop = drmModeGetProperty(fd, connector->props[j]);
                    if (!prop) continue;

                    if (strcmp(prop->name, "EDID") == 0) {
                        uint64_t blobId = connector->prop_values[j];
                        drmModePropertyBlobPtr blob = drmModeGetPropertyBlob(fd, blobId);

                        if (blob && blob->data && blob->length > 0) {
                            ALOGI("Found EDID blob, size: %zu", blob->length);

                            // Create Java byte array
                            jbyteArray result = env->NewByteArray(blob->length);
                            if (result) {
                                env->SetByteArrayRegion(result, 0, blob->length,
                                                       (const jbyte*)blob->data);
                            }

                            drmModeFreePropertyBlob(blob);
                            drmModeFreeProperty(prop);
                            drmModeFreeConnector(connector);
                            drmModeFreeResources(resources);
                            close(fd);
                            return result;
                        }

                        if (blob) drmModeFreePropertyBlob(blob);
                    }

                    drmModeFreeProperty(prop);
                }
            }

            drmModeFreeConnector(connector);
        }

        drmModeFreeResources(resources);
        close(fd);
    }

    ALOGW("Could not find EDID from DRM devices");
#else
    ALOGW("libdrm not available - cannot get EDID from DRM");
#endif
    return nullptr;
}

// Parse EDID data (called from Java)
extern "C" JNIEXPORT jobject JNICALL
Java_com_framebuffer_client_EdidParser_parseEdidNative(JNIEnv *env, jclass clazz, jbyteArray edidData) {
    if (!edidData) {
        return nullptr;
    }

    jsize len = env->GetArrayLength(edidData);
    jbyte* data = env->GetByteArrayElements(edidData, nullptr);
    if (!data) {
        return nullptr;
    }

    std::string displayName;
    std::vector<EdidMode> modes;

    bool success = parseEdidBasic((const uint8_t*)data, len, displayName, modes);

    env->ReleaseByteArrayElements(edidData, data, JNI_ABORT);

    if (!success || modes.empty()) {
        ALOGW("Failed to parse EDID or no modes found");
        return nullptr;
    }

    // Create EdidInfo object
    jclass edidInfoClass = env->FindClass("com/framebuffer/client/EdidParser$EdidInfo");
    if (!edidInfoClass) {
        ALOGE("Could not find EdidInfo class");
        return nullptr;
    }

    jmethodID constructor = env->GetMethodID(edidInfoClass, "<init>", "()V");
    if (!constructor) {
        ALOGE("Could not find EdidInfo constructor");
        return nullptr;
    }

    jobject edidInfo = env->NewObject(edidInfoClass, constructor);
    if (!edidInfo) {
        ALOGE("Could not create EdidInfo object");
        return nullptr;
    }

    // Set display name
    jfieldID nameField = env->GetFieldID(edidInfoClass, "displayName", "Ljava/lang/String;");
    if (nameField) {
        jstring nameStr = env->NewStringUTF(displayName.c_str());
        env->SetObjectField(edidInfo, nameField, nameStr);
    }

    // Create DisplayMode array
    jclass displayModeClass = env->FindClass("com/framebuffer/client/Protocol$DisplayMode");
    if (!displayModeClass) {
        ALOGE("Could not find DisplayMode class");
        return nullptr;
    }

    jmethodID modeConstructor = env->GetMethodID(displayModeClass, "<init>", "()V");
    if (!modeConstructor) {
        ALOGE("Could not find DisplayMode constructor");
        return nullptr;
    }

    jobjectArray modeArray = env->NewObjectArray(modes.size(), displayModeClass, nullptr);
    if (!modeArray) {
        ALOGE("Could not create DisplayMode array");
        return nullptr;
    }

    for (size_t i = 0; i < modes.size(); i++) {
        jobject mode = env->NewObject(displayModeClass, modeConstructor);
        if (mode) {
            jfieldID widthField = env->GetFieldID(displayModeClass, "width", "I");
            jfieldID heightField = env->GetFieldID(displayModeClass, "height", "I");
            jfieldID refreshField = env->GetFieldID(displayModeClass, "refreshRate", "I");

            if (widthField) env->SetIntField(mode, widthField, modes[i].width);
            if (heightField) env->SetIntField(mode, heightField, modes[i].height);
            if (refreshField) env->SetIntField(mode, refreshField, modes[i].refresh_rate);

            env->SetObjectArrayElement(modeArray, i, mode);
        }
    }

    // Set modes array
    jfieldID modesField = env->GetFieldID(edidInfoClass, "modes",
                                         "[Lcom/framebuffer/client/Protocol$DisplayMode;");
    if (modesField) {
        env->SetObjectField(edidInfo, modesField, modeArray);
    }

    return edidInfo;
}

