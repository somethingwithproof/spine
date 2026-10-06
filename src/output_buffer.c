/* Copyright (C) 2004-2026 The Cacti Group.
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
