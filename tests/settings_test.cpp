/* Settings file migration test (hardening.cpp settings_migrate), run under Wine by tests/settings_test.sh.
   Usage: settings_test.exe <ddraw_proxy.ini> <version>: migrates the file and prints the log line. */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include "hardening.h"

void plog(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); putchar('\n');
}

int main(int argc, char **argv)
{
    if (argc < 3) { printf("usage: settings_test <ini> <version>\n"); return 2; }
    std::string r = settings_migrate(argv[1], argv[2]);
    printf("%s\n", r.empty() ? "(no change)" : r.c_str());
    return 0;
}
