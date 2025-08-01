/*
 * Copyright (c) 2003-2010 Michael Niedermayer <michaelni@gmx.at>
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

/**
 * @file
 * Accelerated start code search function for start codes common to
 * MPEG-1/2/4 video, VC-1, H.264/5
 * @author Michael Niedermayer <michaelni@gmx.at>
 */

#include "libavutil/intreadwrite.h"
#include "startcode.h"
#include "config.h"

int ff_startcode_find_candidate_c(const uint8_t *buf, int size)
{
    int i = 0;
#if HAVE_FAST_UNALIGNED
    /* we check i < size instead of i + 3 / 7 because it is
     * simpler and there must be AV_INPUT_BUFFER_PADDING_SIZE
     * bytes at the end.
     */
#if HAVE_FAST_64BIT
    while (i < size &&
            !((~AV_RN64(buf + i) &
                    (AV_RN64(buf + i) - 0x0101010101010101ULL)) &
                    0x8080808080808080ULL))
        i += 8;
#else
    while (i < size &&
            !((~AV_RN32(buf + i) &
                    (AV_RN32(buf + i) - 0x01010101U)) &
                    0x80808080U))
        i += 4;
#endif
#endif
    for (; i < size; i++)
        if (!buf[i])
            break;
    return i;
}
