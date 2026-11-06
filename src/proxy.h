/* Shared declarations for the Daytona USA ddraw proxy */
#pragma once
#include <windows.h>
#include <mmsystem.h>

void plog(const char *fmt, ...);

/* Function-pointer helpers: C++ does not convert between function and data pointers implicitly. */
template <typename F> inline void *fnptr(F f) { return reinterpret_cast<void *>(f); }
template <typename F> inline void set_fn(F &dst, FARPROC p) { dst = reinterpret_cast<F>(reinterpret_cast<void *>(p)); }
template <typename F> inline void set_fn(F &dst, void *p) { dst = reinterpret_cast<F>(p); }

/* CD audio emulation (cdaudio_emu.cpp) */
int      cda_init(const char *gamedir, int volume_percent);   /* returns number of audio tracks found */
int      cda_available(void);
int      cda_track_ticks20(void);   /* play position in the current track, 20ths of a second; -1 if unknown */
int      cda_is_device(MCIDEVICEID id);
int      cda_is_open_request(UINT msg, DWORD_PTR flags, DWORD_PTR param);
MCIERROR cda_command(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR param);
void     cda_shutdown(void);
