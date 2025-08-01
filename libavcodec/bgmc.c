/*
 * Block Gilbert-Moore decoder
 * Copyright (c) 2010 Thilo Borgmann <thilo.borgmann _at_ mail.de>
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
 * Block Gilbert-Moore decoder as used by MPEG-4 ALS
 * @author Thilo Borgmann <thilo.borgmann _at_ mail.de>
 */

#include "libavutil/attributes.h"
#include "libavutil/mem.h"
#include "bgmc.h"

#define FREQ_BITS  14                      // bits used by frequency counters
#define VALUE_BITS 18                      // bits used to represent the values
#define TOP_VALUE  ((1 << VALUE_BITS) - 1) // maximum value
#define FIRST_QTR  (TOP_VALUE / 4 + 1)     // first quarter of values maximum value
#define HALF       (2 * FIRST_QTR)         // first half of values maximum value
#define THIRD_QTR  (3 * FIRST_QTR)         // third quarter of values maximum value

#define LUT_BITS   (FREQ_BITS - 8)         // number of bits used to index lookup tables
#define LUT_SIZE   (1 << LUT_BITS)         // size of the lookup tables
#define LUT_BUFF   4                       // number of buffered lookup tables


/** Cumulative frequency tables for block Gilbert-Moore coding. */
static const uint16_t cf_tables_1[3][129] = {
    {
        16384, 16066, 15748, 15431, 15114, 14799, 14485, 14173, 13861, 13552,
        13243, 12939, 12635, 12336, 12038, 11745, 11452, 11161, 10870, 10586,
        10303, 10027,  9751,  9483,  9215,  8953,  8692,  8440,  8189,  7946,
         7704,  7472,  7240,  7008,  6776,  6554,  6333,  6122,  5912,  5711,
         5512,  5320,  5128,  4947,  4766,  4595,  4425,  4264,  4104,  3946,
         3788,  3640,  3493,  3355,  3218,  3090,  2963,  2842,  2721,  2609,
         2498,  2395,  2292,  2196,  2100,  2004,  1908,  1820,  1732,  1651,
         1570,  1497,  1424,  1355,  1287,  1223,  1161,  1100,  1044,   988,
          938,   888,   839,   790,   746,   702,   662,   623,   588,   553,
          520,   488,   459,   431,   405,   380,   357,   334,   311,   288,
          268,   248,   230,   213,   197,   182,   168,   154,   142,   130,
          119,   108,    99,    90,    81,    72,    64,    56,    49,    42,
           36,    30,    25,    20,    15,    11,     7,     3,     0
    },
    {
        16384, 16080, 15776, 15473, 15170, 14868, 14567, 14268, 13970, 13674,
        13378, 13086, 12794, 12505, 12218, 11936, 11654, 11373, 11092, 10818,
        10544, 10276, 10008,  9749,  9490,  9236,  8982,  8737,  8492,  8256,
         8020,  7792,  7564,  7336,  7108,  6888,  6669,  6459,  6249,  6050,
         5852,  5660,  5468,  5286,  5104,  4931,  4760,  4598,  4436,  4275,
         4115,  3965,  3816,  3674,  3534,  3403,  3272,  3147,  3023,  2907,
         2792,  2684,  2577,  2476,  2375,  2274,  2173,  2079,  1986,  1897,
         1810,  1724,  1645,  1567,  1493,  1419,  1351,  1284,  1222,  1161,
         1105,  1050,   995,   941,   891,   842,   797,   753,   713,   673,
          636,   599,   566,   533,   503,   473,   446,   419,   392,   365,
          340,   316,   294,   272,   253,   234,   216,   199,   184,   169,
          155,   142,   130,   118,   106,    95,    85,    75,    66,    57,
           49,    41,    34,    27,    21,    15,    10,     5,     0
    },
    {
        16384, 16092, 15801, 15510, 15219, 14930, 14641, 14355, 14069, 13785,
        13501, 13219, 12938, 12661, 12384, 12112, 11841, 11571, 11301, 11037,
        10773, 10514, 10256, 10005,  9754,  9508,  9263,  9025,  8787,  8557,
         8327,  8103,  7879,  7655,  7431,  7215,  7000,  6792,  6585,  6387,
         6190,  5998,  5807,  5625,  5445,  5272,  5100,  4937,  4774,  4613,
         4452,  4301,  4150,  4007,  3865,  3731,  3597,  3469,  3341,  3218,
         3099,  2981,  2869,  2758,  2652,  2546,  2440,  2334,  2234,  2134,
         2041,  1949,  1864,  1779,  1699,  1620,  1547,  1474,  1407,  1340,
         1278,  1217,  1157,  1097,  1043,   989,   940,   891,   846,   801,
          759,   718,   680,   643,   609,   575,   543,   511,   479,   447,
          418,   389,   363,   337,   314,   291,   270,   249,   230,   212,
          195,   179,   164,   149,   135,   121,   108,    96,    85,    74,
          64,     54,    45,    36,    28,    20,    13,     6,     0
    }
};


static const uint16_t cf_tables_2[8][193] = {
    {
        16384, 16104, 15825, 15546, 15268, 14991, 14714, 14439, 14164, 13891,
        13620, 13350, 13081, 12815, 12549, 12287, 12025, 11765, 11505, 11250,
        10996, 10746, 10497, 10254, 10011,  9772,  9534,  9303,  9072,  8848,
         8624,  8406,  8188,  7970,  7752,  7539,  7327,  7123,  6919,  6724,
         6529,  6339,  6150,  5970,  5790,  5618,  5446,  5282,  5119,  4957,
         4795,  4642,  4490,  4345,  4201,  4065,  3929,  3798,  3669,  3547,
         3425,  3310,  3196,  3086,  2976,  2866,  2756,  2650,  2545,  2447,
         2350,  2260,  2170,  2085,  2000,  1921,  1843,  1770,  1698,  1632,
         1566,  1501,  1436,  1376,  1316,  1261,  1207,  1157,  1108,  1061,
         1015,   973,   931,   893,   855,   819,   783,   747,   711,   677,
          644,   614,   584,   557,   530,   505,   480,   458,   436,   416,
          396,   378,   360,   343,   326,   310,   295,   281,   267,   255,
          243,   232,   221,   211,   201,   192,   183,   174,   166,   158,
          150,   142,   134,   126,   119,   112,   106,   100,    95,    90,
           85,    80,    76,    72,    69,    66,    63,    60,    57,    54,
           51,    48,    46,    44,    42,    40,    38,    36,    34,    33,
           32,    31,    30,    29,    28,    27,    26,    25,    24,    23,
           22,    21,    20,    19,    18,    17,    16,    15,    14,    13,
           12,    11,    10,     9,     8,     7,     6,     5,     4,     3,
            2,     1,     0
    },
    {
        16384, 16116, 15849, 15582, 15316, 15050, 14785, 14521, 14257, 13995,
        13734, 13476, 13218, 12963, 12708, 12457, 12206, 11956, 11706, 11460,
        11215, 10975, 10735, 10500, 10265, 10034,  9803,  9579,  9355,  9136,
         8917,  8703,  8489,  8275,  8061,  7853,  7645,  7444,  7244,  7051,
         6858,  6671,  6484,  6305,  6127,  5956,  5785,  5622,  5459,  5298,
         5137,  4983,  4830,  4684,  4539,  4401,  4263,  4131,  3999,  3874,
         3750,  3632,  3515,  3401,  3287,  3173,  3059,  2949,  2840,  2737,
         2635,  2539,  2444,  2354,  2264,  2181,  2098,  2020,  1943,  1872,
         1801,  1731,  1661,  1596,  1532,  1472,  1412,  1357,  1303,  1251,
         1200,  1153,  1106,  1063,  1020,   979,   938,   897,   856,   818,
          780,   746,   712,   681,   650,   621,   592,   566,   540,   517,
          494,   473,   452,   431,   410,   391,   373,   356,   340,   325,
          310,   296,   282,   270,   258,   247,   236,   225,   214,   203,
          192,   182,   172,   162,   153,   144,   136,   128,   121,   114,
          108,   102,    97,    92,    87,    82,    77,    73,    69,    65,
           62,    59,    56,    53,    50,    47,    45,    43,    41,    39,
           37,    35,    33,    31,    29,    27,    26,    25,    24,    23,
           22,    21,    20,    19,    18,    17,    16,    15,    14,    13,
           12,    11,    10,     9,     8,     7,     6,     5,     4,     3,
            2,     1,     0
    },
    {
        16384, 16128, 15872, 15617, 15362, 15107, 14853, 14600, 14347, 14096,
        13846, 13597, 13350, 13105, 12860, 12618, 12376, 12135, 11894, 11657,
        11421, 11189, 10957, 10730, 10503, 10279, 10056,  9838,  9620,  9407,
         9195,  8987,  8779,  8571,  8363,  8159,  7955,  7758,  7561,  7371,
         7182,  6997,  6812,  6635,  6459,  6289,  6120,  5957,  5795,  5634,
         5473,  5319,  5165,  5018,  4871,  4732,  4593,  4458,  4324,  4197,
         4071,  3951,  3831,  3714,  3597,  3480,  3363,  3250,  3138,  3032,
         2927,  2828,  2729,  2635,  2541,  2453,  2366,  2284,  2202,  2126,
         2050,  1975,  1900,  1830,  1761,  1697,  1633,  1574,  1515,  1459,
         1403,  1351,  1300,  1252,  1205,  1160,  1115,  1070,  1025,   982,
          939,   899,   860,   824,   789,   756,   723,   693,   663,   636,
          609,   584,   559,   535,   511,   489,   467,   447,   427,   409,
          391,   374,   358,   343,   328,   313,   300,   287,   274,   261,
          248,   235,   223,   211,   200,   189,   179,   169,   160,   151,
          143,   135,   128,   121,   115,   109,   103,    97,    92,    87,
           82,    77,    73,    69,    65,    61,    58,    55,    52,    49,
           46,    43,    40,    37,    35,    33,    31,    29,    27,    25,
           23,    21,    20,    19,    18,    17,    16,    15,    14,    13,
           12,    11,    10,     9,     8,     7,     6,     5,     4,     3,
            2,     1,     0
    },
    {
        16384, 16139, 15894, 15649, 15405, 15162, 14919, 14677, 14435, 14195,
        13955, 13717, 13479, 13243, 13008, 12775, 12542, 12310, 12079, 11851,
        11623, 11399, 11176, 10956, 10737, 10521, 10305, 10094,  9883,  9677,
         9471,  9268,  9065,  8862,  8659,  8459,  8260,  8067,  7874,  7688,
         7502,  7321,  7140,  6965,  6790,  6621,  6452,  6290,  6128,  5968,
         5808,  5655,  5503,  5356,  5209,  5069,  4929,  4794,  4660,  4532,
         4404,  4282,  4160,  4041,  3922,  3803,  3684,  3568,  3452,  3343,
         3234,  3131,  3029,  2931,  2833,  2741,  2649,  2563,  2477,  2396,
         2316,  2236,  2157,  2083,  2009,  1940,  1871,  1807,  1743,  1683,
         1623,  1567,  1511,  1459,  1407,  1357,  1307,  1257,  1207,  1159,
         1111,  1067,  1023,   983,   943,   905,   868,   834,   800,   769,
          738,   709,   681,   653,   625,   600,   575,   552,   529,   508,
          487,   466,   447,   428,   410,   392,   376,   360,   344,   328,
          313,   298,   283,   268,   255,   242,   230,   218,   207,   196,
          186,   176,   167,   158,   150,   142,   135,   128,   121,   114,
          108,   102,    97,    92,    87,    82,    78,    74,    70,    66,
           62,    58,    54,    50,    47,    44,    41,    38,    35,    32,
           30,    28,    26,    24,    22,    20,    18,    16,    14,    13,
           12,    11,    10,     9,     8,     7,     6,     5,     4,     3,
            2,     1,     0
    },
    {
        16384, 16149, 15915, 15681, 15447, 15214, 14981, 14749, 14517, 14286,
        14055, 13827, 13599, 13373, 13147, 12923, 12699, 12476, 12253, 12034,
        11815, 11599, 11383, 11171, 10959, 10750, 10541, 10337, 10133,  9933,
         9733,  9536,  9339,  9142,  8945,  8751,  8557,  8369,  8181,  7998,
         7816,  7638,  7460,  7288,  7116,  6950,  6785,  6625,  6465,  6306,
         6147,  5995,  5843,  5697,  5551,  5411,  5271,  5135,  5000,  4871,
         4742,  4618,  4495,  4374,  4253,  4132,  4011,  3893,  3775,  3663,
         3552,  3446,  3340,  3239,  3138,  3043,  2948,  2858,  2768,  2684,
         2600,  2516,  2433,  2355,  2278,  2205,  2133,  2065,  1997,  1932,
         1867,  1807,  1747,  1690,  1634,  1580,  1526,  1472,  1418,  1366,
         1314,  1266,  1218,  1174,  1130,  1088,  1047,  1009,   971,   936,
          901,   868,   836,   804,   772,   743,   714,   685,   658,   631,
          606,   582,   559,   536,   515,   494,   475,   456,   437,   418,
          399,   380,   362,   344,   328,   312,   297,   283,   270,   257,
          245,   233,   222,   211,   201,   191,   181,   172,   163,   155,
          147,   139,   132,   125,   119,   113,   107,   101,    96,    91,
           86,    81,    76,    71,    66,    62,    58,    54,    50,    46,
           43,    40,    37,    34,    31,    28,    26,    24,    22,    20,
           18,    16,    14,    12,    10,     8,     6,     5,     4,     3,
            2,     1,     0
    },
    {
        16384, 16159, 15934, 15709, 15485, 15261, 15038, 14816, 14594, 14373,
        14152, 13933, 13714, 13497, 13280, 13065, 12850, 12636, 12422, 12211,
        12000, 11791, 11583, 11378, 11173, 10971, 10769, 10571, 10373, 10179,
         9985,  9793,  9601,  9409,  9217,  9029,  8842,  8658,  8475,  8297,
         8120,  7946,  7773,  7604,  7435,  7271,  7108,  6950,  6792,  6634,
         6477,  6326,  6175,  6029,  5883,  5742,  5602,  5466,  5330,  5199,
         5068,  4943,  4818,  4696,  4574,  4452,  4330,  4211,  4093,  3979,
         3866,  3759,  3652,  3549,  3446,  3348,  3250,  3157,  3065,  2977,
         2889,  2802,  2716,  2634,  2553,  2476,  2399,  2326,  2254,  2185,
         2117,  2052,  1987,  1926,  1866,  1808,  1750,  1692,  1634,  1578,
         1522,  1470,  1418,  1369,  1321,  1275,  1229,  1187,  1145,  1105,
         1066,  1027,   991,   955,   919,   883,   850,   817,   786,   756,
          728,   700,   674,   648,   624,   600,   578,   556,   534,   512,
          490,   468,   447,   426,   407,   388,   371,   354,   338,   322,
          307,   293,   280,   267,   255,   243,   231,   219,   209,   199,
          189,   179,   170,   161,   153,   145,   138,   131,   124,   117,
          111,   105,    99,    93,    87,    81,    76,    71,    66,    61,
           57,    53,    49,    45,    42,    39,    36,    33,    30,    27,
           24,    21,    19,    17,    15,    13,    11,     9,     7,     5,
            3,     1,     0
    },
    {
        16384, 16169, 15954, 15739, 15524, 15310, 15096, 14883, 14670, 14458,
        14246, 14035, 13824, 13614, 13405, 13198, 12991, 12785, 12579, 12376,
        12173, 11972, 11772, 11574, 11377, 11182, 10987, 10795, 10603, 10414,
        10226, 10040,  9854,  9668,  9482,  9299,  9116,  8937,  8759,  8585,
         8411,  8241,  8071,  7906,  7741,  7580,  7419,  7263,  7107,  6952,
         6797,  6647,  6497,  6353,  6209,  6070,  5931,  5796,  5661,  5531,
         5401,  5275,  5150,  5027,  4904,  4781,  4658,  4538,  4419,  4304,
         4190,  4081,  3972,  3867,  3762,  3662,  3562,  3467,  3372,  3281,
         3191,  3101,  3012,  2928,  2844,  2764,  2684,  2608,  2533,  2460,
         2387,  2318,  2250,  2185,  2121,  2059,  1997,  1935,  1873,  1813,
         1754,  1698,  1642,  1588,  1535,  1483,  1433,  1384,  1338,  1292,
         1249,  1206,  1165,  1125,  1085,  1045,  1008,   971,   937,   903,
          871,   840,   810,   780,   752,   724,   698,   672,   647,   622,
          597,   572,   548,   524,   502,   480,   460,   440,   421,   403,
          386,   369,   353,   337,   323,   309,   295,   281,   268,   255,
          243,   231,   220,   209,   199,   189,   180,   171,   163,   155,
          147,   139,   131,   123,   116,   109,   102,    95,    89,    83,
           77,    72,    67,    62,    57,    52,    48,    44,    40,    36,
           32,    28,    25,    22,    19,    16,    13,    10,     8,     6,
            4,     2,     0
    },
    {
        16384, 16177, 15970, 15764, 15558, 15353, 15148, 14944, 14740, 14537,
        14334, 14132, 13930, 13729, 13529, 13330, 13131, 12933, 12735, 12539,
        12343, 12150, 11957, 11766, 11576, 11388, 11200, 11015, 10830, 10647,
        10465, 10285, 10105,  9925,  9745,  9568,  9391,  9218,  9045,  8876,
         8707,  8541,  8375,  8213,  8051,  7894,  7737,  7583,  7429,  7277,
         7125,  6977,  6830,  6687,  6544,  6406,  6268,  6133,  5998,  5868,
         5738,  5612,  5487,  5364,  5241,  5118,  4995,  4875,  4755,  4640,
         4525,  4414,  4304,  4198,  4092,  3990,  3888,  3790,  3693,  3600,
         3507,  3415,  3323,  3235,  3147,  3064,  2981,  2902,  2823,  2746,
         2670,  2594,  2522,  2450,  2382,  2314,  2248,  2182,  2116,  2050,
         1987,  1924,  1864,  1804,  1748,  1692,  1638,  1585,  1534,  1484,
         1437,  1390,  1346,  1302,  1258,  1215,  1174,  1133,  1095,  1057,
         1021,   986,   952,   918,   887,   856,   827,   798,   770,   742,
          714,   686,   659,   632,   607,   582,   559,   536,   514,   492,
          472,   452,   433,   415,   398,   381,   364,   348,   333,   318,
          304,   290,   277,   264,   252,   240,   229,   218,   208,   198,
          188,   178,   168,   158,   149,   140,   132,   124,   116,   108,
          101,    94,    87,    81,    75,    69,    64,    59,    54,    49,
           44,    39,    35,    31,    27,    23,    19,    15,    12,     9,
            6,     3,     0
    }
};


static const uint16_t cf_tables_3[5][257] = {
    {
        16384, 16187, 15990, 15793, 15597, 15401, 15205, 15009, 14813, 14618,
        14423, 14230, 14037, 13845, 13653, 13463, 13273, 13083, 12894, 12706,
        12518, 12332, 12146, 11962, 11778, 11597, 11416, 11237, 11059, 10882,
        10706, 10532, 10358, 10184, 10010,  9838,  9666,  9497,  9328,  9163,
         8999,  8837,  8675,  8517,  8359,  8205,  8051,  7901,  7751,  7602,
         7453,  7308,  7163,  7022,  6882,  6745,  6609,  6476,  6343,  6214,
         6085,  5960,  5835,  5712,  5589,  5466,  5343,  5223,  5103,  4987,
         4872,  4761,  4650,  4542,  4435,  4332,  4229,  4130,  4031,  3936,
         3841,  3747,  3653,  3563,  3473,  3387,  3302,  3220,  3138,  3059,
         2980,  2905,  2830,  2759,  2688,  2619,  2550,  2481,  2412,  2345,
         2278,  2215,  2152,  2092,  2032,  1974,  1917,  1863,  1809,  1758,
         1707,  1659,  1611,  1564,  1517,  1473,  1429,  1387,  1346,  1307,
         1268,  1230,  1193,  1158,  1123,  1090,  1058,  1026,   994,   962,
          930,   899,   869,   841,   813,   786,   760,   735,   710,   687,
          664,   643,   622,   602,   582,   562,   543,   525,   507,   490,
          473,   457,   442,   427,   412,   398,   385,   373,   361,   349,
          337,   325,   313,   301,   290,   279,   269,   259,   249,   240,
          231,   222,   214,   206,   199,   192,   185,   178,   171,   165,
          159,   153,   148,   143,   138,   133,   128,   123,   119,   115,
          111,   107,   103,    99,    95,    91,    87,    83,    80,    77,
           74,    71,    68,    65,    63,    61,    59,    57,    55,    53,
           51,    49,    47,    45,    43,    41,    40,    39,    38,    37,
           36,    35,    34,    33,    32,    31,    30,    29,    28,    27,
           26,    25,    24,    23,    22,    21,    20,    19,    18,    17,
           16,    15,    14,    13,    12,    11,    10,     9,     8,     7,
            6,     5,     4,     3,     2,     1,     0
    },
    {
        16384, 16195, 16006, 15817, 15629, 15441, 15253, 15065, 14878, 14692,
        14506, 14321, 14136, 13952, 13768, 13585, 13402, 13219, 13037, 12857,
        12677, 12499, 12321, 12144, 11967, 11792, 11617, 11444, 11271, 11100,
        10930, 10762, 10594, 10426, 10258, 10091,  9925,  9761,  9598,  9438,
         9278,  9120,  8963,  8809,  8655,  8504,  8354,  8207,  8060,  7914,
         7769,  7627,  7485,  7347,  7209,  7074,  6939,  6807,  6676,  6548,
         6420,  6296,  6172,  6050,  5928,  5806,  5684,  5564,  5444,  5328,
         5212,  5100,  4988,  4879,  4771,  4667,  4563,  4462,  4362,  4265,
         4169,  4073,  3978,  3886,  3795,  3707,  3619,  3535,  3451,  3369,
         3288,  3210,  3133,  3059,  2985,  2913,  2841,  2769,  2697,  2627,
         2557,  2490,  2424,  2360,  2297,  2237,  2177,  2119,  2062,  2007,
         1953,  1901,  1849,  1798,  1748,  1700,  1652,  1607,  1562,  1519,
         1476,  1435,  1394,  1355,  1317,  1281,  1245,  1210,  1175,  1140,
         1105,  1071,  1037,  1005,   973,   943,   913,   885,   857,   830,
          804,   779,   754,   731,   708,   685,   663,   642,   621,   601,
          581,   563,   545,   528,   511,   495,   479,   463,   448,   433,
          419,   405,   391,   377,   364,   351,   338,   326,   314,   302,
          291,   280,   270,   260,   251,   242,   234,   226,   218,   210,
          202,   195,   188,   181,   174,   168,   162,   156,   150,   144,
          139,   134,   129,   124,   119,   114,   109,   104,   100,    96,
           92,    88,    84,    80,    77,    74,    71,    68,    65,    62,
           59,    56,    54,    52,    50,    48,    46,    44,    42,    40,
           38,    36,    34,    33,    32,    31,    30,    29,    28,    27,
           26,    25,    24,    23,    22,    21,    20,    19,    18,    17,
           16,    15,    14,    13,    12,    11,    10,     9,     8,     7,
            6,     5,     4,     3,     2,     1,     0
    },
    {
        16384, 16203, 16022, 15842, 15662, 15482, 15302, 15122, 14942, 14763,
        14584, 14406, 14228, 14051, 13874, 13698, 13522, 13347, 13172, 12998,
        12824, 12652, 12480, 12310, 12140, 11971, 11803, 11637, 11471, 11307,
        11143, 10980, 10817, 10654, 10491, 10330, 10169, 10011,  9853,  9697,
         9542,  9389,  9236,  9086,  8936,  8789,  8642,  8498,  8355,  8212,
         8070,  7931,  7792,  7656,  7520,  7388,  7256,  7126,  6996,  6870,
         6744,  6621,  6498,  6377,  6256,  6135,  6014,  5895,  5776,  5660,
         5545,  5433,  5321,  5212,  5104,  4999,  4895,  4793,  4692,  4594,
         4496,  4400,  4304,  4211,  4118,  4028,  3939,  3853,  3767,  3684,
         3601,  3521,  3441,  3364,  3287,  3212,  3137,  3062,  2987,  2915,
         2843,  2773,  2704,  2638,  2572,  2508,  2445,  2384,  2324,  2266,
         2208,  2153,  2098,  2044,  1990,  1939,  1888,  1839,  1791,  1745,
         1699,  1655,  1611,  1569,  1527,  1487,  1448,  1409,  1370,  1331,
         1292,  1255,  1218,  1183,  1148,  1115,  1082,  1051,  1020,   990,
          960,   932,   904,   878,   852,   826,   801,   777,   753,   731,
          709,   687,   666,   645,   625,   605,   586,   567,   550,   533,
          516,   499,   482,   465,   449,   433,   418,   403,   389,   375,
          362,   349,   337,   325,   314,   303,   293,   283,   273,   263,
          254,   245,   236,   227,   219,   211,   204,   197,   190,   183,
          177,   171,   165,   159,   153,   147,   141,   135,   130,   125,
          120,   115,   110,   105,   101,    97,    93,    89,    85,    81,
           77,    74,    71,    68,    65,    62,    59,    56,    53,    51,
           49,    47,    45,    43,    41,    39,    37,    35,    33,    31,
           29,    27,    25,    23,    22,    21,    20,    19,    18,    17,
           16,    15,    14,    13,    12,    11,    10,     9,     8,     7,
            6,     5,     4,     3,     2,     1,     0
    },
    {
        16384, 16210, 16036, 15863, 15690, 15517, 15344, 15172, 15000, 14828,
        14656, 14485, 14314, 14145, 13976, 13808, 13640, 13472, 13304, 13137,
        12970, 12804, 12639, 12475, 12312, 12149, 11987, 11827, 11667, 11508,
        11349, 11192, 11035, 10878, 10721, 10565, 10410, 10257, 10104,  9953,
         9802,  9654,  9506,  9359,  9213,  9070,  8927,  8787,  8647,  8508,
         8369,  8233,  8097,  7964,  7831,  7700,  7570,  7442,  7315,  7190,
         7065,  6943,  6821,  6701,  6581,  6461,  6341,  6223,  6105,  5990,
         5876,  5764,  5653,  5545,  5437,  5331,  5226,  5124,  5022,  4924,
         4826,  4729,  4632,  4538,  4444,  4353,  4262,  4174,  4087,  4002,
         3917,  3835,  3753,  3674,  3595,  3518,  3441,  3364,  3287,  3212,
         3138,  3066,  2995,  2926,  2858,  2792,  2726,  2662,  2599,  2538,
         2478,  2420,  2362,  2305,  2249,  2195,  2141,  2089,  2037,  1988,
         1939,  1891,  1844,  1799,  1754,  1711,  1668,  1626,  1584,  1542,
         1500,  1459,  1418,  1380,  1342,  1305,  1269,  1234,  1199,  1166,
         1133,  1102,  1071,  1041,  1012,   983,   954,   926,   899,   872,
          847,   822,   798,   774,   751,   728,   707,   686,   666,   646,
          627,   608,   589,   570,   552,   534,   517,   500,   484,   468,
          453,   438,   424,   410,   397,   384,   372,   360,   348,   336,
          325,   314,   303,   293,   283,   273,   264,   255,   246,   237,
          229,   221,   213,   205,   197,   189,   181,   174,   167,   160,
          154,   148,   142,   136,   131,   126,   121,   116,   111,   106,
          101,    97,    93,    89,    85,    81,    77,    73,    70,    67,
           64,    61,    58,    55,    52,    49,    46,    43,    40,    37,
           35,    33,    31,    29,    27,    25,    23,    21,    19,    17,
           16,    15,    14,    13,    12,    11,    10,     9,     8,     7,
            6,     5,     4,     3,     2,     1,     0
    },
    {
        16384, 16218, 16052, 15886, 15720, 15554, 15389, 15224, 15059, 14895,
        14731, 14567, 14403, 14240, 14077, 13915, 13753, 13591, 13429, 13269,
        13109, 12950, 12791, 12633, 12476, 12320, 12164, 12009, 11854, 11701,
        11548, 11396, 11244, 11092, 10940, 10790, 10640, 10492, 10344, 10198,
        10052,  9908,  9764,  9622,  9481,  9342,  9203,  9066,  8929,  8793,
         8657,  8524,  8391,  8261,  8131,  8003,  7875,  7749,  7624,  7502,
         7380,  7260,  7140,  7022,  6904,  6786,  6668,  6551,  6435,  6322,
         6209,  6099,  5989,  5881,  5773,  5668,  5563,  5461,  5359,  5260,
         5161,  5063,  4965,  4871,  4777,  4686,  4595,  4506,  4417,  4331,
         4245,  4162,  4079,  3999,  3919,  3841,  3763,  3685,  3607,  3530,
         3454,  3380,  3307,  3236,  3166,  3097,  3029,  2963,  2897,  2834,
         2771,  2710,  2650,  2591,  2532,  2475,  2418,  2363,  2309,  2257,
         2205,  2155,  2105,  2057,  2009,  1963,  1918,  1873,  1828,  1783,
         1738,  1694,  1650,  1607,  1565,  1524,  1484,  1445,  1407,  1369,
         1333,  1297,  1263,  1229,  1197,  1165,  1134,  1103,  1073,  1043,
         1015,   987,   960,   933,   907,   882,   858,   834,   811,   788,
          766,   744,   722,   700,   679,   658,   638,   618,   599,   581,
          563,   545,   528,   511,   495,   480,   465,   451,   437,   423,
          410,   397,   384,   372,   360,   348,   337,   326,   315,   305,
          295,   285,   275,   265,   255,   245,   236,   227,   219,   211,
          203,   195,   188,   181,   174,   167,   161,   155,   149,   143,
          137,   131,   126,   121,   116,   111,   106,   101,    97,    93,
           89,    85,    81,    77,    73,    69,    65,    61,    58,    55,
           52,    49,    46,    43,    40,    37,    34,    32,    30,    28,
           26,    24,    22,    20,    18,    16,    14,    12,    10,     8,
            6,     5,     4,     3,     2,     1,     0
    }
};


static const uint16_t *const cf_table[16] = {
    cf_tables_1[0], cf_tables_1[1], cf_tables_1[2], cf_tables_2[0],
    cf_tables_2[1], cf_tables_2[2], cf_tables_2[3], cf_tables_2[4],
    cf_tables_2[5], cf_tables_2[6], cf_tables_2[7], cf_tables_3[0],
    cf_tables_3[1], cf_tables_3[2], cf_tables_3[3], cf_tables_3[4]
};


/** Initialize a given lookup table using a given delta */
static void bgmc_lut_fillp(uint8_t *lut, int *lut_status, int delta)
{
    unsigned int sx, i;

    for (sx = 0; sx < 16; sx++)
        for (i = 0; i < LUT_SIZE; i++) {
            unsigned int target = (i + 1) << (FREQ_BITS - LUT_BITS);
            unsigned int symbol = 1 << delta;

            while (cf_table[sx][symbol] > target)
                symbol += 1 << delta;

            *lut++ = symbol >> delta;
        }

    *lut_status = delta;
}


/** Retune the index of a suitable lookup table for a given delta */
static uint8_t *bgmc_lut_getp(uint8_t *lut, int *lut_status, int delta)
{
    unsigned int i = av_clip(delta, 0, LUT_BUFF - 1);

    lut += (i * LUT_SIZE) << 4;

    if (lut_status[i] != delta)
        bgmc_lut_fillp(lut, &lut_status[i], delta);

    return lut;
}


/** Initialize the lookup table arrays */
av_cold int ff_bgmc_init(void *logctx,
                         uint8_t **cf_lut, int **cf_lut_status)
{
    *cf_lut        = av_malloc(sizeof(**cf_lut)        * LUT_BUFF * 16 * LUT_SIZE);
    *cf_lut_status = av_malloc(sizeof(**cf_lut_status) * LUT_BUFF);

    if (!*cf_lut || !*cf_lut_status) {
        ff_bgmc_end(cf_lut, cf_lut_status);
        av_log(logctx, AV_LOG_ERROR, "Allocating buffer memory failed.\n");
        return AVERROR(ENOMEM);
    } else {
        // initialize lut_status buffer to a value never used to compare against
        memset(*cf_lut_status, -1, sizeof(**cf_lut_status) * LUT_BUFF);
    }

    return 0;
}


/** Release the lookup table arrays */
av_cold void ff_bgmc_end(uint8_t **cf_lut, int **cf_lut_status)
{
    av_freep(cf_lut);
    av_freep(cf_lut_status);
}


/** Initialize decoding and reads the first value */
int ff_bgmc_decode_init(GetBitContext *gb, unsigned int *h,
                         unsigned int *l, unsigned int *v)
{
    if (get_bits_left(gb) < VALUE_BITS)
        return AVERROR_INVALIDDATA;

    *h = TOP_VALUE;
    *l = 0;
    *v = get_bits(gb, VALUE_BITS);

    return 0;
}


/** Finish decoding */
void ff_bgmc_decode_end(GetBitContext *gb)
{
    skip_bits_long(gb, -(VALUE_BITS - 2));
}


/** Read and decode a block Gilbert-Moore coded symbol */
void ff_bgmc_decode(GetBitContext *gb, unsigned int num, int32_t *dst,
                    int delta, unsigned int sx,
                    unsigned int *h, unsigned int *l, unsigned int *v,
                    uint8_t *cf_lut, int *cf_lut_status)
{
    unsigned int i;
    uint8_t *lut = bgmc_lut_getp(cf_lut, cf_lut_status, delta);

    // read current state
    unsigned int high  = *h;
    unsigned int low   = *l;
    unsigned int value = *v;

    lut += sx * LUT_SIZE;

    // decode num samples
    for (i = 0; i < num; i++) {
        unsigned int range  = high - low + 1;
        unsigned int target = (((value - low + 1) << FREQ_BITS) - 1) / range;
        unsigned int symbol = lut[target >> (FREQ_BITS - LUT_BITS)] << delta;

        while (cf_table[sx][symbol] > target)
            symbol += 1 << delta;

        symbol = (symbol >> delta) - 1;

        high = low + ((range * cf_table[sx][(symbol)     << delta] - (1 << FREQ_BITS)) >> FREQ_BITS);
        low  = low + ((range * cf_table[sx][(symbol + 1) << delta])                    >> FREQ_BITS);

        while (1) {
            if (high >= HALF) {
                if (low >= HALF) {
                    value -= HALF;
                    low   -= HALF;
                    high  -= HALF;
                } else if (low >= FIRST_QTR && high < THIRD_QTR) {
                    value -= FIRST_QTR;
                    low   -= FIRST_QTR;
                    high  -= FIRST_QTR;
                } else
                    break;
            }

            low  *= 2;
            high  = 2 * high + 1;
            value = 2 * value + get_bits1(gb);
        }

        *dst++ = symbol;
    }

    // save current state
    *h = high;
    *l = low;
    *v = value;
}
