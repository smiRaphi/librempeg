;*****************************************************************************
;* x86-optimized functions for yadif filter
;*
;* Copyright (C) 2006 Michael Niedermayer <michaelni@gmx.at>
;* Copyright (c) 2013 Daniel Kang <daniel.d.kang@gmail.com>
;*
;* This file is part of Librempeg.
;*
;* Librempeg is free software; you can redistribute it and/or
;* modify it under the terms of the GNU Lesser General Public
;* License as published by the Free Software Foundation; either
;* version 2.1 of the License, or (at your option) any later version.
;*
;* Librempeg is distributed in the hope that it will be useful,
;* but WITHOUT ANY WARRANTY; without even the implied warranty of
;* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
;* Lesser General Public License for more details.
;*
;* You should have received a copy of the GNU Lesser General Public
;* License along with FFmpeg; if not, write to the Free Software
;* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
;******************************************************************************

%include "libavutil/x86/x86util.asm"

SECTION_RODATA

pb_1: times 16 db 1
pw_1: times  8 dw 1

SECTION .text

%macro CHECK 2
    movu      m2, [curq+t1+%1]
    movu      m3, [curq+t0+%2]
    mova      m4, m2
    mova      m5, m2
    pxor      m4, m3
    pavgb     m5, m3
    pand      m4, [pb_1]
    psubusb   m5, m4
    RSHIFT    m5, 1
    punpcklbw m5, m7
    mova      m4, m2
    psubusb   m2, m3
    psubusb   m3, m4
    pmaxub    m2, m3
    mova      m3, m2
    mova      m4, m2
    RSHIFT    m3, 1
    RSHIFT    m4, 2
    punpcklbw m2, m7
    punpcklbw m3, m7
    punpcklbw m4, m7
    paddw     m2, m3
    paddw     m2, m4
%endmacro

%macro CHECK1 0
    mova    m3, m0
    pcmpgtw m3, m2
    pminsw  m0, m2
    mova    m6, m3
    pand    m5, m3
    pandn   m3, m1
    por     m3, m5
    mova    m1, m3
%endmacro

%macro CHECK2 0
    paddw   m6, [pw_1]
    psllw   m6, 14
    paddsw  m2, m6
    mova    m3, m0
    pcmpgtw m3, m2
    pminsw  m0, m2
    pand    m5, m3
    pandn   m3, m1
    por     m3, m5
    mova    m1, m3
%endmacro

%macro LOAD 2
    movh      %1, %2
    punpcklbw %1, m7
%endmacro

%macro FILTER 3
.loop%1:
    pxor         m7, m7
    LOAD         m0, [curq+t1]
    LOAD         m1, [curq+t0]
    LOAD         m2, [%2]
    LOAD         m3, [%3]
    mova         m4, m3
    paddw        m3, m2
    psraw        m3, 1
    mova   [rsp+ 0], m0
    mova   [rsp+16], m3
    mova   [rsp+32], m1
    psubw        m2, m4
    ABS1         m2, m4
    LOAD         m3, [prevq+t1]
    LOAD         m4, [prevq+t0]
    psubw        m3, m0
    psubw        m4, m1
    ABS1         m3, m5
    ABS1         m4, m5
    paddw        m3, m4
    psrlw        m2, 1
    psrlw        m3, 1
    pmaxsw       m2, m3
    LOAD         m3, [nextq+t1]
    LOAD         m4, [nextq+t0]
    psubw        m3, m0
    psubw        m4, m1
    ABS1         m3, m5
    ABS1         m4, m5
    paddw        m3, m4
    psrlw        m3, 1
    pmaxsw       m2, m3
    mova   [rsp+48], m2

    paddw        m1, m0
    paddw        m0, m0
    psubw        m0, m1
    psrlw        m1, 1
    ABS1         m0, m2

    movu         m2, [curq+t1-1]
    movu         m3, [curq+t0-1]
    mova         m4, m2
    psubusb      m2, m3
    psubusb      m3, m4
    pmaxub       m2, m3
    mova         m3, m2
    psrldq       m3, 2
    punpcklbw    m2, m7
    punpcklbw    m3, m7
    paddw        m0, m2
    paddw        m0, m3
    psubw        m0, [pw_1]

    CHECK -2, 0
    CHECK1
    CHECK -3, 1
    CHECK2
    CHECK 0, -2
    CHECK1
    CHECK 1, -3
    CHECK2

    mova         m6, [rsp+48]
    cmp   DWORD r8m, 2
    jge .end%1
    LOAD         m2, [%2+t1*2]
    LOAD         m4, [%3+t1*2]
    LOAD         m3, [%2+t0*2]
    LOAD         m5, [%3+t0*2]
    paddw        m2, m4
    paddw        m3, m5
    psrlw        m2, 1
    psrlw        m3, 1
    mova         m4, [rsp+ 0]
    mova         m5, [rsp+16]
    mova         m7, [rsp+32]
    psubw        m2, m4
    psubw        m3, m7
    mova         m0, m5
    psubw        m5, m4
    psubw        m0, m7
    mova         m4, m2
    pminsw       m2, m3
    pmaxsw       m3, m4
    pmaxsw       m2, m5
    pminsw       m3, m5
    pmaxsw       m2, m0
    pminsw       m3, m0
    pxor         m4, m4
    pmaxsw       m6, m3
    psubw        m4, m2
    pmaxsw       m6, m4

.end%1:
    mova         m2, [rsp+16]
    mova         m3, m2
    psubw        m2, m6
    paddw        m3, m6
    pmaxsw       m1, m2
    pminsw       m1, m3
    packuswb     m1, m1

    movh     [dstq], m1
    add        dstq, mmsize/2
    add       prevq, mmsize/2
    add        curq, mmsize/2
    add       nextq, mmsize/2
    sub   DWORD r4m, mmsize/2
    jg .loop%1
%endmacro

%macro YADIF 0
%if ARCH_X86_32
cglobal yadif_filter_line, 4, 6, 8, 80, dst, prev, cur, next, w, prefs, \
                                        mrefs, parity, mode
%else
cglobal yadif_filter_line, 4, 7, 8, 80, dst, prev, cur, next, w, prefs, \
                                        mrefs, parity, mode
%endif
%if ARCH_X86_32
    mov            r4, r5mp
    mov            r5, r6mp
    DECLARE_REG_TMP 4,5
%else
    movsxd         r5, DWORD r5m
    movsxd         r6, DWORD r6m
    DECLARE_REG_TMP 5,6
%endif

    cmp DWORD paritym, 0
    je .parity0
    FILTER 1, prevq, curq
    jmp .ret

.parity0:
    FILTER 0, curq, nextq

.ret:
    RET
%endmacro

INIT_XMM ssse3
YADIF
INIT_XMM sse2
YADIF
