// SPDX-License-Identifier: Apache-2.0 OR MIT
// On-device loader test for Android headsets: loader init, runtime broker
// discovery, an instance, and a GLES session running a short frame loop.
// Logs "XRL_TEST PASS" or "XRL_TEST FAIL" to logcat under the xrl_test tag.

#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <jni.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "xrl_test", __VA_ARGS__)
#define CHECK(call) do { XrResult r_ = (call); if (XR_FAILED(r_)) { LOG("FAIL %s -> %d (line %d)", #call, (int)r_, __LINE__); return false; } } while (0)

typedef struct gl_ctx_t {
	EGLDisplay display;
	EGLConfig  config;
	EGLContext context;
	EGLSurface surface;
} gl_ctx_t;

static void pump_android(struct android_app* app) {
	int32_t                     events;
	struct android_poll_source* source;
	while (ALooper_pollOnce(0, NULL, &events, (void**)&source) >= 0) {
		if (source) source->process(app, source);
	}
}

static bool gl_init(gl_ctx_t* out_gl) {
	out_gl->display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (!eglInitialize(out_gl->display, NULL, NULL)) return false;
	const EGLint attribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
	EGLint count = 0;
	if (!eglChooseConfig(out_gl->display, attribs, &out_gl->config, 1, &count) || count == 0) return false;
	const EGLint ctx_attribs[]  = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	const EGLint surf_attribs[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	out_gl->context = eglCreateContext(out_gl->display, out_gl->config, EGL_NO_CONTEXT, ctx_attribs);
	out_gl->surface = eglCreatePbufferSurface(out_gl->display, out_gl->config, surf_attribs);
	return out_gl->context != EGL_NO_CONTEXT && out_gl->surface != EGL_NO_SURFACE && eglMakeCurrent(out_gl->display, out_gl->surface, out_gl->surface, out_gl->context);
}

static bool has_ext(const XrExtensionProperties* props, uint32_t count, const char* name) {
	for (uint32_t i = 0; i < count; i++) if (strcmp(props[i].extensionName, name) == 0) return true;
	return false;
}

static bool run_session(struct android_app* app, XrInstance instance, XrSystemId system, const gl_ctx_t* gl) {
	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = NULL;
	CHECK(xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction*)&get_reqs));
	XrGraphicsRequirementsOpenGLESKHR reqs = { .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	CHECK(get_reqs(instance, system, &reqs));

	XrGraphicsBindingOpenGLESAndroidKHR binding = { .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR, .display = gl->display, .config = gl->config, .context = gl->context };
	XrSession session = XR_NULL_HANDLE;
	CHECK(xrCreateSession(instance, &(XrSessionCreateInfo){ .type = XR_TYPE_SESSION_CREATE_INFO, .next = &binding, .systemId = system }, &session));

	XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
	XrReferenceSpaceCreateInfo space_info = { .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO, .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL, .poseInReferenceSpace = { .orientation = { .w = 1 } } };
	CHECK(xrCreateReferenceSpace(session, &space_info, &local));
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	CHECK(xrCreateReferenceSpace(session, &space_info, &view));

	bool    running = false, done = false;
	int32_t frames  = 0, located = 0;
	// Idle iterations sleep 10ms, so this gives the session about 30s to start
	for (int32_t idle = 0; idle < 3000 && !done;) {
		pump_android(app);
		XrEventDataBuffer event = { .type = XR_TYPE_EVENT_DATA_BUFFER };
		while (xrPollEvent(instance, &event) == XR_SUCCESS) {
			if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
				XrSessionState state = ((XrEventDataSessionStateChanged*)&event)->state;
				LOG("session state %d", (int)state);
				if (state == XR_SESSION_STATE_READY) {
					CHECK(xrBeginSession(session, &(XrSessionBeginInfo){ .type = XR_TYPE_SESSION_BEGIN_INFO, .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO }));
					running = true;
				} else if (state == XR_SESSION_STATE_STOPPING) {
					CHECK(xrEndSession(session));
					running = false;
				} else if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) {
					done = true;
				}
			}
			event = (XrEventDataBuffer){ .type = XR_TYPE_EVENT_DATA_BUFFER };
		}
		if (!running) {
			usleep(10000);
			idle++;
			continue;
		}

		XrFrameState frame = { .type = XR_TYPE_FRAME_STATE };
		CHECK(xrWaitFrame (session, &(XrFrameWaitInfo){  .type = XR_TYPE_FRAME_WAIT_INFO  }, &frame));
		CHECK(xrBeginFrame(session, &(XrFrameBeginInfo){ .type = XR_TYPE_FRAME_BEGIN_INFO }));
		XrSpaceLocation loc = { .type = XR_TYPE_SPACE_LOCATION };
		CHECK(xrLocateSpace(view, local, frame.predictedDisplayTime, &loc));
		if (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) located++;
		CHECK(xrEndFrame(session, &(XrFrameEndInfo){ .type = XR_TYPE_FRAME_END_INFO, .displayTime = frame.predictedDisplayTime, .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE }));
		if (++frames == 180) CHECK(xrRequestExitSession(session));
	}
	LOG("%d frames, %d with a located head", frames, located);

	CHECK(xrDestroySpace(view));
	CHECK(xrDestroySpace(local));
	CHECK(xrDestroySession(session));
	return frames >= 180;
}

static bool run_test(struct android_app* app, const gl_ctx_t* gl) {
	PFN_xrInitializeLoaderKHR init = NULL;
	CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)&init));
	XrLoaderInitInfoAndroidKHR init_info = { .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR, .applicationVM = app->activity->vm, .applicationContext = app->activity->clazz };
	CHECK(init((const XrLoaderInitInfoBaseHeaderKHR*)&init_info));

	uint32_t layer_count = 0;
	CHECK(xrEnumerateApiLayerProperties(0, &layer_count, NULL));
	uint32_t ext_count = 0;
	CHECK(xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL));
	XrExtensionProperties* exts = calloc(ext_count, sizeof(XrExtensionProperties));
	for (uint32_t i = 0; i < ext_count; i++) exts[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	CHECK(xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, exts));
	bool debug_utils = has_ext(exts, ext_count, XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
	LOG("%u API layers, %u instance extensions, debug_utils %s", layer_count, ext_count, debug_utils ? "yes" : "no");
	if (!has_ext(exts, ext_count, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) || !has_ext(exts, ext_count, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) {
		LOG("FAIL runtime lacks GLES or android_create_instance");
		return false;
	}
	free(exts);

	for (int32_t pass = 0; pass < 2; pass++) {
		const char* enabled[] = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME, XR_EXT_DEBUG_UTILS_EXTENSION_NAME };
		XrInstanceCreateInfoAndroidKHR android_info = { .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR, .applicationVM = app->activity->vm, .applicationActivity = app->activity->clazz };
		XrInstanceCreateInfo create_info = {
			.type                  = XR_TYPE_INSTANCE_CREATE_INFO,
			.next                  = &android_info,
			.applicationInfo       = { .applicationName = "xrl_test", .apiVersion = XR_API_VERSION_1_0 },
			.enabledExtensionCount = debug_utils ? 3 : 2,
			.enabledExtensionNames = enabled,
		};
		XrInstance instance = XR_NULL_HANDLE;
		CHECK(xrCreateInstance(&create_info, &instance));

		XrInstanceProperties inst_props = { .type = XR_TYPE_INSTANCE_PROPERTIES };
		CHECK(xrGetInstanceProperties(instance, &inst_props));
		XrSystemId system = XR_NULL_SYSTEM_ID;
		CHECK(xrGetSystem(instance, &(XrSystemGetInfo){ .type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY }, &system));
		XrSystemProperties sys_props = { .type = XR_TYPE_SYSTEM_PROPERTIES };
		CHECK(xrGetSystemProperties(instance, system, &sys_props));
		LOG("pass %d: runtime '%s' %u.%u.%u, system '%s'", pass, inst_props.runtimeName,
			XR_VERSION_MAJOR(inst_props.runtimeVersion), XR_VERSION_MINOR(inst_props.runtimeVersion), XR_VERSION_PATCH(inst_props.runtimeVersion), sys_props.systemName);

		if (pass == 0 && !run_session(app, instance, system, gl)) return false;
		CHECK(xrDestroyInstance(instance));
	}
	return true;
}

void android_main(struct android_app* app) {
	JNIEnv* env;
	(*app->activity->vm)->AttachCurrentThread(app->activity->vm, &env, NULL);

	gl_ctx_t gl  = {0};
	bool     ok  = gl_init(&gl) && run_test(app, &gl);
	LOG("XRL_TEST %s", ok ? "PASS" : "FAIL");

	ANativeActivity_finish(app->activity);
	while (!app->destroyRequested) {
		int32_t                     events;
		struct android_poll_source* source;
		if (ALooper_pollOnce(-1, NULL, &events, (void**)&source) >= 0 && source) source->process(app, source);
	}
	(*app->activity->vm)->DetachCurrentThread(app->activity->vm);
}
