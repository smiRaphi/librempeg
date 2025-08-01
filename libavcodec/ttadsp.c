/*
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

#include "libavutil/attributes.h"
#include "ttadsp.h"
#include "config.h"

static void tta_filter_process_c(int32_t *qmi, int32_t *dx, int32_t *dl,
                                 int32_t *error, int32_t *in, int32_t shift,
                                 int32_t round)
{
    uint32_t *qm = qmi;

    if (*error < 0) {
        qm[0] -= dx[0]; qm[1] -= dx[1]; qm[2] -= dx[2]; qm[3] -= dx[3];
        qm[4] -= dx[4]; qm[5] -= dx[5]; qm[6] -= dx[6]; qm[7] -= dx[7];
    } else if (*error > 0) {
        qm[0] += dx[0]; qm[1] += dx[1]; qm[2] += dx[2]; qm[3] += dx[3];
        qm[4] += dx[4]; qm[5] += dx[5]; qm[6] += dx[6]; qm[7] += dx[7];
    }

    round += dl[0] * qm[0] + dl[1] * qm[1] + dl[2] * qm[2] + dl[3] * qm[3] +
             dl[4] * qm[4] + dl[5] * qm[5] + dl[6] * qm[6] + dl[7] * qm[7];

    dx[0] = dx[1]; dx[1] = dx[2]; dx[2] = dx[3]; dx[3] = dx[4];
    dl[0] = dl[1]; dl[1] = dl[2]; dl[2] = dl[3]; dl[3] = dl[4];

    dx[4] = ((dl[4] >> 30) | 1);
    dx[5] = ((dl[5] >> 30) | 2) & ~1;
    dx[6] = ((dl[6] >> 30) | 2) & ~1;
    dx[7] = ((dl[7] >> 30) | 4) & ~3;

    *error = *in;
    *in += (round >> shift);

    dl[4] = -(unsigned)dl[5]; dl[5] = -(unsigned)dl[6];
    dl[6] = *in -(unsigned)dl[7]; dl[7] = *in;
    dl[5] += (unsigned)dl[6]; dl[4] += (unsigned)dl[5];
}

av_cold void ff_ttadsp_init(TTADSPContext *c)
{
    c->filter_process = tta_filter_process_c;

#if ARCH_X86
    ff_ttadsp_init_x86(c);
#endif
}
