// SPDX-License-Identifier: Apache-2.0 OR MIT
// Loader logging: stderr, stdout via XR_LOADER_DEBUG, logcat, the Windows
// debugger, and any XR_EXT_debug_utils messengers the application created.

#include "loader.h"

#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__)
	#include <android/log.h>
#elif defined(_WIN32)
	#include <windows.h>
#endif

typedef struct log_messenger_t {
	XrDebugUtilsMessengerEXT             handle;
	PFN_xrDebugUtilsMessengerCallbackEXT callback;
	void*                                user_data;
	XrDebugUtilsMessageSeverityFlagsEXT  severities;
	XrDebugUtilsMessageTypeFlagsEXT      types;
} log_messenger_t;

static struct {
	bool             ready;
	bool             quiet;          // XR_LOADER_DEBUG=none
	uint32_t         stdout_sevs;    // From XR_LOADER_DEBUG's level
	log_messenger_t* messengers;
	int32_t          messenger_count;
	int32_t          messenger_capacity;
} g_log;

///////////////////////////////////////////

static void log_init(void) {
	char level[16];
	if (!xrl_prop_get("XR_LOADER_DEBUG", false, level, sizeof(level))) level[0] = '\0';

	uint32_t sevs = 0;
	if      (strcmp(level, "error") == 0) sevs = xrl_sev_error;
	else if (strcmp(level, "warn")  == 0) sevs = xrl_sev_error | xrl_sev_warn;
	else if (strcmp(level, "info")  == 0) sevs = xrl_sev_error | xrl_sev_warn | xrl_sev_info;
	else if (strcmp(level, "all")   == 0 || strcmp(level, "verbose") == 0) sevs = xrl_sev_error | xrl_sev_warn | xrl_sev_info | xrl_sev_verbose;

	g_log.quiet       = strcmp(level, "none") == 0;
	g_log.stdout_sevs = sevs;
	g_log.ready       = true;
}

///////////////////////////////////////////

static void log_message(xrl_sev_ sev, XrDebugUtilsMessageTypeFlagsEXT type, const char* id, const char* command, const char* fmt, va_list args) {
	if (!g_log.ready) log_init();

	bool to_messengers = g_log.messenger_count > 0;
	bool to_console    = (g_log.stdout_sevs & sev) || (!g_log.quiet && sev == xrl_sev_error);
#if defined(__ANDROID__)
	to_console = to_console || !g_log.quiet;
#elif defined(_WIN32)
	to_console = to_console || sev == xrl_sev_error;
#endif
	if (!to_console && !to_messengers) return;

	char message[1024];
	xrl_vfmt(message, sizeof(message), fmt, args);

	if (to_console) {
		const char* sev_name  = sev == xrl_sev_error ? "Error" : sev == xrl_sev_warn ? "Warning" : sev == xrl_sev_info ? "Info" : "Verbose";
		const char* type_name = type == XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT ? "SPEC" : "GENERAL";
		char line[1280];
		xrl_fmt(line, sizeof(line), "%s [%s | %s | %s] : %s\n", sev_name, type_name, command, id, message);

		if (g_log.stdout_sevs & sev)              xrl_console_write(false, line);
		if (!g_log.quiet && sev == xrl_sev_error) xrl_console_write(true,  line);
#if defined(__ANDROID__)
		if (!g_log.quiet) {
			int32_t prio = sev == xrl_sev_error ? ANDROID_LOG_ERROR : sev == xrl_sev_warn ? ANDROID_LOG_WARN : sev == xrl_sev_info ? ANDROID_LOG_INFO : ANDROID_LOG_VERBOSE;
			__android_log_write(prio, "OpenXR-Loader", line);
		}
#elif defined(_WIN32)
		if (sev == xrl_sev_error) OutputDebugStringA(line);
#endif
	}

	if (to_messengers) {
		XrDebugUtilsMessengerCallbackDataEXT data = {
			.type         = XR_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT,
			.messageId    = id,
			.functionName = command,
			.message      = message,
		};
		xrl_log_submit((XrDebugUtilsMessageSeverityFlagsEXT)sev, type, &data);
	}
}

///////////////////////////////////////////

void xrl_log(xrl_sev_ sev, const char* command, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_message(sev, XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, "OpenXR-Loader", command, fmt, args);
	va_end(args);
}

///////////////////////////////////////////

void xrl_log_vuid(const char* vuid, const char* command, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	log_message(xrl_sev_error, XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT, vuid, command, fmt, args);
	va_end(args);
}

///////////////////////////////////////////

void xrl_log_add_messenger(XrDebugUtilsMessengerEXT messenger, const XrDebugUtilsMessengerCreateInfoEXT* info) {
	xrl_log_lock();
	if (g_log.messenger_count == g_log.messenger_capacity) {
		int32_t          capacity   = g_log.messenger_capacity ? g_log.messenger_capacity * 2 : 4;
		log_messenger_t* messengers = (log_messenger_t*)realloc(g_log.messengers, (size_t)capacity * sizeof(log_messenger_t));
		if (messengers == NULL) { xrl_log_unlock(); return; }
		g_log.messengers         = messengers;
		g_log.messenger_capacity = capacity;
	}
	g_log.messengers[g_log.messenger_count++] = (log_messenger_t){
		.handle     = messenger,
		.callback   = info->userCallback,
		.user_data  = info->userData,
		.severities = info->messageSeverities,
		.types      = info->messageTypes,
	};
	xrl_log_unlock();
}

///////////////////////////////////////////

void xrl_log_remove_messenger(XrDebugUtilsMessengerEXT messenger) {
	xrl_log_lock();
	for (int32_t i = 0; i < g_log.messenger_count; i++) {
		if (g_log.messengers[i].handle != messenger) continue;
		g_log.messengers[i] = g_log.messengers[--g_log.messenger_count];
		break;
	}
	xrl_log_unlock();
}

///////////////////////////////////////////

void xrl_log_clear_messengers(void) {
	xrl_log_lock();
	free(g_log.messengers);
	g_log.messengers         = NULL;
	g_log.messenger_count    = 0;
	g_log.messenger_capacity = 0;
	xrl_log_unlock();
}

///////////////////////////////////////////

void xrl_log_submit(XrDebugUtilsMessageSeverityFlagsEXT sev, XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT* data) {
	xrl_log_lock();
	for (int32_t i = 0; i < g_log.messenger_count; i++) {
		const log_messenger_t* m = &g_log.messengers[i];
		if ((m->severities & sev) && (m->types & types)) m->callback(sev, types, data, m->user_data);
	}
	xrl_log_unlock();
}
