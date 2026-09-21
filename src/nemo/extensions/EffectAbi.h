#pragma once

/* Nemo pointwise effect ABI 1. Trusted installed native code, not a sandbox.
 * No Qt, STL, exceptions, allocation ownership or live document objects cross
 * this interface. This is deliberately not OpenFX or a general plugin SDK.
 *
 * The entrypoint returns immutable process-lifetime metadata. Callbacks are
 * reentrant and may run concurrently on workers. Strings, arrays and buffers
 * are borrowed for the invocation only. The host owns every output buffer.
 * Every callback returns 0 on success; on failure it returns nonzero and writes
 * a NUL-terminated diagnostic into the supplied nonempty buffer. The package
 * catches all exceptions. The host retains the library through all callbacks
 * and submitted GPU work; restart is required to change installed packages.
 *
 * Parameters are a UTF-8 JSON object of resolved plain values (numbers,
 * booleans, strings or arrays), including defaults/animation/instance overrides.
 * CPU pixels are interleaved scene-linear RGBA floats; count is pixels, not
 * bytes. Input/output never alias. Alpha association is explicit in flags.
 * Payload layout/size, shader binding version, schema and implementation/state
 * versions are declared in the validated manifest before this entrypoint runs.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEMO_EFFECT_ABI_VERSION 1u
#define NEMO_EFFECT_PREMULTIPLIED 1u
/* Preserve samples for data images or an incomplete primary RGB channel set. */
#define NEMO_EFFECT_BYPASS_COLOR 2u

#if defined(_WIN32) && defined(NEMO_EFFECT_BUILD)
#define NEMO_EFFECT_EXPORT __declspec(dllexport)
#elif defined(_WIN32)
#define NEMO_EFFECT_EXPORT
#else
#define NEMO_EFFECT_EXPORT __attribute__((visibility("default")))
#endif

typedef struct NemoEffectV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    int (*validate)(const char* parameters, char* error, uint32_t error_capacity);
    int (*process)(const char* parameters, const float* input, float* output, uint64_t pixel_count, uint32_t flags,
                   char* error, uint32_t error_capacity);
    int (*prepare)(const char* parameters, uint32_t flags, void* payload, uint32_t payload_bytes, char* error,
                   uint32_t error_capacity);
} NemoEffectV1;

typedef const NemoEffectV1* (*NemoEffectEntryV1)(void);
NEMO_EFFECT_EXPORT const NemoEffectV1* nemo_effect_v1(void);

#ifdef __cplusplus
}
#endif
