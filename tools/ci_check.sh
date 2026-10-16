#!/bin/bash
# Builds this loader for every platform the machine can, then runs upstream's
# loader_test and the differential probe against it. Prints a markdown table of
# results to stdout, with logs in <build dir>/logs. Exits non-zero if anything failed.
#
# Usage: tools/ci_check.sh <OpenXR-SDK-Source checkout> [build dir]

set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$(cd "$1" && pwd)
B=$(mkdir -p "${2:-$ROOT/build/ci}" && cd "${2:-$ROOT/build/ci}" && pwd)
JOBS=${JOBS:-4}
LOGS=$B/logs
mkdir -p "$LOGS"

failed=0
rows=""
fail_details=""

# check <name> <command...>: runs the command, recording a row and the log tail on failure
check() {
	local name=$1; shift
	local log="$LOGS/${name// /_}.log"
	if "$@" > "$log" 2>&1; then
		rows+="| $name | ✅ |"$'\n'
	else
		rows+="| $name | ❌ |"$'\n'
		fail_details+="<details><summary>$name log tail</summary>"$'\n\n```\n'"$(tail -n 40 "$log")"$'\n```\n</details>\n\n'
		failed=1
	fi
}

skip() {
	rows+="| $1 | skipped, $2 |"$'\n'
}

build() {
	local dir=$1; shift
	cmake -S "$ROOT" -B "$B/$dir" -G Ninja "$@" && ninja -C "$B/$dir" -j "$JOBS"
}

# Consumers like StereoKit turn on UNITY_BUILD for the loader, so the builds
# not used by the tests below merge their sources to catch name collisions
check "Linux shared" build linux-shared -DXRL_BUILD_TESTS=ON
check "Linux static (unity)" build linux-static -DDYNAMIC_LOADER=OFF -DCMAKE_UNITY_BUILD=ON

if command -v x86_64-w64-mingw32-gcc > /dev/null; then
	check "Windows DLL (MinGW, unity)" build mingw-dll -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc -DDYNAMIC_LOADER=ON -DCMAKE_UNITY_BUILD=ON
else
	skip "Windows DLL (MinGW, unity)" "no x86_64-w64-mingw32-gcc"
fi

NDK=${ANDROID_NDK_LATEST_HOME:-${ANDROID_NDK_HOME:-${ANDROID_NDK:-}}}
if [ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
	check "Android arm64 (unity)" build android-arm64 -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=26 -DCMAKE_UNITY_BUILD=ON
else
	skip "Android arm64 (unity)" "no Android NDK"
fi

# Upstream's code generation vendors an old jinja2 that wants markupsafe's
# removed soft_unicode. Put a patched copy of markupsafe ahead of the real one.
PYTHON=$(command -v python3)
if ! "$PYTHON" -c "from markupsafe import soft_unicode" 2> /dev/null; then
	MARKUPSAFE=$("$PYTHON" -c "import markupsafe, os; print(os.path.dirname(markupsafe.__file__))")
	rm -rf "$B/pyshim" && mkdir -p "$B/pyshim"
	cp -r "$MARKUPSAFE" "$B/pyshim/markupsafe"
	echo "soft_unicode = soft_str" >> "$B/pyshim/markupsafe/__init__.py"
	printf '#!/bin/sh\nPYTHONPATH="%s${PYTHONPATH:+:$PYTHONPATH}" exec "%s" "$@"\n' "$B/pyshim" "$PYTHON" > "$B/pyshim/python3"
	chmod +x "$B/pyshim/python3"
	PYTHON=$B/pyshim/python3
fi

UP=$B/upstream
check "Upstream tests build" bash -c "cmake -S '$SRC' -B '$UP' -G Ninja -DCMAKE_BUILD_TYPE=Release -DDYNAMIC_LOADER=ON -DBUILD_TESTS=ON -DBUILD_API_LAYERS=ON \
	-DBUILD_CONFORMANCE_TESTS=OFF -DBUILD_SDK_TESTS=OFF -DBUILD_WITH_SYSTEM_JSONCPP=OFF -DPython3_EXECUTABLE='$PYTHON' && ninja -C '$UP' -j $JOBS loader_test openxr_loader"

TESTS=$UP/src/tests/loader_test
OURS=$B/linux-shared
if [ -x "$TESTS/loader_test" ] && [ -f "$OURS/libopenxr_loader.so.1" ]; then
	# XDG_CONFIG_HOME keeps the test on its own runtime instead of the machine's
	check "Upstream loader_test" bash -c "cd '$TESTS' && LD_LIBRARY_PATH='$OURS' XDG_CONFIG_HOME='$TESTS' ./loader_test"

	# Upstream crashes on the broken badnegotiate layers, so the comparison skips them, and ours runs them alone
	probe_env=(XDG_CONFIG_HOME="$TESTS" XR_API_LAYER_PATH="$TESTS/resources/layers" XR_API_DUMP_FILE_NAME="$B/api_dump.txt" XR_LOADER_DEBUG=none)
	env "${probe_env[@]}" XRL_PROBE_SKIP_LAYERS=badnegotiate LD_LIBRARY_PATH="$UP/src/loader" timeout 120 "$OURS/probe" 2> /dev/null | grep -v VALID_DEBUG > "$LOGS/probe_upstream.txt"
	env "${probe_env[@]}" XRL_PROBE_SKIP_LAYERS=badnegotiate LD_LIBRARY_PATH="$OURS"          timeout 120 "$OURS/probe" 2> /dev/null | grep -v VALID_DEBUG > "$LOGS/probe_ours.txt"
	check "Probe matches upstream" diff -u "$LOGS/probe_upstream.txt" "$LOGS/probe_ours.txt"
	check "Probe survives broken layers" env "${probe_env[@]}" LD_LIBRARY_PATH="$OURS" timeout 120 "$OURS/probe"
else
	skip "Upstream loader_test" "tests didn't build"
	skip "Probe matches upstream" "tests didn't build"
fi

echo "| Check | Result |"
echo "|---|---|"
printf '%s' "$rows"
echo
printf '%s' "$fail_details"
exit $failed
