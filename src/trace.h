/* API trace for the logging pass ([Test] TraceCalls=1). See trace.cpp. */
#pragma once
#include <windows.h>

void trace_init(BYTE *base, int enabled);       /* wraps the game's imports; call once the import table is bound */
int  trace_enabled();
void trace_directdraw(void *dd);                /* trace an IDirectDraw object's methods */
void trace_surface(void *surface);              /* trace an IDirectDrawSurface's methods (once per vtable) */
void trace_name_object(void *p, const char *name);
void trace_message(UINT msg);                   /* count a window message for the current screen visit */
/* If 'current' is a trace stub, make the stub lead to 'fn' (our hook) instead of replacing the stub;
   the stub's previous target is returned through orig_out. Returns 1 if handled. */
int  trace_retarget(void *current, void *fn, void **orig_out);
void trace_shutdown();                          /* final report, call before the log is closed */
