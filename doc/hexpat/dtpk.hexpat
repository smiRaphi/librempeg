/*
 * DTPK HexPattern
 * Copyright (c) 2025 smiRaphi
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

#pragma endian little

struct SampleInfEntry {
    u8 spi_id;
    padding[1];
    u8 sample_id;
    padding[3];
    u8 pan;
    u8 gain;
    padding[2];
    u16 rate;
    u8 attack_sustain_decay_flags[4];
    u8 low_pass_filter_flags[10];
    padding[4];
    u8 fx_override;
    u8 fx_flags;
    padding[2];
    u8 group_override_flags;
    u8 group_priority;
    u8 self_priority;
    u8 low_pass_Q;
    padding[2];
    u8 random_tune;
    padding[5];
    u8 low_freq_osc_delay;
    u8 low_freq_osc_feed;
    u8 low_freq_osc_depth_flags;
    u8 low_freq_osc_form_flags;
    padding[14];
};

bitfield Flags {
    offset : 23;
    bool low_freq : 1;
    bool compressed : 1;
    bool unk1 : 1;
};
struct SampleEntry {
    Flags info;
    u16 loop_start;
    u16 loop_end;
    u32 low_freq_alt;
    u32 size;
};

struct DTPK {
    char magic[4];
    u32 id;
    u32 size;
    padding[4];
    u32 unk1;
    u32 unk2;
    u32 unk3;
    padding[4];

    u32 combination_table_off;
    u32 program_def_table_off;
    u32 unk_table_off;
    u32 sequencer_table_off;
    u32 sample_playback_inf_table_off;
    u32 ICS_table_off;
    u32 effect_table_off;
    u32 sample_table_off;

    u32 sequencer_table_num @ sequencer_table_off;
    u32 sequencer_table[sequencer_table_num+1] @ (sequencer_table_off+4);

    u8 sample_playback_inf_table_num @ (sample_playback_inf_table_off+0x10);
    SampleInfEntry sample_playback_inf_table[sample_playback_inf_table_num+1] @ (sample_playback_inf_table_off+0x50);

    u32 sample_table_num @ sample_table_off;
    SampleEntry sample_table[sample_table_num+1] @ (sample_table_off+4);
};

DTPK DTPK @ 0;