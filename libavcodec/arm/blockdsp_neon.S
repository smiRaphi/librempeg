/*
 * ARM NEON optimised block functions
 * Copyright (c) 2008 Mans Rullgard <mans@mansr.com>
 *
 * This file is part of Librempeg
 *
 * Librempeg is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Librempeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with Librempeg; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "libavutil/arm/asm.S"

function ff_clear_block_neon, export=1
        vmov.i16        q0,  #0
        .rept           8
        vst1.16         {q0}, [r0,:128]!
        .endr
        bx              lr
endfunc

function ff_clear_blocks_neon, export=1
        vmov.i16        q0,  #0
        .rept           8*6
        vst1.16         {q0}, [r0,:128]!
        .endr
        bx              lr
endfunc
