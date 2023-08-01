/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef D3D12_WGL_PUBLIC_H
#define D3D12_WGL_PUBLIC_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pipe_resource;
struct pipe_screen;
struct pipe_context;
struct sw_winsys;

struct pipe_screen *
d3d12_wgl_create_screen(struct sw_winsys *winsys,
                        HDC hDC);

void
d3d12_wgl_present(struct pipe_screen *screen,
                  struct pipe_context *context,
                  struct pipe_resource *res,
                  HDC hDC);

unsigned
d3d12_wgl_get_pfd_flags(struct pipe_screen *screen);

struct stw_winsys_framebuffer *
d3d12_wgl_create_framebuffer(struct pipe_screen *screen,
                             HWND hWnd,
                             int iPixelFormat);

#ifdef __cplusplus
}
#endif

#endif
