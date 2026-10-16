# openxr_loader_nano

A plain C11 drop-in replacement for the Khronos OpenXR loader. It has the same library name, exports, CMake targets, manifest discovery, and layer and runtime negotiation as upstream, and passes upstream's `loader_test`.

## Why?

openxr_loader_nano is a 4-12x smaller binary, compiles about 20x faster, saves a few nanoseconds on every call through its exported functions, and needs no third-party libraries or C++ runtime. In StereoKit, the Android loader it ships goes from 722KB to 61KB, and the loader compiles in under a second.

## Using it

Point your FetchContent or CPM dependency at this repository instead of `KhronosGroup/OpenXR-SDK`. The `openxr_loader`, `headers`, `OpenXR::openxr_loader` and `OpenXR::headers` targets work as before, and so do `DYNAMIC_LOADER` and `BUILD_LOADER`. Upstream's other options are ignored.

```cmake
FetchContent_Declare(OpenXR GIT_REPOSITORY https://github.com/maluoi/openxr_loader_nano.git GIT_TAG main)
FetchContent_MakeAvailable(OpenXR)
target_link_libraries(my_app PRIVATE OpenXR::openxr_loader)
```

## Installing

An install writes the same files as upstream's: `libopenxr_loader`, `include/openxr`, `OpenXRConfig.cmake` and `openxr.pc`. Installed system wide, it replaces the official loader for every program that finds it. Prefer a private prefix that you add to `CMAKE_PREFIX_PATH` or `LD_LIBRARY_PATH`, or package it as a replacement for your distro's OpenXR loader package. Never install over `/usr`.

To check which loader you have, `find_package(OpenXR)` sets `OpenXR_LOADER_IMPLEMENTATION` to `openxr_loader_nano`, and `XR_LOADER_DEBUG=info` logs the loader's name when it loads a runtime.

## Differences from upstream

A few deliberate behavior differences, mostly bug fixes, are listed in [CLAUDE.md](CLAUDE.md#deliberate-differences-from-upstream).

## License

Apache-2.0 OR MIT, the same as the Khronos OpenXR loader. Both texts are in [LICENSE](LICENSE).
