// SPDX-FileCopyrightText: 2026 ypb6010-ux
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <stddef.h>
#include <stdint.h>

#define FIELD_RUNTIME_DRIVER_ABI_V1 1u

#if !defined(FIELD_RUNTIME_DRIVER_BUILD)
#define FIELD_RUNTIME_DRIVER_EXPORT
#elif defined(_WIN32) || defined(_WIN64)
#define FIELD_RUNTIME_DRIVER_EXPORT __declspec(dllexport)
#else
#define FIELD_RUNTIME_DRIVER_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FieldRuntimeDriverHostV1 {
    void* context;
    void (*publish)(void* context, const char* device_id,
                    const char* target_id, const uint8_t* data, size_t size);
    void (*log)(void* context, int level, const char* message);
} FieldRuntimeDriverHostV1;

typedef struct FieldRuntimeDriverApiV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char* driver_id;
    void* (*create)(const char* config, const FieldRuntimeDriverHostV1* host,
                    char* error, size_t error_size);
    void (*destroy)(void* instance);
    int (*start)(void* instance, char* error, size_t error_size);
    void (*stop)(void* instance);
    int (*write)(void* instance, const char* device_id, const char* target_id,
                 const uint8_t* data, size_t size,
                 char* error, size_t error_size);
} FieldRuntimeDriverApiV1;

typedef const FieldRuntimeDriverApiV1* (*FieldRuntimeDriverGetApiFn)(
    uint32_t host_abi_version);

FIELD_RUNTIME_DRIVER_EXPORT const FieldRuntimeDriverApiV1*
fieldRuntimeDriverGetApi(uint32_t host_abi_version);

#ifdef __cplusplus
}
#endif
