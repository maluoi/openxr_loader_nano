// SPDX-License-Identifier: Apache-2.0 OR MIT
// Internal declarations shared by the loader's modules.

#pragma once

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(XRL_SHARED) && !defined(_WIN32)
	#define XRL_EXPORT __attribute__((visibility("default")))
#else
	#define XRL_EXPORT // Windows DLLs export through openxr_loader.def
#endif

#if defined(_WIN32)
	#define XRL_PATH_SEP ';'
	#define XRL_DIR_SEP  "\\"
#else
	#define XRL_PATH_SEP ':'
	#define XRL_DIR_SEP  "/"
#endif

#define XRL_PATH_MAX 4096

#include "exports.gen.h"

#if defined(_MSC_VER)
	#define XRL_COLD __declspec(noinline)
#else
	#define XRL_COLD __attribute__((noinline, cold))
#endif

///////////////////////////////////////////
// loader.c
///////////////////////////////////////////

XRL_COLD XrResult xrl_dispatch_missing(xrl_fn_ fn);
bool     xrl_prop_get        (const char* name, bool secure, char* out_value, size_t value_size);
bool     xrl_prop_is_set     (const char* name);

///////////////////////////////////////////
// log.c
///////////////////////////////////////////

// Same bits as XR_DEBUG_UTILS_MESSAGE_SEVERITY_*, which are variables rather than constants in C
typedef enum xrl_sev_ {
	xrl_sev_verbose = 0x0001,
	xrl_sev_info    = 0x0010,
	xrl_sev_warn    = 0x0100,
	xrl_sev_error   = 0x1000,
} xrl_sev_;

#if defined(__GNUC__) || defined(__clang__)
	#define XRL_PRINTF(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
	#define XRL_PRINTF(fmt_idx, arg_idx)
#endif

void xrl_log                (xrl_sev_ sev, const char* command, const char* fmt, ...) XRL_PRINTF(3, 4);
void xrl_log_vuid           (const char* vuid, const char* command, const char* fmt, ...) XRL_PRINTF(3, 4);
void xrl_log_add_messenger  (XrDebugUtilsMessengerEXT messenger, const XrDebugUtilsMessengerCreateInfoEXT* info);
void xrl_log_remove_messenger(XrDebugUtilsMessengerEXT messenger);
void xrl_log_clear_messengers(void);
void xrl_log_submit         (XrDebugUtilsMessageSeverityFlagsEXT sev, XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT* data);

///////////////////////////////////////////
// fmt.c
///////////////////////////////////////////

// snprintf semantics, for %s %.*s %d %u and %% only.
int32_t xrl_fmt (char* out_str, size_t str_size, const char* fmt, ...) XRL_PRINTF(3, 4);
int32_t xrl_vfmt(char* out_str, size_t str_size, const char* fmt, va_list args);

///////////////////////////////////////////
// json.c
///////////////////////////////////////////

typedef enum xrl_json_ {
	xrl_json_invalid = 0,
	xrl_json_object,
	xrl_json_array,
	xrl_json_string,
	xrl_json_number,
	xrl_json_literal, // true, false, null
} xrl_json_;

typedef struct xrl_json_tok_t {
	xrl_json_ type;
	int32_t   start; // Byte range in the source text, strings exclude their quotes
	int32_t   end;
	int32_t   next;  // Index of the first token after this one's children
} xrl_json_tok_t;

// Returns the number of tokens, or -1 on a syntax error. Token 0 is the root.
int32_t xrl_json_parse    (const char* text, int32_t text_len, xrl_json_tok_t* out_tokens, int32_t token_capacity);
int32_t xrl_json_get      (const char* text, const xrl_json_tok_t* tokens, int32_t object, const char* key);
bool    xrl_json_is_string(const xrl_json_tok_t* tokens, int32_t tok);
char*   xrl_json_unescape (const char* text, const xrl_json_tok_t* token, char* out_str, int32_t str_size);

///////////////////////////////////////////
// manifest.c
///////////////////////////////////////////

typedef enum xrl_manifest_ {
	xrl_manifest_runtime,
	xrl_manifest_layer_implicit,
	xrl_manifest_layer_explicit,
} xrl_manifest_;

typedef struct xrl_ext_t {
	const char* name;
	uint32_t    version;
} xrl_ext_t;

typedef struct xrl_func_t {
	const char* name;   // OpenXR function name
	const char* symbol; // Exported symbol the library uses for it
} xrl_func_t;

typedef struct xrl_manifest_t {
	const char* file;        // Absolute manifest path, empty for Android's broker
	const char* library;     // Resolved library path, or a bare name for the system search
	const char* name;        // Layers only
	const char* description; // Layers only
	XrVersion   api_version; // Layers only
	uint32_t    impl_version;
	int32_t     ext_start, ext_count;
	int32_t     func_start, func_count;
} xrl_manifest_t;

typedef struct xrl_block_t xrl_block_t;

typedef struct xrl_manifests_t {
	xrl_block_t*    strings; // Arena that owns every string referenced below
	xrl_manifest_t* items;
	int32_t         count, capacity;
	xrl_ext_t*      exts;
	int32_t         ext_count, ext_capacity;
	xrl_func_t*     funcs;
	int32_t         func_count, func_capacity;
} xrl_manifests_t;

void        xrl_manifests_free       (xrl_manifests_t* ref_list);
void        xrl_manifests_find       (xrl_manifests_t* ref_list, xrl_manifest_ type, const char* command);
void        xrl_manifests_parse      (xrl_manifests_t* ref_list, xrl_manifest_ type, const char* file, const char* json, int32_t json_len, const char* opt_lib_dir);
void        xrl_manifests_add_runtime(xrl_manifests_t* ref_list, const char* library, const xrl_func_t* funcs, int32_t func_count);
const char* xrl_manifest_symbol      (const xrl_manifests_t* list, const xrl_manifest_t* manifest, const char* function);
bool        xrl_manifest_has_ext     (const xrl_manifests_t* list, const xrl_manifest_t* manifest, const char* extension);
void        xrl_manifest_append_exts (const xrl_manifests_t* list, const xrl_manifest_t* manifest, XrExtensionProperties** ref_props, uint32_t* ref_count);

void        xrl_ext_merge            (XrExtensionProperties** ref_props, uint32_t* ref_count, const char* name, uint32_t version, bool prefer_new);

///////////////////////////////////////////
// platform.c
///////////////////////////////////////////

typedef void* xrl_lib_t;

xrl_lib_t xrl_lib_open      (const char* path);
void      xrl_lib_close     (xrl_lib_t lib);
void*     xrl_lib_sym       (xrl_lib_t lib, const char* name);
void      xrl_lib_error     (const char* path, char* out_msg, size_t msg_size);

bool      xrl_env_get       (const char* name, bool secure, char* out_value, size_t value_size);
bool      xrl_env_is_set    (const char* name);
bool      xrl_is_elevated   (void);

void      xrl_console_write (bool to_stderr, const char* text);
char*     xrl_file_read     (const char* path, int32_t* out_size);
bool      xrl_path_exists   (const char* path);
bool      xrl_path_is_file  (const char* path);
bool      xrl_path_is_abs   (const char* path);
bool      xrl_path_absolute (const char* path, char* out_path, size_t path_size);
bool      xrl_path_canonical(const char* path, char* out_path, size_t path_size);
void      xrl_path_parent   (const char* path, char* out_path, size_t path_size);
void      xrl_path_join     (const char* parent, const char* child, char* out_path, size_t path_size);
void      xrl_dir_list      (const char* dir, void (*on_file)(void* ctx, const char* path), void* ctx);

#if defined(_WIN32)
bool      xrl_reg_active_runtime(char* out_path, size_t path_size);
void      xrl_reg_layers        (bool implicit, void (*on_file)(void* ctx, const char* path), void* ctx);
#endif

void      xrl_lock          (void);
void      xrl_unlock        (void);
void      xrl_log_lock      (void);
void      xrl_log_unlock    (void);

///////////////////////////////////////////
// android.c
///////////////////////////////////////////

#if defined(__ANDROID__)
XrResult                             xrl_android_init         (const XrLoaderInitInfoBaseHeaderKHR* info);
bool                                 xrl_android_initialized  (void);
const XrLoaderInitInfoBaseHeaderKHR* xrl_android_init_param   (void);
bool                                 xrl_android_find_runtime (xrl_manifests_t* ref_list);
void                                 xrl_android_find_layers  (xrl_manifests_t* ref_list, xrl_manifest_ type);
#endif
