// SPDX-License-Identifier: Apache-2.0 OR MIT
// OS abstraction: dynamic libraries, environment, files and paths, locks.

#include "loader.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

#include <windows.h>

///////////////////////////////////////////
// Windows
///////////////////////////////////////////

static SRWLOCK g_lock     = SRWLOCK_INIT;
static SRWLOCK g_log_lock = SRWLOCK_INIT;

void xrl_lock      (void) { AcquireSRWLockExclusive(&g_lock); }
void xrl_unlock    (void) { ReleaseSRWLockExclusive(&g_lock); }
void xrl_log_lock  (void) { AcquireSRWLockExclusive(&g_log_lock); }
void xrl_log_unlock(void) { ReleaseSRWLockExclusive(&g_log_lock); }

///////////////////////////////////////////

static bool to_wide(const char* utf8, wchar_t* out_wide, int32_t wide_count) {
	return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out_wide, wide_count) > 0;
}

static bool to_utf8(const wchar_t* wide, char* out_utf8, size_t utf8_size) {
	return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out_utf8, (int)utf8_size, NULL, NULL) > 0;
}

///////////////////////////////////////////

xrl_lib_t xrl_lib_open(const char* path) {
	wchar_t wpath[XRL_PATH_MAX];
	if (!to_wide(path, wpath, XRL_PATH_MAX)) return NULL;

	HMODULE lib = LoadLibraryW(wpath);
	// A library whose own dependencies sit beside it needs the DLL-dir search path
	if (lib == NULL && GetLastError() == ERROR_MOD_NOT_FOUND) {
		DWORD attrib = GetFileAttributesW(wpath);
		if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY))
			lib = LoadLibraryExW(wpath, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
	}
	return (xrl_lib_t)lib;
}

void  xrl_lib_close(xrl_lib_t lib)                   { FreeLibrary((HMODULE)lib); }
void* xrl_lib_sym  (xrl_lib_t lib, const char* name) { return (void*)GetProcAddress((HMODULE)lib, name); }

void xrl_lib_error(const char* path, char* out_msg, size_t msg_size) {
	DWORD   err = GetLastError();
	wchar_t wmsg[512];
	char    msg [1024] = "(unknown)";
	if (FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), wmsg, 512, NULL))
		to_utf8(wmsg, msg, sizeof(msg));
	size_t len = strlen(msg);
	while (len > 0 && (msg[len - 1] == '\r' || msg[len - 1] == '\n')) msg[--len] = '\0';
	xrl_fmt(out_msg, msg_size, "Failed to open dynamic library %s with error %u: %s", path, (uint32_t)err, msg);
}

///////////////////////////////////////////

bool xrl_is_elevated(void) {
	static int32_t elevated = -1;
	if (elevated >= 0) return elevated == 1;

	elevated = 0;
	HANDLE token;
	if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_QUERY_SOURCE, &token)) {
		uint8_t buffer[SECURITY_MAX_SID_SIZE + sizeof(DWORD)] = {0};
		DWORD   size;
		if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &size)) {
			const TOKEN_MANDATORY_LABEL* label = (const TOKEN_MANDATORY_LABEL*)buffer;
			if (label->Label.Sid != NULL) {
				DWORD level = *GetSidSubAuthority(label->Label.Sid, *GetSidSubAuthorityCount(label->Label.Sid) - 1);
				elevated = level > SECURITY_MANDATORY_MEDIUM_RID ? 1 : 0;
			}
		}
		CloseHandle(token);
	}
	return elevated == 1;
}

///////////////////////////////////////////

bool xrl_env_get(const char* name, bool secure, char* out_value, size_t value_size) {
	wchar_t wname [256];
	wchar_t wvalue[XRL_PATH_MAX];
	if (!to_wide(name, wname, 256)) return false;

	DWORD len = GetEnvironmentVariableW(wname, wvalue, XRL_PATH_MAX);
	if (len == 0 || len >= XRL_PATH_MAX || !to_utf8(wvalue, out_value, value_size)) return false;

	// Medium integrity processes can set the environment of high integrity ones
	if (secure && xrl_is_elevated()) {
		xrl_log(xrl_sev_error, "", "!!! WARNING !!! Environment variable %s is being ignored due to running from an elevated context. The value '%s' will NOT be used.", name, out_value);
		return false;
	}
	return true;
}

bool xrl_env_is_set(const char* name) {
	wchar_t wname[256];
	return to_wide(name, wname, 256) && GetEnvironmentVariableW(wname, NULL, 0) != 0;
}

///////////////////////////////////////////

// Straight to the handle, so static CRT builds don't carry stdio's buffering
void xrl_console_write(bool to_stderr, const char* text) {
	HANDLE handle = GetStdHandle(to_stderr ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
	DWORD  written;
	if (handle != NULL && handle != INVALID_HANDLE_VALUE) WriteFile(handle, text, (DWORD)strlen(text), &written, NULL);
}

///////////////////////////////////////////

char* xrl_file_read(const char* path, int32_t* out_size) {
	wchar_t wpath[XRL_PATH_MAX];
	if (!to_wide(path, wpath, XRL_PATH_MAX)) return NULL;

	HANDLE file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) return NULL;

	LARGE_INTEGER size;
	char*         data = NULL;
	DWORD         read = 0;
	if (GetFileSizeEx(file, &size) && size.QuadPart < INT32_MAX) {
		data = (char*)malloc((size_t)size.QuadPart + 1);
		if (data && !ReadFile(file, data, (DWORD)size.QuadPart, &read, NULL)) { free(data); data = NULL; }
	}
	CloseHandle(file);
	if (data == NULL) return NULL;
	data[read] = '\0';
	*out_size  = (int32_t)read;
	return data;
}

///////////////////////////////////////////

static DWORD file_attributes(const char* path) {
	wchar_t wpath[XRL_PATH_MAX];
	return to_wide(path, wpath, XRL_PATH_MAX) ? GetFileAttributesW(wpath) : INVALID_FILE_ATTRIBUTES;
}

bool xrl_path_exists (const char* path) { return file_attributes(path) != INVALID_FILE_ATTRIBUTES; }
bool xrl_path_is_file(const char* path) {
	DWORD attrib = file_attributes(path);
	return attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY);
}

bool xrl_path_is_abs(const char* path) {
	bool sep0 = path[0] == '\\' || path[0] == '/';
	bool sep1 = path[0] != '\0' && (path[1] == '\\' || path[1] == '/');
	bool sep2 = path[0] != '\0' && path[1] != '\0' && (path[2] == '\\' || path[2] == '/');
	bool drive = ((path[0] >= 'a' && path[0] <= 'z') || (path[0] >= 'A' && path[0] <= 'Z')) && path[1] == ':';
	return (drive && sep2) || (sep0 && sep1);
}

bool xrl_path_absolute(const char* path, char* out_path, size_t path_size) {
	wchar_t wpath[XRL_PATH_MAX];
	wchar_t wfull[XRL_PATH_MAX];
	if (!to_wide(path, wpath, XRL_PATH_MAX)) return false;
	DWORD len = GetFullPathNameW(wpath, XRL_PATH_MAX, wfull, NULL);
	return len > 0 && len < XRL_PATH_MAX && to_utf8(wfull, out_path, path_size);
}

// std::filesystem::canonical misbehaves on UWP, upstream uses the full path here too
bool xrl_path_canonical(const char* path, char* out_path, size_t path_size) {
	return xrl_path_absolute(path, out_path, path_size);
}

///////////////////////////////////////////

void xrl_dir_list(const char* dir, void (*on_file)(void* ctx, const char* path), void* ctx) {
	char    pattern[XRL_PATH_MAX];
	wchar_t wpattern[XRL_PATH_MAX];
	xrl_fmt(pattern, sizeof(pattern), "%s\\*", dir);
	if (!to_wide(pattern, wpattern, XRL_PATH_MAX)) return;

	WIN32_FIND_DATAW data;
	HANDLE find = FindFirstFileW(wpattern, &data);
	if (find == INVALID_HANDLE_VALUE) return;
	do {
		char name[XRL_PATH_MAX];
		char full[XRL_PATH_MAX];
		if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		if (!to_utf8(data.cFileName, name, sizeof(name))) continue;
		xrl_path_join(dir, name, full, sizeof(full));
		on_file(ctx, full);
	} while (FindNextFileW(find, &data));
	FindClose(find);
}

///////////////////////////////////////////

#define XRL_REG_ROOT L"SOFTWARE\\Khronos\\OpenXR\\1"

bool xrl_reg_active_runtime(char* out_path, size_t path_size) {
	wchar_t value[1024];
	DWORD   size = sizeof(value);
	// Without KEY_WOW64_64KEY, 32-bit apps read the 32-bit view, which is what we want
	LSTATUS result = RegGetValueW(HKEY_LOCAL_MACHINE, XRL_REG_ROOT, L"ActiveRuntime", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, value, &size);
	if (result != ERROR_SUCCESS) {
		xrl_log(xrl_sev_warn, "", "Failed to read registry value HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\\ActiveRuntime");
		return false;
	}
	return to_utf8(value, out_path, path_size);
}

static bool reg_layers_in(HKEY hive, const wchar_t* key_name, void (*on_file)(void* ctx, const char* path), void* ctx) {
	HKEY key;
	if (RegOpenKeyExW(hive, key_name, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;

	for (DWORD i = 0;; i++) {
		wchar_t name[1024];
		DWORD   name_size  = 1023;
		DWORD   type;
		DWORD   value      = 0;
		DWORD   value_size = sizeof(value);
		LSTATUS result     = RegEnumValueW(key, i, name, &name_size, NULL, &type, (LPBYTE)&value, &value_size);
		if (result == ERROR_NO_MORE_ITEMS) break;
		if (result != ERROR_SUCCESS && result != ERROR_MORE_DATA) {
			xrl_log(xrl_sev_info, "", "Unexpected error %d enumerating registry layers", (int32_t)result);
			break;
		}

		char path[XRL_PATH_MAX];
		if (!to_utf8(name, path, sizeof(path))) continue;
		if (result != ERROR_SUCCESS || type != REG_DWORD || value_size != sizeof(value)) {
			xrl_log(xrl_sev_info, "", "Skipping invalid registry layer key %s", path);
		} else if (value != 0) {
			xrl_log(xrl_sev_info, "", "Skipping disabled registry layer %s", path);
		} else if (xrl_path_is_file(path)) {
			on_file(ctx, path);
		}
	}
	RegCloseKey(key);
	return true;
}

void xrl_reg_layers(bool implicit, void (*on_file)(void* ctx, const char* path), void* ctx) {
	const wchar_t* key = implicit ? XRL_REG_ROOT L"\\ApiLayers\\Implicit" : XRL_REG_ROOT L"\\ApiLayers\\Explicit";

	// High integrity processes shouldn't act on data medium integrity processes control
	bool found = reg_layers_in(HKEY_LOCAL_MACHINE, key, on_file, ctx);
	if (!xrl_is_elevated()) found = reg_layers_in(HKEY_CURRENT_USER, key, on_file, ctx) || found;
	if (!found) xrl_log(xrl_sev_warn, "", "Failed to read the %s API layer registry key", implicit ? "implicit" : "explicit");
}

#else

///////////////////////////////////////////
// POSIX
///////////////////////////////////////////

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t g_lock     = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

void xrl_lock      (void) { pthread_mutex_lock  (&g_lock); }
void xrl_unlock    (void) { pthread_mutex_unlock(&g_lock); }
void xrl_log_lock  (void) { pthread_mutex_lock  (&g_log_lock); }
void xrl_log_unlock(void) { pthread_mutex_unlock(&g_log_lock); }

///////////////////////////////////////////

xrl_lib_t xrl_lib_open (const char* path)                { return dlopen(path, RTLD_LAZY | RTLD_LOCAL); }
void      xrl_lib_close(xrl_lib_t lib)                   { dlclose(lib); }
void*     xrl_lib_sym  (xrl_lib_t lib, const char* name) { return dlsym(lib, name); }

void xrl_lib_error(const char* path, char* out_msg, size_t msg_size) {
	const char* err = dlerror();
	xrl_fmt(out_msg, msg_size, "%s", err ? err : path);
}

///////////////////////////////////////////

bool xrl_is_elevated(void) {
	return geteuid() != getuid() || getegid() != getgid();
}

///////////////////////////////////////////

#if defined(__ANDROID__)

// Android apps have no meaningful environment, loader properties replace it
bool xrl_env_get   (const char* name, bool secure, char* out_value, size_t value_size) { (void)name; (void)secure; (void)out_value; (void)value_size; return false; }
bool xrl_env_is_set(const char* name) { (void)name; return false; }

#else

bool xrl_env_get(const char* name, bool secure, char* out_value, size_t value_size) {
	const char* value = getenv(name);
	if (value == NULL || value[0] == '\0') return false;

#if defined(__linux__)
	#if defined(__GLIBC__)
	bool insecure = secure && secure_getenv(name) == NULL;
	#else
	bool insecure = secure && xrl_is_elevated();
	#endif
	if (insecure) {
		xrl_log(xrl_sev_error, "", "!!! WARNING !!! Environment variable %s is being ignored due to running with secure execution. The value '%s' will NOT be used.", name, value);
		return false;
	}
#else
	(void)secure; // Upstream's Apple path has no secure_getenv either
#endif
	xrl_fmt(out_value, value_size, "%s", value);
	return true;
}

bool xrl_env_is_set(const char* name) { return getenv(name) != NULL; }

#endif

///////////////////////////////////////////

void xrl_console_write(bool to_stderr, const char* text) {
	fputs(text, to_stderr ? stderr : stdout);
}

///////////////////////////////////////////

char* xrl_file_read(const char* path, int32_t* out_size) {
	FILE* file = fopen(path, "rb");
	if (file == NULL) return NULL;

	char* data = NULL;
	long  size = 0;
	if (fseek(file, 0, SEEK_END) == 0 && (size = ftell(file)) >= 0 && size < INT32_MAX && fseek(file, 0, SEEK_SET) == 0) {
		data = (char*)malloc((size_t)size + 1);
		if (data && fread(data, 1, (size_t)size, file) != (size_t)size) { free(data); data = NULL; }
	}
	fclose(file);
	if (data == NULL) return NULL;
	data[size] = '\0';
	*out_size  = (int32_t)size;
	return data;
}

///////////////////////////////////////////

bool xrl_path_exists (const char* path) { struct stat s; return stat(path, &s) == 0; }
bool xrl_path_is_file(const char* path) { struct stat s; return stat(path, &s) == 0 && S_ISREG(s.st_mode); }
bool xrl_path_is_abs (const char* path) { return path[0] == '/'; }

bool xrl_path_absolute(const char* path, char* out_path, size_t path_size) {
	if (xrl_path_is_abs(path)) {
		xrl_fmt(out_path, path_size, "%s", path);
		return true;
	}
	char cwd[XRL_PATH_MAX];
	if (getcwd(cwd, sizeof(cwd)) == NULL) return false;
	xrl_path_join(cwd, path, out_path, path_size);
	return true;
}

bool xrl_path_canonical(const char* path, char* out_path, size_t path_size) {
	char* resolved = realpath(path, NULL);
	if (resolved == NULL) return false;
	xrl_fmt(out_path, path_size, "%s", resolved);
	free(resolved);
	return true;
}

///////////////////////////////////////////

void xrl_dir_list(const char* dir, void (*on_file)(void* ctx, const char* path), void* ctx) {
	DIR* d = opendir(dir);
	if (d == NULL) return;

	struct dirent* entry;
	while ((entry = readdir(d)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		char full[XRL_PATH_MAX];
		xrl_path_join(dir, entry->d_name, full, sizeof(full));
		on_file(ctx, full);
	}
	closedir(d);
}

#endif

///////////////////////////////////////////
// Shared
///////////////////////////////////////////

static bool is_dir_sep(char c) {
#if defined(_WIN32)
	return c == '/' || c == '\\';
#else
	return c == '/';
#endif
}

void xrl_path_parent(const char* path, char* out_path, size_t path_size) {
	size_t len = strlen(path);
	while (len > 0 && !is_dir_sep(path[len - 1])) len--;
	// Keep a lone root separator, drop a trailing one
	if (len > 1 && is_dir_sep(path[len - 1])) len--;
	xrl_fmt(out_path, path_size, "%.*s", (int)len, path);
}

void xrl_path_join(const char* parent, const char* child, char* out_path, size_t path_size) {
	size_t len = strlen(parent);
	if (xrl_path_is_abs(child) || len == 0) xrl_fmt(out_path, path_size, "%s", child);
	else if (is_dir_sep(parent[len - 1]))   xrl_fmt(out_path, path_size, "%s%s", parent, child);
	else                                    xrl_fmt(out_path, path_size, "%s" XRL_DIR_SEP "%s", parent, child);
}
