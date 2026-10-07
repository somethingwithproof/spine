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
#ifndef SPINE_OUTPUT_BUFFER_H
#define SPINE_OUTPUT_BUFFER_H

#include <stddef.h>

/* Include room for a single oversized tuple after a batch has been flushed,
 * and for the larger destination prefix, suffix, and terminating NUL. */
size_t spine_output_buffer_size(size_t batch_limit, size_t tuple_capacity,
	size_t prefix_length, size_t suffix_length);

/* Preserve the existing threshold rule without overflowing the sum. */
int spine_output_buffer_needs_flush(size_t used, size_t tuple_length, size_t batch_limit);

/* Return zero without modifying the destination if it is unterminated or
 * cannot hold the append plus its terminating NUL. Inputs must not alias. */
int spine_output_buffer_append(char *buffer, size_t capacity,
	const char *part, size_t length);

#endif
