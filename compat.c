/*
 ex: set tabstop=4 shiftwidth=4 autoindent:
 +-------------------------------------------------------------------------+
 | Copyright (C) 2004-2026 The Cacti Group                                 |
 |                                                                         |
 | This program is free software; you can redistribute it and/or           |
 | modify it under the terms of the GNU Lesser General Public              |
 | License as published by the Free Software Foundation; either            |
 | version 2.1 of the License, or (at your option) any later version.     |
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
 | spine: a backend data gatherer for cacti                                |
 +-------------------------------------------------------------------------+
*/

/*
 * Fallback implementations of strlcpy / strlcat for platforms that
 * lack them (Linux glibc < 2.38, Solaris, AIX).  The semantics match
 * the OpenBSD originals: return value is the total length that
 * *would* have been produced, enabling truncation detection.
 */

#include <string.h>
#include "compat.h"

#ifndef HAVE_STRLCPY
size_t spine_strlcpy(char *dst, const char *src, size_t size) {
	size_t srclen = strlen(src);

	if (size > 0) {
		size_t copylen = (srclen >= size) ? size - 1 : srclen;
		memcpy(dst, src, copylen);
		dst[copylen] = '\0';
	}

	return srclen;
}
#endif

#ifndef HAVE_STRLCAT
size_t spine_strlcat(char *dst, const char *src, size_t size) {
	size_t dstlen = 0;
	size_t srclen = strlen(src);

	/* find existing NUL, but do not read past size bytes */
	while (dstlen < size && dst[dstlen] != '\0') {
		dstlen++;
	}

	/* dst is not NUL-terminated within size bytes */
	if (dstlen == size) {
		return size + srclen;
	}

	if (srclen < size - dstlen) {
		memcpy(dst + dstlen, src, srclen + 1);
	} else {
		memcpy(dst + dstlen, src, size - dstlen - 1);
		dst[size - 1] = '\0';
	}

	return dstlen + srclen;
}
#endif
