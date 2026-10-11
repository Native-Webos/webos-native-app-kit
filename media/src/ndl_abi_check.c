/* SPDX-License-Identifier: GPL-3.0-or-later
 * Compile-time proof that ndl_abi.h matches the NDK's NDL v2 header (webOS builds only). */
#define NDL_DIRECTMEDIA_API_VERSION 2
#include <libndl-media/NDL_directmedia_v2.h>
#include <stddef.h>

#include "ndl_abi.h"

_Static_assert(sizeof(wnk_ndl_data_info) == sizeof(NDL_DIRECTMEDIA_DATA_INFO_T), "data info size");
_Static_assert(offsetof(wnk_ndl_data_info, audio) == offsetof(NDL_DIRECTMEDIA_DATA_INFO_T, audio), "audio offset");
_Static_assert(sizeof(wnk_ndl_pcm_info) == sizeof(NDL_DIRECTAUDIO_PCM_INFO_T), "pcm info size");
_Static_assert(offsetof(wnk_ndl_pcm_info, sample_rate) == offsetof(NDL_DIRECTAUDIO_PCM_INFO_T, sampleRate), "pcm rate offset");
_Static_assert(offsetof(wnk_ndl_pcm_info, channel_mode) == offsetof(NDL_DIRECTAUDIO_PCM_INFO_T, channelMode), "pcm mode offset");
_Static_assert((int)WNK_NDL_VIDEO_H265 == (int)NDL_VIDEO_TYPE_H265 && (int)WNK_NDL_AUDIO_MP3 == (int)NDL_AUDIO_TYPE_MP3, "enum values");

int wnk_ndl_abi_checked = 1;
