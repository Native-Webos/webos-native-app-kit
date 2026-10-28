// SPDX-License-Identifier: MIT
//
// The six libdovi C API functions the kit calls, declared as in dolby_vision 3.4.0's src/capi.rs and
// src/c_structs/buffers.rs (quietvoid, MIT). Its generated rpu_parser.h names the opaque type
// DoviRpuOpaque and the buffer DoviData; cbindgen is not needed for these few.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DoviRpuOpaque DoviRpuOpaque;

typedef struct DoviData {
    const uint8_t *data;
    size_t len;
} DoviData;

DoviRpuOpaque *dovi_parse_unspec62_nalu(const uint8_t *buf, size_t len);
void dovi_rpu_free(DoviRpuOpaque *ptr);
const char *dovi_rpu_get_error(const DoviRpuOpaque *ptr);
// mode 2: Profile 8.1, luma and chroma mapping set to no-op (handles source profiles 5, 7 and 8).
int32_t dovi_convert_rpu_with_mode(DoviRpuOpaque *ptr, uint8_t mode);
const DoviData *dovi_write_unspec62_nalu(DoviRpuOpaque *ptr);
void dovi_data_free(const DoviData *data);

#ifdef __cplusplus
}
#endif
