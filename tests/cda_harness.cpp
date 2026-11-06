/* CD audio emulation test client. Build: i686-w64-mingw32-g++-win32 -std=c++17 -O2 -static -o cda_harness.exe cda_harness.cpp -lwinmm
   Needs a proxy built with -DCDA_TEST and the Music folder (TrackNN.flac, cdaudio.toc) beside it. Run under Wine or Windows. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
static int fails;
#define CHECK(c, ...) do { if (c) printf("  ok   "); else { printf("  FAIL "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)
static HWND hw; static volatile LONG last_notify = -1, last_dev, n_notify;
static LRESULT CALLBACK wp(HWND h, UINT m, WPARAM w, LPARAM l)
{ if (m == MM_MCINOTIFY) { last_notify = (LONG)w; last_dev = (LONG)l; InterlockedIncrement(&n_notify); } return DefWindowProcA(h, m, w, l); }
static int wait_notify(DWORD ms) { DWORD t = GetTickCount(); LONG n0 = n_notify; MSG m;
  while (GetTickCount() - t < ms) { while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m); if (n_notify != n0) return (int)(GetTickCount() - t); Sleep(5); } return -1; }
static void pump(DWORD ms) { DWORD t = GetTickCount(); MSG m; while (GetTickCount() - t < ms) { while (PeekMessageA(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageA(&m); Sleep(5); } }
static DWORD st(UINT dev, DWORD flags, DWORD item, DWORD trk, MCIERROR *e) { MCI_STATUS_PARMS p = {0}; p.dwItem = item; p.dwTrack = trk; *e = mciSendCommandA(dev, MCI_STATUS, flags, (DWORD_PTR)&p); return (DWORD)p.dwReturn; }
static int msf2f(DWORD v) { return (MCI_MSF_MINUTE(v) * 60 + MCI_MSF_SECOND(v)) * 75 + MCI_MSF_FRAME(v); }
int main(void)
{
  WNDCLASSA wc = {0}; MCIERROR e; int i; DWORD pos[24];
  HMODULE d = LoadLibraryA("ddraw.dll");
  printf("proxy loaded: %s\n", d ? "yes" : "NO");
  wc.lpfnWndProc = wp; wc.lpszClassName = "t"; RegisterClassA(&wc);
  hw = CreateWindowA("t", "t", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, 0, 0); printf("test window %p\n", (void*)hw);
  /* game: open cdaudio, type string, shareable */
  MCI_OPEN_PARMSA o = {0}; o.lpstrDeviceType = "cdaudio";
  e = mciSendCommandA(0, MCI_OPEN, 0x2100, (DWORD_PTR)&o);
  UINT dev = o.wDeviceID;
  CHECK(e == 0 && dev == 0xCDA, "OPEN cdaudio -> err %lu device 0x%X", e, dev);
  CHECK(st(dev, MCI_STATUS_ITEM, MCI_STATUS_TIME_FORMAT, 0, &e) == MCI_FORMAT_MSF && !e, "time format is MSF");
  DWORD nt = st(dev, MCI_STATUS_ITEM, MCI_STATUS_NUMBER_OF_TRACKS, 0, &e);
  CHECK(nt == 22, "number of tracks %lu", nt);
  DWORD len = st(dev, MCI_STATUS_ITEM, MCI_STATUS_LENGTH, 0, &e);
  CHECK(msf2f(len) == 219226 + 150, "disc length %02lu:%02lu:%02lu (%d frames)", MCI_MSF_MINUTE(len), MCI_MSF_SECOND(len), MCI_MSF_FRAME(len), msf2f(len));
  for (i = 1; i <= 22; i++) pos[i] = st(dev, MCI_STATUS_ITEM | MCI_TRACK, MCI_STATUS_POSITION, i, &e);
  CHECK(msf2f(pos[1]) == 150 && msf2f(pos[2]) == 44960 + 150 && msf2f(pos[22]) == 197596 + 150,
        "track starts: 1=%d 2=%d 22=%d frames", msf2f(pos[1]), msf2f(pos[2]), msf2f(pos[22]));
  DWORD tl = st(dev, MCI_STATUS_ITEM | MCI_TRACK, MCI_STATUS_LENGTH, 10, &e);
  CHECK(msf2f(tl) == 724, "track 10 length %d frames", msf2f(tl));
  CHECK(st(dev, MCI_STATUS_ITEM, MCI_STATUS_MODE, 0, &e) == MCI_MODE_STOP, "mode stopped after open");
  /* game: play track N from its start to next track start, notify (flags 0xD) */
  MCI_PLAY_PARMS pp = {0}; pp.dwCallback = (DWORD_PTR)hw; pp.dwFrom = pos[10]; pp.dwTo = pos[11];
  DWORD t0 = GetTickCount();
  e = mciSendCommandA(dev, MCI_PLAY, MCI_NOTIFY | MCI_FROM | MCI_TO, (DWORD_PTR)&pp);
  CHECK(e == 0, "PLAY track 10 with notify");
  pump(300);
  CHECK(st(dev, MCI_STATUS_ITEM, MCI_STATUS_MODE, 0, &e) == MCI_MODE_PLAY, "mode is PLAY while playing");
  CHECK(st(dev, MCI_STATUS_ITEM, MCI_STATUS_CURRENT_TRACK, 0, &e) == 10, "current track 10");
  int w = wait_notify(15000);
  DWORD el = GetTickCount() - t0;
  CHECK(w >= 0 && last_notify == MCI_NOTIFY_SUCCESSFUL && last_dev == 0xCDA, "notify SUCCESSFUL dev 0x%lX at end", last_dev);
  CHECK(el > 9300 && el < 10600, "track 10 took %lu ms (expect ~9653)", el);
  CHECK(st(dev, MCI_STATUS_ITEM, MCI_STATUS_MODE, 0, &e) == MCI_MODE_STOP, "mode STOP after end");
  /* game pause: STOP (flags MCI_WAIT) while notify pending -> ABORTED, position kept */
  pp.dwFrom = pos[3]; pp.dwTo = pos[4];
  mciSendCommandA(dev, MCI_PLAY, MCI_NOTIFY | MCI_FROM | MCI_TO, (DWORD_PTR)&pp);
  pump(1500);
  e = mciSendCommandA(dev, MCI_STOP, MCI_WAIT, 0);
  w = wait_notify(1000);
  CHECK(e == 0 && w >= 0 && last_notify == MCI_NOTIFY_ABORTED, "STOP during play -> notify ABORTED");
  DWORD p = st(dev, MCI_STATUS_ITEM, MCI_STATUS_POSITION, 0, &e);
  int into = msf2f(p) - msf2f(pos[3]);
  CHECK(into > 90 && into < 140, "position kept after STOP: %d frames into track 3 (~112 expected)", into);
  /* new PLAY with notify while one pending -> SUPERSEDED */
  pp.dwFrom = pos[5]; pp.dwTo = pos[6];
  mciSendCommandA(dev, MCI_PLAY, MCI_NOTIFY | MCI_FROM | MCI_TO, (DWORD_PTR)&pp);
  pump(200);
  pp.dwFrom = pos[12]; pp.dwTo = pos[13];
  mciSendCommandA(dev, MCI_PLAY, MCI_NOTIFY | MCI_FROM | MCI_TO, (DWORD_PTR)&pp);
  w = wait_notify(1000);
  CHECK(w >= 0 && last_notify == MCI_NOTIFY_SUPERSEDED, "second PLAY -> notify SUPERSEDED for the first");
  CHECK(st(dev, MCI_STATUS_ITEM, MCI_STATUS_CURRENT_TRACK, 0, &e) == 12, "now on track 12");
  /* last track: game uses FROM only (flags 5) */
  e = mciSendCommandA(dev, MCI_STOP, MCI_WAIT, 0); wait_notify(500);
  pp.dwFrom = pos[22];
  e = mciSendCommandA(dev, MCI_PLAY, MCI_NOTIFY | MCI_FROM, (DWORD_PTR)&pp);
  pump(200);
  CHECK(e == 0 && st(dev, MCI_STATUS_ITEM, MCI_STATUS_CURRENT_TRACK, 0, &e) == 22, "PLAY last track with FROM only");
  /* out of range track status */
  st(dev, MCI_STATUS_ITEM | MCI_TRACK, MCI_STATUS_POSITION, 30, &e);
  CHECK(e == MCIERR_OUTOFRANGE, "track 30 status -> MCIERR_OUTOFRANGE");
  /* close */
  e = mciSendCommandA(dev, MCI_CLOSE, 0, 0);
  w = wait_notify(500);
  CHECK(e == 0 && last_notify == MCI_NOTIFY_ABORTED, "CLOSE while playing -> ABORTED");
  /* decode path: whole track 5, and a span crossing tracks 12 -> 13 */
  int (*dec)(int, int, const char *) = reinterpret_cast<int (*)(int, int, const char *)>(reinterpret_cast<void *>(GetProcAddress(d, "cda_test_decode")));
  if (dec) {
    printf("  decoded track 5: %d samples\n", dec(52548, 52548 + 2411, "dec_t5.raw"));
    printf("  decoded 12->13 span: %d samples\n", dec(141213 + 400, 142003 + 300, "dec_span.raw"));
  }
  printf("RESULT: %d failure(s)\n", fails);
  return fails;
}
