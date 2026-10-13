// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// flipper-doom: minimal stand-in for the DOS header. Only referenced by code
// paths for EMS/XMS that are never taken on this platform.
#ifndef FD_COMPAT_DOS_H
#define FD_COMPAT_DOS_H
#include <stdint.h>
union REGS {
    struct { uint16_t ax, bx, cx, dx, si, di, cflag; } w;
    struct { uint8_t al, ah, bl, bh, cl, ch, dl, dh; } h;
};
struct SREGS { uint16_t es, cs, ss, ds; };
static inline int int86(int n, union REGS* in, union REGS* out) { (void)n; (void)in; out->h.ah = 0xff; return 0; }
#endif
