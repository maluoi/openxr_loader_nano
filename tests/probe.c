// SPDX-License-Identifier: Apache-2.0 OR MIT
// Differential probe: prints a deterministic transcript of loader behavior,
// so the same environment can be compared between this loader and upstream's
// by diffing their output. Uses only public API, so it works with either.

#include <openxr/openxr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* result_str(XrResult r) {
	switch (r) {
#define XRL_RESULT_CASE(name) case name: return #name;
	XRL_RESULT_CASE(XR_SUCCESS)
	XRL_RESULT_CASE(XR_ERROR_VALIDATION_FAILURE)
	XRL_RESULT_CASE(XR_ERROR_RUNTIME_FAILURE)
	XRL_RESULT_CASE(XR_ERROR_HANDLE_INVALID)
	XRL_RESULT_CASE(XR_ERROR_INITIALIZATION_FAILED)
	XRL_RESULT_CASE(XR_ERROR_API_VERSION_UNSUPPORTED)
	XRL_RESULT_CASE(XR_ERROR_EXTENSION_NOT_PRESENT)
	XRL_RESULT_CASE(XR_ERROR_LIMIT_REACHED)
	XRL_RESULT_CASE(XR_ERROR_SIZE_INSUFFICIENT)
	XRL_RESULT_CASE(XR_ERROR_FUNCTION_UNSUPPORTED)
	XRL_RESULT_CASE(XR_ERROR_API_LAYER_NOT_PRESENT)
	XRL_RESULT_CASE(XR_ERROR_RUNTIME_UNAVAILABLE)
	XRL_RESULT_CASE(XR_ERROR_NAME_INVALID)
	XRL_RESULT_CASE(XR_ERROR_FILE_ACCESS_ERROR)
	XRL_RESULT_CASE(XR_ERROR_FILE_CONTENTS_INVALID)
#undef XRL_RESULT_CASE
	default: return "other";
	}
}

static int cmp_ext(const void* a, const void* b) {
	return strcmp(((const XrExtensionProperties*)a)->extensionName, ((const XrExtensionProperties*)b)->extensionName);
}

static void print_exts(const char* label, const char* layer) {
	uint32_t count = 0;
	XrResult r     = xrEnumerateInstanceExtensionProperties(layer, 0, &count, NULL);
	printf("%s: %s count=%u\n", label, result_str(r), count);
	if (XR_FAILED(r) || count == 0) return;

	XrExtensionProperties* props = calloc(count, sizeof(XrExtensionProperties));
	for (uint32_t i = 0; i < count; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	r = xrEnumerateInstanceExtensionProperties(layer, count, &count, props);
	qsort(props, count, sizeof(XrExtensionProperties), cmp_ext);
	for (uint32_t i = 0; i < count; i++) printf("  %s v%u\n", props[i].extensionName, props[i].extensionVersion);

	// Too small a buffer must report the needed size
	uint32_t small = 0;
	if (count > 1) printf("  undersized: %s\n", result_str(xrEnumerateInstanceExtensionProperties(layer, count - 1, &small, props)));
	free(props);
}

static void try_create(const char* label, uint32_t layer_count, const char* const* layers, uint32_t ext_count, const char* const* exts) {
	XrInstanceCreateInfo info = {
		.type                  = XR_TYPE_INSTANCE_CREATE_INFO,
		.applicationInfo       = { .applicationName = "probe", .apiVersion = XR_API_VERSION_1_0 },
		.enabledApiLayerCount  = layer_count,
		.enabledApiLayerNames  = layers,
		.enabledExtensionCount = ext_count,
		.enabledExtensionNames = exts,
	};
	XrInstance instance = XR_NULL_HANDLE;
	XrResult   r        = xrCreateInstance(&info, &instance);
	printf("create %s: %s\n", label, result_str(r));
	if (XR_FAILED(r)) return;

	const char* names[] = { "xrGetSystem", "xrCreateDebugUtilsMessengerEXT", "xrNotAFunction" };
	for (int i = 0; i < 3; i++) {
		PFN_xrVoidFunction fn = (PFN_xrVoidFunction)1; // Must come back NULL on failure
		XrResult           gr = xrGetInstanceProcAddr(instance, names[i], &fn);
		printf("  gipa %s: %s %s\n", names[i], result_str(gr), fn ? "set" : "null");
	}
	XrInstance second = XR_NULL_HANDLE;
	printf("  second instance: %s\n", result_str(xrCreateInstance(&info, &second)));
	XrSystemId system = XR_NULL_SYSTEM_ID;
	printf("  xrGetSystem: %s\n", result_str(xrGetSystem(instance, &(XrSystemGetInfo){ .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY }, &system)));
	printf("  destroy: %s\n", result_str(xrDestroyInstance(instance)));
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0); // Keep the transcript intact if a layer or runtime hangs or crashes
	uint32_t layer_count = 0;
	XrResult r           = xrEnumerateApiLayerProperties(0, &layer_count, NULL);
	printf("layers: %s count=%u\n", result_str(r), layer_count);
	XrApiLayerProperties* layers = calloc(layer_count + 1, sizeof(XrApiLayerProperties));
	for (uint32_t i = 0; i < layer_count; i++) layers[i].type = XR_TYPE_API_LAYER_PROPERTIES;
	if (layer_count > 0) {
		uint32_t small = 0;
		printf("  undersized: %s\n", result_str(xrEnumerateApiLayerProperties(layer_count - 1, &small, layers)));
		xrEnumerateApiLayerProperties(layer_count, &layer_count, layers);
	}
	for (uint32_t i = 0; i < layer_count; i++) {
		printf("  %s spec=%u.%u impl=%u \"%s\"\n", layers[i].layerName, (unsigned)XR_VERSION_MAJOR(layers[i].specVersion),
			(unsigned)XR_VERSION_MINOR(layers[i].specVersion), layers[i].layerVersion, layers[i].description);
	}

	print_exts("exts", NULL);
	print_exts("exts bogus layer", "XR_APILAYER_bogus");
	for (uint32_t i = 0; i < layer_count; i++) {
		char label[300];
		snprintf(label, sizeof(label), "exts %s", layers[i].layerName);
		print_exts(label, layers[i].layerName);
	}

	PFN_xrVoidFunction fn = NULL;
	printf("gipa null xrCreateInstance: %s\n",       result_str(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrCreateInstance", &fn)));
	printf("gipa null xrInitializeLoaderKHR: %s\n",  result_str(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &fn)));
	printf("gipa null xrGetSystem: %s\n",            result_str(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrGetSystem", &fn)));
	printf("gipa null xrDestroyInstance: %s\n",      result_str(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrDestroyInstance", &fn)));
	printf("destroy null: %s\n",                     result_str(xrDestroyInstance(XR_NULL_HANDLE)));

	const char* bogus_ext   = "XR_KHR_not_real";
	const char* debug_utils = XR_EXT_DEBUG_UTILS_EXTENSION_NAME;
	const char* bogus_layer = "XR_APILAYER_bogus";
	try_create("plain",       0, NULL, 0, NULL);
	try_create("debug_utils", 0, NULL, 1, &debug_utils);
	try_create("bogus ext",   0, NULL, 1, &bogus_ext);
	try_create("bogus layer", 1, &bogus_layer, 0, NULL);
	// Upstream crashes creating instances with some deliberately broken test layers, so a differential run can skip them
	const char* skip = getenv("XRL_PROBE_SKIP_LAYERS");
	for (uint32_t i = 0; i < layer_count; i++) {
		const char* name = layers[i].layerName;
		char        label[300];
		if (skip && skip[0] && strstr(name, skip)) continue;
		snprintf(label, sizeof(label), "layer %s", name);
		try_create(label, 1, &name, 0, NULL);
	}
	free(layers);
	return 0;
}
