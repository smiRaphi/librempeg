/*
 * Copyright (c) 2007 Michael Niedermayer
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

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>

static uint32_t state;
static uint32_t ran(void)
{
    return state = state * 1664525 + 1013904223;
}

static void checked_seek(FILE *stream, int64_t offset, int whence)
{
    offset = fseek(stream, offset, whence);
    if (offset < 0) {
        fprintf(stderr, "seek failed\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    FILE *f;
    int count, maxburst, length;

    if (argc < 5) {
        printf("USAGE: trasher <filename> <count> <maxburst> <seed>\n");
        return 1;
    }

    f = fopen(argv[1], "rb+");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    count    = atoi(argv[2]);
    maxburst = atoi(argv[3]);
    state    = atoi(argv[4]);

    checked_seek(f, 0, SEEK_END);
    length = ftell(f);
    checked_seek(f, 0, SEEK_SET);

    while (count--) {
        int burst = 1 + ran() * (uint64_t) (abs(maxburst) - 1) / UINT32_MAX;
        int pos   = ran() * (uint64_t) length / UINT32_MAX;
        checked_seek(f, pos, SEEK_SET);

        if (maxburst < 0)
            burst = -maxburst;

        if (pos + burst > length)
            continue;

        while (burst--) {
            int val = ran() * 256ULL / UINT32_MAX;

            if (maxburst < 0)
                val = 0;

            fwrite(&val, 1, 1, f);
        }
    }

    return 0;
}
