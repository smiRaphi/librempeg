/*
 * internals for BMP codecs
 * Copyright (c) 2005 Mans Rullgard
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

#ifndef AVCODEC_BMP_H
#define AVCODEC_BMP_H

typedef enum {
    BMP_RGB         =0,
    BMP_RLE8        =1,
    BMP_RLE4        =2,
    BMP_BITFIELDS   =3,
} BiCompression;

#endif /* AVCODEC_BMP_H */
