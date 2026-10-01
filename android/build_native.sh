#!/bin/bash
# Cross compiles the native game library (libmain.so) and its shared dependencies for arm64 Android with OpenXR
# support, then copies them into the Gradle project. Requires the Android SDK/NDK and the OpenXR loader AAR (see
# android/README.md) and a desktop build of file_to_c for the host.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
: "${ANDROID_NDK_HOME:?Set ANDROID_NDK_HOME}"
: "${ANDROID_HOME:?Set ANDROID_HOME}"
# OpenXR headers and loader from the Khronos loader AAR (org.khronos.openxr:openxr_loader_for_android), extracted.
: "${OPENXR_SDK:?Set OPENXR_SDK to the extracted openxr_loader_for_android AAR}"
: "${RECOMP_HOST_FILE_TO_C:=$ROOT/out/build/x64-Release/file_to_c.exe}"
BUILD_DIR=${BUILD_DIR:-$ROOT/out/build/android-arm64}
CMAKE_BIN=${CMAKE_BIN:-$ANDROID_HOME/cmake/3.30.5/bin}
NDK_BIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/windows-x86_64

"$CMAKE_BIN/cmake" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$CMAKE_BIN/ninja.exe" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DRECOMP_HOST_FILE_TO_C="$RECOMP_HOST_FILE_TO_C" \
    -DRECOMP_VR=ON \
    -DOPENXR_INCLUDE_DIR="$OPENXR_SDK/prefab/modules/headers/include" \
    -DOPENXR_LOADER_LIBRARY="$OPENXR_SDK/jni/arm64-v8a/libopenxr_loader.so" \
    ${FETCHCONTENT_ARGS} > /dev/null
"$CMAKE_BIN/cmake" --build "$BUILD_DIR" --target MegaMan64Recompiled -j ${JOBS:-24}

JNI_DIR=$ROOT/android/app/src/main/jniLibs/arm64-v8a
rm -rf "$JNI_DIR"
mkdir -p "$JNI_DIR"
STRIP=$NDK_BIN/bin/llvm-strip.exe
# The game and every shared library it was linked against (SDL2, FreeType...).
for lib in "$BUILD_DIR"/*.so "$BUILD_DIR"/_deps/*/*.so; do
    [ -f "$lib" ] && "$STRIP" --strip-debug -o "$JNI_DIR/$(basename "$lib")" "$lib"
done
cp "$NDK_BIN/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$JNI_DIR/"
cp "$OPENXR_SDK/jni/arm64-v8a/libopenxr_loader.so" "$JNI_DIR/"
ls "$JNI_DIR"
echo "Native libraries copied to $JNI_DIR"
