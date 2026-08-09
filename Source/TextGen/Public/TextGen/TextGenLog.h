#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

// Declare a global log category for TextGen across compilation units
DECLARE_LOG_CATEGORY_EXTERN(LogTextGenAPI, Log, VeryVerbose);

/**
 * Debug logging helpers for TextGen.
 *
 * Keep this header lightweight and safe to include from any compilation unit.
 *
 * Debug logging can be enabled globally via the console variable:
 *   TextGen.Debug = 1
 *
 * For call-sites that have their own local flag (e.g. UTextGenSubsystem::bEnableDebugLogging),
 * use the *_LOCAL variants to combine local + global gates.
 */

/** Returns true if TextGen debug logging is enabled through the TextGen.Debug CVar. */
TEXTGEN_API bool TextGen_IsDebugEnabled();

FORCEINLINE bool TextGen_IsDebugEnabledLocal(const bool bLocalDebugEnabled)
{
    return bLocalDebugEnabled || TextGen_IsDebugEnabled();
}

#define TEXTGEN_DEBUG_LOG(Verbosity, Format, ...) \
    do { \
        if (TextGen_IsDebugEnabled()) { UE_LOG(LogTextGenAPI, Verbosity, Format, ##__VA_ARGS__); } \
    } while (0)

#define TEXTGEN_DEBUG_LOG_IF(Condition, Verbosity, Format, ...) \
    do { \
        if (TextGen_IsDebugEnabled() && (Condition)) { UE_LOG(LogTextGenAPI, Verbosity, Format, ##__VA_ARGS__); } \
    } while (0)

#define TEXTGEN_DEBUG_LOG_LOCAL(LocalDebugEnabled, Verbosity, Format, ...) \
    do { \
        if (TextGen_IsDebugEnabledLocal(LocalDebugEnabled)) { UE_LOG(LogTextGenAPI, Verbosity, Format, ##__VA_ARGS__); } \
    } while (0)

#define TEXTGEN_DEBUG_LOG_IF_LOCAL(LocalDebugEnabled, Condition, Verbosity, Format, ...) \
    do { \
        if (TextGen_IsDebugEnabledLocal(LocalDebugEnabled) && (Condition)) { UE_LOG(LogTextGenAPI, Verbosity, Format, ##__VA_ARGS__); } \
    } while (0)
