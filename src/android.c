// SPDX-License-Identifier: Apache-2.0 OR MIT
// Android: xrInitializeLoaderKHR data, the runtime broker content providers,
// and API layer manifests packaged in the APK's assets.

#if defined(__ANDROID__)

#include <jni.h>
#include <dlfcn.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#define XR_USE_PLATFORM_ANDROID
#include "loader.h"
#include <openxr/openxr_platform.h>

#include <stdlib.h>
#include <string.h>

#if defined(__aarch64__)
	#define XRL_ANDROID_ABI "arm64-v8a"
#elif defined(__arm__)
	#define XRL_ANDROID_ABI "armeabi-v7l" // Upstream's spelling, brokers match on it
#elif defined(__x86_64__)
	#define XRL_ANDROID_ABI "x86_64"
#elif defined(__i386__)
	#define XRL_ANDROID_ABI "x86"
#else
	#error "Unknown Android ABI!"
#endif

static struct {
	bool                       initialized;
	XrLoaderInitInfoAndroidKHR info;         // As given to xrInitializeLoaderKHR, with next cleared
	AAssetManager*             assets;
	jobject                    assets_ref;   // Global ref that keeps the AssetManager alive
	char                       lib_dir[XRL_PATH_MAX];
} g_android;

///////////////////////////////////////////
// JNI helpers
///////////////////////////////////////////

static JNIEnv* jni_begin(bool* out_attached) {
	JavaVM* vm  = (JavaVM*)g_android.info.applicationVM;
	JNIEnv* env = NULL;
	*out_attached = false;
	if ((*vm)->GetEnv(vm, (void**)&env, JNI_VERSION_1_6) != JNI_OK) {
		if ((*vm)->AttachCurrentThread(vm, &env, NULL) != JNI_OK) return NULL;
		*out_attached = true;
	}
	if ((*env)->PushLocalFrame(env, 64) != JNI_OK) {
		if (*out_attached) (*vm)->DetachCurrentThread(vm);
		return NULL;
	}
	return env;
}

static void jni_end(JNIEnv* env, bool attached) {
	JavaVM* vm = (JavaVM*)g_android.info.applicationVM;
	(*env)->PopLocalFrame(env, NULL);
	if (attached) (*vm)->DetachCurrentThread(vm);
}

// Returns true if a Java exception was pending, and clears it.
static bool jni_failed(JNIEnv* env) {
	if (!(*env)->ExceptionCheck(env)) return false;
	(*env)->ExceptionClear(env);
	return true;
}

static jobject jni_call_obj(JNIEnv* env, jobject obj, const char* method, const char* sig) {
	jclass    cls = (*env)->GetObjectClass(env, obj);
	jmethodID id  = (*env)->GetMethodID(env, cls, method, sig);
	if (id == NULL || jni_failed(env)) return NULL;
	jobject result = (*env)->CallObjectMethod(env, obj, id);
	return jni_failed(env) ? NULL : result;
}

static void jni_string(JNIEnv* env, jstring str, char* out_str, size_t str_size) {
	out_str[0] = '\0';
	if (str == NULL) return;
	const char* chars = (*env)->GetStringUTFChars(env, str, NULL);
	if (chars == NULL) { jni_failed(env); return; }
	xrl_fmt(out_str, str_size, "%s", chars);
	(*env)->ReleaseStringUTFChars(env, str, chars);
}

///////////////////////////////////////////
// Init data
///////////////////////////////////////////

XrResult xrl_android_init(const XrLoaderInitInfoBaseHeaderKHR* info) {
	while (info != NULL && info->type != XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR) info = (const XrLoaderInitInfoBaseHeaderKHR*)info->next;
	if (info == NULL) return XR_ERROR_VALIDATION_FAILURE;

	const XrLoaderInitInfoAndroidKHR* android = (const XrLoaderInitInfoAndroidKHR*)info;
	if (android->applicationVM == NULL || android->applicationContext == NULL) return XR_ERROR_VALIDATION_FAILURE;

	bool    attached;
	JNIEnv* env = NULL;
	if (g_android.assets_ref && (env = jni_begin(&attached)) != NULL) {
		(*env)->DeleteGlobalRef(env, g_android.assets_ref);
		jni_end(env, attached);
	}
	memset(&g_android, 0, sizeof(g_android));
	g_android.info      = *android;
	g_android.info.next = NULL;

	if ((env = jni_begin(&attached)) == NULL) return XR_ERROR_INITIALIZATION_FAILED;
	jobject context  = (jobject)g_android.info.applicationContext;
	jobject assets   = jni_call_obj(env, context, "getAssets", "()Landroid/content/res/AssetManager;");
	jobject app_info = jni_call_obj(env, context, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
	if (assets != NULL) {
		g_android.assets_ref = (*env)->NewGlobalRef(env, assets);
		g_android.assets     = AAssetManager_fromJava(env, g_android.assets_ref);
	}
	if (app_info != NULL) {
		jfieldID dir_id = (*env)->GetFieldID(env, (*env)->GetObjectClass(env, app_info), "nativeLibraryDir", "Ljava/lang/String;");
		if (dir_id != NULL && !jni_failed(env)) jni_string(env, (jstring)(*env)->GetObjectField(env, app_info, dir_id), g_android.lib_dir, sizeof(g_android.lib_dir));
		jni_failed(env);
	}
	jni_end(env, attached);

	g_android.initialized = true;
	return XR_SUCCESS;
}

bool xrl_android_initialized(void) {
	return g_android.initialized;
}

const XrLoaderInitInfoBaseHeaderKHR* xrl_android_init_param(void) {
	return g_android.initialized ? (const XrLoaderInitInfoBaseHeaderKHR*)&g_android.info : NULL;
}

///////////////////////////////////////////
// Runtime broker
///////////////////////////////////////////

static const char* broker_authority(bool system) {
	return system ? "org.khronos.openxr.system_runtime_broker" : "org.khronos.openxr.runtime_broker";
}

// Queries a content provider, returning a Cursor with at least one row, or NULL.
static jobject broker_query(JNIEnv* env, jobject resolver, const char* uri_str, const char* const* columns, int32_t column_count) {
	xrl_log(xrl_sev_info, "", "Querying runtime broker URI %s", uri_str);

	jclass    uri_cls  = (*env)->FindClass(env, "android/net/Uri");
	jmethodID parse_id = uri_cls ? (*env)->GetStaticMethodID(env, uri_cls, "parse", "(Ljava/lang/String;)Landroid/net/Uri;") : NULL;
	jclass    str_cls  = (*env)->FindClass(env, "java/lang/String");
	if (parse_id == NULL || str_cls == NULL || jni_failed(env)) return NULL;

	jobject      uri        = (*env)->CallStaticObjectMethod(env, uri_cls, parse_id, (*env)->NewStringUTF(env, uri_str));
	jobjectArray projection = (*env)->NewObjectArray(env, column_count, str_cls, NULL);
	if (uri == NULL || projection == NULL || jni_failed(env)) return NULL;
	for (int32_t i = 0; i < column_count; i++) (*env)->SetObjectArrayElement(env, projection, i, (*env)->NewStringUTF(env, columns[i]));

	jmethodID query_id = (*env)->GetMethodID(env, (*env)->GetObjectClass(env, resolver), "query",
		"(Landroid/net/Uri;[Ljava/lang/String;Ljava/lang/String;[Ljava/lang/String;Ljava/lang/String;)Landroid/database/Cursor;");
	if (query_id == NULL || jni_failed(env)) return NULL;

	// Throws when the provider is missing or not visible to this app
	jobject cursor = (*env)->CallObjectMethod(env, resolver, query_id, uri, projection, NULL, NULL, NULL);
	if (jni_failed(env) || cursor == NULL) {
		xrl_log(xrl_sev_info, "", "No cursor from the runtime broker");
		return NULL;
	}

	jclass    cursor_cls = (*env)->GetObjectClass(env, cursor);
	jmethodID count_id   = (*env)->GetMethodID(env, cursor_cls, "getCount", "()I");
	jint      count      = count_id ? (*env)->CallIntMethod(env, cursor, count_id) : 0;
	if (jni_failed(env) || count < 1) {
		jmethodID close_id = (*env)->GetMethodID(env, cursor_cls, "close", "()V");
		if (close_id) (*env)->CallVoidMethod(env, cursor, close_id);
		jni_failed(env);
		xrl_log(xrl_sev_info, "", "Empty cursor from the runtime broker");
		return NULL;
	}
	return cursor;
}

typedef struct cursor_api_t {
	jmethodID move_first, move_next, column_index, get_string, get_int, close;
} cursor_api_t;

static bool cursor_api(JNIEnv* env, jobject cursor, cursor_api_t* out_api) {
	jclass cls = (*env)->GetObjectClass(env, cursor);
	*out_api = (cursor_api_t){
		.move_first   = (*env)->GetMethodID(env, cls, "moveToFirst",    "()Z"),
		.move_next    = (*env)->GetMethodID(env, cls, "moveToNext",     "()Z"),
		.column_index = (*env)->GetMethodID(env, cls, "getColumnIndex", "(Ljava/lang/String;)I"),
		.get_string   = (*env)->GetMethodID(env, cls, "getString",      "(I)Ljava/lang/String;"),
		.get_int      = (*env)->GetMethodID(env, cls, "getInt",         "(I)I"),
		.close        = (*env)->GetMethodID(env, cls, "close",          "()V"),
	};
	return !jni_failed(env) && out_api->move_first && out_api->move_next && out_api->column_index && out_api->get_string && out_api->get_int && out_api->close;
}

static void cursor_string(JNIEnv* env, jobject cursor, const cursor_api_t* api, const char* column, char* out_str, size_t str_size) {
	jint col = (*env)->CallIntMethod(env, cursor, api->column_index, (*env)->NewStringUTF(env, column));
	jstring str = jni_failed(env) ? NULL : (jstring)(*env)->CallObjectMethod(env, cursor, api->get_string, col);
	if (jni_failed(env)) str = NULL;
	jni_string(env, str, out_str, str_size);
}

static jint cursor_int(JNIEnv* env, jobject cursor, const cursor_api_t* api, const char* column) {
	jint col    = (*env)->CallIntMethod(env, cursor, api->column_index, (*env)->NewStringUTF(env, column));
	jint result = jni_failed(env) ? 0 : (*env)->CallIntMethod(env, cursor, api->get_int, col);
	return jni_failed(env) ? 0 : result;
}

///////////////////////////////////////////

// Reads the runtime's function rename table. Returns false if the broker couldn't provide it.
static bool broker_functions(JNIEnv* env, jobject resolver, bool system, const char* package, xrl_func_t** out_funcs, int32_t* out_count, char** out_strings) {
	static const char* columns[] = { "function_name", "symbol_name" };
	char uri[1024];
	xrl_fmt(uri, sizeof(uri), "content://%s/openxr/1/abi/" XRL_ANDROID_ABI "/runtimes/%s/functions", broker_authority(system), package);

	*out_funcs   = NULL;
	*out_count   = 0;
	*out_strings = NULL;
	jobject      cursor = broker_query(env, resolver, uri, columns, 2);
	cursor_api_t api;
	if (cursor == NULL || !cursor_api(env, cursor, &api)) return false;

	// Each row's two names share one 512 byte slot in out_strings
	int32_t capacity = 0;
	bool    ok       = (*env)->CallBooleanMethod(env, cursor, api.move_first) && !jni_failed(env);
	while (ok) {
		if (*out_count == capacity) {
			capacity = capacity ? capacity * 2 : 16;
			xrl_func_t* funcs   = (xrl_func_t*)realloc(*out_funcs,   (size_t)capacity * sizeof(xrl_func_t));
			char*       strings = (char*)      realloc(*out_strings, (size_t)capacity * 512);
			if (funcs)   *out_funcs   = funcs;
			if (strings) *out_strings = strings;
			if (!funcs || !strings) break;
		}
		char* slot = *out_strings + (size_t)*out_count * 512;
		cursor_string(env, cursor, &api, "function_name", slot,       256);
		cursor_string(env, cursor, &api, "symbol_name",   slot + 256, 256);
		(*out_count)++;
		ok = (*env)->CallBooleanMethod(env, cursor, api.move_next) && !jni_failed(env);
	}
	// Strings may have moved while growing, so point at them once they're done
	for (int32_t i = 0; i < *out_count; i++) {
		(*out_funcs)[i] = (xrl_func_t){ .name = *out_strings + (size_t)i * 512, .symbol = *out_strings + (size_t)i * 512 + 256 };
	}
	(*env)->CallVoidMethod(env, cursor, api.close);
	jni_failed(env);
	return true;
}

///////////////////////////////////////////

bool xrl_android_find_runtime(xrl_manifests_t* ref_list) {
	if (!g_android.initialized) return false;

	bool    attached;
	JNIEnv* env = jni_begin(&attached);
	if (env == NULL) return false;

	static const char* columns[] = { "package_name", "native_lib_dir", "so_filename", "has_functions" };
	jobject resolver = jni_call_obj(env, (jobject)g_android.info.applicationContext, "getContentResolver", "()Landroid/content/ContentResolver;");
	jobject cursor   = NULL;
	bool    system   = false;
	for (int32_t i = 0; resolver != NULL && cursor == NULL && i < 2; i++) {
		char uri[512];
		system = i == 1;
		xrl_fmt(uri, sizeof(uri), "content://%s/openxr/1/abi/" XRL_ANDROID_ABI "/runtimes/active", broker_authority(system));
		cursor = broker_query(env, resolver, uri, columns, 4);
	}

	cursor_api_t api;
	bool         found = false;
	if (cursor == NULL || !cursor_api(env, cursor, &api)) {
		xrl_log(xrl_sev_info, "", "Could access neither the installable nor system runtime broker");
		jni_end(env, attached);
		return false;
	}

	bool more = (*env)->CallBooleanMethod(env, cursor, api.move_first) && !jni_failed(env);
	while (more && !found) {
		char package[256], lib_dir[XRL_PATH_MAX], so_name[256], lib_path[XRL_PATH_MAX];
		cursor_string(env, cursor, &api, "package_name",   package, sizeof(package));
		cursor_string(env, cursor, &api, "native_lib_dir", lib_dir, sizeof(lib_dir));
		cursor_string(env, cursor, &api, "so_filename",    so_name, sizeof(so_name));
		bool has_functions = cursor_int(env, cursor, &api, "has_functions") == 1;
		xrl_fmt(lib_path, sizeof(lib_path), "%s/%s", lib_dir, so_name);
		xrl_log(xrl_sev_info, "", "Got runtime: package: %s, so filename: %s, native lib dir: %s, has functions: %s", package, so_name, lib_dir, has_functions ? "yes" : "no");

		void* lib = dlopen(lib_path, RTLD_LAZY | RTLD_LOCAL);
		if (lib != NULL) {
			dlclose(lib);
			xrl_func_t* funcs      = NULL;
			int32_t     func_count = 0;
			char*       strings    = NULL;
			if (!has_functions || broker_functions(env, resolver, system, package, &funcs, &func_count, &strings)) {
				xrl_manifests_add_runtime(ref_list, lib_path, funcs, func_count);
				found = true;
			} else {
				xrl_log(xrl_sev_warn, "", "Unable to populate functions from runtime %s, checking for more records", lib_path);
			}
			free(funcs);
			free(strings);
		} else {
			xrl_log(xrl_sev_verbose, "", "Unable to open broker provided runtime at %s, checking for more records", lib_path);
		}
		if (!found) more = (*env)->CallBooleanMethod(env, cursor, api.move_next) && !jni_failed(env);
	}
	if (!found) xrl_log(xrl_sev_warn, "", "Unable to open any of the broker provided runtimes");

	(*env)->CallVoidMethod(env, cursor, api.close);
	jni_failed(env);
	jni_end(env, attached);
	return found;
}

///////////////////////////////////////////
// API layers in assets
///////////////////////////////////////////

void xrl_android_find_layers(xrl_manifests_t* ref_list, xrl_manifest_ type) {
	if (!g_android.initialized) {
		xrl_log(xrl_sev_warn, "", "Unable to look for API layers in assets, xrInitializeLoaderKHR wasn't called");
		return;
	}
	if (g_android.assets == NULL) return;

	const char* dir_name = type == xrl_manifest_layer_implicit ? "openxr/1/api_layers/implicit.d" : "openxr/1/api_layers/explicit.d";
	AAssetDir*  dir      = AAssetManager_openDir(g_android.assets, dir_name);
	if (dir == NULL) return;

	const char* file_name;
	while ((file_name = AAssetDir_getNextFileName(dir)) != NULL) {
		size_t len = strlen(file_name);
		if (len < 5 || strcmp(file_name + len - 5, ".json") != 0) continue;

		char path[XRL_PATH_MAX];
		xrl_fmt(path, sizeof(path), "%s/%s", dir_name, file_name);
		AAsset* asset = AAssetManager_open(g_android.assets, path, AASSET_MODE_BUFFER);
		if (asset == NULL) {
			xrl_log(xrl_sev_warn, "", "Unable to open asset %s, skipping", path);
			continue;
		}
		const char* buffer = (const char*)AAsset_getBuffer(asset);
		if (buffer != NULL) xrl_manifests_parse(ref_list, type, path, buffer, (int32_t)AAsset_getLength(asset), g_android.lib_dir);
		else                xrl_log(xrl_sev_warn, "", "Unable to access asset %s, skipping", path);
		AAsset_close(asset);
	}
	AAssetDir_close(dir);
}

#endif
