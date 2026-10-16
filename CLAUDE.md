# CLAUDE.md

This file provides guidance to AI agents when working with code in this repository.

## What this is

A small, plain C11 replacement for the Khronos OpenXR loader (`openxr_loader`). It's a drop-in: same library name, same exported functions, same manifest discovery and layer/runtime negotiation behavior. Priorities, in order: correctness on every platform (Windows, Linux, Android, macOS), small binary size, fast compile, then performance.

The upstream loader is the reference for behavior. When this loader and upstream disagree, upstream is right unless we've deliberately documented otherwise.

## Repository layout

- `src/` the loader. `exports.gen.*` and the `openxr_loader.{def,map,expsym}` export lists are generated, and checked in.
- `include/openxr/` the upstream OpenXR SDK headers, vendored so consumers build without a network pull. Their `XR_CURRENT_API_VERSION` is the single record of which SDK release this tracks, and CMake's project version comes from it. Don't edit them by hand.
- `tools/gen_exports.py [xr.xml]` regenerates the exports. With no argument it downloads `xr.xml` for the vendored headers' release, since `xr.xml` is only needed for development and isn't stored.
- `tools/sync_upstream.py`, `tools/ci_check.sh` and `.github/workflows/upstream-sync.yml` handle upstream releases, see below.
- `tests/` our own tests: `xr_smoke` (real runtime), `probe` (differential transcript), and `android/` (on-device APK).
- `external/OpenXR-SDK*/` local full clones of upstream for reference: upstream's loader source, `loader_test`, and the loader design doc (`specification/loader/`). Gitignored, nothing builds from them.
- `research/` the size and performance investigation that led to this project. Gitignored.

## Updating to a new upstream release

A GitHub Action (`upstream-sync.yml`) checks daily for a new OpenXR SDK release. When one appears it runs `tools/sync_upstream.py` to replace the vendored headers and regenerate the exports, runs `tools/ci_check.sh`, and opens a PR titled "OpenXR SDK X.Y.Z". The PR body logs what changed, flags anything risky, lists upstream's loader changes and "Loader:" changelog entries, and ends with a prompt for an agent to do the behavioral review. If upstream rearranges files the sync depends on, it records each one under "Sync problems", keeps going with whatever still works, and opens the PR as a draft. Fixing those includes updating `tools/sync_upstream.py`, so the next release syncs cleanly. To do the same by hand:

1. `python3 tools/sync_upstream.py --version X.Y.Z` writes `build/sync_report.md`, and full upstream diffs to `build/sync_diffs/`.
2. Port the behavior changes the report lists, following its "Agent review prompt".
3. `tools/ci_check.sh <OpenXR-SDK-Source checkout at the new release>` builds every platform it can and runs upstream's `loader_test` and the probe differential.

## Coding Guidelines

Before writing code, make it clear to the user what code you're working on, and that you're going to work on code. This is especially important if the user has not asked for code.

When writing code, please use the Edit tool! Edit is safer, and lets me observe changes (I enjoy reading the edits!). For changing code algorithmically (mass rename across multiple files), it's okay to use other tools, but be cautious when doing so.

**C**
- Plain C11. No C++ in the loader. No STL, no virtual functions, no operator overloads, no exceptions.
- POD structs only - no constructors, no classes, no default values. Zero-initialized structs should be a valid default state.
- Use `stdint.h` types: `int32_t` not `int`, `uint8_t` not `unsigned char`.
- Use designated initializers for structs, inline at call sites when possible:
  `compile(&(svsl_options_t){ .spirv_version = SVSL_SPIRV_1_3 });`
- Use `//` comments, not `/* */`.
- Use function parameter prefixes for pointers: `opt_` = optional (nullable), `out_` = written to, `ref_` = read and written, const keyword for read only.
- Sources must survive a unity build, since consumers like StereoKit turn it on. File-scope `static` names must be unique across all of `src/`, and feature macros like `_GNU_SOURCE` go on the target in CMake, never in a source.
- No printf or scanf family in the loader, they drag the CRT's formatting code into static CRT builds. Use `xrl_fmt` / `xrl_vfmt`, which only support `%s %.*s %d %u %%` (pass `int32_t`/`uint32_t`), and parse numbers with `strtoul`.

**General:**
- **Functional style**: functions should be pure where possible, taking specific arguments rather than whole contexts. Exception: public functions of singleton-like modules.
- **Data-oriented design**: prefer flat arrays and indices over pointer graphs, be aware of memory usage patterns and architect data for cache efficiency. Coding is _data transformation_, not shaping objects.
- **Single source of truth**: caching a value in multiple places is a source of bugs. Derive, don't duplicate. If a cache is genuinely needed, it must be private to one module.
- Prefer simplicity, low indentation, and early-outs.
- Immediate-mode interface design. Keep code stateless where possible.
- Tabs for indentation, spaces for alignment (mix them so alignment holds at any tab width).
- Prefer early returns over deep nesting.
- Vertical alignment of related code is encouraged.
- API names (things developers type) should be short and self-explanatory. Units of measure go in names developers don't type.
- Prefer pure functions when possible, minimize unnecessary function inputs.
- Null checking should be done sparingly, an NRE is very visible and easy to debug. If an item should not be null, it should _not_ be checked for null. If an item can be null, indicate it is _optional_ - this repo's parameter prefixes carry that contract (`opt_`, `opt_out_`, plus `ref_`/`out_` for mutated and output parameters). The exception is the OpenXR API boundary, where the spec requires validating application input.
- For threading, prefer lock-free techniques and atomics whenever possible. Actual locks are a last resort.
- Non commentdoc comments: delete any that restate the code. Don't use them to preserve the reasoning from the session that produced the code, state what the next reader needs rather than what you learned. What survives that is one line; two only if the second is doing real work. If it needs more, it belongs in the commit message or a doc, not the source. A comment that explains what the code does means the code should be clearer, not that it needs a comment: explain why, or fix the name. Before finishing a change, list every comment in the diff and judge each one. Don't scan for the long ones.

  Too long:
  ```c
  // TimedWaitAny legalizes a blocking skr_future_wait, which cubemap lighting
  // and texture readback both need. In a browser that rests on Asyncify, and
  // asking without it fails instance creation outright rather than degrading,
  // so the web build always links -sASYNCIFY.
  ```
  Better:
  ```c
  // Asking for this without Asyncify fails instance creation outright
  ```
- Comments in private structs should be short, on the same line as the field.
- Forward function declarations don't get commentdocs. This keeps header files condensed and clean.
- Please avoid free standing - and —, check for run-on sentences.

## Building and testing

Keep builds light: no more than `-j8`.

```bash
# Linux, shared like upstream's default, plus our smoke test
cmake -S . -B build/linux-shared -G Ninja -DXRL_BUILD_TESTS=ON && ninja -C build/linux-shared
# Static, as StereoKit links it on desktop
cmake -S . -B build/linux-static -G Ninja -DDYNAMIC_LOADER=OFF && ninja -C build/linux-static
# Android arm64
cmake -S . -B build/android-arm64 -G Ninja -DCMAKE_TOOLCHAIN_FILE=$HOME/Android/Sdk/ndk/25.2.9519653/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=26
# Windows with real MSVC through msvc-wine (~/Tools/msvc)
PATH=$HOME/Tools/msvc/bin/x64:$PATH CC=cl cmake -S . -B build/msvc-ON -G Ninja -DCMAKE_SYSTEM_NAME=Windows -DDYNAMIC_LOADER=ON -DXRL_BUILD_TESTS=ON
```

**Windows behavior under wine** uses a private prefix, `WINEPREFIX=build/wineprefix`, never the default one. Wine runs everything elevated, so like on real elevated Windows, `XR_RUNTIME_JSON` and `XR_API_LAYER_PATH` are ignored. Point the registry at runtimes and layers instead (`wine reg add 'HKLM\SOFTWARE\Khronos\OpenXR\1' /v ActiveRuntime ...`, and DWORD 0 values under `ApiLayers\Explicit` or `Implicit`). Manifests generated by a cross-compile hold Linux paths, so prefix them with `Z:`.

**Differential probe**: `tests/probe.c` prints a deterministic transcript of loader behavior using only the public API. Run it against upstream's loader and ours in the same environment and diff the output; on Windows copy `probe.exe` next to each `openxr_loader.dll`. Set `XR_API_DUMP_FILE_NAME` so the api_dump layer doesn't write into the transcript.

**Upstream's loader_test and the Linux probe differential**: `JOBS=4 tools/ci_check.sh external/OpenXR-SDK-Source` builds upstream's tests into `build/ci/upstream` and runs them against our `.so` by swapping the library, then diffs the probe between the two loaders. It works around upstream's vendored jinja2 needing an old markupsafe, and points `XDG_CONFIG_HOME` at the test runtime so the machine's real active runtime stays out of it. To rerun just loader_test afterwards:
```bash
cd build/ci/upstream/src/tests/loader_test
LD_LIBRARY_PATH=$PWD/../../../../linux-shared XDG_CONFIG_HOME=$PWD ./loader_test
```

**Real runtime on desktop**: `build/linux-shared/xr_smoke` runs a headless session against Monado. Start `monado-service` with stdin on a pipe (`sleep 60 | monado-service &`), then run with `XR_RUNTIME_JSON=/usr/local/share/openxr/1/openxr_monado.json`.

**On device**: a Quest is usually attached over adb. `tests/android/build_apk.sh <libopenxr_loader.so> <out.apk>` packages the GLES session test around any loader build, and `tests/android/run_on_device.sh <apk>` installs, runs, and prints its logcat. A headset nobody is wearing keeps sessions IDLE, so run `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close` first, and `...automation_disable` after.

## Deliberate differences from upstream

- Simplification: loader log messages don't carry debug_utils object names or session labels.
- Bug fix: if the next-chain debug messenger can't be created, `xrCreateInstance` fails and cleans up, instead of leaving an orphaned instance active that blocks every later `xrCreateInstance`.
- Bug fix: `xrEnumerateInstanceExtensionProperties` keeps the application's `next` pointers instead of overwriting them.
- Robustness: a trampoline for a core function the runtime didn't provide returns `XR_ERROR_FUNCTION_UNSUPPORTED` instead of calling NULL.
- Robustness: an API layer whose negotiation returns a NULL `createApiLayerInstance` is skipped with `XR_ERROR_FILE_CONTENTS_INVALID`. Upstream calls it and crashes, as with its own `badnegotiate_invalid_interface` and `invalid_api` test layers.
- Simplification: no verbose "Entering loader trampoline" style logging.
- Thread safety: `xrInitializeLoaderKHR` takes the loader lock.
- Identification: loading a runtime logs an extra info message naming this loader. The installed `OpenXRConfig.cmake` sets `OpenXR_LOADER_IMPLEMENTATION`, and `openxr.pc` and the Windows `ProductName` carry the name too. Everything else uses upstream's names.
