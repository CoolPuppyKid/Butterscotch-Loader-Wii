#pragma once

#include <stdbool.h>
#include <stdio.h>

bool WiiLog_open(const char* path);
void WiiLog_close(void);
FILE* WiiLog_file(void);
int WiiLog_fprintf(FILE* stream, const char* fmt, ...);
