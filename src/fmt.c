// SPDX-License-Identifier: Apache-2.0 OR MIT
// A small snprintf covering only the specifiers the loader uses, so builds
// with a static C runtime don't carry the CRT's full printf machinery.

#include "loader.h"

typedef struct fmt_out_t {
	char*  buf;
	size_t size;
	size_t len; // Length the full output needs, which can exceed size
} fmt_out_t;

static void fmt_char(fmt_out_t* ref_out, char c) {
	if (ref_out->len + 1 < ref_out->size) ref_out->buf[ref_out->len] = c;
	ref_out->len++;
}

static void fmt_str(fmt_out_t* ref_out, const char* str, size_t max_len) {
	for (size_t i = 0; i < max_len && str[i]; i++) fmt_char(ref_out, str[i]);
}

static void fmt_uint(fmt_out_t* ref_out, uint32_t value) {
	char    digits[10];
	int32_t count = 0;
	do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
	while (count) fmt_char(ref_out, digits[--count]);
}

///////////////////////////////////////////

int32_t xrl_vfmt(char* out_str, size_t str_size, const char* fmt, va_list args) {
	fmt_out_t out = { .buf = out_str, .size = str_size };
	for (const char* p = fmt; *p; p++) {
		if (*p != '%') { fmt_char(&out, *p); continue; }
		p++;

		size_t max_len = SIZE_MAX;
		if (p[0] == '.' && p[1] == '*') {
			int precision = va_arg(args, int);
			if (precision >= 0) max_len = (size_t)precision;
			p += 2;
		}
		switch (*p) {
		case 's': {
			const char* str = va_arg(args, const char*);
			fmt_str(&out, str ? str : "(null)", max_len);
		} break;
		case 'd': {
			int32_t value = va_arg(args, int32_t);
			if (value < 0) fmt_char(&out, '-');
			fmt_uint(&out, value < 0 ? 0u - (uint32_t)value : (uint32_t)value);
		} break;
		case 'u':  fmt_uint(&out, va_arg(args, uint32_t)); break;
		case '%':  fmt_char(&out, '%'); break;
		case '\0': p--; break;
		default:   fmt_char(&out, '%'); fmt_char(&out, *p); break;
		}
	}
	if (str_size > 0) out_str[out.len < str_size ? out.len : str_size - 1] = '\0';
	return (int32_t)out.len;
}

///////////////////////////////////////////

int32_t xrl_fmt(char* out_str, size_t str_size, const char* fmt, ...) {
	va_list args;
	va_start(args, fmt);
	int32_t len = xrl_vfmt(out_str, str_size, fmt, args);
	va_end(args);
	return len;
}
