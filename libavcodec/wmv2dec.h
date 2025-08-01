/*
 * Copyright (c) 2002 The FFmpeg Project
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

#ifndef AVCODEC_WMV2DEC_H
#define AVCODEC_WMV2DEC_H

#include "mpegvideo.h"
struct H263DecContext;

int ff_wmv2_decode_secondary_picture_header(struct H263DecContext *const h);
void ff_wmv2_add_mb(MpegEncContext *s, int16_t block[6][64],
                    uint8_t *dest_y, uint8_t *dest_cb, uint8_t *dest_cr);

#endif
