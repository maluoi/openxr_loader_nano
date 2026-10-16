// SPDX-License-Identifier: Apache-2.0 OR MIT
// The loader's API entry points: instance lifetime, runtime and API layer
// negotiation, and the parts of XR_EXT_debug_utils the loader implements.

#include "loader.h"

#include <stdlib.h>
#include <string.h>

#define XRL_STRINGIZE2(s) #s
#define XRL_STRINGIZE(s)  XRL_STRINGIZE2(s)
#define XRL_NAME_STR      XRL_STRINGIZE(XRL_NAME)

typedef struct loader_layer_t {
	xrl_lib_t                    lib;
	PFN_xrGetInstanceProcAddr    gipa;
	PFN_xrCreateApiLayerInstance create;
	int32_t                      manifest; // Index into the manifest list the layer was loaded from
	char                         name[XR_MAX_API_LAYER_NAME_SIZE];
} loader_layer_t;

static struct {
	// The runtime loads on demand, and unloads with its instance
	xrl_lib_t                           rt_lib;
	PFN_xrGetInstanceProcAddr           rt_gipa;
	PFN_xrCreateDebugUtilsMessengerEXT  rt_create_messenger; // The runtime's own XR_EXT_debug_utils, if it has one
	PFN_xrDestroyDebugUtilsMessengerEXT rt_destroy_messenger;
	PFN_xrSubmitDebugUtilsMessageEXT    rt_submit_message;
	PFN_xrSetDebugUtilsObjectNameEXT    rt_set_object_name;

	// The single active instance
	XrInstance                instance;
	PFN_xrGetInstanceProcAddr top_gipa;         // First API layer, or the loader's terminator
	loader_layer_t*           layers;
	int32_t                   layer_count;
	bool                      debug_utils;      // The application enabled XR_EXT_debug_utils
	XrDebugUtilsMessengerEXT  create_messenger; // From XrInstanceCreateInfo's next chain
	uint64_t                  fake_messengers;  // Handle source when the runtime lacks XR_EXT_debug_utils

	// XR_EXT_loader_init_properties overrides, packed as "name\0value\0..."
	char*                     props;
	size_t                    props_size;
} g;

#define XRL_DEBUG_UTILS XR_EXT_DEBUG_UTILS_EXTENSION_NAME

///////////////////////////////////////////
// Properties
///////////////////////////////////////////

static const char* prop_override(const char* name) {
	const char* p   = g.props;
	const char* end = g.props + g.props_size;
	while (p && p < end) {
		const char* value = p + strlen(p) + 1;
		if (strcmp(p, name) == 0) return value;
		p = value + strlen(value) + 1;
	}
	return NULL;
}

bool xrl_prop_get(const char* name, bool secure, char* out_value, size_t value_size) {
	const char* value = prop_override(name);
	if (value == NULL) return xrl_env_get(name, secure, out_value, value_size);
	xrl_fmt(out_value, value_size, "%s", value);
	return value[0] != '\0';
}

bool xrl_prop_is_set(const char* name) {
	return prop_override(name) != NULL || xrl_env_is_set(name);
}

///////////////////////////////////////////

static XrResult props_init(const XrLoaderInitInfoBaseHeaderKHR* info) {
	while (info != NULL && info->type != XR_TYPE_LOADER_INIT_INFO_PROPERTIES_EXT) info = (const XrLoaderInitInfoBaseHeaderKHR*)info->next;
	if (info == NULL) return XR_SUCCESS;

	const XrLoaderInitInfoPropertiesEXT* props = (const XrLoaderInitInfoPropertiesEXT*)info;
	size_t size = 0;
	for (uint32_t i = 0; i < props->propertyValueCount; i++) {
		const XrLoaderInitPropertyValueEXT* prop = &props->propertyValues[i];
		if (prop->name == NULL || prop->value == NULL || prop->name[0] == '\0') return XR_ERROR_VALIDATION_FAILURE;
		size += strlen(prop->name) + strlen(prop->value) + 2;
	}

	char* packed = size ? (char*)malloc(size) : NULL;
	if (size && packed == NULL) return XR_ERROR_OUT_OF_MEMORY;
	char* p = packed;
	for (uint32_t i = 0; i < props->propertyValueCount; i++) {
		size_t name_size  = strlen(props->propertyValues[i].name)  + 1;
		size_t value_size = strlen(props->propertyValues[i].value) + 1;
		memcpy(p, props->propertyValues[i].name,  name_size);  p += name_size;
		memcpy(p, props->propertyValues[i].value, value_size); p += value_size;
	}
	free(g.props);
	g.props      = packed;
	g.props_size = size;
	return XR_SUCCESS;
}

///////////////////////////////////////////

static const XrLoaderInitInfoBaseHeaderKHR* init_param(void) {
#if defined(__ANDROID__)
	return xrl_android_init_param();
#else
	return NULL;
#endif
}

///////////////////////////////////////////
// Runtime
///////////////////////////////////////////

static void runtime_unload(void) {
	if (g.rt_lib) xrl_lib_close(g.rt_lib);
	g.rt_lib               = NULL;
	g.rt_gipa              = NULL;
	g.rt_create_messenger  = NULL;
	g.rt_destroy_messenger = NULL;
	g.rt_submit_message    = NULL;
	g.rt_set_object_name   = NULL;
}

///////////////////////////////////////////

static XrResult runtime_try(const xrl_manifests_t* list, const xrl_manifest_t* manifest, const char* command) {
	xrl_lib_t lib = xrl_lib_open(manifest->library);
	if (lib == NULL) {
		char err[1024];
		xrl_lib_error(manifest->library, err, sizeof(err));
		xrl_log(xrl_sev_error, command, "Skipping runtime manifest %s, failed to load with message \"%s\"", manifest->file, err);
		return XR_ERROR_FILE_ACCESS_ERROR;
	}

	// Platforms with loader init data forward it to the runtime, before negotiation if it exports the function
	const XrLoaderInitInfoBaseHeaderKHR* param     = init_param();
	bool                                 forwarded = false;
	if (param != NULL) {
		PFN_xrInitializeLoaderKHR init = (PFN_xrInitializeLoaderKHR)xrl_lib_sym(lib, xrl_manifest_symbol(list, manifest, "xrInitializeLoaderKHR"));
		if (init != NULL) {
			XrResult res = init(param);
			if (XR_FAILED(res)) {
				xrl_log(xrl_sev_error, command, "Forwarding xrInitializeLoaderKHR to the runtime failed");
				xrl_lib_close(lib);
				return res;
			}
			forwarded = true;
		}
	}

	const char* negotiate_name = xrl_manifest_symbol(list, manifest, "xrNegotiateLoaderRuntimeInterface");
	PFN_xrNegotiateLoaderRuntimeInterface negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)xrl_lib_sym(lib, negotiate_name);

	XrNegotiateLoaderInfo loader_info = {
		.structType          = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
		.structVersion       = XR_LOADER_INFO_STRUCT_VERSION,
		.structSize          = sizeof(XrNegotiateLoaderInfo),
		.minInterfaceVersion = 1,
		.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION,
		.minApiVersion       = XR_MAKE_VERSION(1, 0, 0),
		.maxApiVersion       = XR_MAKE_VERSION(1, 0x3ff, 0xfff),
	};
	XrNegotiateRuntimeRequest request = {
		.structType    = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST,
		.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION,
		.structSize    = sizeof(XrNegotiateRuntimeRequest),
	};

	XrResult res = XR_ERROR_RUNTIME_FAILURE;
	if (negotiate != NULL) res = negotiate(&loader_info, &request);
	else                   xrl_log(xrl_sev_error, command, "Failed to find the runtime's negotiate function %s", negotiate_name);

	if (XR_SUCCEEDED(res)) {
		const char* problem = NULL;
		if      (request.getInstanceProcAddr == NULL)                                                                   problem = "returned a NULL getInstanceProcAddr";
		else if (request.runtimeInterfaceVersion == 0 || request.runtimeInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION) problem = "returned an invalid interface version";
		else if (XR_VERSION_MAJOR(request.runtimeApiVersion) != XR_VERSION_MAJOR(XR_CURRENT_API_VERSION))                   problem = "returned an OpenXR version this loader isn't compatible with";
		if (problem) {
			xrl_log(xrl_sev_error, command, "Skipping runtime manifest %s, negotiation succeeded but %s", manifest->file, problem);
			res = XR_ERROR_FILE_CONTENTS_INVALID;
		}
	}

	if (XR_SUCCEEDED(res) && !forwarded && param != NULL) {
		PFN_xrInitializeLoaderKHR init = NULL;
		if (XR_SUCCEEDED(request.getInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&init))) {
			if (init == NULL) {
				xrl_log(xrl_sev_error, command, "The runtime's xrGetInstanceProcAddr succeeded for xrInitializeLoaderKHR, but returned NULL");
				res = XR_ERROR_RUNTIME_FAILURE;
			} else {
				res = init(param);
				if (XR_FAILED(res)) xrl_log(xrl_sev_error, command, "Forwarding xrInitializeLoaderKHR to the runtime failed");
			}
		}
	}

	if (XR_FAILED(res)) {
		xrl_log(xrl_sev_error, command, "Skipping runtime manifest %s, negotiation failed with error %d", manifest->file, (int)res);
		xrl_lib_close(lib);
		return res;
	}

	xrl_log(xrl_sev_info, command, "This is the " XRL_NAME_STR " loader, built with OpenXR %u.%u.%u headers",
		(uint32_t)XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), (uint32_t)XR_VERSION_MINOR(XR_CURRENT_API_VERSION), (uint32_t)XR_VERSION_PATCH(XR_CURRENT_API_VERSION));
	xrl_log(xrl_sev_info, command, "Loaded the runtime from manifest %s using interface version %u and OpenXR API version %u.%u", manifest->file,
		request.runtimeInterfaceVersion, (uint32_t)XR_VERSION_MAJOR(request.runtimeApiVersion), (uint32_t)XR_VERSION_MINOR(request.runtimeApiVersion));
	g.rt_lib  = lib;
	g.rt_gipa = request.getInstanceProcAddr;
	return XR_SUCCESS;
}

///////////////////////////////////////////

static XrResult runtime_load(const char* command) {
	if (g.rt_gipa) return XR_SUCCESS;

#if defined(__ANDROID__)
	if (!xrl_android_initialized()) {
		xrl_log(xrl_sev_error, command, "Can't load a runtime, xrInitializeLoaderKHR wasn't successfully called");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
#endif

	xrl_manifests_t list   = {0};
	XrResult        result = XR_ERROR_RUNTIME_UNAVAILABLE;
	xrl_manifests_find(&list, xrl_manifest_runtime, command);
	for (int32_t i = 0; i < list.count && XR_FAILED(result); i++) {
		result = runtime_try(&list, &list.items[i], command);
	}
	xrl_manifests_free(&list);

	if (XR_FAILED(result)) {
		xrl_log(xrl_sev_error, command, "Failed to load a runtime");
		return XR_ERROR_RUNTIME_UNAVAILABLE;
	}
	return XR_SUCCESS;
}

///////////////////////////////////////////

// Asks the runtime for its instance extensions. The caller frees the result.
static XrExtensionProperties* runtime_exts(uint32_t* out_count) {
	*out_count = 0;
	PFN_xrEnumerateInstanceExtensionProperties enumerate = NULL;
	g.rt_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&enumerate);
	uint32_t count = 0;
	if (enumerate == NULL || XR_FAILED(enumerate(NULL, 0, &count, NULL)) || count == 0) return NULL;

	XrExtensionProperties* props = (XrExtensionProperties*)calloc(count, sizeof(XrExtensionProperties));
	if (props == NULL) return NULL;
	for (uint32_t i = 0; i < count; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	if (XR_FAILED(enumerate(NULL, count, &count, props))) count = 0;
	*out_count = count;
	return props;
}

static bool exts_contain(const XrExtensionProperties* props, uint32_t count, const char* name) {
	for (uint32_t i = 0; i < count; i++) {
		if (strcmp(props[i].extensionName, name) == 0) return true;
	}
	return false;
}

///////////////////////////////////////////
// API layers
///////////////////////////////////////////

static void layers_unload(loader_layer_t* layers, int32_t count) {
	for (int32_t i = 0; i < count; i++) xrl_lib_close(layers[i].lib);
	free(layers);
}

///////////////////////////////////////////

// Appends the named layer to the load order, returning false if no manifest provides it.
static bool layer_select(const xrl_manifests_t* list, int32_t implicit_count, int32_t* ref_order, int32_t* ref_order_count, const char* name, size_t name_len) {
	for (int32_t i = 0; i < *ref_order_count; i++) {
		const char* selected = list->items[ref_order[i]].name;
		if (strlen(selected) == name_len && strncmp(selected, name, name_len) == 0) return true;
	}
	for (int32_t i = implicit_count; i < list->count; i++) {
		const char* candidate = list->items[i].name;
		if (strlen(candidate) == name_len && strncmp(candidate, name, name_len) == 0) {
			ref_order[(*ref_order_count)++] = i;
			return true;
		}
	}
	xrl_log(xrl_sev_error, "xrCreateInstance", "Failed to find API layer %.*s", (int)name_len, name);
	return false;
}

///////////////////////////////////////////

// out_fatal is set when the failure should stop loading the remaining layers too.
static XrResult layer_try(const xrl_manifests_t* list, int32_t manifest_idx, loader_layer_t* out_layer, bool* out_fatal, const char* command) {
	const xrl_manifest_t* manifest = &list->items[manifest_idx];
	*out_fatal = false;
	xrl_lib_t             lib      = xrl_lib_open(manifest->library);
	if (lib == NULL) {
		char err[1024];
		xrl_lib_error(manifest->library, err, sizeof(err));
		xrl_log(xrl_sev_warn, command, "Skipping API layer %s, failed to load with message \"%s\"", manifest->name, err);
		return XR_ERROR_FILE_ACCESS_ERROR;
	}

	const XrLoaderInitInfoBaseHeaderKHR* param     = init_param();
	bool                                 forwarded = false;
	if (param != NULL) {
		PFN_xrInitializeLoaderKHR init = (PFN_xrInitializeLoaderKHR)xrl_lib_sym(lib, xrl_manifest_symbol(list, manifest, "xrInitializeLoaderKHR"));
		if (init != NULL) {
			XrResult res = init(param);
			if (XR_FAILED(res)) {
				xrl_log(xrl_sev_error, command, "Forwarding xrInitializeLoaderKHR to API layer %s failed", manifest->name);
				xrl_lib_close(lib);
				*out_fatal = true;
				return res;
			}
			forwarded = true;
		}
	}

	const char* negotiate_name = xrl_manifest_symbol(list, manifest, "xrNegotiateLoaderApiLayerInterface");
	PFN_xrNegotiateLoaderApiLayerInterface negotiate = (PFN_xrNegotiateLoaderApiLayerInterface)xrl_lib_sym(lib, negotiate_name);
	if (negotiate == NULL) {
		xrl_log(xrl_sev_error, command, "Skipping API layer %s because negotiation function %s was not found", manifest->name, negotiate_name);
		xrl_lib_close(lib);
		return XR_ERROR_API_LAYER_NOT_PRESENT;
	}

	XrNegotiateLoaderInfo loader_info = {
		.structType          = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO,
		.structVersion       = XR_LOADER_INFO_STRUCT_VERSION,
		.structSize          = sizeof(XrNegotiateLoaderInfo),
		.minInterfaceVersion = 1,
		.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION,
		.minApiVersion       = XR_MAKE_VERSION(1, 0, 0),
		.maxApiVersion       = XR_MAKE_VERSION(1, 0x3ff, 0xfff),
	};
	XrNegotiateApiLayerRequest request = {
		.structType    = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST,
		.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION,
		.structSize    = sizeof(XrNegotiateApiLayerRequest),
	};
	XrResult res = negotiate(&loader_info, manifest->name, &request);
	// Upstream doesn't check createApiLayerInstance, and crashes calling it when it's NULL
	if (XR_SUCCEEDED(res) && (request.getInstanceProcAddr == NULL || request.createApiLayerInstance == NULL)) {
		xrl_log(xrl_sev_warn, command, "Skipping API layer %s, negotiation did not return valid function pointers", manifest->name);
		res = XR_ERROR_FILE_CONTENTS_INVALID;
	}

	if (XR_SUCCEEDED(res) && !forwarded && param != NULL) {
		PFN_xrInitializeLoaderKHR init = NULL;
		if (XR_SUCCEEDED(request.getInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&init))) {
			if (init == NULL) {
				xrl_log(xrl_sev_error, command, "API layer %s's xrGetInstanceProcAddr succeeded for xrInitializeLoaderKHR, but returned NULL", manifest->name);
				res = XR_ERROR_RUNTIME_FAILURE;
			} else {
				res = init(param);
				if (XR_FAILED(res)) xrl_log(xrl_sev_error, command, "Forwarding xrInitializeLoaderKHR to API layer %s failed", manifest->name);
			}
		}
	}

	if (XR_FAILED(res)) {
		xrl_log(xrl_sev_warn, command, "Skipping API layer %s due to failed negotiation with error %d", manifest->name, (int)res);
		xrl_lib_close(lib);
		return res;
	}

	xrl_log(xrl_sev_info, command, "Loaded API layer %s using interface version %u and OpenXR API version %u.%u", manifest->name,
		request.layerInterfaceVersion, (uint32_t)XR_VERSION_MAJOR(request.layerApiVersion), (uint32_t)XR_VERSION_MINOR(request.layerApiVersion));
	*out_layer = (loader_layer_t){
		.lib      = lib,
		.gipa     = request.getInstanceProcAddr,
		.create   = request.createApiLayerInstance,
		.manifest = manifest_idx,
	};
	xrl_fmt(out_layer->name, sizeof(out_layer->name), "%s", manifest->name);
	return XR_SUCCESS;
}

///////////////////////////////////////////

static XrResult layers_load(const char* command, uint32_t name_count, const char* const* names, xrl_manifests_t* ref_list, loader_layer_t** out_layers, int32_t* out_count) {
	*out_layers = NULL;
	*out_count  = 0;
	if (name_count > 0 && names == NULL) {
		xrl_log_vuid("VUID-XrInstanceCreateInfo-enabledApiLayerNames-parameter", command, "enabledApiLayerCount is non-0 but array is NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	xrl_manifests_find(ref_list, xrl_manifest_layer_implicit, command);
	int32_t implicit_count = ref_list->count;
	xrl_manifests_find(ref_list, xrl_manifest_layer_explicit, command);

	// Every implicit layer loads closest to the app, then the explicitly enabled ones in the order they were named
	int32_t* order       = (int32_t*)malloc(((size_t)ref_list->count + 1) * sizeof(int32_t));
	int32_t  order_count = 0;
	bool     found_all   = true;
	if (order == NULL) return XR_ERROR_OUT_OF_MEMORY;
	for (int32_t i = 0; i < implicit_count; i++) order[order_count++] = i;

	char env[XRL_PATH_MAX];
	if (xrl_prop_get("XR_ENABLE_API_LAYERS", false, env, sizeof(env))) {
		const char* start = env;
		for (const char* sep; (sep = strchr(start, XRL_PATH_SEP)) != NULL; start = sep + 1) {
			found_all = layer_select(ref_list, implicit_count, order, &order_count, start, (size_t)(sep - start)) && found_all;
		}
		if (*start) found_all = layer_select(ref_list, implicit_count, order, &order_count, start, strlen(start)) && found_all;
	}
	for (uint32_t i = 0; i < name_count; i++) {
		found_all = layer_select(ref_list, implicit_count, order, &order_count, names[i], strlen(names[i])) && found_all;
	}

	loader_layer_t* layers     = (loader_layer_t*)calloc((size_t)order_count + 1, sizeof(loader_layer_t));
	int32_t         count      = 0;
	XrResult        last_error = XR_SUCCESS;
	bool            any_loaded = false;
	bool            fatal      = false;
	for (int32_t i = 0; layers && i < order_count && !fatal; i++) {
		XrResult res = layer_try(ref_list, order[i], &layers[count], &fatal, command);
		if (XR_SUCCEEDED(res)) {
			count++;
			any_loaded = true;
			last_error = XR_SUCCESS;
		} else if (fatal || res == XR_ERROR_API_LAYER_NOT_PRESENT || !any_loaded) {
			// Upstream reports a missing negotiate function even after earlier layers loaded
			last_error = res;
		}
	}
	free(order);
	if (layers == NULL) return XR_ERROR_OUT_OF_MEMORY;
	if (!found_all && !fatal) last_error = XR_ERROR_API_LAYER_NOT_PRESENT;

	if (XR_FAILED(last_error)) {
		layers_unload(layers, count);
		return last_error;
	}
	*out_layers = layers;
	*out_count  = count;
	return XR_SUCCESS;
}

///////////////////////////////////////////
// Terminators, the bottom of the call chain under all API layers
///////////////////////////////////////////

static bool str_terminated(const char* str, size_t size) {
	return memchr(str, '\0', size) != NULL;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_create_instance(const XrInstanceCreateInfo* info, XrInstance* instance) {
	const char* cmd = "xrCreateInstance";
	if (info->type != XR_TYPE_INSTANCE_CREATE_INFO) {
		xrl_log_vuid("VUID-XrInstanceCreateInfo-type-type", cmd, "expected XR_TYPE_INSTANCE_CREATE_INFO.");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	if (info->createFlags != 0) {
		xrl_log_vuid("VUID-XrInstanceCreateInfo-createFlags-zerobitmask", cmd, "flags must be 0.");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	if (!str_terminated(info->applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE)) {
		xrl_log_vuid("VUID-XrApplicationInfo-applicationName-parameter", cmd, "application name missing NULL terminator.");
		return XR_ERROR_NAME_INVALID;
	}
	if (!str_terminated(info->applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE)) {
		xrl_log_vuid("VUID-XrApplicationInfo-engineName-parameter", cmd, "engine name missing NULL terminator.");
		return XR_ERROR_NAME_INVALID;
	}
	if (info->applicationInfo.applicationName[0] == '\0') {
		xrl_log_vuid("VUID-XrApplicationInfo-applicationName-parameter", cmd, "application name must not be empty.");
		return XR_ERROR_NAME_INVALID;
	}
	if (info->enabledExtensionCount != 0 && info->enabledExtensionNames == NULL) {
		xrl_log_vuid("VUID-XrInstanceCreateInfo-enabledExtensionNames-parameter", cmd, "enabledExtensionCount is non-0 but array is NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	PFN_xrCreateInstance create = NULL;
	g.rt_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&create);
	if (create == NULL) return XR_ERROR_RUNTIME_FAILURE;
	XrResult result = create(info, instance);
	if (XR_FAILED(result)) return result;

	g.rt_gipa(*instance, "xrCreateDebugUtilsMessengerEXT",  (PFN_xrVoidFunction*)&g.rt_create_messenger);
	g.rt_gipa(*instance, "xrDestroyDebugUtilsMessengerEXT", (PFN_xrVoidFunction*)&g.rt_destroy_messenger);
	g.rt_gipa(*instance, "xrSubmitDebugUtilsMessageEXT",    (PFN_xrVoidFunction*)&g.rt_submit_message);
	g.rt_gipa(*instance, "xrSetDebugUtilsObjectNameEXT",    (PFN_xrVoidFunction*)&g.rt_set_object_name);
	return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_create_api_layer_instance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer_info, XrInstance* instance) {
	(void)layer_info;
	return term_create_instance(info, instance);
}

static XRAPI_ATTR XrResult XRAPI_CALL term_destroy_instance(XrInstance instance) {
	xrl_log_clear_messengers();
	PFN_xrDestroyInstance destroy = NULL;
	g.rt_gipa(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&destroy);
	if (destroy) destroy(instance);
	return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_create_messenger(XrInstance instance, const XrDebugUtilsMessengerCreateInfoEXT* info, XrDebugUtilsMessengerEXT* messenger) {
	if (messenger == NULL) {
		xrl_log_vuid("VUID-xrCreateDebugUtilsMessengerEXT-messenger-parameter", "xrCreateDebugUtilsMessengerEXT", "invalid messenger pointer");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	XrResult result = XR_SUCCESS;
	if (g.rt_create_messenger) result     = g.rt_create_messenger(instance, info, messenger);
	else                       *messenger = (XrDebugUtilsMessengerEXT)(uintptr_t)++g.fake_messengers;
	if (XR_SUCCEEDED(result)) xrl_log_add_messenger(*messenger, info);
	return result;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_destroy_messenger(XrDebugUtilsMessengerEXT messenger) {
	xrl_log_remove_messenger(messenger);
	return g.rt_destroy_messenger ? g.rt_destroy_messenger(messenger) : XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_submit_message(XrInstance instance, XrDebugUtilsMessageSeverityFlagsEXT sev, XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT* data) {
	// A runtime with the extension delivers to every messenger itself, so only deliver when it can't
	if (g.rt_submit_message) return g.rt_submit_message(instance, sev, types, data);
	xrl_log_submit(sev, types, data);
	return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_set_object_name(XrInstance instance, const XrDebugUtilsObjectNameInfoEXT* info) {
	return g.rt_set_object_name ? g.rt_set_object_name(instance, info) : XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL term_get_instance_proc_addr(XrInstance instance, const char* name, PFN_xrVoidFunction* function);

static const struct { const char* name; PFN_xrVoidFunction fn; } g_terminators[] = {
	{ "xrGetInstanceProcAddr",           (PFN_xrVoidFunction)term_get_instance_proc_addr },
	{ "xrCreateInstance",                (PFN_xrVoidFunction)term_create_instance },
	{ "xrCreateApiLayerInstance",        (PFN_xrVoidFunction)term_create_api_layer_instance },
	{ "xrDestroyInstance",               (PFN_xrVoidFunction)term_destroy_instance },
	{ "xrCreateDebugUtilsMessengerEXT",  (PFN_xrVoidFunction)term_create_messenger },
	{ "xrDestroyDebugUtilsMessengerEXT", (PFN_xrVoidFunction)term_destroy_messenger },
	{ "xrSubmitDebugUtilsMessageEXT",    (PFN_xrVoidFunction)term_submit_message },
	{ "xrSetDebugUtilsObjectNameEXT",    (PFN_xrVoidFunction)term_set_object_name },
};

static XRAPI_ATTR XrResult XRAPI_CALL term_get_instance_proc_addr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
	for (size_t i = 0; i < sizeof(g_terminators) / sizeof(g_terminators[0]); i++) {
		if (strcmp(name, g_terminators[i].name) == 0) {
			*function = g_terminators[i].fn;
			return XR_SUCCESS;
		}
	}
	*function = NULL;
	return g.rt_gipa(instance, name, function);
}

///////////////////////////////////////////
// Instance
///////////////////////////////////////////

XrResult xrl_dispatch_missing(xrl_fn_ fn) {
	const char* name = xrl_fn_names;
	for (int32_t i = 0; i < (int32_t)fn; i++) name += strlen(name) + 1;

	if (g.instance == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, name, "No active XrInstance handle.");
		return XR_ERROR_HANDLE_INVALID;
	}
	xrl_log(xrl_sev_error, name, "The active runtime and API layers don't provide this function.");
	return XR_ERROR_FUNCTION_UNSUPPORTED;
}

static void dispatch_fill(PFN_xrGetInstanceProcAddr gipa, XrInstance instance) {
	const char* name = xrl_fn_names;
	for (int32_t i = 0; i < xrl_fn_count; i++) {
		xrl_dispatch[i] = NULL;
		if (XR_FAILED(gipa(instance, name, &xrl_dispatch[i]))) xrl_dispatch[i] = NULL;
		name += strlen(name) + 1;
	}
}

///////////////////////////////////////////

static XrResult instance_chain_create(const XrInstanceCreateInfo* info, const xrl_manifests_t* layer_list, const loader_layer_t* layers, int32_t layer_count, XrInstance* out_instance, PFN_xrGetInstanceProcAddr* out_top_gipa) {
	if (info->enabledExtensionCount > 0 && info->enabledExtensionNames == NULL) {
		xrl_log_vuid("VUID-XrInstanceCreateInfo-enabledExtensionNames-parameter", "xrCreateInstance", "enabledExtensionCount is non-0 but array is NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	uint32_t               rt_ext_count;
	XrExtensionProperties* rt_exts = runtime_exts(&rt_ext_count);
	const char**           names   = (const char**)malloc(((size_t)info->enabledExtensionCount + 1) * sizeof(const char*));
	uint32_t               count   = 0;
	XrResult               result  = names ? XR_SUCCESS : XR_ERROR_OUT_OF_MEMORY;

	// Every extension needs a provider. The loader's own XR_EXT_debug_utils is hidden from a runtime that lacks it.
	for (uint32_t e = 0; XR_SUCCEEDED(result) && e < info->enabledExtensionCount; e++) {
		const char* ext       = info->enabledExtensionNames[e];
		bool        in_rt     = exts_contain(rt_exts, rt_ext_count, ext);
		bool        is_loader = strcmp(ext, XRL_DEBUG_UTILS) == 0;
		bool        in_layer  = false;
		for (int32_t l = 0; l < layer_count && !in_layer; l++) in_layer = xrl_manifest_has_ext(layer_list, &layer_list->items[layers[l].manifest], ext);
		if (!in_rt && !is_loader && !in_layer) {
			xrl_log(xrl_sev_error, "xrCreateInstance", "No support found for requested extension: %s", ext);
			result = XR_ERROR_EXTENSION_NOT_PRESENT;
		}
		if (in_rt || !is_loader) names[count++] = ext;
	}
	free(rt_exts);

	PFN_xrGetInstanceProcAddr top_gipa = term_get_instance_proc_addr;
	if (XR_SUCCEEDED(result)) {
		XrInstanceCreateInfo modified = *info;
		modified.enabledExtensionCount = count;
		modified.enabledExtensionNames = count ? names : NULL;

		if (layer_count == 0) {
			result = term_create_instance(&modified, out_instance);
		} else {
			// Link the chain from the runtime upward, so each layer's next info points toward the runtime
			XrApiLayerNextInfo* next_infos = (XrApiLayerNextInfo*)calloc((size_t)layer_count, sizeof(XrApiLayerNextInfo));
			PFN_xrCreateApiLayerInstance top_create = term_create_api_layer_instance;
			XrApiLayerNextInfo*          top_next   = NULL;
			for (int32_t i = layer_count - 1; next_infos && i >= 0; i--) {
				next_infos[i] = (XrApiLayerNextInfo){
					.structType                 = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO,
					.structVersion              = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION,
					.structSize                 = sizeof(XrApiLayerNextInfo),
					.nextGetInstanceProcAddr    = top_gipa,
					.nextCreateApiLayerInstance = top_create,
					.next                       = top_next,
				};
				xrl_fmt(next_infos[i].layerName, sizeof(next_infos[i].layerName), "%s", layers[i].name);
				top_next   = &next_infos[i];
				top_gipa   = layers[i].gipa;
				top_create = layers[i].create;
			}
			XrApiLayerCreateInfo layer_info = {
				.structType    = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO,
				.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION,
				.structSize    = sizeof(XrApiLayerCreateInfo),
				.nextInfo      = next_infos,
			};
			result = next_infos ? top_create(&modified, &layer_info, out_instance) : XR_ERROR_OUT_OF_MEMORY;
			free(next_infos);
		}
		if (XR_FAILED(result)) xrl_log(xrl_sev_error, "xrCreateInstance", "The chained xrCreateInstance call failed");
	}
	free(names);
	*out_top_gipa = top_gipa;
	return result;
}

///////////////////////////////////////////

static void instance_release(void) {
	memset(xrl_dispatch, 0, sizeof(xrl_dispatch));
	xrl_log_clear_messengers();
	layers_unload(g.layers, g.layer_count);
	g.layers           = NULL;
	g.layer_count      = 0;
	g.instance         = XR_NULL_HANDLE;
	g.top_gipa         = NULL;
	g.debug_utils      = false;
	g.create_messenger = XR_NULL_HANDLE;
	runtime_unload();
}

///////////////////////////////////////////
// Loader implemented XR_EXT_debug_utils functions
///////////////////////////////////////////

static XRAPI_ATTR XrResult XRAPI_CALL loader_create_messenger(XrInstance instance, const XrDebugUtilsMessengerCreateInfoEXT* info, XrDebugUtilsMessengerEXT* messenger) {
	if (instance == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, "xrCreateDebugUtilsMessengerEXT", "Instance handle is XR_NULL_HANDLE.");
		return XR_ERROR_HANDLE_INVALID;
	}
	PFN_xrCreateDebugUtilsMessengerEXT fn = (PFN_xrCreateDebugUtilsMessengerEXT)xrl_dispatch[xrl_fn_xrCreateDebugUtilsMessengerEXT];
	return fn ? fn(instance, info, messenger) : xrl_dispatch_missing(xrl_fn_xrCreateDebugUtilsMessengerEXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_destroy_messenger(XrDebugUtilsMessengerEXT messenger) {
	if (messenger == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, "xrDestroyDebugUtilsMessengerEXT", "Messenger handle is XR_NULL_HANDLE.");
		return XR_ERROR_HANDLE_INVALID;
	}
	PFN_xrDestroyDebugUtilsMessengerEXT fn = (PFN_xrDestroyDebugUtilsMessengerEXT)xrl_dispatch[xrl_fn_xrDestroyDebugUtilsMessengerEXT];
	return fn ? fn(messenger) : xrl_dispatch_missing(xrl_fn_xrDestroyDebugUtilsMessengerEXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_begin_label(XrSession session, const XrDebugUtilsLabelEXT* label) {
	if (session == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, "xrSessionBeginDebugUtilsLabelRegionEXT", "Session handle is XR_NULL_HANDLE.");
		return XR_ERROR_HANDLE_INVALID;
	}
	if (label == NULL) {
		xrl_log_vuid("VUID-xrSessionBeginDebugUtilsLabelRegionEXT-labelInfo-parameter", "xrSessionBeginDebugUtilsLabelRegionEXT", "labelInfo must be non-NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	if (g.instance == XR_NULL_HANDLE) return xrl_dispatch_missing(xrl_fn_xrSessionBeginDebugUtilsLabelRegionEXT);
	PFN_xrSessionBeginDebugUtilsLabelRegionEXT fn = (PFN_xrSessionBeginDebugUtilsLabelRegionEXT)xrl_dispatch[xrl_fn_xrSessionBeginDebugUtilsLabelRegionEXT];
	return fn ? fn(session, label) : XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_end_label(XrSession session) {
	if (session == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, "xrSessionEndDebugUtilsLabelRegionEXT", "Session handle is XR_NULL_HANDLE.");
		return XR_ERROR_HANDLE_INVALID;
	}
	if (g.instance == XR_NULL_HANDLE) return xrl_dispatch_missing(xrl_fn_xrSessionEndDebugUtilsLabelRegionEXT);
	PFN_xrSessionEndDebugUtilsLabelRegionEXT fn = (PFN_xrSessionEndDebugUtilsLabelRegionEXT)xrl_dispatch[xrl_fn_xrSessionEndDebugUtilsLabelRegionEXT];
	return fn ? fn(session) : XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_insert_label(XrSession session, const XrDebugUtilsLabelEXT* label) {
	if (session == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, "xrSessionInsertDebugUtilsLabelEXT", "Session handle is XR_NULL_HANDLE.");
		return XR_ERROR_HANDLE_INVALID;
	}
	if (g.instance == XR_NULL_HANDLE) return xrl_dispatch_missing(xrl_fn_xrSessionInsertDebugUtilsLabelEXT);
	if (label == NULL) {
		xrl_log_vuid("VUID-xrSessionInsertDebugUtilsLabelEXT-labelInfo-parameter", "xrSessionInsertDebugUtilsLabelEXT", "labelInfo must be non-NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	PFN_xrSessionInsertDebugUtilsLabelEXT fn = (PFN_xrSessionInsertDebugUtilsLabelEXT)xrl_dispatch[xrl_fn_xrSessionInsertDebugUtilsLabelEXT];
	return fn ? fn(session, label) : XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_set_object_name(XrInstance instance, const XrDebugUtilsObjectNameInfoEXT* info) {
	PFN_xrSetDebugUtilsObjectNameEXT fn = (PFN_xrSetDebugUtilsObjectNameEXT)xrl_dispatch[xrl_fn_xrSetDebugUtilsObjectNameEXT];
	return fn ? fn(instance, info) : xrl_dispatch_missing(xrl_fn_xrSetDebugUtilsObjectNameEXT);
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_submit_message(XrInstance instance, XrDebugUtilsMessageSeverityFlagsEXT sev, XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT* data) {
	PFN_xrSubmitDebugUtilsMessageEXT fn = (PFN_xrSubmitDebugUtilsMessageEXT)xrl_dispatch[xrl_fn_xrSubmitDebugUtilsMessageEXT];
	return fn ? fn(instance, sev, types, data) : xrl_dispatch_missing(xrl_fn_xrSubmitDebugUtilsMessageEXT);
}

static const struct { const char* name; PFN_xrVoidFunction fn; } g_debug_utils_fns[] = {
	{ "xrCreateDebugUtilsMessengerEXT",         (PFN_xrVoidFunction)loader_create_messenger },
	{ "xrDestroyDebugUtilsMessengerEXT",        (PFN_xrVoidFunction)loader_destroy_messenger },
	{ "xrSessionBeginDebugUtilsLabelRegionEXT", (PFN_xrVoidFunction)loader_begin_label },
	{ "xrSessionEndDebugUtilsLabelRegionEXT",   (PFN_xrVoidFunction)loader_end_label },
	{ "xrSessionInsertDebugUtilsLabelEXT",      (PFN_xrVoidFunction)loader_insert_label },
	{ "xrSetDebugUtilsObjectNameEXT",           (PFN_xrVoidFunction)loader_set_object_name },
	{ "xrSubmitDebugUtilsMessageEXT",           (PFN_xrVoidFunction)loader_submit_message },
};

///////////////////////////////////////////
// Core functions the loader implements itself. Exported names forward to
// these, so an application that defines the same symbols can't redirect them.
///////////////////////////////////////////

static XRAPI_ATTR XrResult XRAPI_CALL loader_initialize(const XrLoaderInitInfoBaseHeaderKHR* info) {
	xrl_lock();
	if (g.instance != XR_NULL_HANDLE) {
		xrl_unlock();
		xrl_log(xrl_sev_error, "xrInitializeLoaderKHR", "An active instance currently exists while trying to reinitialize the loader");
		return XR_ERROR_INITIALIZATION_FAILED;
	}
	runtime_unload();
	XrResult result = props_init(info);
#if defined(__ANDROID__)
	if (XR_SUCCEEDED(result)) result = xrl_android_init(info);
#endif
	xrl_unlock();
	return result;
}

///////////////////////////////////////////

static XRAPI_ATTR XrResult XRAPI_CALL loader_enumerate_layers(uint32_t capacity, uint32_t* count, XrApiLayerProperties* props) {
	const char* cmd = "xrEnumerateApiLayerProperties";
	for (uint32_t i = 0; props && i < capacity; i++) {
		if (props[i].type != XR_TYPE_API_LAYER_PROPERTIES) {
			xrl_log_vuid("VUID-XrApiLayerProperties-type-type", cmd, "unknown type in api_layer_properties");
			return XR_ERROR_VALIDATION_FAILURE;
		}
	}
	if (count == NULL) {
		xrl_log_vuid("VUID-xrEnumerateApiLayerProperties-propertyCountOutput-parameter", cmd, "null propertyCountOutput");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	xrl_lock();
	xrl_manifests_t list = {0};
	xrl_manifests_find(&list, xrl_manifest_layer_implicit, cmd);
	xrl_manifests_find(&list, xrl_manifest_layer_explicit, cmd);
	xrl_unlock();

	XrResult result = XR_SUCCESS;
	*count = (uint32_t)list.count;
	if (capacity > 0 && props == NULL) {
		xrl_log_vuid("VUID-xrEnumerateApiLayerProperties-properties-parameter", cmd, "non-zero capacity but null array");
		result = XR_ERROR_VALIDATION_FAILURE;
	} else if (capacity > 0 && capacity < (uint32_t)list.count) {
		xrl_log_vuid("VUID-xrEnumerateApiLayerProperties-propertyCapacityInput-parameter", cmd, "insufficient space in array");
		result = XR_ERROR_SIZE_INSUFFICIENT;
	} else {
		for (uint32_t i = 0; i < capacity && i < (uint32_t)list.count; i++) {
			const xrl_manifest_t* m = &list.items[i];
			props[i].layerVersion = m->impl_version;
			props[i].specVersion  = m->api_version;
			xrl_fmt(props[i].layerName,   sizeof(props[i].layerName),   "%s", m->name);
			xrl_fmt(props[i].description, sizeof(props[i].description), "%s", m->description);
		}
	}
	xrl_manifests_free(&list);
	return result;
}

///////////////////////////////////////////

// Layer extensions come first. Runtime versions win over layers, and the loader's own only raise versions.
static XrResult collect_instance_exts(const char* layer_name, XrExtensionProperties** ref_props, uint32_t* ref_count) {
	const char*     cmd    = "xrEnumerateInstanceExtensionProperties";
	xrl_manifests_t list   = {0};
	XrResult        result = XR_SUCCESS;

	xrl_manifests_find(&list, xrl_manifest_layer_implicit, cmd);
	int32_t implicit_count = list.count;
	if (layer_name != NULL) {
		xrl_manifests_find(&list, xrl_manifest_layer_explicit, cmd);
		int32_t found = -1;
		for (int32_t i = 0; i < list.count && found < 0; i++) {
			if (strcmp(list.items[i].name, layer_name) == 0) found = i;
		}
		if (found < 0) result = XR_ERROR_API_LAYER_NOT_PRESENT;
		else           xrl_manifest_append_exts(&list, &list.items[found], ref_props, ref_count);
		xrl_manifests_free(&list);
		return result;
	}

	// Explicit layers enabled through the environment are as good as implicit here
	char env[XRL_PATH_MAX];
	for (int32_t i = 0; i < implicit_count; i++) xrl_manifest_append_exts(&list, &list.items[i], ref_props, ref_count);
	if (xrl_prop_get("XR_ENABLE_API_LAYERS", false, env, sizeof(env))) {
		xrl_manifests_find(&list, xrl_manifest_layer_explicit, cmd);
		for (int32_t i = implicit_count; i < list.count; i++) {
			const char* name = list.items[i].name;
			size_t      len  = strlen(name);
			for (const char* p = env; (p = strstr(p, name)) != NULL; p += len) {
				bool starts = p == env || p[-1] == XRL_PATH_SEP;
				bool ends   = p[len] == '\0' || p[len] == XRL_PATH_SEP;
				if (starts && ends) { xrl_manifest_append_exts(&list, &list.items[i], ref_props, ref_count); break; }
			}
		}
	}
	xrl_manifests_free(&list);

	result = runtime_load(cmd);
	if (XR_FAILED(result)) {
		xrl_log(xrl_sev_error, cmd, "Failed to find the default runtime");
		return result;
	}
	uint32_t               rt_count;
	XrExtensionProperties* rt_exts = runtime_exts(&rt_count);
	for (uint32_t i = 0; i < rt_count; i++) xrl_ext_merge(ref_props, ref_count, rt_exts[i].extensionName, rt_exts[i].extensionVersion, true);
	free(rt_exts);

	xrl_ext_merge(ref_props, ref_count, XRL_DEBUG_UTILS, XR_EXT_debug_utils_SPEC_VERSION, false);
	return XR_SUCCESS;
}

static XRAPI_ATTR XrResult XRAPI_CALL loader_enumerate_exts(const char* layer_name, uint32_t capacity, uint32_t* count, XrExtensionProperties* props) {
	const char* cmd = "xrEnumerateInstanceExtensionProperties";
	if (count == NULL) return XR_ERROR_VALIDATION_FAILURE;
	if (layer_name != NULL && layer_name[0] == '\0') layer_name = NULL;

	XrExtensionProperties* exts      = NULL;
	uint32_t               ext_count = 0;
	xrl_lock();
	XrResult result = collect_instance_exts(layer_name, &exts, &ext_count);
	xrl_unlock();
	if (XR_FAILED(result)) {
		xrl_log(xrl_sev_error, cmd, "Failed querying extension properties");
		free(exts);
		return result;
	}

	*count = ext_count;
	if (capacity == 0) {
		result = XR_SUCCESS;
	} else if (props == NULL) {
		result = XR_ERROR_VALIDATION_FAILURE;
	} else if (capacity < ext_count) {
		xrl_log_vuid("VUID-xrEnumerateInstanceExtensionProperties-propertyCountOutput-parameter", cmd, "insufficient space in array");
		result = XR_ERROR_SIZE_INSUFFICIENT;
	} else {
		for (uint32_t i = 0; i < ext_count && XR_SUCCEEDED(result); i++) {
			if (props[i].type != XR_TYPE_EXTENSION_PROPERTIES) {
				xrl_log_vuid("VUID-XrExtensionProperties-type-type", cmd, "unknown type in properties");
				result = XR_ERROR_VALIDATION_FAILURE;
				break;
			}
			void* next = props[i].next;
			props[i]      = exts[i];
			props[i].next = next;
		}
	}
	free(exts);
	return result;
}

///////////////////////////////////////////

static XRAPI_ATTR XrResult XRAPI_CALL loader_destroy_instance(XrInstance instance);

static XRAPI_ATTR XrResult XRAPI_CALL loader_create_instance(const XrInstanceCreateInfo* info, XrInstance* instance) {
	const char* cmd = "xrCreateInstance";
	if (info == NULL) {
		xrl_log_vuid("VUID-xrCreateInstance-info-parameter", cmd, "must be non-NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	uint32_t app_major = XR_VERSION_MAJOR(info->applicationInfo.apiVersion);
	uint32_t app_minor = XR_VERSION_MINOR(info->applicationInfo.apiVersion);
	uint32_t max_major = XR_VERSION_MAJOR(XR_CURRENT_API_VERSION);
	uint32_t max_minor = XR_VERSION_MINOR(XR_CURRENT_API_VERSION);
	if (app_major > max_major || (app_major == max_major && app_minor > max_minor)) {
		xrl_log(xrl_sev_error, cmd, "xrCreateInstance called with invalid API version %u.%u. Max supported version is %u.%u", app_major, app_minor, max_major, max_minor);
		return XR_ERROR_API_VERSION_UNSUPPORTED;
	}
	if (instance == NULL) {
		xrl_log_vuid("VUID-xrCreateInstance-instance-parameter", cmd, "must be non-NULL");
		return XR_ERROR_VALIDATION_FAILURE;
	}

	xrl_lock();
	// One instance at a time, since the loader can't map handles it doesn't know about back to an instance
	if (g.instance != XR_NULL_HANDLE) {
		xrl_unlock();
		xrl_log(xrl_sev_error, cmd, "Loader does not support simultaneous XrInstances");
		return XR_ERROR_LIMIT_REACHED;
	}

	xrl_manifests_t           layer_list  = {0};
	loader_layer_t*           layers      = NULL;
	int32_t                   layer_count = 0;
	XrInstance                handle      = XR_NULL_HANDLE;
	PFN_xrGetInstanceProcAddr top_gipa    = NULL;

	XrResult result = runtime_load(cmd);
	if (XR_SUCCEEDED(result)) result = layers_load(cmd, info->enabledApiLayerCount, info->enabledApiLayerNames, &layer_list, &layers, &layer_count);
	else                      xrl_log(xrl_sev_error, cmd, "Failed loading runtime information");
	if (XR_SUCCEEDED(result)) result = instance_chain_create(info, &layer_list, layers, layer_count, &handle, &top_gipa);
	xrl_manifests_free(&layer_list);

	if (XR_FAILED(result)) {
		layers_unload(layers, layer_count);
		runtime_unload();
		xrl_unlock();
		xrl_log(xrl_sev_error, cmd, "xrCreateInstance failed");
		return result;
	}

	g.instance    = handle;
	g.top_gipa    = top_gipa;
	g.layers      = layers;
	g.layer_count = layer_count;
	for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
		if (strcmp(info->enabledExtensionNames[i], XRL_DEBUG_UTILS) == 0) g.debug_utils = true;
	}
	dispatch_fill(top_gipa, handle);

	// A messenger in the next chain covers the rest of the instance's life
	for (const XrBaseInStructure* next = (const XrBaseInStructure*)info->next; next != NULL; next = next->next) {
		if (next->type != XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) continue;
		if (XR_FAILED(loader_create_messenger(handle, (const XrDebugUtilsMessengerCreateInfoEXT*)next, &g.create_messenger))) {
			xrl_unlock();
			loader_destroy_instance(handle);
			return XR_ERROR_VALIDATION_FAILURE;
		}
		break;
	}
	xrl_unlock();

	*instance = handle;
	return XR_SUCCESS;
}

///////////////////////////////////////////

static XRAPI_ATTR XrResult XRAPI_CALL loader_destroy_instance(XrInstance instance) {
	const char* cmd = "xrDestroyInstance";
	if (instance == XR_NULL_HANDLE) {
		xrl_log(xrl_sev_error, cmd, "Instance handle is XR_NULL_HANDLE.");
		return XR_ERROR_HANDLE_INVALID;
	}

	xrl_lock();
	if (g.instance == XR_NULL_HANDLE) {
		xrl_unlock();
		xrl_log(xrl_sev_error, cmd, "No active XrInstance handle.");
		return XR_ERROR_HANDLE_INVALID;
	}
	if (g.create_messenger != XR_NULL_HANDLE) loader_destroy_messenger(g.create_messenger);

	PFN_xrDestroyInstance destroy = NULL;
	g.top_gipa(instance, "xrDestroyInstance", (PFN_xrVoidFunction*)&destroy);
	if (destroy == NULL || XR_FAILED(destroy(instance))) xrl_log(xrl_sev_error, cmd, "Unknown error occurred calling down chain");

	instance_release();
	xrl_unlock();
	return XR_SUCCESS;
}

///////////////////////////////////////////

static XRAPI_ATTR XrResult XRAPI_CALL loader_get_instance_proc_addr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
	const char* cmd = "xrGetInstanceProcAddr";
	if (function == NULL) {
		xrl_log_vuid("VUID-xrGetInstanceProcAddr-function-parameter", cmd, "Invalid Function pointer");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	if (name == NULL) {
		xrl_log_vuid("VUID-xrGetInstanceProcAddr-function-parameter", cmd, "Invalid Name pointer");
		return XR_ERROR_VALIDATION_FAILURE;
	}
	*function = NULL;

	if (instance != XR_NULL_HANDLE) {
		if (g.instance == XR_NULL_HANDLE) {
			xrl_log(xrl_sev_error, cmd, "No active XrInstance handle.");
			return XR_ERROR_HANDLE_INVALID;
		}
		if (g.instance != instance) return XR_ERROR_HANDLE_INVALID;
	}

	static const struct { const char* name; PFN_xrVoidFunction fn; bool needs_instance; } loader_fns[] = {
		{ "xrInitializeLoaderKHR",                  (PFN_xrVoidFunction)loader_initialize,             false },
		{ "xrEnumerateApiLayerProperties",          (PFN_xrVoidFunction)loader_enumerate_layers,       false },
		{ "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction)loader_enumerate_exts,         false },
		{ "xrCreateInstance",                       (PFN_xrVoidFunction)loader_create_instance,        false },
		{ "xrGetInstanceProcAddr",                  (PFN_xrVoidFunction)loader_get_instance_proc_addr, true  },
		{ "xrDestroyInstance",                      (PFN_xrVoidFunction)loader_destroy_instance,       true  },
	};
	for (size_t i = 0; i < sizeof(loader_fns) / sizeof(loader_fns[0]); i++) {
		if (strcmp(name, loader_fns[i].name) != 0) continue;
		if (loader_fns[i].needs_instance && instance == XR_NULL_HANDLE) break;
		*function = loader_fns[i].fn;
		return XR_SUCCESS;
	}

	if (instance == XR_NULL_HANDLE) {
		xrl_log_vuid("VUID-xrGetInstanceProcAddr-instance-parameter", cmd, "XR_NULL_HANDLE for instance but query for %s requires a valid instance", name);
		return XR_ERROR_HANDLE_INVALID;
	}

	// XR_EXT_debug_utils is partly implemented by the loader, so it hands out its own versions when enabled
	for (size_t i = 0; i < sizeof(g_debug_utils_fns) / sizeof(g_debug_utils_fns[0]); i++) {
		if (strcmp(name, g_debug_utils_fns[i].name) != 0) continue;
		if (!g.debug_utils) return XR_ERROR_FUNCTION_UNSUPPORTED;
		*function = g_debug_utils_fns[i].fn;
		return XR_SUCCESS;
	}
	return g.top_gipa(instance, name, function);
}

///////////////////////////////////////////
// Exports
///////////////////////////////////////////

XRL_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
	return loader_get_instance_proc_addr(instance, name, function);
}

XRL_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput, uint32_t* propertyCountOutput, XrApiLayerProperties* properties) {
	return loader_enumerate_layers(propertyCapacityInput, propertyCountOutput, properties);
}

XRL_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char* layerName, uint32_t propertyCapacityInput, uint32_t* propertyCountOutput, XrExtensionProperties* properties) {
	return loader_enumerate_exts(layerName, propertyCapacityInput, propertyCountOutput, properties);
}

XRL_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrCreateInstance(const XrInstanceCreateInfo* createInfo, XrInstance* instance) {
	return loader_create_instance(createInfo, instance);
}

XRL_EXPORT XRAPI_ATTR XrResult XRAPI_CALL xrDestroyInstance(XrInstance instance) {
	return loader_destroy_instance(instance);
}
