/**************************************************************************
 * 
 * Copyright 2008 VMware, Inc.
 * All Rights Reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/**
 * @file
 * Gallium configuration defines.
 * 
 * This header file sets several defines based on the compiler, processor 
 * architecture, and operating system being used. These defines should be used 
 * throughout the code to facilitate porting to new platforms. It is likely that 
 * this file is auto-generated by an autoconf-like tool at some point, as some 
 * things cannot be determined by pre-defined environment alone. 
 * 
 * See also:
 * - http://gcc.gnu.org/onlinedocs/cpp/Common-Predefined-Macros.html
 * - echo | gcc -dM -E - | sort
 * - http://msdn.microsoft.com/en-us/library/b0084kay.aspx
 * 
 * @author José Fonseca <jfonseca@vmware.com>
 */

#ifndef UTIL_DETECT_ARCH_H_
#define UTIL_DETECT_ARCH_H_

#include <limits.h>

#include "util/detect_cc.h"

/*
 * Processor architecture
 */

#if defined(__i386__) /* gcc */ || defined(_M_IX86) /* msvc */ || defined(_X86_) || defined(__386__) || defined(i386) || defined(__i386) /* Sun cc */
#define DETECT_ARCH_X86 1
#endif

#if defined(__x86_64__) /* gcc */ || defined(_M_X64) /* msvc */ || defined(_M_AMD64) /* msvc */ || defined(__x86_64) /* Sun cc */
#define DETECT_ARCH_X86_64 1
#endif

#if DETECT_ARCH_X86 || DETECT_ARCH_X86_64
#if DETECT_CC_GCC && !defined(__SSE2__)
/* #warning SSE2 support requires -msse -msse2 compiler options */
#else
#define DETECT_ARCH_SSE 1
#endif
#endif

#ifndef __NO_FPRS__
#if defined(__ppc__) || defined(__ppc64__) || defined(__PPC__) || defined(__PPC64__)
#define DETECT_ARCH_PPC 1
#if defined(__ppc64__) || defined(__PPC64__)
#define DETECT_ARCH_PPC_64 1
#endif
#endif
#endif

#if defined(__s390x__)
#define DETECT_ARCH_S390 1
#endif

#if defined(__arm__)
#define DETECT_ARCH_ARM 1
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define DETECT_ARCH_AARCH64 1
#endif

#if defined(__mips64) && defined(__LP64__)
#define DETECT_ARCH_MIPS64 1
#endif

#if defined(__mips__)
#define DETECT_ARCH_MIPS 1
#endif

#ifndef DETECT_ARCH_X86
#define DETECT_ARCH_X86 0
#endif

#ifndef DETECT_ARCH_X86_64
#define DETECT_ARCH_X86_64 0
#endif

#ifndef DETECT_ARCH_SSE
#define DETECT_ARCH_SSE 0
#endif

#ifndef DETECT_ARCH_PPC
#define DETECT_ARCH_PPC 0
#endif

#ifndef DETECT_ARCH_PPC_64
#define DETECT_ARCH_PPC_64 0
#endif

#ifndef DETECT_ARCH_S390
#define DETECT_ARCH_S390 0
#endif

#ifndef DETECT_ARCH_ARM
#define DETECT_ARCH_ARM 0
#endif

#ifndef DETECT_ARCH_AARCH64
#define DETECT_ARCH_AARCH64 0
#endif

#ifndef DETECT_ARCH_MIPS64
#define DETECT_ARCH_MIPS64 0
#endif

#ifndef DETECT_ARCH_MIPS
#define DETECT_ARCH_MIPS 0
#endif

#endif /* UTIL_DETECT_ARCH_H_ */
