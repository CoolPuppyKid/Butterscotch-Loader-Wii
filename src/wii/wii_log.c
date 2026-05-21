#include "wii_log.h"

#include <stdarg.h>

static FILE* g_wiiLogFile = NULL;

bool WiiLog_open(const char* path)
{
    g_wiiLogFile = fopen(path, "w");
    if (!g_wiiLogFile) return false;
    setvbuf(g_wiiLogFile, NULL, _IONBF, 0);
    fprintf(g_wiiLogFile, "=== Butterscotch Wii log opened ===\n");
    return true;
}

void WiiLog_close(void)
{
    if (!g_wiiLogFile) return;
    fclose(g_wiiLogFile);
    g_wiiLogFile = NULL;
}

FILE* WiiLog_file(void)
{
    return g_wiiLogFile;
}

int WiiLog_fprintf(FILE* stream, const char* fmt, ...)
{
    if (stream == stderr && !g_wiiLogFile) return 0;

    va_list args;
    va_start(args, fmt);

    FILE* target = (stream == stderr && g_wiiLogFile) ? g_wiiLogFile : stream;
    int result = vfprintf(target, fmt, args);
    if (target) fflush(target);

    va_end(args);
    return result;
}
