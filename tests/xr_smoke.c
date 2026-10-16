// SPDX-License-Identifier: Apache-2.0 OR MIT
// End to end smoke test against a real runtime: instance, debug messenger,
// a headless session (XR_MND_headless), actions, spaces, and a frame loop.
// Exits non-zero on the first failure.

#include <openxr/openxr.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(call) do { XrResult r_ = (call); if (XR_FAILED(r_)) { printf("FAIL %s -> %d (line %d)\n", #call, (int)r_, __LINE__); exit(1); } } while (0)

static int32_t g_messages;

static XrBool32 XRAPI_CALL on_message(XrDebugUtilsMessageSeverityFlagsEXT sev, XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
	(void)sev; (void)types; (void)user_data;
	g_messages++;
	printf("  messenger: [%s] %s\n", data->functionName ? data->functionName : "", data->message ? data->message : "");
	return XR_FALSE;
}

static bool has_ext(const XrExtensionProperties* props, uint32_t count, const char* name) {
	for (uint32_t i = 0; i < count; i++) if (strcmp(props[i].extensionName, name) == 0) return true;
	return false;
}

int main(void) {
	uint32_t ext_count = 0;
	CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL));
	XrExtensionProperties* exts = calloc(ext_count, sizeof(XrExtensionProperties));
	for (uint32_t i = 0; i < ext_count; i++) exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	CHECK(xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, exts));
	printf("%u instance extensions\n", ext_count);
	if (!has_ext(exts, ext_count, XR_MND_HEADLESS_EXTENSION_NAME) || !has_ext(exts, ext_count, XR_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
		printf("FAIL runtime lacks XR_MND_headless or XR_EXT_debug_utils\n");
		return 1;
	}
	free(exts);

	uint32_t layer_count = 0;
	CHECK(xrEnumerateApiLayerProperties(0, &layer_count, NULL));
	printf("%u API layers\n", layer_count);

	XrDebugUtilsMessengerCreateInfoEXT messenger_info = {
		.type              = XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
		.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
		.messageTypes      = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
		.userCallback      = on_message,
	};
	const char* enabled[] = { XR_MND_HEADLESS_EXTENSION_NAME, XR_EXT_DEBUG_UTILS_EXTENSION_NAME };
	XrInstanceCreateInfo create_info = {
		.type                  = XR_TYPE_INSTANCE_CREATE_INFO,
		.next                  = &messenger_info,
		.applicationInfo       = { .applicationName = "xrl_smoke", .apiVersion = XR_API_VERSION_1_0 },
		.enabledExtensionCount = 2,
		.enabledExtensionNames = enabled,
	};
	XrInstance instance = XR_NULL_HANDLE;
	CHECK(xrCreateInstance(&create_info, &instance));

	// A second instance must be refused
	XrInstance second = XR_NULL_HANDLE;
	if (xrCreateInstance(&create_info, &second) != XR_ERROR_LIMIT_REACHED) { printf("FAIL second instance wasn't refused\n"); return 1; }

	XrInstanceProperties inst_props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
	CHECK(xrGetInstanceProperties(instance, &inst_props));
	printf("runtime: %s\n", inst_props.runtimeName);

	PFN_xrSubmitDebugUtilsMessageEXT submit = NULL;
	CHECK(xrGetInstanceProcAddr(instance, "xrSubmitDebugUtilsMessageEXT", (PFN_xrVoidFunction*)&submit));
	XrDebugUtilsMessengerCallbackDataEXT msg = { .type = XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT, .messageId = "smoke", .functionName = "main", .message = "hello from the app" };
	CHECK(submit(instance, XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &msg));

	XrSystemId     system      = XR_NULL_SYSTEM_ID;
	XrSystemGetInfo system_info = { .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY };
	CHECK(xrGetSystem(instance, &system_info, &system));

	XrSessionCreateInfo session_info = { .type = XR_TYPE_SESSION_CREATE_INFO, .systemId = system };
	XrSession           session      = XR_NULL_HANDLE;
	CHECK(xrCreateSession(instance, &session_info, &session));

	XrActionSetCreateInfo set_info = { .type = XR_TYPE_ACTION_SET_CREATE_INFO, .actionSetName = "smoke", .localizedActionSetName = "Smoke" };
	XrActionSet           set      = XR_NULL_HANDLE;
	CHECK(xrCreateActionSet(instance, &set_info, &set));
	XrActionCreateInfo action_info = { .type = XR_TYPE_ACTION_CREATE_INFO, .actionType = XR_ACTION_TYPE_BOOLEAN_INPUT, .actionName = "select", .localizedActionName = "Select" };
	XrAction           action      = XR_NULL_HANDLE;
	CHECK(xrCreateAction(set, &action_info, &action));
	XrSessionActionSetsAttachInfo attach = { .type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, .countActionSets = 1, .actionSets = &set };
	CHECK(xrAttachSessionActionSets(session, &attach));

	XrReferenceSpaceCreateInfo space_info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = { .orientation = { .w = 1 } } };
	XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
	CHECK(xrCreateReferenceSpace(session, &space_info, &local));
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	CHECK(xrCreateReferenceSpace(session, &space_info, &view));

	// Pump events until the session can begin, then run some frames
	bool    running = false;
	int32_t frames  = 0;
	for (int32_t iter = 0; iter < 2000 && frames < 60; iter++) {
		XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(instance, &event) == XR_SUCCESS) {
			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrSessionState state = ((XrEventDataSessionStateChanged*)&event)->state;
				if (state == XR_SESSION_STATE_READY) {
					XrSessionBeginInfo begin = { .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO };
					CHECK(xrBeginSession(session, &begin));
					running = true;
				}
			}
			event = (XrEventDataBuffer){ .type = XR_TYPE_EVENT_DATA_BUFFER };
		}
		if (!running) continue;

		// Headless sessions pace with xrWaitFrame, but have no frames to begin or end
		XrFrameState frame = { .type = XR_TYPE_FRAME_STATE };
		CHECK(xrWaitFrame(session, &(XrFrameWaitInfo){ .type = XR_TYPE_FRAME_WAIT_INFO }, &frame));

		XrActiveActionSet active = { .actionSet = set };
		CHECK(xrSyncActions(session, &(XrActionsSyncInfo){ .type = XR_TYPE_ACTIONS_SYNC_INFO, .countActiveActionSets = 1, .activeActionSets = &active }));
		XrActionStateBoolean state = { .type = XR_TYPE_ACTION_STATE_BOOLEAN };
		CHECK(xrGetActionStateBoolean(session, &(XrActionStateGetInfo){ .type = XR_TYPE_ACTION_STATE_GET_INFO, .action = action }, &state));
		XrSpaceLocation loc = { .type = XR_TYPE_SPACE_LOCATION };
		if (frame.predictedDisplayTime > 0) CHECK(xrLocateSpace(view, local, frame.predictedDisplayTime, &loc));
		frames++;
	}
	printf("%d frames\n", frames);
	if (frames == 0) { printf("FAIL session never ran\n"); return 1; }

	CHECK(xrRequestExitSession(session));
	CHECK(xrDestroySpace(view));
	CHECK(xrDestroySpace(local));
	CHECK(xrDestroyAction(action));
	CHECK(xrDestroyActionSet(set));
	CHECK(xrDestroySession(session));
	CHECK(xrDestroyInstance(instance));

	// With no instance, trampolines must fail cleanly rather than crash
	XrInstanceProperties after = { .type = XR_TYPE_INSTANCE_PROPERTIES };
	if (xrGetInstanceProperties(instance, &after) != XR_ERROR_HANDLE_INVALID) { printf("FAIL call after destroy didn't return XR_ERROR_HANDLE_INVALID\n"); return 1; }

	// And a fresh instance must work again
	create_info.next = NULL;
	CHECK(xrCreateInstance(&create_info, &instance));
	CHECK(xrDestroyInstance(instance));

	printf("%d messages\nPASS\n", g_messages);
	return g_messages >= 1 ? 0 : 1;
}
