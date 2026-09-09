/*
 * str_buffer.c - A str_buffer for building strings without knowing the size.
 *
 * Author: Larry Laffer (larrylaffer130@gmail.com)
 * Copyright (C) 2025 Larry Laffer
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include "str_buffer.h"

#include "../mem/mem.h"
#include "../dprint.h"

#include <stdarg.h>

/**
 * @brief Resize str_buffer storage if current usage and len will not fit in it.
 *
 * @param buf str_buffer to resize
 * @param len len that needs to fit in str_buffer
 * @return 1 on success else 0
 */
static inline int resize_if_required(str_buffer *buf, int len)
{
	if(buf->storage.len + len >= (1 << buf->scaling) * BUFFER_SCALE_PERCENT) {
		char *res = NULL;

		while(buf->storage.len + len
				>= (1 << buf->scaling) * BUFFER_SCALE_PERCENT) {
			++buf->scaling;
		}

		res = pkg_realloc(buf->storage.s, 1 << buf->scaling);
		if(!res) {
			buf->error = 1;
			LM_ERR("oom\n");
			return 0;
		}

		buf->storage.s = res;
	}

	return 1;
}

/**
 * @brief Calculate the strlen after the replacement in format string.
 *
 * @param fmt  format string
 * @param args argument list
 * @return length of string after the replacement plus 1 because of \0
 */
static inline int length_after_replaced(char *fmt, va_list args)
{
	int len = 0;
	va_list tmp_args;

	va_copy(tmp_args, args);
	len = vsnprintf(NULL, 0, fmt, tmp_args) + 1;
	va_end(tmp_args);

	return len;
}

/**
 * @brief Append a given format with replaced values to the str_buffer.
 *
 * @param buf  str_buffer where to append
 * @param src  char* format to append
 * @param len  length of the char*
 * @param args replacement for the format
 * @return 1 on success else 0
 */
static inline int str_buffer_append_varg(
		str_buffer *buf, char *src, int len, va_list args)
{
	va_list tmp_args;
	char *tmp = NULL;
	int replaced_len = 0;

	if(!buf) {
		LM_BUG("Wrong usage passing NULL as the buffer\n");
		return 0;
	}
	if(!src || len == 0) {
		return 1;
	}

	// temp one bigger than original and zero filled, to make sure its zero terminated
	tmp = pkg_malloc(len + 1);
	if(!tmp) {
		buf->error = 1;
		LM_ERR("oom\n");
		return 0;
	}
	memset(tmp, 0, len + 1);
	memcpy(tmp, src, len);

	replaced_len = length_after_replaced(tmp, args);
	if(!resize_if_required(buf, replaced_len)) {
		pkg_free(tmp);
		return 0;
	}

	va_copy(tmp_args, args);
	len = vsnprintf(buf->storage.s + buf->storage.len,
			(1 << buf->scaling) - buf->storage.len, tmp, tmp_args);
	buf->storage.len += len;
	va_end(tmp_args);

	pkg_free(tmp);

	return 1;
}

str_buffer *new_str_buffer(void)
{
	str_buffer *buf = NULL;
	buf = pkg_malloc(sizeof(str_buffer));
	if(!buf) {
		LM_ERR("oom\n");
		return NULL;
	}

	buf->storage.s = pkg_malloc(1 << BUFFER_START_BLOCK_SIZE);
	if(!buf->storage.s) {
		LM_ERR("oom\n");
		pkg_free(buf);
		return NULL;
	}
	memset(buf->storage.s, 0, 1 << BUFFER_START_BLOCK_SIZE);

	buf->storage.len = 0;
	buf->scaling = BUFFER_START_BLOCK_SIZE;
	buf->error = 0;

	return buf;
}

void free_str_buffer(str_buffer *buf)
{
	if(!buf) {
		LM_BUG("Wrong usage passing NULL as the buffer\n");
		return;
	}

	if(buf->storage.s) {
		pkg_free(buf->storage.s);
	}

	pkg_free(buf);
}

int str_buffer_has_error(str_buffer *buf)
{
	if(!buf) {
		LM_BUG("Wrong usage passing NULL as the buffer\n");
		return 0;
	}

	return buf->error;
}

int str_buffer_append_str(str_buffer *buf, str *src)
{
	if(!src) {
		return 1;
	}

	return str_buffer_append_char_ptr(buf, src->s, src->len);
}

int str_buffer_append_char_ptr(str_buffer *buf, char *src, int len)
{
	if(!buf) {
		LM_BUG("Wrong usage passing NULL as the buffer\n");
		return 0;
	}
	if(!src || len == 0) {
		return 1;
	}
	if(!resize_if_required(buf, len)) {
		return 0;
	}

	memcpy(buf->storage.s + buf->storage.len, src, len);
	buf->storage.len += len;

	return 1;
}

int str_buffer_append_str_fmt(str_buffer *buf, str *src, ...)
{
	int res = 0;
	va_list args;

	if(!src) {
		return 1;
	}

	va_start(args, src);
	res = str_buffer_append_varg(buf, src->s, src->len, args);
	va_end(args);

	return res;
}

int str_buffer_append_char_ptr_fmt(str_buffer *buf, char *src, int len, ...)
{
	int res = 0;
	va_list args;

	va_start(args, len);
	res = str_buffer_append_varg(buf, src, len, args);
	va_end(args);

	return res;
}

int str_buffer_append_int(str_buffer *buf, int val)
{
	return str_buffer_append_char_ptr_fmt(buf, "%d", 2, val);
}

int str_buffer_to_str(str_buffer *buf, str *dest)
{
	if(!dest) {
		LM_BUG("Wrong usage passing NULL as the destination\n");
		return 0;
	}

	return str_buffer_to_char_ptr(buf, &dest->s, &dest->len);
}

int str_buffer_to_char_ptr(str_buffer *buf, char **dest, int *len)
{
	if(!buf) {
		LM_BUG("Wrong usage passing NULL as the buffer\n");
		return 0;
	}
	if(!dest) {
		LM_BUG("Wrong usage passing NULL as the destination\n");
		return 0;
	}

	*dest = pkg_malloc(buf->storage.len + 1);
	if(!*dest) {
		LM_ERR("oom\n");
		return 0;
	}
	memset(*dest, 0, buf->storage.len + 1);
	memcpy(*dest, buf->storage.s, buf->storage.len);
	if(len) {
		*len = buf->storage.len;
	}

	return 1;
}
