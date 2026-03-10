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
 * compat.h -- platform portability shims for Solaris, AIX, FreeBSD,
 * macOS, and Linux.
 *
 * Include after system headers; include before spine headers.
 */

#ifndef SPINE_COMPAT_H
#define SPINE_COMPAT_H

#include "config/config.h"

/* strlcpy / strlcat -------------------------------------------------
 * BSD and macOS provide these natively.  Linux, Solaris, and AIX
 * typically do not.  configure.ac probes for them; if absent we
 * supply our own implementations (in compat.c).
 */
#ifndef HAVE_STRLCPY
size_t spine_strlcpy(char *dst, const char *src, size_t size);
#define strlcpy spine_strlcpy
#endif

#ifndef HAVE_STRLCAT
size_t spine_strlcat(char *dst, const char *src, size_t size);
#define strlcat spine_strlcat
#endif

/* pthread_setname_np ------------------------------------------------
 * Signature differs across platforms; Solaris and AIX lack it entirely.
 * Wrapping behind a single macro avoids #ifdef at every call site.
 */
#if defined(HAVE_PTHREAD_SETNAME_NP)
#  if defined(__APPLE__)
     /* macOS: sets name of the *calling* thread only */
#    define spine_thread_setname(t, name) pthread_setname_np(name)
#  elif defined(__linux__)
     /* Linux: takes (pthread_t, const char *) */
#    define spine_thread_setname(t, name) pthread_setname_np((t), (name))
#  else
     /* Other platforms with pthread_setname_np (e.g. newer FreeBSD) */
#    define spine_thread_setname(t, name) pthread_setname_np((t), (name))
#  endif
#elif defined(HAVE_PTHREAD_SET_NAME_NP)
   /* Older FreeBSD uses pthread_set_name_np */
#  define spine_thread_setname(t, name) pthread_set_name_np((t), (name))
#else
   /* Solaris, AIX, or anything without thread naming -- no-op */
#  define spine_thread_setname(t, name) ((void)0)
#endif

/* GCC/Clang branch-prediction hints ---------------------------------
 * xlc (AIX) and Sun Studio do not support __builtin_expect.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define SPINE_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define SPINE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#  define SPINE_LIKELY(x)   (x)
#  define SPINE_UNLIKELY(x) (x)
#endif

/* Portable unused-variable annotation --------------------------------
 * spine.h already has UNUSED_PARAMETER / UNUSED_VARIABLE macros that
 * cast to void.  This attribute form silences the warning earlier,
 * at declaration time, on compilers that support it.
 */
#if defined(__GNUC__) || defined(__clang__)
#  define SPINE_UNUSED __attribute__((unused))
#else
#  define SPINE_UNUSED
#endif

/* Diagnostic pragma portability -------------------------------------
 * GCC 4.6+ and Clang support _Pragma-based push/pop.  Sun Studio
 * and xlc ignore unknown pragmas silently, so the wrapper expands
 * to nothing on those toolchains.
 */
#if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#  define SPINE_DIAG_PUSH       _Pragma("GCC diagnostic push")
#  define SPINE_DIAG_POP        _Pragma("GCC diagnostic pop")
#  define SPINE_DIAG_IGNORE(w)  _Pragma(#w)
#elif defined(__clang__)
#  define SPINE_DIAG_PUSH       _Pragma("clang diagnostic push")
#  define SPINE_DIAG_POP        _Pragma("clang diagnostic pop")
#  define SPINE_DIAG_IGNORE(w)  _Pragma(#w)
#else
#  define SPINE_DIAG_PUSH
#  define SPINE_DIAG_POP
#  define SPINE_DIAG_IGNORE(w)
#endif

#endif /* SPINE_COMPAT_H */
