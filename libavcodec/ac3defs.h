/*
 * Common AC-3 definitions
 * Copyright (c) 2000, 2001, 2002 Fabrice Bellard
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

#ifndef AVCODEC_AC3DEFS_H
#define AVCODEC_AC3DEFS_H

#define EAC3_MAX_CHANNELS 16          /**< maximum number of channels in EAC3 */
#define AC3_MAX_CHANNELS 7            /**< maximum number of channels, including coupling channel */
#define CPL_CH 0                      /**< coupling channel index */

#define AC3_MAX_COEFS   256
#define AC3_BLOCK_SIZE  256
#define AC3_MAX_BLOCKS    6
#define AC3_FRAME_SIZE (AC3_MAX_BLOCKS * 256)
#define AC3_WINDOW_SIZE (AC3_BLOCK_SIZE * 2)
#define AC3_CRITICAL_BANDS 50
#define AC3_MAX_CPL_BANDS  18

#define EAC3_SR_CODE_REDUCED 3

/* pre-defined gain values */
#define LEVEL_PLUS_3DB          M_SQRT2
#define LEVEL_PLUS_1POINT5DB    1.1892071150027209
#define LEVEL_MINUS_1POINT5DB   0.8408964152537145
#define LEVEL_MINUS_3DB         M_SQRT1_2
#define LEVEL_MINUS_4POINT5DB   0.5946035575013605
#define LEVEL_MINUS_6DB         0.5000000000000000
#define LEVEL_MINUS_9DB         0.3535533905932738
#define LEVEL_ZERO              0.0000000000000000
#define LEVEL_ONE               1.0000000000000000

/* exponent encoding strategy */
#define EXP_REUSE 0
#define EXP_NEW   1

#define EXP_D15   1
#define EXP_D25   2
#define EXP_D45   3

/** Delta bit allocation strategy */
typedef enum {
    DBA_REUSE = 0,
    DBA_NEW,
    DBA_NONE,
    DBA_RESERVED
} AC3DeltaStrategy;

/** Channel mode (audio coding mode) */
typedef enum {
    AC3_CHMODE_DUALMONO = 0,
    AC3_CHMODE_MONO,
    AC3_CHMODE_STEREO,
    AC3_CHMODE_3F,
    AC3_CHMODE_2F1R,
    AC3_CHMODE_3F1R,
    AC3_CHMODE_2F2R,
    AC3_CHMODE_3F2R
} AC3ChannelMode;

/** Dolby Surround mode */
typedef enum AC3DolbySurroundMode {
    AC3_DSURMOD_NOTINDICATED = 0,
    AC3_DSURMOD_OFF,
    AC3_DSURMOD_ON,
    AC3_DSURMOD_RESERVED
} AC3DolbySurroundMode;

/** Dolby Surround EX mode */
typedef enum AC3DolbySurroundEXMode {
    AC3_DSUREXMOD_NOTINDICATED = 0,
    AC3_DSUREXMOD_OFF,
    AC3_DSUREXMOD_ON,
    AC3_DSUREXMOD_PLIIZ
} AC3DolbySurroundEXMode;

/** Dolby Headphone mode */
typedef enum AC3DolbyHeadphoneMode {
    AC3_DHEADPHONMOD_NOTINDICATED = 0,
    AC3_DHEADPHONMOD_OFF,
    AC3_DHEADPHONMOD_ON,
    AC3_DHEADPHONMOD_RESERVED
} AC3DolbyHeadphoneMode;

/** Preferred Stereo Downmix mode */
typedef enum AC3PreferredStereoDownmixMode {
    AC3_DMIXMOD_NOTINDICATED = 0,
    AC3_DMIXMOD_LTRT,
    AC3_DMIXMOD_LORO,
    AC3_DMIXMOD_DPLII // reserved value in A/52, but used by encoders to indicate DPL2
} AC3PreferredStereoDownmixMode;

typedef enum {
    EAC3_FRAME_TYPE_INDEPENDENT = 0,
    EAC3_FRAME_TYPE_DEPENDENT,
    EAC3_FRAME_TYPE_AC3_CONVERT,
    EAC3_FRAME_TYPE_RESERVED
} EAC3FrameType;

#endif /* AVCODEC_AC3DEFS_H */
