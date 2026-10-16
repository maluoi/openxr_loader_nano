#!/bin/bash
# Builds the on-device loader test APK around a given libopenxr_loader.so, so
# the same test can run against this loader or upstream's.
#
# Usage: tests/android/build_apk.sh <libopenxr_loader.so> <out.apk>
# Then:  adb install -r out.apk && adb shell am start -n com.xrl.loadertest/android.app.NativeActivity
#        adb logcat -s xrl_test OpenXR-Loader

set -euo pipefail

LOADER=$(realpath "$1")
OUT=$(realpath -m "$2")
HERE=$(dirname "$(realpath "$0")")
ROOT=$(realpath "$HERE/../..")
SDK=${ANDROID_HOME:-$HOME/Android/Sdk}
NDK=${ANDROID_NDK:-$SDK/ndk/25.2.9519653}
TOOLS=$SDK/build-tools/$(ls "$SDK/build-tools" | sort -V | tail -1)
API=29
CC="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android$API-clang"
GLUE=$NDK/sources/android/native_app_glue
INCLUDE=${OPENXR_INCLUDE:-$ROOT/include}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/apk/lib/arm64-v8a"
cp "$LOADER" "$WORK/apk/lib/arm64-v8a/libopenxr_loader.so"
"$CC" -std=c11 -O2 -shared -fPIC -I"$GLUE" -I"$INCLUDE" \
	"$HERE/main.c" "$GLUE/android_native_app_glue.c" \
	-L"$WORK/apk/lib/arm64-v8a" -lopenxr_loader -lEGL -lGLESv3 -landroid -llog \
	-u ANativeActivity_onCreate -Wl,-z,max-page-size=16384 \
	-o "$WORK/apk/lib/arm64-v8a/libxrl_test.so"

"$TOOLS/aapt2" link -o "$WORK/base.apk" --manifest "$HERE/AndroidManifest.xml" -I "$SDK/platforms/android-32/android.jar"
(cd "$WORK/apk" && zip -q -r "$WORK/base.apk" lib)
"$TOOLS/zipalign" -f -p 4 "$WORK/base.apk" "$WORK/aligned.apk"

# A throwaway signing key, kept beside the build output instead of in ~/.android
KEY=$(dirname "$OUT")/xrl_test.keystore
[ -f "$KEY" ] || keytool -genkeypair -keystore "$KEY" -storepass android -keypass android -alias xrl -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=xrl test" > /dev/null 2>&1
"$TOOLS/apksigner" sign --ks "$KEY" --ks-pass pass:android --out "$OUT" "$WORK/aligned.apk"
echo "Built $OUT"
