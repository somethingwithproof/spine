/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.      |
 |                                                                         |
 | This program is distributed in the hope that it will be useful,         |
 | but WITHOUT ANY WARRANTY; without even the implied warranty of          |
 | MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           |
 | GNU Lesser General Public License for more details.                     |
 |                                                                         |
 | You should have received a copy of the GNU Lesser General Public        |
 | License along with this library; if not, write to the Free Software     |
 | Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA           |
 | 02110-1301, USA                                                         |
 |                                                                         |
 +-------------------------------------------------------------------------+
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include "output_buffer.h"

#include <stdint.h>
#include <string.h>

size_t spine_output_buffer_size(size_t batch_limit, size_t tuple_capacity,
	size_t prefix_length, size_t suffix_length) {
	size_t capacity = batch_limit;
	const size_t reserves[] = {tuple_capacity, prefix_length, suffix_length, 1};

	for (size_t i = 0; i < sizeof(reserves) / sizeof(reserves[0]); i++) {
		if (reserves[i] > SIZE_MAX - capacity) {
			return 0;
		}
		capacity += reserves[i];
	}
	return capacity;
}

int spine_output_buffer_needs_flush(size_t used, size_t tuple_length, size_t batch_limit) {
	return used >= batch_limit || tuple_length >= batch_limit - used;
}

int spine_output_buffer_append(char *buffer, size_t capacity,
	const char *part, size_t length) {
	if (buffer == NULL || part == NULL || capacity == 0) {
		return 0;
	}
	const char *end = memchr(buffer, '\0', capacity);
	if (end == NULL) {
		return 0;
	}
	const size_t used = (size_t)(end - buffer);
	if (length >= capacity - used) {
		return 0;
	}
	memcpy(buffer + used, part, length);
	buffer[used + length] = '\0';
	return 1;
}
