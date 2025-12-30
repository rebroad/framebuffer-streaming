#!/bin/bash
# Build and deploy the Android TV receiver app via adb

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Application info
APP_ID="com.framebuffer.client"
MAIN_ACTIVITY="com.framebuffer.client.MainActivity"

# Parse command-line arguments
BUILD_TYPE="debug"
INSTALL_ONLY=false
LAUNCH_APP=false
CLEAN_BUILD=false

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -r, --release       Build release APK (default: debug)"
    echo "  -d, --debug         Build debug APK (default)"
    echo "  -i, --install-only  Only install, don't build (requires existing APK)"
    echo "  -l, --launch        Launch app after installation"
    echo "  -c, --clean         Clean before building"
    echo "  -h, --help          Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                  # Build and install debug APK"
    echo "  $0 -r -l            # Build release APK, install and launch"
    echo "  $0 -i -l            # Install existing APK and launch"
    echo "  $0 -c               # Clean build and install"
}

while [[ $# -gt 0 ]]; do
    case $1 in
        -r|--release)
            BUILD_TYPE="release"
            shift
            ;;
        -d|--debug)
            BUILD_TYPE="debug"
            shift
            ;;
        -i|--install-only)
            INSTALL_ONLY=true
            shift
            ;;
        -l|--launch)
            LAUNCH_APP=true
            shift
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo -e "${RED}Error: Unknown option: $1${NC}"
            usage
            exit 1
            ;;
    esac
done

# Check if adb is available
if ! command -v adb &> /dev/null; then
    echo -e "${RED}Error: adb not found. Please install Android SDK platform-tools.${NC}"
    exit 1
fi

# Check if device is connected
if ! adb devices | grep -q "device$"; then
    echo -e "${RED}Error: No Android device connected.${NC}"
    echo "Please connect a device via USB or enable ADB over network."
    echo "Run 'adb devices' to check connected devices."
    exit 1
fi

DEVICE_COUNT=$(adb devices | grep -c "device$")
if [ "$DEVICE_COUNT" -gt 1 ]; then
    echo -e "${YELLOW}Warning: Multiple devices connected. Using first device.${NC}"
    echo "Connected devices:"
    adb devices | grep "device$"
    echo ""
fi

# Build APK if not install-only
if [ "$INSTALL_ONLY" = false ]; then
    echo -e "${GREEN}Building ${BUILD_TYPE} APK...${NC}"

    # Clean if requested
    if [ "$CLEAN_BUILD" = true ]; then
        echo "Cleaning build..."
        ./gradlew clean
    fi

    # Build APK
    if [ "$BUILD_TYPE" = "release" ]; then
        ./gradlew assembleRelease
        APK_PATH="app/build/outputs/apk/release/app-release.apk"
    else
        ./gradlew assembleDebug
        APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
    fi

    if [ ! -f "$APK_PATH" ]; then
        echo -e "${RED}Error: APK not found at $APK_PATH${NC}"
        exit 1
    fi

    echo -e "${GREEN}Build successful: $APK_PATH${NC}"
else
    # Find existing APK
    if [ "$BUILD_TYPE" = "release" ]; then
        APK_PATH="app/build/outputs/apk/release/app-release.apk"
    else
        APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
    fi

    if [ ! -f "$APK_PATH" ]; then
        echo -e "${RED}Error: APK not found at $APK_PATH${NC}"
        echo "Please build the APK first or remove --install-only flag."
        exit 1
    fi

    echo -e "${GREEN}Using existing APK: $APK_PATH${NC}"
fi

# Uninstall existing app (optional, but helps avoid conflicts)
echo -e "${YELLOW}Uninstalling existing app (if present)...${NC}"
adb uninstall "$APP_ID" 2>/dev/null || true

# Install APK
echo -e "${GREEN}Installing APK...${NC}"
if adb install -r "$APK_PATH"; then
    echo -e "${GREEN}Installation successful!${NC}"
else
    echo -e "${RED}Error: Installation failed${NC}"
    exit 1
fi

# Launch app if requested
if [ "$LAUNCH_APP" = true ]; then
    echo -e "${GREEN}Launching app...${NC}"
    adb shell am start -n "$APP_ID/$MAIN_ACTIVITY"
    echo -e "${GREEN}App launched!${NC}"
fi

