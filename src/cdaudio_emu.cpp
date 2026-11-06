/*
 * MCI "cdaudio" emulation for Daytona USA.
 *
 * Answers the game's mciSendCommandA calls for the CD audio device and plays the disc's
 * audio tracks from Music\TrackNN.flac (lossless rips, one file per disc track, each running
 * from its INDEX 01 to the next track's INDEX 01). Disc layout comes from Music\cdaudio.toc;
 * if that is missing it is rebuilt from the FLAC lengths.
 *
 * Behaviour copied from a real cdaudio device as the game relies on it:
 *  - default time format MSF, absolute disc positions include the 2 second (150 frame) lead-in
 *  - PLAY with MCI_NOTIFY posts MM_MCINOTIFY(MCI_NOTIFY_SUCCESSFUL, id) to the callback when it
 *    reaches the end; STOP/PAUSE/SEEK/CLOSE while a notify is pending post MCI_NOTIFY_ABORTED;
 *    a new PLAY with notify posts MCI_NOTIFY_SUPERSEDED for the old one
 *  - STOP keeps the current position (the game uses STOP as pause)
 *
 * Output: waveOut, 44.1 kHz 16-bit stereo, streamed from a worker thread.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include "proxy.h"

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#define DR_FLAC_NO_STDIO_WIDE
#include "third_party/dr_flac.h"

#define CDA_DEVICE_ID   0xCDA
#define MAX_TRACKS      99
#define LEADIN          150          /* frames */
#define SPF             588          /* samples per CD frame */
#define NBUF            6
#define BUF_SAMPLES     4410         /* 100 ms per buffer */

typedef struct { int start, length, audio; char path[MAX_PATH]; } Track;   /* sectors, no lead-in */

static Track  t_tracks[MAX_TRACKS + 1];
static int    t_count;               /* number of tracks on disc (incl. data track 1) */
static int    t_naudio;
static int    t_total;               /* total sectors */
static int    t_volume = 100;
static int    t_ready;

static CRITICAL_SECTION cs;
static HANDLE   wake_ev, thread_h;
static volatile int quit_flag;

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
static int      st = ST_STOPPED;
static DWORD    timefmt = MCI_FORMAT_MSF;
static int      open_count;
static long long pos;                /* current position in samples from sector 0 (no lead-in) */
static long long play_to;            /* end, samples */
static long long dec_pos;            /* next sample to decode */
static long long stream_base;        /* disc sample at which the current waveOut stream started */
static HWND     notify_hwnd;
static int      notify_pending;
static int      ending;              /* decoder reached play_to, waiting for buffers to drain */

static HWAVEOUT wo;
static WAVEHDR  hdr[NBUF];
static short    bufmem[NBUF][BUF_SAMPLES * 2];
static int      hdr_inuse[NBUF];

static int      nullout;             /* no audio device: keep time silently so notifications still arrive */
static DWORD    tick0;
static int      nullout_logged;
static drflac  *cur_dec;
static int      cur_track;

/* ---------- table of contents ---------- */
static int track_of_sector(int s)
{
    int t;
    for (t = t_count; t >= 1; t--) if (s >= t_tracks[t].start) return t;
    return 1;
}

static int load_toc(const char *dir)
{
    char p[MAX_PATH]; FILE *f; int t;
    snprintf(p, sizeof p, "%sMusic\\cdaudio.toc", dir);
    memset(t_tracks, 0, sizeof t_tracks); t_count = 0;
    if ((f = fopen(p, "r"))) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            int n, s, l; char type[16];
            if (line[0] == ';' || line[0] == '#') continue;
            if (sscanf(line, "%d %d %d %15s", &n, &s, &l, type) == 4 && n >= 1 && n <= MAX_TRACKS) {
                t_tracks[n].start = s; t_tracks[n].length = l; t_tracks[n].audio = !_stricmp(type, "audio");
                if (n > t_count) t_count = n;
            }
        }
        fclose(f);
        plog("cdaudio: table of contents from %s (%d tracks)", p, t_count);
    } else {
        /* rebuild from the FLAC files: track 1 data (length from this disc), then audio tracks */
        int next;
        t_tracks[1].start = 0; t_tracks[1].length = 44960; t_tracks[1].audio = 0; t_count = 1;
        next = t_tracks[1].length;
        for (t = 2; t <= MAX_TRACKS; t++) {
            char fp[MAX_PATH]; drflac *d;
            snprintf(fp, sizeof fp, "%sMusic\\Track%02d.flac", dir, t);
            if (!(d = drflac_open_file(fp, NULL))) break;
            t_tracks[t].start = next; t_tracks[t].length = (int)(d->totalPCMFrameCount / SPF); t_tracks[t].audio = 1;
            next += t_tracks[t].length; t_count = t; drflac_close(d);
        }
        plog("cdaudio: no cdaudio.toc, layout rebuilt from FLAC files (%d tracks)", t_count);
    }
    t_naudio = 0; t_total = 0;
    for (t = 1; t <= t_count; t++) {
        if (t_tracks[t].audio) {
            snprintf(t_tracks[t].path, MAX_PATH, "%sMusic\\Track%02d.flac", dir, t);
            if (GetFileAttributesA(t_tracks[t].path) == INVALID_FILE_ATTRIBUTES) {
                plog("cdaudio: missing %s (track will play as silence)", t_tracks[t].path);
                t_tracks[t].path[0] = 0;
            } else t_naudio++;
        }
        if (t_tracks[t].start + t_tracks[t].length > t_total) t_total = t_tracks[t].start + t_tracks[t].length;
    }
    return t_naudio;
}

/* ---------- time format conversion (sectors are absolute, without lead-in) ---------- */
static int from_fmt(DWORD v)
{
    int s;
    switch (timefmt) {
    case MCI_FORMAT_TMSF: {
        int t = MCI_TMSF_TRACK(v);
        if (t < 1 || t > t_count) t = 1;
        s = t_tracks[t].start + (MCI_TMSF_MINUTE(v) * 60 + MCI_TMSF_SECOND(v)) * 75 + MCI_TMSF_FRAME(v);
        break; }
    case MCI_FORMAT_MILLISECONDS: s = (int)((long long)v * 75 / 1000) - LEADIN; break;
    default: s = (MCI_MSF_MINUTE(v) * 60 + MCI_MSF_SECOND(v)) * 75 + MCI_MSF_FRAME(v) - LEADIN; break;
    }
    if (s < 0) s = 0;
    if (s > t_total) s = t_total;
    return s;
}

static DWORD msf_of(int frames) { return MCI_MAKE_MSF(frames / 4500, (frames / 75) % 60, frames % 75); }

static DWORD to_fmt_pos(int sector)      /* absolute position */
{
    switch (timefmt) {
    case MCI_FORMAT_TMSF: { int t = track_of_sector(sector), o = sector - t_tracks[t].start;
        return MCI_MAKE_TMSF(t, o / 4500, (o / 75) % 60, o % 75); }
    case MCI_FORMAT_MILLISECONDS: return (DWORD)((long long)(sector + LEADIN) * 1000 / 75);
    default: return msf_of(sector + LEADIN);
    }
}

static DWORD to_fmt_len(int frames)      /* duration */
{
    if (timefmt == MCI_FORMAT_MILLISECONDS) return (DWORD)((long long)frames * 1000 / 75);
    return msf_of(frames);                /* TMSF lengths are reported as MSF */
}

/* ---------- playback ---------- */
static void post_notify(UINT code)
{
    if (notify_pending && notify_hwnd) {
        PostMessageA(notify_hwnd, MM_MCINOTIFY, code, CDA_DEVICE_ID);
        plog("cdaudio: notify %s", code == MCI_NOTIFY_SUCCESSFUL ? "SUCCESSFUL" : code == MCI_NOTIFY_ABORTED ? "ABORTED" :
             code == MCI_NOTIFY_SUPERSEDED ? "SUPERSEDED" : "FAILURE");
    }
    notify_pending = 0;
}

static long long played_now(void)   /* caller holds cs */
{
    if (st == ST_PLAYING && nullout) {
        long long p = stream_base + (long long)(GetTickCount() - tick0) * 44100 / 1000;
        return p > play_to ? play_to : p;
    }
    if (st == ST_PLAYING && wo) {
        MMTIME mt; mt.wType = TIME_SAMPLES;
        if (waveOutGetPosition(wo, &mt, sizeof mt) == MMSYSERR_NOERROR && mt.wType == TIME_SAMPLES) {
            long long p = stream_base + mt.u.sample;
            return p > play_to ? play_to : p;
        }
    }
    return pos;
}

static void close_decoder(void) { if (cur_dec) drflac_close(cur_dec); cur_dec = NULL; cur_track = 0; }

static void halt_output(void)       /* caller holds cs; keeps pos */
{
    int i;
    if (nullout) { pos = played_now(); nullout = 0; }
    else if (wo) {
        pos = played_now();
        waveOutReset(wo);
        for (i = 0; i < NBUF; i++) if (hdr_inuse[i]) { waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]); hdr_inuse[i] = 0; }
    }
    close_decoder();
    ending = 0;
}

static int ensure_wave(void)
{
    WAVEFORMATEX f; MMRESULT r;
    if (wo) return 1;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM; f.nChannels = 2; f.nSamplesPerSec = 44100; f.wBitsPerSample = 16;
    f.nBlockAlign = 4; f.nAvgBytesPerSec = 44100 * 4;
    r = waveOutOpen(&wo, WAVE_MAPPER, &f, (DWORD_PTR)wake_ev, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        if (!nullout_logged++) plog("cdaudio: waveOutOpen failed %u; playing silently (timing and notifications still emulated)", r);
        wo = NULL; return 0;
    }
    return 1;
}

static void start_stream(long long from)   /* caller holds cs */
{
    pos = from; dec_pos = from; stream_base = from; ending = 0;
    nullout = !ensure_wave();
    tick0 = GetTickCount();
    st = ST_PLAYING;
    SetEvent(wake_ev);
}

/* decode n samples (stereo frames) from disc sample dec_pos into out; returns samples written */
static int decode(short *out, int n)
{
    int done = 0;
    while (done < n && dec_pos < play_to) {
        int sector = (int)(dec_pos / SPF), t = track_of_sector(sector);
        long long tstart = (long long)t_tracks[t].start * SPF, tend = tstart + (long long)t_tracks[t].length * SPF;
        long long lim = tend < play_to ? tend : play_to;
        int want = n - done; if (dec_pos + want > lim) want = (int)(lim - dec_pos);
        if (want <= 0) { dec_pos = lim; continue; }
        if (!t_tracks[t].audio || !t_tracks[t].path[0]) {
            memset(out + done * 2, 0, (size_t)want * 4);
        } else {
            if (cur_track != t) {
                close_decoder();
                cur_dec = drflac_open_file(t_tracks[t].path, NULL);
                cur_track = t;
                if (cur_dec) drflac_seek_to_pcm_frame(cur_dec, (drflac_uint64)(dec_pos - tstart));
                else plog("cdaudio: cannot open %s", t_tracks[t].path);
            }
            drflac_uint64 got = cur_dec ? drflac_read_pcm_frames_s16(cur_dec, (drflac_uint64)want, out + done * 2) : 0;
            if ((int)got < want) memset(out + (done + (int)got) * 2, 0, (size_t)(want - (int)got) * 4);
            if (t_volume < 100) { int k; short *p = out + done * 2; for (k = 0; k < want * 2; k++) p[k] = (short)(p[k] * t_volume / 100); }
        }
        done += want; dec_pos += want;
    }
    return done;
}

static DWORD WINAPI worker(LPVOID arg)
{
    (void)arg;
    while (!quit_flag) {
        WaitForSingleObject(wake_ev, 50);
        EnterCriticalSection(&cs);
        if (st == ST_PLAYING && nullout) {
            if (played_now() >= play_to) {
                pos = play_to; st = ST_STOPPED; nullout = 0;
                post_notify(MCI_NOTIFY_SUCCESSFUL);
            }
        } else if (st == ST_PLAYING && wo) {
            int i, busy = 0;
            for (i = 0; i < NBUF; i++) {
                if (hdr_inuse[i] && (hdr[i].dwFlags & WHDR_DONE)) { waveOutUnprepareHeader(wo, &hdr[i], sizeof hdr[i]); hdr_inuse[i] = 0; }
                if (!hdr_inuse[i] && !ending) {
                    int n = decode(bufmem[i], BUF_SAMPLES);
                    if (n > 0) {
                        memset(&hdr[i], 0, sizeof hdr[i]);
                        hdr[i].lpData = (LPSTR)bufmem[i]; hdr[i].dwBufferLength = (DWORD)n * 4;
                        waveOutPrepareHeader(wo, &hdr[i], sizeof hdr[i]);
                        waveOutWrite(wo, &hdr[i], sizeof hdr[i]);
                        hdr_inuse[i] = 1;
                    }
                    if (dec_pos >= play_to) ending = 1;
                }
                if (hdr_inuse[i]) busy++;
            }
            if (ending && busy == 0) {
                pos = play_to; st = ST_STOPPED; ending = 0; close_decoder();
                waveOutReset(wo);
                post_notify(MCI_NOTIFY_SUCCESSFUL);
            }
        }
        LeaveCriticalSection(&cs);
    }
    return 0;
}

/* ---------- public ---------- */
int cda_init(const char *gamedir, int volume_percent)
{
    InitializeCriticalSection(&cs);
    t_volume = volume_percent < 0 ? 0 : volume_percent > 100 ? 100 : volume_percent;
    if (load_toc(gamedir) == 0) { plog("cdaudio: no audio tracks found in %sMusic, emulation off", gamedir); return 0; }
    wake_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    thread_h = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    t_ready = 1;
    plog("cdaudio: emulation ready, %d audio tracks, disc length %d:%02d, music volume %d%%",
         t_naudio, (t_total + LEADIN) / 4500, ((t_total + LEADIN) / 75) % 60, t_volume);
    return t_naudio;
}

int cda_available(void) { return t_ready; }

/* Play position inside the track being played, in 20ths of a second (the karaoke lyric clock, 0.0.24); -1 if unknown.
   Taken from the samples actually played, so it follows the music through pauses. */
int cda_track_ticks20(void)
{
    if (!t_ready) return -1;
    EnterCriticalSection(&cs);
    long long p = played_now();
    int t = track_of_sector((int)(p / SPF));
    long long rel = p - (long long)t_tracks[t].start * SPF;
    LeaveCriticalSection(&cs);
    return rel < 0 ? 0 : (int)(rel * 20 / 44100);
}
int cda_is_device(MCIDEVICEID id) { return t_ready && id == CDA_DEVICE_ID; }

int cda_is_open_request(UINT msg, DWORD_PTR flags, DWORD_PTR param)
{
    MCI_OPEN_PARMSA *o = (MCI_OPEN_PARMSA *)param;
    if (!t_ready || msg != MCI_OPEN || !o || !(flags & MCI_OPEN_TYPE)) return 0;
    if (flags & MCI_OPEN_TYPE_ID) return LOWORD((DWORD_PTR)o->lpstrDeviceType) == MCI_DEVTYPE_CD_AUDIO;
    return o->lpstrDeviceType && !IS_INTRESOURCE(o->lpstrDeviceType) && !_stricmp(o->lpstrDeviceType, "cdaudio");
}

static void notify_now(DWORD_PTR flags, DWORD_PTR param)
{
    if ((flags & MCI_NOTIFY) && param) {
        HWND h = (HWND)((MCI_GENERIC_PARMS *)param)->dwCallback;
        if (h) PostMessageA(h, MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, CDA_DEVICE_ID);
    }
}

MCIERROR cda_command(MCIDEVICEID id, UINT msg, DWORD_PTR flags, DWORD_PTR param)
{
    MCIERROR r = 0;
    (void)id;
    EnterCriticalSection(&cs);
    switch (msg) {
    case MCI_OPEN: {
        MCI_OPEN_PARMSA *o = (MCI_OPEN_PARMSA *)param;
        o->wDeviceID = CDA_DEVICE_ID; open_count++;
        if (open_count == 1) { timefmt = MCI_FORMAT_MSF; pos = t_tracks[2].start ? (long long)t_tracks[2].start * SPF : 0; }
        notify_now(flags, param);
        break; }
    case MCI_CLOSE:
        if (open_count > 0) open_count--;
        if (open_count == 0) { if (notify_pending) post_notify(MCI_NOTIFY_ABORTED); halt_output(); st = ST_STOPPED; }
        notify_now(flags, param);
        break;
    case MCI_SET: {
        MCI_SET_PARMS *p = (MCI_SET_PARMS *)param;
        if ((flags & MCI_SET_TIME_FORMAT) && p) {
            if (p->dwTimeFormat == MCI_FORMAT_MSF || p->dwTimeFormat == MCI_FORMAT_TMSF || p->dwTimeFormat == MCI_FORMAT_MILLISECONDS)
                timefmt = p->dwTimeFormat;
            else r = MCIERR_BAD_TIME_FORMAT;
        }
        if (!r) notify_now(flags, param);
        break; }
    case MCI_STATUS: {
        MCI_STATUS_PARMS *p = (MCI_STATUS_PARMS *)param;
        int trk = (flags & MCI_TRACK) ? (int)p->dwTrack : 0;
        if (!p || !(flags & MCI_STATUS_ITEM)) { r = MCIERR_MISSING_PARAMETER; break; }
        if ((flags & MCI_TRACK) && (trk < 1 || trk > t_count)) { r = MCIERR_OUTOFRANGE; break; }
        switch (p->dwItem) {
        case MCI_STATUS_LENGTH:
            p->dwReturn = trk ? to_fmt_len(t_tracks[trk].length) : to_fmt_len(t_total + LEADIN); break;
        case MCI_STATUS_POSITION:
            if (trk) p->dwReturn = timefmt == MCI_FORMAT_TMSF ? MCI_MAKE_TMSF(trk, 0, 0, 0) : to_fmt_pos(t_tracks[trk].start);
            else p->dwReturn = to_fmt_pos((int)(played_now() / SPF));
            break;
        case MCI_STATUS_NUMBER_OF_TRACKS: p->dwReturn = (DWORD)t_count; break;
        case MCI_STATUS_MODE: p->dwReturn = st == ST_PLAYING ? MCI_MODE_PLAY : st == ST_PAUSED ? MCI_MODE_PAUSE : MCI_MODE_STOP; break;
        case MCI_STATUS_MEDIA_PRESENT: case MCI_STATUS_READY: p->dwReturn = TRUE; break;
        case MCI_STATUS_TIME_FORMAT: p->dwReturn = timefmt; break;
        case MCI_STATUS_CURRENT_TRACK: p->dwReturn = (DWORD)track_of_sector((int)(played_now() / SPF)); break;
        case MCI_CDA_STATUS_TYPE_TRACK:
            if (!trk) { r = MCIERR_MISSING_PARAMETER; break; }
            p->dwReturn = t_tracks[trk].audio ? MCI_CDA_TRACK_AUDIO : MCI_CDA_TRACK_OTHER; break;
        default: r = MCIERR_UNSUPPORTED_FUNCTION; break;
        }
        if (!r) notify_now(flags, param);
        break; }
    case MCI_PLAY: {
        MCI_PLAY_PARMS *p = (MCI_PLAY_PARMS *)param;
        long long from = (flags & MCI_FROM) && p ? (long long)from_fmt(p->dwFrom) * SPF : played_now();
        long long to = (flags & MCI_TO) && p ? (long long)from_fmt(p->dwTo) * SPF : (long long)t_total * SPF;
        if (to > (long long)t_total * SPF) to = (long long)t_total * SPF;
        if (to <= from) { r = MCIERR_OUTOFRANGE; break; }
        if (notify_pending) post_notify((flags & MCI_NOTIFY) ? MCI_NOTIFY_SUPERSEDED : MCI_NOTIFY_ABORTED);
        halt_output();
        play_to = to;
        if ((flags & MCI_NOTIFY) && p) { notify_hwnd = (HWND)p->dwCallback; notify_pending = 1; }
        plog("cdaudio: play track %d %d:%02d to track %d %d:%02d", track_of_sector((int)(from / SPF)),
             (int)((from / SPF - t_tracks[track_of_sector((int)(from / SPF))].start) / 4500),
             (int)(((from / SPF - t_tracks[track_of_sector((int)(from / SPF))].start) / 75) % 60),
             track_of_sector((int)((to - 1) / SPF)), (int)((to / SPF - t_tracks[track_of_sector((int)((to - 1) / SPF))].start) / 4500),
             (int)(((to / SPF - t_tracks[track_of_sector((int)((to - 1) / SPF))].start) / 75) % 60));
        start_stream(from);
        break; }
    case MCI_STOP:
    case MCI_PAUSE:
        if (notify_pending) post_notify(MCI_NOTIFY_ABORTED);
        if (st == ST_PLAYING) halt_output();
        st = (msg == MCI_PAUSE && st != ST_STOPPED) ? ST_PAUSED : ST_STOPPED;
        notify_now(flags, param);
        break;
    case MCI_RESUME:
        if (st == ST_PAUSED) start_stream(pos);
        notify_now(flags, param);
        break;
    case MCI_SEEK: {
        MCI_SEEK_PARMS *p = (MCI_SEEK_PARMS *)param;
        if (notify_pending) post_notify(MCI_NOTIFY_ABORTED);
        halt_output(); st = ST_STOPPED;
        if (flags & MCI_SEEK_TO_START) pos = 0;
        else if (flags & MCI_SEEK_TO_END) pos = (long long)t_total * SPF;
        else if ((flags & MCI_TO) && p) pos = (long long)from_fmt(p->dwTo) * SPF;
        notify_now(flags, param);
        break; }
    case MCI_GETDEVCAPS: {
        MCI_GETDEVCAPS_PARMS *p = (MCI_GETDEVCAPS_PARMS *)param;
        if (!p || !(flags & MCI_GETDEVCAPS_ITEM)) { r = MCIERR_MISSING_PARAMETER; break; }
        switch (p->dwItem) {
        case MCI_GETDEVCAPS_DEVICE_TYPE: p->dwReturn = MCI_DEVTYPE_CD_AUDIO; break;
        case MCI_GETDEVCAPS_CAN_PLAY: case MCI_GETDEVCAPS_HAS_AUDIO: case MCI_GETDEVCAPS_CAN_EJECT: p->dwReturn = TRUE; break;
        default: p->dwReturn = FALSE; break;
        }
        break; }
    case MCI_INFO: {
        MCI_INFO_PARMSA *p = (MCI_INFO_PARMSA *)param;
        if (p && p->lpstrReturn && p->dwRetSize) lstrcpynA(p->lpstrReturn, "Daytona USA CD audio (emulated)", (int)p->dwRetSize);
        break; }
    default:
        r = 0;  /* accept anything else silently, like a lenient driver */
        break;
    }
    LeaveCriticalSection(&cs);
    return r;
}

void cda_shutdown(void)
{
    if (!t_ready) return;
    quit_flag = 1; SetEvent(wake_ev);
    if (thread_h) { WaitForSingleObject(thread_h, 1000); CloseHandle(thread_h); }
    EnterCriticalSection(&cs);
    halt_output();
    if (wo) { waveOutClose(wo); wo = NULL; }
    LeaveCriticalSection(&cs);
}

#ifdef CDA_TEST
/* test only: decode disc sectors [from, to) to a raw file through the same path as playback */
extern "C" __declspec(dllexport) int cda_test_decode(int from_sector, int to_sector, const char *out)
{
    FILE *f = fopen(out, "wb"); short buf[4410 * 2]; int n, total = 0;
    if (!f) return -1;
    EnterCriticalSection(&cs);
    dec_pos = (long long)from_sector * SPF; play_to = (long long)to_sector * SPF;
    while ((n = decode(buf, 4410)) > 0) { fwrite(buf, 4, (size_t)n, f); total += n; }
    close_decoder();
    LeaveCriticalSection(&cs);
    fclose(f);
    return total;
}
#endif
