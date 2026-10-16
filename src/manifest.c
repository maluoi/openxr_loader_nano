// SPDX-License-Identifier: Apache-2.0 OR MIT
// Runtime and API layer manifest discovery and parsing.

#include "loader.h"

#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
	#include <unistd.h>
#endif

#ifndef XRL_FALLBACK_CONFIG_DIRS
	#define XRL_FALLBACK_CONFIG_DIRS "/etc/xdg"
#endif
#ifndef XRL_FALLBACK_DATA_DIRS
	#define XRL_FALLBACK_DATA_DIRS "/usr/local/share:/usr/share"
#endif
#ifndef XRL_SYSCONFDIR
	#define XRL_SYSCONFDIR "/etc"
#endif

#if defined(__x86_64__) && defined(__ILP32__)
	#define XRL_ARCH_ABI "x32"
#elif defined(_M_X64) || defined(__x86_64__)
	#define XRL_ARCH_ABI "x86_64"
#elif defined(_M_IX86) || defined(__i386__) || defined(_X86_)
	#define XRL_ARCH_ABI "i686"
#elif (defined(__aarch64__) && defined(__LP64__)) || defined(_M_ARM64)
	#define XRL_ARCH_ABI "aarch64"
#elif (defined(__ARM_ARCH) && __ARM_ARCH >= 7 && (defined(__ARM_PCS_VFP) || defined(__ANDROID__))) || defined(_M_ARM)
	#define XRL_ARCH_ABI "armv7a-vfp"
#elif defined(__ARM_ARCH_5TE__) || (defined(__ARM_ARCH) && __ARM_ARCH > 5)
	#define XRL_ARCH_ABI "armv5te"
#elif defined(__mips64)
	#define XRL_ARCH_ABI "mips64"
#elif defined(__mips)
	#define XRL_ARCH_ABI "mips"
#elif defined(__powerpc64__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	#define XRL_ARCH_ABI "ppc64"
#elif defined(__powerpc__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	#define XRL_ARCH_ABI "ppc64el"
#elif defined(__s390x__) || defined(__zarch__)
	#define XRL_ARCH_ABI "s390x"
#elif defined(__riscv_xlen) && (__riscv_xlen == 64)
	#define XRL_ARCH_ABI "riscv64"
#elif defined(__sparc__) && defined(__arch64__)
	#define XRL_ARCH_ABI "sparc64"
#elif defined(__loongarch64)
	#define XRL_ARCH_ABI "loongarch64"
#else
	#error "No architecture string known!"
#endif

#define XRL_MAJOR_STR "1"
_Static_assert(XR_VERSION_MAJOR(XR_CURRENT_API_VERSION) == 1, "Update XRL_MAJOR_STR for the new major version");

///////////////////////////////////////////
// Storage
///////////////////////////////////////////

struct xrl_block_t {
	xrl_block_t* prev;
	size_t       used;
	size_t       capacity;
	char         data[];
};

static const char* store_str(xrl_manifests_t* ref_list, const char* str, size_t len) {
	xrl_block_t* block = ref_list->strings;
	if (block == NULL || block->capacity - block->used < len + 1) {
		size_t capacity = len + 1 > 4096 ? len + 1 : 4096;
		xrl_block_t* new_block = (xrl_block_t*)malloc(sizeof(xrl_block_t) + capacity);
		if (new_block == NULL) return "";
		*new_block = (xrl_block_t){ .prev = block, .capacity = capacity };
		ref_list->strings = block = new_block;
	}
	char* result = block->data + block->used;
	memcpy(result, str, len);
	result[len]  = '\0';
	block->used += len + 1;
	return result;
}

///////////////////////////////////////////

static const char* store_json_str(xrl_manifests_t* ref_list, const char* json, const xrl_json_tok_t* token) {
	char   stack_buf[512];
	size_t raw_len = (size_t)(token->end - token->start);
	char*  buf     = raw_len < sizeof(stack_buf) ? stack_buf : (char*)malloc(raw_len + 1);
	if (buf == NULL) return "";
	xrl_json_unescape(json, token, buf, (int32_t)raw_len + 1);
	const char* result = store_str(ref_list, buf, strlen(buf));
	if (buf != stack_buf) free(buf);
	return result;
}

///////////////////////////////////////////

static bool grow(void** ref_items, int32_t* ref_capacity, int32_t count, size_t item_size) {
	if (count < *ref_capacity) return true;
	int32_t capacity = *ref_capacity ? *ref_capacity * 2 : 8;
	void*   items    = realloc(*ref_items, (size_t)capacity * item_size);
	if (items == NULL) return false;
	*ref_items    = items;
	*ref_capacity = capacity;
	return true;
}

///////////////////////////////////////////

void xrl_manifests_free(xrl_manifests_t* ref_list) {
	while (ref_list->strings) {
		xrl_block_t* prev = ref_list->strings->prev;
		free(ref_list->strings);
		ref_list->strings = prev;
	}
	free(ref_list->items);
	free(ref_list->exts);
	free(ref_list->funcs);
	*ref_list = (xrl_manifests_t){0};
}

///////////////////////////////////////////

const char* xrl_manifest_symbol(const xrl_manifests_t* list, const xrl_manifest_t* manifest, const char* function) {
	for (int32_t i = manifest->func_start; i < manifest->func_start + manifest->func_count; i++) {
		if (strcmp(list->funcs[i].name, function) == 0) return list->funcs[i].symbol;
	}
	return function;
}

///////////////////////////////////////////

bool xrl_manifest_has_ext(const xrl_manifests_t* list, const xrl_manifest_t* manifest, const char* extension) {
	for (int32_t i = manifest->ext_start; i < manifest->ext_start + manifest->ext_count; i++) {
		if (strcmp(list->exts[i].name, extension) == 0) return true;
	}
	return false;
}

///////////////////////////////////////////

void xrl_ext_merge(XrExtensionProperties** ref_props, uint32_t* ref_count, const char* name, uint32_t version, bool prefer_new) {
	for (uint32_t i = 0; i < *ref_count; i++) {
		XrExtensionProperties* prop = &(*ref_props)[i];
		if (strcmp(prop->extensionName, name) != 0) continue;
		if (prefer_new || prop->extensionVersion < version) prop->extensionVersion = version;
		return;
	}
	// Only grown here, from 16 by doubling, so the count alone shows when it's full
	uint32_t count = *ref_count;
	if (count == 0 || (count >= 16 && (count & (count - 1)) == 0)) {
		uint32_t capacity = count ? count * 2 : 16;
		XrExtensionProperties* props = (XrExtensionProperties*)realloc(*ref_props, capacity * sizeof(XrExtensionProperties));
		if (props == NULL) return;
		*ref_props = props;
	}
	XrExtensionProperties* prop = &(*ref_props)[(*ref_count)++];
	*prop = (XrExtensionProperties){ .type = XR_TYPE_EXTENSION_PROPERTIES, .extensionVersion = version };
	xrl_fmt(prop->extensionName, sizeof(prop->extensionName), "%s", name);
}

///////////////////////////////////////////

void xrl_manifest_append_exts(const xrl_manifests_t* list, const xrl_manifest_t* manifest, XrExtensionProperties** ref_props, uint32_t* ref_count) {
	for (int32_t i = manifest->ext_start; i < manifest->ext_start + manifest->ext_count; i++) {
		xrl_ext_merge(ref_props, ref_count, list->exts[i].name, list->exts[i].version, false);
	}
}

///////////////////////////////////////////
// Parsing
///////////////////////////////////////////

static bool has_dir_symbol(const char* path) {
	return strchr(path, '/') != NULL || strchr(path, '\\') != NULL;
}

///////////////////////////////////////////

static bool is_uint(const char* json, const xrl_json_tok_t* tok) {
	if (tok->type != xrl_json_number) return false;
	for (int32_t i = tok->start; i < tok->end; i++) {
		if (json[i] < '0' || json[i] > '9') return false;
	}
	return tok->end - tok->start <= 10 && strtoull(json + tok->start, NULL, 10) <= UINT32_MAX;
}

///////////////////////////////////////////

// Parses "1.2.3" style versions like sscanf's "%u.%u.%u", which would pull scanf into static CRT builds.
static int32_t parse_version(const char* str, uint32_t* out_parts, int32_t max_parts) {
	int32_t count = 0;
	while (count < max_parts) {
		char*         end;
		unsigned long value = strtoul(str, &end, 10);
		if (end == str) break;
		out_parts[count++] = (uint32_t)value;
		if (*end != '.') break;
		str = end + 1;
	}
	return count;
}

///////////////////////////////////////////

// Both manifest types share an "instance_extensions" array and a "functions" rename map.
static void parse_common(xrl_manifests_t* ref_list, xrl_manifest_t* ref_manifest, const char* json, const xrl_json_tok_t* toks, int32_t node) {
	ref_manifest->ext_start  = ref_list->ext_count;
	ref_manifest->func_start = ref_list->func_count;

	int32_t exts = xrl_json_get(json, toks, node, "instance_extensions");
	if (exts >= 0 && toks[exts].type == xrl_json_array) {
		for (int32_t e = exts + 1; e < toks[exts].next; e = toks[e].next) {
			int32_t name    = xrl_json_get(json, toks, e, "name");
			int32_t version = xrl_json_get(json, toks, e, "extension_version");
			if (!xrl_json_is_string(toks, name) || version < 0) continue;

			uint32_t ver;
			if (toks[version].type == xrl_json_string) {
				char buf[32];
				ver = (uint32_t)strtoul(xrl_json_unescape(json, &toks[version], buf, sizeof(buf)), NULL, 10);
			} else if (is_uint(json, &toks[version])) {
				ver = (uint32_t)strtoul(json + toks[version].start, NULL, 10);
			} else {
				continue;
			}
			if (!grow((void**)&ref_list->exts, &ref_list->ext_capacity, ref_list->ext_count, sizeof(xrl_ext_t))) break;
			ref_list->exts[ref_list->ext_count++] = (xrl_ext_t){ .name = store_json_str(ref_list, json, &toks[name]), .version = ver };
			ref_manifest->ext_count++;
		}
	}

	int32_t funcs = xrl_json_get(json, toks, node, "functions");
	if (funcs >= 0 && toks[funcs].type == xrl_json_object) {
		for (int32_t f = funcs + 1; f < toks[funcs].next; f = toks[f + 1].next) {
			if (toks[f + 1].type != xrl_json_string) {
				xrl_log(xrl_sev_warn, "", "%s \"functions\" section contains non-string values.", ref_manifest->file);
				continue;
			}
			if (!grow((void**)&ref_list->funcs, &ref_list->func_capacity, ref_list->func_count, sizeof(xrl_func_t))) break;
			ref_list->funcs[ref_list->func_count++] = (xrl_func_t){
				.name   = store_json_str(ref_list, json, &toks[f]),
				.symbol = store_json_str(ref_list, json, &toks[f + 1]) };
			ref_manifest->func_count++;
		}
	}
}

///////////////////////////////////////////

static bool parse_file_version(const char* json, const xrl_json_tok_t* toks, const char* file) {
	int32_t ver = xrl_json_get(json, toks, 0, "file_format_version");
	if (!xrl_json_is_string(toks, ver)) {
		xrl_log(xrl_sev_error, "", "Manifest %s is missing \"file_format_version\"", file);
		return false;
	}
	char     buf[64];
	uint32_t v[3] = {0};
	int32_t  fields = parse_version(xrl_json_unescape(json, &toks[ver], buf, sizeof(buf)), v, 3);
	if (fields != 3 || v[0] != 1 || v[1] != 0 || v[2] != 0) {
		xrl_log(xrl_sev_error, "", "Manifest %s \"file_format_version\" %s is not supported", file, buf);
		return false;
	}
	return true;
}

///////////////////////////////////////////

static bool parse_runtime(xrl_manifest_t* out_manifest, const char* json, const xrl_json_tok_t* toks, const char* file, char* out_lib, size_t lib_size) {
	int32_t rt  = xrl_json_get(json, toks, 0, "runtime");
	int32_t lib = xrl_json_get(json, toks, rt, "library_path");
	if (!xrl_json_is_string(toks, lib)) {
		xrl_log(xrl_sev_error, "", "Runtime manifest %s is missing required fields", file);
		return false;
	}
	xrl_json_unescape(json, &toks[lib], out_lib, (int32_t)lib_size);

	// Relative paths resolve against the real manifest file, not a symlink to it
	if (has_dir_symbol(out_lib) && !xrl_path_is_abs(out_lib)) {
		char canonical[XRL_PATH_MAX];
		char parent   [XRL_PATH_MAX];
		char relative [XRL_PATH_MAX];
		if (!xrl_path_canonical(file, canonical, sizeof(canonical)))
			xrl_fmt(canonical, sizeof(canonical), "%s", file);
		xrl_path_parent(canonical, parent, sizeof(parent));
		xrl_fmt(relative, sizeof(relative), "%s", out_lib);
		xrl_path_join  (parent, relative, out_lib, lib_size);
	}
	*out_manifest = (xrl_manifest_t){0};
	return true;
}

///////////////////////////////////////////

static bool parse_layer(xrl_manifest_t* out_manifest, xrl_manifest_ type, const char* json, const xrl_json_tok_t* toks, const char* file, const char* opt_lib_dir, char* out_lib, size_t lib_size) {
	int32_t layer   = xrl_json_get(json, toks, 0, "api_layer");
	int32_t name    = xrl_json_get(json, toks, layer, "name");
	int32_t api_ver = xrl_json_get(json, toks, layer, "api_version");
	int32_t lib     = xrl_json_get(json, toks, layer, "library_path");
	int32_t impl    = xrl_json_get(json, toks, layer, "implementation_version");
	if (!xrl_json_is_string(toks, name) || !xrl_json_is_string(toks, api_ver) || !xrl_json_is_string(toks, lib) || !xrl_json_is_string(toks, impl)) {
		xrl_log(xrl_sev_error, "", "Layer manifest %s is missing required fields", file);
		return false;
	}

	char buf[256];
	if (type == xrl_manifest_layer_implicit) {
		int32_t disable = xrl_json_get(json, toks, layer, "disable_environment");
		int32_t enable  = xrl_json_get(json, toks, layer, "enable_environment");
		if (!xrl_json_is_string(toks, disable)) {
			xrl_log(xrl_sev_error, "", "Implicit layer %s is missing \"disable_environment\"", file);
			return false;
		}
		bool enabled = true;
		if (xrl_json_is_string(toks, enable) && !xrl_prop_is_set(xrl_json_unescape(json, &toks[enable], buf, sizeof(buf)))) enabled = false;
		if (xrl_prop_is_set(xrl_json_unescape(json, &toks[disable], buf, sizeof(buf)))) enabled = false;
		if (!enabled) {
			xrl_log(xrl_sev_info, "", "Implicit layer %s is disabled", file);
			return false;
		}
	}

	uint32_t v[2]   = {0};
	int32_t  fields = parse_version(xrl_json_unescape(json, &toks[api_ver], buf, sizeof(buf)), v, 2);
	uint32_t major  = v[0], minor = v[1];
	if (fields != 2 || (major == 0 && minor == 0) || major > XR_VERSION_MAJOR(XR_CURRENT_API_VERSION)) {
		xrl_log(xrl_sev_warn, "", "Layer %s has an invalid API version, skipping it", file);
		return false;
	}

	char*    impl_end;
	uint32_t impl_version = (uint32_t)strtoul(xrl_json_unescape(json, &toks[impl], buf, sizeof(buf)), &impl_end, 10);
	if (*impl_end != '\0') {
		xrl_log(xrl_sev_warn, "", "Layer %s has an invalid implementation version", file);
		return false;
	}

	xrl_json_unescape(json, &toks[lib], out_lib, (int32_t)lib_size);
	if (has_dir_symbol(out_lib)) {
		if (!xrl_path_is_abs(out_lib)) {
			char parent  [XRL_PATH_MAX];
			char relative[XRL_PATH_MAX];
			if (opt_lib_dir) xrl_fmt(parent, sizeof(parent), "%s", opt_lib_dir);
			else             xrl_path_parent(file, parent, sizeof(parent));
			xrl_fmt(relative, sizeof(relative), "%s", out_lib);
			xrl_path_join(parent, relative, out_lib, lib_size);
		}
		if (!xrl_path_exists(out_lib)) {
			xrl_log(xrl_sev_error, "", "Layer %s library %s does not appear to exist", file, out_lib);
			return false;
		}
	}

	*out_manifest = (xrl_manifest_t){ .api_version = XR_MAKE_VERSION(major, minor, 0), .impl_version = impl_version };
	return true;
}

///////////////////////////////////////////

void xrl_manifests_parse(xrl_manifests_t* ref_list, xrl_manifest_ type, const char* file, const char* json, int32_t json_len, const char* opt_lib_dir) {
	// Every token consumes at least one byte, so this can't run out
	xrl_json_tok_t* toks = (xrl_json_tok_t*)malloc(((size_t)json_len + 1) * sizeof(xrl_json_tok_t));
	if (toks == NULL) return;

	int32_t tok_count = xrl_json_parse(json, json_len, toks, json_len + 1);
	if (tok_count <= 0 || toks[0].type != xrl_json_object) {
		xrl_log(xrl_sev_error, "", "Failed to parse manifest %s, is it valid JSON?", file);
		free(toks);
		return;
	}

	char           lib[XRL_PATH_MAX];
	xrl_manifest_t manifest;
	bool valid = parse_file_version(json, toks, file) && (type == xrl_manifest_runtime
		? parse_runtime(&manifest,       json, toks, file,              lib, sizeof(lib))
		: parse_layer  (&manifest, type, json, toks, file, opt_lib_dir, lib, sizeof(lib)));
	if (!valid || !grow((void**)&ref_list->items, &ref_list->capacity, ref_list->count, sizeof(xrl_manifest_t))) {
		free(toks);
		return;
	}

	manifest.file    = store_str(ref_list, file, strlen(file));
	manifest.library = store_str(ref_list, lib,  strlen(lib));
	int32_t node     = xrl_json_get(json, toks, 0, type == xrl_manifest_runtime ? "runtime" : "api_layer");
	if (type != xrl_manifest_runtime) {
		int32_t name = xrl_json_get(json, toks, node, "name");
		int32_t desc = xrl_json_get(json, toks, node, "description");
		manifest.name        = store_json_str(ref_list, json, &toks[name]);
		manifest.description = xrl_json_is_string(toks, desc) ? store_json_str(ref_list, json, &toks[desc]) : "";
	}
	parse_common(ref_list, &manifest, json, toks, node);
	ref_list->items[ref_list->count++] = manifest;
	free(toks);
}

///////////////////////////////////////////

void xrl_manifests_add_runtime(xrl_manifests_t* ref_list, const char* library, const xrl_func_t* funcs, int32_t func_count) {
	if (!grow((void**)&ref_list->items, &ref_list->capacity, ref_list->count, sizeof(xrl_manifest_t))) return;

	xrl_manifest_t manifest = { .file = "", .library = store_str(ref_list, library, strlen(library)), .func_start = ref_list->func_count };
	for (int32_t i = 0; i < func_count; i++) {
		if (!grow((void**)&ref_list->funcs, &ref_list->func_capacity, ref_list->func_count, sizeof(xrl_func_t))) break;
		ref_list->funcs[ref_list->func_count++] = (xrl_func_t){
			.name   = store_str(ref_list, funcs[i].name,   strlen(funcs[i].name)),
			.symbol = store_str(ref_list, funcs[i].symbol, strlen(funcs[i].symbol)) };
		manifest.func_count++;
	}
	ref_list->items[ref_list->count++] = manifest;
}

///////////////////////////////////////////
// Discovery
///////////////////////////////////////////

static void parse_file(xrl_manifests_t* ref_list, xrl_manifest_ type, const char* file) {
	int32_t size;
	char*   json = xrl_file_read(file, &size);
	if (json == NULL) {
		xrl_log(xrl_sev_error, "", "Failed to open manifest %s, does it exist?", file);
		return;
	}
	xrl_manifests_parse(ref_list, type, file, json, size, NULL);
	free(json);
}

///////////////////////////////////////////

static bool ends_with_json(const char* path) {
	size_t len = strlen(path);
	return len >= 5 && strcmp(path + len - 5, ".json") == 0;
}

typedef struct find_ctx_t {
	xrl_manifests_t* list;
	xrl_manifest_    type;
} find_ctx_t;

static void on_manifest_file(void* ctx, const char* path) {
	find_ctx_t* find = (find_ctx_t*)ctx;
	char        absolute[XRL_PATH_MAX];
	if (!ends_with_json(path) || !xrl_path_absolute(path, absolute, sizeof(absolute))) return;
	parse_file(find->list, find->type, absolute);
}

///////////////////////////////////////////

// Calls on_dir with each entry of a separator delimited list, with suffix appended.
static void for_each_dir(const char* list, const char* suffix, void (*on_dir)(void* ctx, const char* dir), void* ctx) {
	const char* start = list;
	while (*start) {
		const char* end = strchr(start, XRL_PATH_SEP);
		size_t      len = end ? (size_t)(end - start) : strlen(start);
		if (len > 0) {
			char    dir[XRL_PATH_MAX];
			bool    slash = start[len - 1] == '/' || start[len - 1] == '\\';
			int32_t n     = xrl_fmt(dir, sizeof(dir), "%.*s%s%s", (int)len, start, slash || suffix[0] == '\0' ? "" : "/", suffix);
			if (n > 0 && n < (int32_t)sizeof(dir)) on_dir(ctx, dir);
		}
		if (!end) break;
		start = end + 1;
	}
}

static void on_layer_dir(void* ctx, const char* dir) {
	xrl_dir_list(dir, on_manifest_file, ctx);
}

///////////////////////////////////////////

#if !defined(_WIN32) && !defined(__ANDROID__) && !defined(__APPLE__)
static bool find_active_runtime_in(const char* config_dir, char* out_path, size_t path_size) {
	xrl_log(xrl_sev_info, "", "Looking for active_runtime." XRL_ARCH_ABI ".json or active_runtime.json in %s", config_dir);
	xrl_fmt(out_path, path_size, "%s/openxr/" XRL_MAJOR_STR "/active_runtime." XRL_ARCH_ABI ".json", config_dir);
	if (xrl_path_exists(out_path)) return true;
	xrl_fmt(out_path, path_size, "%s/openxr/" XRL_MAJOR_STR "/active_runtime.json", config_dir);
	return xrl_path_exists(out_path);
}

typedef struct find_runtime_ctx_t {
	char* path;
	bool  found;
} find_runtime_ctx_t;

static void on_config_dir(void* ctx, const char* dir) {
	find_runtime_ctx_t* find = (find_runtime_ctx_t*)ctx;
	if (!find->found) find->found = find_active_runtime_in(dir, find->path, XRL_PATH_MAX);
}

static bool find_active_runtime(char* out_path) {
	char env[XRL_PATH_MAX];
	char dir[XRL_PATH_MAX];

	if      (xrl_prop_get("XDG_CONFIG_HOME", true, env, sizeof(env))) xrl_fmt(dir, sizeof(dir), "%s", env);
	else if (xrl_prop_get("HOME",            true, env, sizeof(env))) xrl_fmt(dir, sizeof(dir), "%s/.config", env);
	else                                                              dir[0] = '\0';
	if (dir[0] && find_active_runtime_in(dir, out_path, XRL_PATH_MAX)) return true;

	find_runtime_ctx_t ctx = { .path = out_path };
	for_each_dir(xrl_prop_get("XDG_CONFIG_DIRS", true, env, sizeof(env)) ? env : XRL_FALLBACK_CONFIG_DIRS, "", on_config_dir, &ctx);
	if (ctx.found) return true;

	if (find_active_runtime_in(XRL_SYSCONFDIR, out_path, XRL_PATH_MAX)) return true;
#if defined(XRL_EXTRASYSCONFDIR)
	if (find_active_runtime_in(XRL_EXTRASYSCONFDIR, out_path, XRL_PATH_MAX)) return true;
#endif
	return false;
}
#endif

///////////////////////////////////////////

#if defined(__ANDROID__) || defined(__APPLE__)
static bool find_global_runtime(char* out_path) {
#if defined(__ANDROID__)
	static const char* prefixes[] = { "/product", "/odm", "/oem", "/vendor", "/system" };
	const char*        subdir     = "/etc/openxr/";
#else
	static const char* prefixes[] = { "/usr/local/share" };
	const char*        subdir     = "/openxr/";
#endif
	for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
		xrl_fmt(out_path, XRL_PATH_MAX, "%s%s" XRL_MAJOR_STR "/active_runtime." XRL_ARCH_ABI ".json", prefixes[i], subdir);
		if (xrl_path_exists(out_path)) return true;
		xrl_fmt(out_path, XRL_PATH_MAX, "%s%s" XRL_MAJOR_STR "/active_runtime.json", prefixes[i], subdir);
		if (xrl_path_exists(out_path)) return true;
	}
	return false;
}
#endif

///////////////////////////////////////////

static void find_runtime(xrl_manifests_t* ref_list, const char* command) {
	char path[XRL_PATH_MAX];
	if (xrl_prop_get("XR_RUNTIME_JSON", true, path, sizeof(path))) {
		xrl_log(xrl_sev_info, command, "Using the XR_RUNTIME_JSON override runtime %s", path);
		parse_file(ref_list, xrl_manifest_runtime, path);
		return;
	}

#if defined(_WIN32)
	char reg_path[XRL_PATH_MAX];
	if (!xrl_reg_active_runtime(reg_path, sizeof(reg_path))) {
		xrl_log(xrl_sev_error, command, "Failed to find the active runtime in the registry");
		return;
	}
	if (!xrl_path_is_file(reg_path) || !ends_with_json(reg_path) || !xrl_path_absolute(reg_path, path, sizeof(path))) {
		xrl_log(xrl_sev_error, command, "The registry's active runtime %s is not a JSON file that exists", reg_path);
		return;
	}
#elif defined(__ANDROID__) || defined(__APPLE__)
	#if defined(__ANDROID__)
	if (xrl_android_find_runtime(ref_list)) return;
	#endif
	if (!find_global_runtime(path)) {
		xrl_log(xrl_sev_error, command, "Failed to determine the active runtime file path for this environment");
		return;
	}
#else
	if (!find_active_runtime(path)) {
		xrl_log(xrl_sev_error, command, "Failed to determine the active runtime file path for this environment");
		return;
	}
#endif
	xrl_log(xrl_sev_info, command, "Using the active runtime %s", path);
	parse_file(ref_list, xrl_manifest_runtime, path);
}

///////////////////////////////////////////

static void find_layers(xrl_manifests_t* ref_list, xrl_manifest_ type) {
	find_ctx_t  ctx      = { .list = ref_list, .type = type };
	const char* relative = type == xrl_manifest_layer_implicit
		? "openxr/" XRL_MAJOR_STR "/api_layers/implicit.d"
		: "openxr/" XRL_MAJOR_STR "/api_layers/explicit.d";

	char override[XRL_PATH_MAX];
	bool override_active = false;
	if (type == xrl_manifest_layer_explicit) {
#if !defined(_WIN32)
		if (geteuid() == getuid() && getegid() == getgid())
#endif
		override_active = xrl_prop_get("XR_API_LAYER_PATH", true, override, sizeof(override));
	}

	if (override_active) {
		for_each_dir(override, "", on_layer_dir, &ctx);
	} else {
#if defined(_WIN32)
		xrl_reg_layers(type == xrl_manifest_layer_implicit, on_manifest_file, &ctx);
#elif defined(__ANDROID__)
		static const char* prefixes[] = { "/product/etc", "/odm/etc", "/oem/etc", "/vendor/etc", "/system/etc" };
		for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) for_each_dir(prefixes[i], relative, on_layer_dir, &ctx);
#else
		char env[XRL_PATH_MAX];
		for_each_dir(xrl_prop_get("XDG_CONFIG_DIRS", true, env, sizeof(env)) ? env : XRL_FALLBACK_CONFIG_DIRS, relative, on_layer_dir, &ctx);
		for_each_dir(XRL_SYSCONFDIR, relative, on_layer_dir, &ctx);
	#if defined(XRL_EXTRASYSCONFDIR)
		for_each_dir(XRL_EXTRASYSCONFDIR, relative, on_layer_dir, &ctx);
	#endif
		for_each_dir(xrl_prop_get("XDG_DATA_DIRS", true, env, sizeof(env)) ? env : XRL_FALLBACK_DATA_DIRS, relative, on_layer_dir, &ctx);
		if (xrl_prop_get("XDG_DATA_HOME", true, env, sizeof(env))) {
			for_each_dir(env, relative, on_layer_dir, &ctx);
		} else if (xrl_prop_get("HOME", true, env, sizeof(env))) {
			char home_relative[256];
			xrl_fmt(home_relative, sizeof(home_relative), ".local/share/%s", relative);
			for_each_dir(env, home_relative, on_layer_dir, &ctx);
		}
#endif
	}
	(void)relative;

#if defined(__ANDROID__)
	xrl_android_find_layers(ref_list, type);
#endif
}

///////////////////////////////////////////

void xrl_manifests_find(xrl_manifests_t* ref_list, xrl_manifest_ type, const char* command) {
	if (type == xrl_manifest_runtime) find_runtime(ref_list, command);
	else                              find_layers (ref_list, type);
}
