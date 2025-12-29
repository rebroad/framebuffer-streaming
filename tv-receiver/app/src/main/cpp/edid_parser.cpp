#include <jni.h>
#include <string>
#include <vector>
#include <android/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <cstring>
#include <cstdlib>
#include <errno.h>
#include <dlfcn.h>

// NDK provides drm/drm.h and drm/drm_mode.h (not linux/drm.h)
// We try to use libdrm functions if available at runtime, otherwise fall back to ioctl()
#include <drm/drm.h>
#include <drm/drm_mode.h>

// Define libdrm structs ourselves (since libdrm headers aren't in NDK)
// These match the actual libdrm structure definitions
struct _drmModeRes {
    int count_fbs;
    uint32_t *fbs;
    int count_crtcs;
    uint32_t *crtcs;
    int count_connectors;
    uint32_t *connectors;
    int count_encoders;
    uint32_t *encoders;
    uint32_t min_width, max_width;
    uint32_t min_height, max_height;
};

struct _drmModeConnector {
    uint32_t connector_id;
    uint32_t encoder_id;
    uint32_t connector_type;
    uint32_t connector_type_id;
    uint32_t connection;
    uint32_t mmWidth, mmHeight;
    uint32_t subpixel;
    int count_modes;
    void *modes;  // drmModeModeInfoPtr
    int count_props;
    uint32_t *props;
    uint64_t *prop_values;
    int count_encoders;
    uint32_t *encoders;
};

struct _drmModeProperty {
    uint32_t prop_id;
    uint32_t flags;
    char name[32];  // DRM_PROP_NAME_LEN is 32
    int count_values;
    uint64_t *values;
    int count_enums;
    void *enums;  // drmModePropertyEnumPtr
    int count_blobs;
    uint32_t *blob_ids;
};

struct _drmModePropertyBlob {
    uint32_t id;
    uint32_t length;
    void *data;
};

typedef struct _drmModeRes drmModeRes;
typedef struct _drmModeConnector drmModeConnector;
typedef struct _drmModeProperty drmModeProperty;
typedef struct _drmModePropertyBlob drmModePropertyBlob;

// DRM_MODE_CONNECTED constant (from drm_mode.h)
#ifndef DRM_MODE_CONNECTED
#define DRM_MODE_CONNECTED 1
#endif

typedef drmModeRes* (*drmModeGetResourcesFunc)(int fd);
typedef drmModeConnector* (*drmModeGetConnectorFunc)(int fd, uint32_t connectorId);
typedef drmModeProperty* (*drmModeGetPropertyFunc)(int fd, uint32_t propertyId);
typedef drmModePropertyBlob* (*drmModeGetPropertyBlobFunc)(int fd, uint32_t blobId);
typedef void (*drmModeFreeResourcesFunc)(drmModeRes* ptr);
typedef void (*drmModeFreeConnectorFunc)(drmModeConnector* ptr);
typedef void (*drmModeFreePropertyFunc)(drmModeProperty* ptr);
typedef void (*drmModeFreePropertyBlobFunc)(drmModePropertyBlob* ptr);

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

// Try to get EDID using libdrm functions (cleaner API if available)
static jbyteArray get_edid_via_libdrm(JNIEnv *env, int fd) {
    void* libdrm_handle = dlopen("libdrm.so", RTLD_LAZY);
    if (!libdrm_handle) {
        return nullptr;
    }

    // Load libdrm functions
    drmModeGetResourcesFunc drmModeGetResources = (drmModeGetResourcesFunc)dlsym(libdrm_handle, "drmModeGetResources");
    drmModeGetConnectorFunc drmModeGetConnector = (drmModeGetConnectorFunc)dlsym(libdrm_handle, "drmModeGetConnector");
    drmModeGetPropertyFunc drmModeGetProperty = (drmModeGetPropertyFunc)dlsym(libdrm_handle, "drmModeGetProperty");
    drmModeGetPropertyBlobFunc drmModeGetPropertyBlob = (drmModeGetPropertyBlobFunc)dlsym(libdrm_handle, "drmModeGetPropertyBlob");
    drmModeFreeResourcesFunc drmModeFreeResources = (drmModeFreeResourcesFunc)dlsym(libdrm_handle, "drmModeFreeResources");
    drmModeFreeConnectorFunc drmModeFreeConnector = (drmModeFreeConnectorFunc)dlsym(libdrm_handle, "drmModeFreeConnector");
    drmModeFreePropertyFunc drmModeFreeProperty = (drmModeFreePropertyFunc)dlsym(libdrm_handle, "drmModeFreeProperty");
    drmModeFreePropertyBlobFunc drmModeFreePropertyBlob = (drmModeFreePropertyBlobFunc)dlsym(libdrm_handle, "drmModeFreePropertyBlob");

    if (!drmModeGetResources || !drmModeGetConnector || !drmModeGetProperty ||
        !drmModeGetPropertyBlob || !drmModeFreeResources || !drmModeFreeConnector ||
        !drmModeFreeProperty || !drmModeFreePropertyBlob) {
        dlclose(libdrm_handle);
        return nullptr;
    }

    drmModeRes* resources = drmModeGetResources(fd);
    if (!resources) {
        dlclose(libdrm_handle);
        return nullptr;
    }

    jbyteArray result = nullptr;

    // Find first connected connector
    for (int i = 0; i < resources->count_connectors; i++) {
        drmModeConnector* connector = drmModeGetConnector(fd, resources->connectors[i]);
        if (!connector) continue;

        if (connector->connection == DRM_MODE_CONNECTED) {
            ALOGI("Found connected connector: %d (using libdrm)", connector->connector_id);

            // Get properties
            for (int j = 0; j < connector->count_props; j++) {
                drmModeProperty* prop = drmModeGetProperty(fd, connector->props[j]);
                if (!prop) continue;

                if (strcmp(prop->name, "EDID") == 0) {
                    uint64_t blobId = connector->prop_values[j];
                    drmModePropertyBlob* blob = drmModeGetPropertyBlob(fd, blobId);

                    if (blob && blob->data && blob->length > 0) {
                        ALOGI("Found EDID blob via libdrm, size: %u", blob->length);

                        result = env->NewByteArray(blob->length);
                        if (result) {
                            env->SetByteArrayRegion(result, 0, blob->length, (const jbyte*)blob->data);
                        }

                        drmModeFreePropertyBlob(blob);
                        drmModeFreeProperty(prop);
                        drmModeFreeConnector(connector);
                        drmModeFreeResources(resources);
                        dlclose(libdrm_handle);
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
    dlclose(libdrm_handle);
    return nullptr;
}

// Get EDID from DRM/KMS using ioctl() directly (fallback when libdrm not available)
// This may fail without root access, but we try anyway
static jbyteArray get_edid_via_ioctl(JNIEnv *env, int fd) {
    // Step 1: Get resources (first call to get counts)
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        ALOGW("Failed to get DRM resources: %s", strerror(errno));
        return nullptr;
    }

    if (res.count_connectors == 0) {
        ALOGD("No connectors found");
        return nullptr;
    }

    // Step 2: Allocate memory and get connector IDs
    uint32_t *connector_ids = (uint32_t*)malloc(res.count_connectors * sizeof(uint32_t));
    if (!connector_ids) {
        ALOGE("Failed to allocate memory for connector IDs");
        return nullptr;
    }

    res.connector_id_ptr = (uint64_t)(unsigned long)connector_ids;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0) {
        ALOGW("Failed to get connector IDs: %s", strerror(errno));
        free(connector_ids);
        return nullptr;
    }

    // Step 3: Iterate through connectors
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector conn;
        memset(&conn, 0, sizeof(conn));
        conn.connector_id = connector_ids[i];

        // First call to get property counts
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            ALOGW("Failed to get connector %u info: %s", connector_ids[i], strerror(errno));
            continue;
        }

        // Check if connector is connected
        if (conn.connection != DRM_MODE_CONNECTED) {
            ALOGD("Connector %u is not connected", connector_ids[i]);
            continue;
        }

        ALOGI("Found connected connector: %u", connector_ids[i]);

        if (conn.count_props == 0) {
            ALOGD("Connector %u has no properties", connector_ids[i]);
            continue;
        }

        // Step 4: Allocate memory for properties
        uint32_t *prop_ids = (uint32_t*)malloc(conn.count_props * sizeof(uint32_t));
        uint64_t *prop_values = (uint64_t*)malloc(conn.count_props * sizeof(uint64_t));
        if (!prop_ids || !prop_values) {
            ALOGE("Failed to allocate memory for properties");
            free(prop_ids);
            free(prop_values);
            continue;
        }

        conn.props_ptr = (uint64_t)(unsigned long)prop_ids;
        conn.prop_values_ptr = (uint64_t)(unsigned long)prop_values;

        // Second call to get property IDs and values
        if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) < 0) {
            ALOGW("Failed to get connector %u properties: %s", connector_ids[i], strerror(errno));
            free(prop_ids);
            free(prop_values);
            continue;
        }

        // Step 5: Iterate through properties to find EDID
        for (uint32_t j = 0; j < conn.count_props; j++) {
                struct drm_mode_get_property prop;
                memset(&prop, 0, sizeof(prop));
                prop.prop_id = prop_ids[j];

                // Get property info (name is included directly in the struct)
                if (ioctl(fd, DRM_IOCTL_MODE_GETPROPERTY, &prop) < 0) {
                    ALOGW("Failed to get property %u info: %s", prop_ids[j], strerror(errno));
                    continue;
                }

                // Check if this is the EDID property (name is already in the struct)
                if (strcmp(prop.name, "EDID") == 0) {
                    ALOGI("Found EDID property, blob_id=%llu", (unsigned long long)prop_values[j]);

                    // Step 6: Get the EDID blob
                    struct drm_mode_get_blob blob;
                    memset(&blob, 0, sizeof(blob));
                    blob.blob_id = prop_values[j];

                    // First call to get blob size
                    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0) {
                        ALOGW("Failed to get EDID blob info: %s", strerror(errno));
                        continue;
                    }

                    if (blob.length == 0) {
                        ALOGW("EDID blob has zero length");
                        continue;
                    }

                    // Allocate memory for blob data
                    void *blob_data = malloc(blob.length);
                    if (!blob_data) {
                        ALOGE("Failed to allocate memory for EDID blob");
                        continue;
                    }

                    blob.data = (uint64_t)(unsigned long)blob_data;

                    // Second call to get blob data
                    if (ioctl(fd, DRM_IOCTL_MODE_GETPROPBLOB, &blob) < 0) {
                        ALOGW("Failed to get EDID blob data: %s", strerror(errno));
                        free(blob_data);
                        continue;
                    }

                    ALOGI("Successfully read EDID blob via ioctl, size: %u", blob.length);

                    // Create Java byte array
                    jbyteArray result = env->NewByteArray(blob.length);
                    if (result) {
                        env->SetByteArrayRegion(result, 0, blob.length, (const jbyte*)blob_data);
                    }

                    // Cleanup
                    free(blob_data);
                    free(prop_ids);
                    free(prop_values);
                    free(connector_ids);
                    return result;
            }
        }

        free(prop_ids);
        free(prop_values);
    }

    free(connector_ids);
    return nullptr;
}

// Main entry point: Try libdrm first, fall back to ioctl()
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_framebuffer_client_EdidParser_getEdidFromDrmNative(JNIEnv *env, jclass clazz) {
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
            ALOGD("Failed to open DRM device %s: %s", drmPath, strerror(errno));
            continue;
        }

        ALOGI("Opened DRM device: %s", drmPath);

        // First try libdrm functions (cleaner API if available)
        jbyteArray result = get_edid_via_libdrm(env, fd);
        if (result) {
            close(fd);
            return result;
        }

        // Fall back to ioctl() if libdrm not available
        result = get_edid_via_ioctl(env, fd);
        if (result) {
            close(fd);
            return result;
        }

        close(fd);
    }

    ALOGW("Could not find EDID from any DRM device (may require root access)");
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

