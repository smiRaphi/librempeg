;******************************************************************************
;* MMX/SSE2-optimized functions for the VP6 decoder
;* Copyright (C) 2009  Sebastien Lucas <sebastien.lucas@gmail.com>
;* Copyright (C) 2009  Zuxy Meng <zuxy.meng@gmail.com>
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

cextern pw_64

SECTION .text

%macro DIAG4 6
    movq          m0, [%1+%2]
    movq          m1, [%1+%3]
    punpcklbw     m0, m7
    punpcklbw     m1, m7
    pmullw        m0, m4         ; src[x-8 ] * biweight [0]
    pmullw        m1, m5         ; src[x   ] * biweight [1]
    paddw         m0, m1
    movq          m1, [%1+%4]
    movq          m2, [%1+%5]
    punpcklbw     m1, m7
    punpcklbw     m2, m7
    pmullw        m1, m6         ; src[x+8 ] * biweight [2]
    pmullw        m2, m3         ; src[x+16] * biweight [3]
    paddw         m1, m2
    paddsw        m0, m1
    paddsw        m0, [pw_64]    ; Add 64
    psraw         m0, 7
    packuswb      m0, m0
    movq        [%6], m0
%endmacro

%macro SPLAT4REGS 0
    pshuflw      m4, m3, 0x0
    pshuflw      m5, m3, 0x55
    pshuflw      m6, m3, 0xAA
    pshuflw      m3, m3, 0xFF
    punpcklqdq   m4, m4
    punpcklqdq   m5, m5
    punpcklqdq   m6, m6
    punpcklqdq   m3, m3
%endmacro

; void ff_vp6_filter_diag4_<opt>(uint8_t *dst, uint8_t *src, ptrdiff_t stride,
;                                const int16_t h_weight[4], const int16_t v_weights[4])
INIT_XMM sse2
cglobal vp6_filter_diag4, 5, 7, 8
    mov          r5, rsp         ; backup stack pointer
    and         rsp, ~(mmsize-1) ; align stack
    sub         rsp, 8*11

    sub          r1, r2

    pxor         m7, m7
    movq         m3, [r3]
    SPLAT4REGS

    mov          r3, rsp
    mov          r6, 11
.nextrow:
    DIAG4        r1, -1, 0, 1, 2, r3
    add          r3, 8
    add          r1, r2
    dec          r6
    jnz .nextrow

    movq         m3, [r4]
    SPLAT4REGS

    lea          r3, [rsp+8]
    mov          r6, 8
.nextcol:
    DIAG4        r3, -8, 0, 8, 16, r0
    add          r3, 8
    add          r0, r2
    dec          r6
    jnz .nextcol

    mov         rsp, r5          ; restore stack pointer
    RET
