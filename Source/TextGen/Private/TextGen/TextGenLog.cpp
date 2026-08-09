#include "TextGen/TextGenLog.h"

#include "HAL/IConsoleManager.h"

DEFINE_LOG_CATEGORY(LogTextGenAPI);

static TAutoConsoleVariable<int32> CVarTextGenDebug(
	TEXT("TextGen.Debug"),
	0,
	TEXT("Enable debug logging for TextGen.\n")
	TEXT("0 = disabled\n")
	TEXT("1 = enabled\n"),
	ECVF_Default);

bool TextGen_IsDebugEnabled()
{
	return CVarTextGenDebug.GetValueOnAnyThread() != 0;
}
