#!/bin/bash
# Installs a test APK, runs it, and prints its logcat until it reports a result.
# Usage: tests/android/run_on_device.sh <apk> [timeout_seconds]
set -uo pipefail
APK=$1
TIMEOUT=${2:-90}
PKG=com.xrl.loadertest
adb install -r "$APK" > /dev/null || exit 1
adb logcat -c
adb shell am start -n $PKG/android.app.NativeActivity > /dev/null
for ((i = 0; i < TIMEOUT; i++)); do
	adb logcat -d -s xrl_test | grep -q 'XRL_TEST' && break
	sleep 1
done
adb logcat -d -s xrl_test OpenXR-Loader | grep -v '^---------'
adb shell am force-stop $PKG
adb logcat -d -s xrl_test | grep -q 'XRL_TEST PASS'
