/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The NDL DirectMedia v2 structures the kit passes to libNDL_directmedia.so.1, mirrored from the
 * NDK's community header (sysroot usr/include/libndl-media, API version 2). That header is C-only,
 * so ndl_abi_check.c (webOS builds) includes both and asserts that sizes and offsets agree. */
#ifndef WNK_MEDIA_NDL_ABI_H
#define WNK_MEDIA_NDL_ABI_H

typedef void (*wnk_ndl_resource_released)(const char *type);
typedef void (*wnk_ndl_load_callback)(int type, long long num, const char *str);

enum { WNK_NDL_VIDEO_H264 = 1, WNK_NDL_VIDEO_H265 = 2, WNK_NDL_VIDEO_VP9 = 3, WNK_NDL_VIDEO_AV1 = 4 };
enum { WNK_NDL_AUDIO_PCM = 1, WNK_NDL_AUDIO_MP3 = 2, WNK_NDL_AUDIO_OPUS = 3 };

typedef struct wnk_ndl_pcm_info {
    int type;
    int unknown1;
    const char *format;
    const char *layout;
    const char *channel_mode;
    int sample_rate; /* NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE */
} wnk_ndl_pcm_info;

typedef struct wnk_ndl_data_info {
    struct {
        int width;
        int height;
        int type;
        int unknown1;
    } video;
    union {
        int type;
        wnk_ndl_pcm_info pcm;
        char padding[32];
    } audio;
} wnk_ndl_data_info;

#endif
