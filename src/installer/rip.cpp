/* Red Book CD audio to FLAC (libFLAC, level 8), each file verified by decoding it again (dr_flac). */
#include "rip.h"
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include "FLAC/stream_encoder.h"
#include "FLAC/metadata.h"
#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_STDIO
#include "../third_party/dr_flac.h"

namespace {

constexpr uint32_t kBlock = 20;   /* sectors compared at a time between the two reads */

/* Windows starts 32-bit threads with a 4-byte aligned stack; libFLAC's SSE code needs 16. Realigned once here,
   every function called from the thread keeps the alignment. */
__attribute__((force_align_arg_pointer)) void run_aligned(std::function<void()> f) { f(); }

std::wstring fmt(const wchar_t *f, ...)
{
    wchar_t b[512];
    va_list a; va_start(a, f); vswprintf(b, 512, f, a); va_end(a);
    return b;
}

/* ---------- reading a track ---------- */

/* A file image reads the same every time. A drive is read twice; blocks that differ are read again until two
   reads agree. */
bool read_track(Disc &disc, const Track &t, std::vector<uint8_t> &pcm, const RipCallbacks &cb,
                std::atomic<uint64_t> &read_done, std::wstring &err)
{
    size_t bytes = (size_t)t.length * kRawSector;
    pcm.resize(bytes);
    for (uint32_t s = 0; s < t.length; s += kBlock) {
        uint32_t n = std::min(kBlock, t.length - s);
        if (cb.cancelled()) { err = L"cancelled"; return false; }
        if (!disc.read_audio(t.start + s, n, pcm.data() + (size_t)s * kRawSector, err)) return false;
        read_done += n;
    }
    if (!disc.needs_double_read()) return true;
    std::vector<uint8_t> again(kBlock * kRawSector), third(kBlock * kRawSector);
    int fixed = 0;
    for (uint32_t s = 0; s < t.length; s += kBlock) {
        uint32_t n = std::min(kBlock, t.length - s);
        size_t len = (size_t)n * kRawSector;
        uint8_t *first = pcm.data() + (size_t)s * kRawSector;
        if (cb.cancelled()) { err = L"cancelled"; return false; }
        if (!disc.read_audio(t.start + s, n, again.data(), err)) return false;
        read_done += n;
        if (!memcmp(first, again.data(), len)) continue;
        bool agreed = false;
        for (int tries = 0; tries < 8 && !agreed; tries++) {
            if (!disc.read_audio(t.start + s, n, third.data(), err)) return false;
            if (!memcmp(third.data(), first, len)) agreed = true;
            else if (!memcmp(third.data(), again.data(), len)) { memcpy(first, third.data(), len); agreed = true; }
            else std::swap(again, third);
        }
        if (!agreed) {
            err = fmt(L"track %d: the drive gives different audio each time at sector %u (a scratched or dirty disc?)", t.number, t.start + s);
            return false;
        }
        fixed++;
    }
    if (fixed) cb.log(fmt(L"Track %02d: %d block(s) read differently once; read again until two reads agreed", t.number, fixed));
    return true;
}

/* ---------- FLAC ---------- */

struct MemOut { std::vector<uint8_t> data; size_t pos = 0, end = 0; };   /* end: furthest written (the header is rewritten last) */

FLAC__StreamEncoderWriteStatus mem_write(const FLAC__StreamEncoder *, const FLAC__byte buf[], size_t n, uint32_t, uint32_t, void *ud)
{
    MemOut *m = (MemOut *)ud;
    if (m->pos + n > m->data.size()) m->data.resize(m->pos + n);
    memcpy(m->data.data() + m->pos, buf, n);
    m->pos += n;
    if (m->pos > m->end) m->end = m->pos;
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}
FLAC__StreamEncoderSeekStatus mem_seek(const FLAC__StreamEncoder *, FLAC__uint64 off, void *ud) { ((MemOut *)ud)->pos = (size_t)off; return FLAC__STREAM_ENCODER_SEEK_STATUS_OK; }
FLAC__StreamEncoderTellStatus mem_tell(const FLAC__StreamEncoder *, FLAC__uint64 *off, void *ud) { *off = ((MemOut *)ud)->pos; return FLAC__STREAM_ENCODER_TELL_STATUS_OK; }

bool encode(const std::vector<uint8_t> &pcm, int track, std::vector<uint8_t> &flac, std::wstring &err)
{
    FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
    if (!e) { err = L"out of memory"; return false; }
    size_t frames = pcm.size() / 4;
    FLAC__stream_encoder_set_verify(e, true);
    FLAC__stream_encoder_set_compression_level(e, 8);
    FLAC__stream_encoder_set_channels(e, 2);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, 44100);
    FLAC__stream_encoder_set_total_samples_estimate(e, frames);
    FLAC__StreamMetadata *vc = FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    FLAC__StreamMetadata_VorbisComment_Entry entry;
    char num[8]; snprintf(num, sizeof num, "%d", track);
    FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(&entry, "TRACKNUMBER", num);
    FLAC__metadata_object_vorbiscomment_append_comment(vc, entry, false);
    FLAC__stream_encoder_set_metadata(e, &vc, 1);
    MemOut out;
    out.data.reserve(pcm.size() / 2);
    bool ok = FLAC__stream_encoder_init_stream(e, mem_write, mem_seek, mem_tell, nullptr, &out) == FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    std::vector<FLAC__int32> buf;
    const int16_t *s = (const int16_t *)pcm.data();
    for (size_t f = 0; ok && f < frames; ) {
        size_t n = std::min<size_t>(frames - f, 4096);
        buf.resize(n * 2);
        for (size_t i = 0; i < n * 2; i++) buf[i] = s[f * 2 + i];
        ok = FLAC__stream_encoder_process_interleaved(e, buf.data(), (uint32_t)n);
        f += n;
    }
    ok = FLAC__stream_encoder_finish(e) && ok;
    if (!ok) err = fmt(L"track %d: FLAC encoder error (%hs)", track, FLAC__StreamEncoderStateString[FLAC__stream_encoder_get_state(e)]);
    FLAC__stream_encoder_delete(e);
    FLAC__metadata_object_delete(vc);
    out.data.resize(out.end);
    flac.swap(out.data);
    return ok;
}

/* decoded with an independent decoder and compared with the disc audio */
bool matches(const std::vector<uint8_t> &flac, const std::vector<uint8_t> &pcm)
{
    unsigned int ch = 0, rate = 0;
    drflac_uint64 frames = 0;
    drflac_int16 *dec = drflac_open_memory_and_read_pcm_frames_s16(flac.data(), flac.size(), &ch, &rate, &frames, nullptr);
    bool ok = dec && ch == 2 && rate == 44100 && frames * 4 == pcm.size() && !memcmp(dec, pcm.data(), pcm.size());
    drflac_free(dec, nullptr);
    return ok;
}

bool load_file(const std::wstring &path, std::vector<uint8_t> &out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = ReadFile(h, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size();
    CloseHandle(h);
    return ok;
}

bool save_file(const std::wstring &path, const void *data, size_t n)
{
    std::wstring tmp = path + L".part";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, (DWORD)n, &put, nullptr) && put == n;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

} // namespace

bool rip_audio(Disc &disc, const std::wstring &dir, const RipCallbacks &cb, std::wstring &err)
{
    CreateDirectoryW(dir.c_str(), nullptr);
    std::vector<Track> audio;
    uint64_t total = 0;
    for (const Track &t : disc.tracks()) if (t.audio && t.length) { audio.push_back(t); total += t.length; }
    if (audio.empty()) { err = L"this disc or image has no CD audio tracks (an .iso holds only the data track). Use the CD or its .cue / .bin image so the music can be ripped, or untick the music option"; return false; }
    int passes = disc.needs_double_read() ? 2 : 1;

    /* work: reading (passes x sectors) then encoding (sectors), shared between this thread and the encoders */
    std::atomic<uint64_t> read_done{0}, encoded{0};
    auto report = [&] { cb.progress((double)(read_done + encoded * 2) / (double)(total * (passes + 2))); };

    struct Job { Track t; std::vector<uint8_t> pcm; };
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Job> queue;
    bool reading_done = false, failed = false;
    std::wstring fail_msg;
    int verified = 0, kept = 0;
    unsigned workers = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned w = 0; w < workers; w++) pool.emplace_back(run_aligned, [&] {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> l(mu);
                cv.wait(l, [&] { return !queue.empty() || reading_done || failed; });
                if (failed || (queue.empty() && reading_done)) return;
                job = std::move(queue.front());
                queue.pop_front();
            }
            cv.notify_all();
            std::wstring e;
            std::vector<uint8_t> flac;
            std::wstring path = dir + fmt(L"\\Track%02d.flac", job.t.number);
            bool ok = encode(job.pcm, job.t.number, flac, e);
            if (ok && !matches(flac, job.pcm)) { ok = false; e = fmt(L"track %d: the FLAC does not decode to the disc audio", job.t.number); }
            if (ok && !save_file(path, flac.data(), flac.size())) { ok = false; e = L"cannot write " + path; }
            std::lock_guard<std::mutex> l(mu);
            if (!ok) { if (!failed) { failed = true; fail_msg = e; } cv.notify_all(); return; }
            verified++;
            encoded += job.t.length;
            cb.log(fmt(L"Track%02d.flac  %6.1f s  %4.1f MB  verified", job.t.number, job.t.length / 75.0, flac.size() / 1048576.0));
            report();
        }
    });

    for (const Track &t : audio) {
        Job job{ t, {} };
        std::wstring e;
        if (!read_track(disc, t, job.pcm, cb, read_done, e)) {
            std::lock_guard<std::mutex> l(mu);
            if (!failed) { failed = true; fail_msg = e; }
            break;
        }
        report();
        std::vector<uint8_t> old;
        std::wstring path = dir + fmt(L"\\Track%02d.flac", t.number);
        if (load_file(path, old) && matches(old, job.pcm)) {   /* an earlier rip that is already right */
            std::lock_guard<std::mutex> l(mu);
            kept++;
            encoded += t.length;
            cb.log(fmt(L"Track%02d.flac  already there and matches the disc: kept", t.number));
            report();
            continue;
        }
        std::unique_lock<std::mutex> l(mu);
        cv.wait(l, [&] { return queue.size() < 2 || failed; });   /* at most two tracks waiting in memory */
        if (failed) break;
        queue.push_back(std::move(job));
        cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> l(mu);
        reading_done = true;
    }
    cv.notify_all();
    for (auto &th : pool) th.join();
    if (failed) { err = fail_msg; return false; }

    /* the disc layout for the proxy's CD audio emulation (same format as tools/cdrip/rip_cd_audio.py) */
    std::string toc = "; Daytona USA disc layout for CD audio emulation\r\n"
                      "; track  start_sector  length_sectors  type   (sectors of 2352 bytes, no 150-frame lead-in)\r\n";
    for (const Track &t : disc.tracks()) {
        char line[96];
        snprintf(line, sizeof line, "%5d  %12u  %14u  %s\r\n", t.number, t.start, t.length, t.audio ? "audio" : "data");
        toc += line;
    }
    if (!save_file(dir + L"\\cdaudio.toc", toc.data(), toc.size())) { err = L"cannot write " + dir + L"\\cdaudio.toc"; return false; }
    cb.log(fmt(L"Music: %d track(s) ripped and verified, %d kept, cdaudio.toc written", verified, kept));
    cb.progress(1.0);
    return true;
}
