// Overseer native <-> managed ABI.
//
// This header is the single source of truth for the interop contract between
// overseer_native.dll (C++) and Overseer.Host (C#). The managed mirror lives in
// managed/src/Overseer.Host/Interop/NativeApi.cs — keep both in sync and bump
// OVERSEER_ABI_VERSION on any layout change.
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVERSEER_ABI_VERSION 1u

enum overseer_log_level {
    OVERSEER_LOG_DEBUG = 0,
    OVERSEER_LOG_INFO = 1,
    OVERSEER_LOG_WARN = 2,
    OVERSEER_LOG_ERROR = 3,
};

enum overseer_command_status {
    OVERSEER_COMMAND_OK = 0,
    OVERSEER_COMMAND_FAILED = 1,
    OVERSEER_COMMAND_UNSUPPORTED = 2,
};

// Functions the native side provides to managed code.
typedef struct overseer_native_api {
    uint32_t size;     // sizeof(overseer_native_api)
    uint32_t version;  // OVERSEER_ABI_VERSION
    const char* root_directory;  // UTF-8, e.g. "...\\SCUM\\Binaries\\Win64\\overseer"

    // Write a UTF-8 message to the native log. Thread-safe.
    void (*log)(int32_t level, const char* message);

    // Queue a SCUM admin command (UTF-8, e.g. "#Announce hi") for execution on
    // the game thread. Returns 0 if accepted. The result is delivered later via
    // overseer_managed_api.on_command_result with the same request_id, possibly
    // on another thread.
    int32_t (*submit_command)(int64_t request_id, const char* command);
} overseer_native_api;

// Functions managed code provides back to the native side (filled in by
// Overseer.Host during initialize).
typedef struct overseer_managed_api {
    uint32_t size;
    uint32_t version;

    void (*on_command_result)(int64_t request_id, int32_t status, const char* output);
} overseer_managed_api;

// Signature of Overseer.Host.Interop.NativeEntry.Initialize
// ([UnmanagedCallersOnly]). Returns 0 on success.
typedef int32_t (*overseer_managed_initialize_fn)(const overseer_native_api* native,
                                                  overseer_managed_api* managed);

#ifdef __cplusplus
}
#endif
