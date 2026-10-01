#include "log.h"

#include <windows.h>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static FILE* g_log = NULL;
static SRWLOCK g_lock = SRWLOCK_INIT;

void LogOpen(const char* path)
{
    // The last session's log is kept (Seasons.previous.log).
    char previous[MAX_PATH];
    sprintf_s(previous, "%.*s.previous.log", (int)(strlen(path) - 4), path);
    MoveFileExA(path, previous, MOVEFILE_REPLACE_EXISTING);
    g_log = _fsopen(path, "w", _SH_DENYNO);
}

void Log(const char* fmt, ...)
{
    if (!g_log)
        return;

    AcquireSRWLockExclusive(&g_lock);

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d  ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_log, fmt, args);
    va_end(args);

    fputc('\n', g_log);
    fflush(g_log);
    ReleaseSRWLockExclusive(&g_lock);
}
