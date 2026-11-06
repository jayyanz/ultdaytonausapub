/* Disc sources for the installer: a .cue / .bin image (one or several .bin files) or a CD in a drive. */
#include "disc.h"
#include <winioctl.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

namespace {

/* ---------- small helpers ---------- */

bool read_whole_file(const std::wstring &path, std::vector<uint8_t> &out, std::wstring &err)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = L"cannot open " + path; return false; }
    LARGE_INTEGER size;
    GetFileSizeEx(h, &size);
    out.resize((size_t)size.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD got = 0, want = (DWORD)std::min<size_t>(out.size() - done, 1 << 24);
        if (!ReadFile(h, out.data() + done, want, &got, nullptr) || !got) { CloseHandle(h); err = L"read error in " + path; return false; }
        done += got;
    }
    CloseHandle(h);
    return true;
}

std::wstring widen(const std::string &s)
{
    std::wstring w(s.size(), 0);
    int n = MultiByteToWideChar(CP_ACP, 0, s.data(), (int)s.size(), &w[0], (int)w.size());
    w.resize(n > 0 ? n : 0);
    return w;
}

/* ---------- ISO 9660 on the data track (images) ---------- */

class Iso9660 {
public:
    /* read one 2048-byte sector of the data track */
    virtual bool data_sector(uint32_t lba, uint8_t *out) = 0;
    virtual ~Iso9660() {}

    bool find(const std::wstring &path, uint32_t &extent, uint32_t &size)
    {
        uint8_t s[2048];
        if (!data_sector(16, s) || s[0] != 1 || memcmp(s + 1, "CD001", 5)) return false;
        const uint8_t *root = s + 156;
        extent = rd32(root + 2); size = rd32(root + 10);
        size_t p = 0;
        while (p < path.size()) {
            size_t q = path.find(L'\\', p);
            std::wstring part = path.substr(p, q == std::wstring::npos ? std::wstring::npos : q - p);
            p = q == std::wstring::npos ? path.size() : q + 1;
            if (!lookup(extent, size, part, extent, size)) return false;
        }
        return true;
    }

private:
    static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

    bool lookup(uint32_t dir, uint32_t dir_size, const std::wstring &name, uint32_t &extent, uint32_t &size)
    {
        uint8_t s[2048];
        for (uint32_t off = 0; off < dir_size; off += 2048) {
            if (!data_sector(dir + off / 2048, s)) return false;
            for (uint32_t i = 0; i < 2048;) {
                uint8_t len = s[i];
                if (!len) break;
                uint8_t nlen = s[i + 32];
                std::string n((const char *)s + i + 33, nlen);
                size_t semi = n.find(';');
                if (semi != std::string::npos) n.resize(semi);
                if (!n.empty() && n.back() == '.') n.pop_back();
                if (nlen > 1 || (s[i + 33] != 0 && s[i + 33] != 1)) {
                    std::wstring wn = widen(n);
                    if (!lstrcmpiW(wn.c_str(), name.c_str())) { extent = rd32(s + i + 2); size = rd32(s + i + 10); return true; }
                }
                i += len;
            }
        }
        return false;
    }
};

/* ---------- .cue / .bin images ---------- */

struct BinFile { std::wstring path; uint64_t bytes; int sector_size; int data_offset; bool big_endian; uint32_t base; uint32_t sectors; };

class CueDisc final : public Disc, private Iso9660 {
public:
    std::wstring describe() const override { return L"disc image " + cue_; }

    bool load(const std::wstring &cue, std::wstring &err)
    {
        cue_ = cue;
        std::vector<uint8_t> text;
        if (!read_whole_file(cue, text, err)) return false;
        std::wstring dir = cue.substr(0, cue.find_last_of(L"\\/") + 1);
        struct Pending { int number; bool audio; int file; uint32_t index1; bool has_index1; };
        std::vector<Pending> pend;
        std::string all(text.begin(), text.end());
        size_t pos = 0;
        while (pos < all.size()) {
            size_t e = all.find('\n', pos);
            std::string line = all.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            pos = e == std::string::npos ? all.size() : e + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            size_t a = line.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            line = line.substr(a);
            if (!_strnicmp(line.c_str(), "FILE ", 5)) {
                std::string name, kind;
                size_t q1 = line.find('"'), q2 = q1 == std::string::npos ? q1 : line.find('"', q1 + 1);
                if (q2 != std::string::npos) { name = line.substr(q1 + 1, q2 - q1 - 1); kind = line.substr(q2 + 1); }
                else { size_t sp = line.find(' ', 5); name = line.substr(5, sp - 5); kind = sp == std::string::npos ? "" : line.substr(sp); }
                while (!kind.empty() && kind[0] == ' ') kind.erase(0, 1);
                if (_stricmp(kind.c_str(), "BINARY") && _stricmp(kind.c_str(), "MOTOROLA")) {
                    err = L"the image's FILE type " + widen(kind) + L" is not supported (BINARY .bin files only)";
                    return false;
                }
                BinFile f{};
                std::wstring wn = widen(name);
                f.path = (wn.find(L':') != std::wstring::npos || (!wn.empty() && wn[0] == L'\\')) ? wn : dir + wn;
                f.big_endian = !_stricmp(kind.c_str(), "MOTOROLA");
                WIN32_FILE_ATTRIBUTE_DATA fa;
                if (!GetFileAttributesExW(f.path.c_str(), GetFileExInfoStandard, &fa)) { err = L"the image's file " + f.path + L" was not found"; return false; }
                f.bytes = (uint64_t)fa.nFileSizeHigh << 32 | fa.nFileSizeLow;
                f.sector_size = kRawSector;
                files_.push_back(f);
            } else if (!_strnicmp(line.c_str(), "TRACK ", 6)) {
                int n = 0; char mode[32] = "";
                sscanf(line.c_str() + 6, "%d %31s", &n, mode);
                if (files_.empty()) { err = L"the .cue has a TRACK before any FILE"; return false; }
                bool audio = !_stricmp(mode, "AUDIO");
                if (!audio) {
                    if (!_stricmp(mode, "MODE1/2048")) { files_.back().sector_size = 2048; files_.back().data_offset = 0; }
                    else if (!_stricmp(mode, "MODE1/2352")) files_.back().data_offset = 16;
                    else if (!_stricmp(mode, "MODE2/2352")) files_.back().data_offset = 24;
                    else { err = L"track mode " + widen(mode) + L" is not supported"; return false; }
                    data_file_ = (int)files_.size() - 1;
                }
                pend.push_back({ n, audio, (int)files_.size() - 1, 0, false });
            } else if (!_strnicmp(line.c_str(), "INDEX 01 ", 9) && !pend.empty()) {
                int m = 0, s = 0, f = 0;
                sscanf(line.c_str() + 9, "%d:%d:%d", &m, &s, &f);
                pend.back().index1 = (uint32_t)((m * 60 + s) * 75 + f);
                pend.back().has_index1 = true;
            }
        }
        if (pend.empty()) { err = L"no tracks in the .cue"; return false; }
        uint32_t base = 0;
        for (auto &f : files_) { f.base = base; f.sectors = (uint32_t)(f.bytes / f.sector_size); base += f.sectors; }
        for (size_t i = 0; i < pend.size(); i++) {
            if (!pend[i].has_index1) { err = L"a track in the .cue has no INDEX 01"; return false; }
            uint32_t start = files_[pend[i].file].base + pend[i].index1;
            uint32_t end = i + 1 < pend.size() ? files_[pend[i + 1].file].base + pend[i + 1].index1 : base;
            if (end < start) { err = L"the .cue's track positions are out of order"; return false; }
            tracks_.push_back({ pend[i].number, pend[i].audio, start, end - start });
        }
        return true;
    }

    bool read_audio(uint32_t sector, uint32_t count, uint8_t *out, std::wstring &err) override
    {
        while (count) {
            const BinFile *f = nullptr;
            for (auto &b : files_) if (sector >= b.base && sector < b.base + b.sectors) { f = &b; break; }
            if (!f || f->sector_size != kRawSector) { err = L"sector outside the image's audio"; return false; }
            uint32_t n = std::min(count, f->base + f->sectors - sector);
            if (!read_at(f->path, (uint64_t)(sector - f->base) * kRawSector, out, (size_t)n * kRawSector, err)) return false;
            if (f->big_endian) for (size_t i = 0; i + 1 < (size_t)n * kRawSector; i += 2) std::swap(out[i], out[i + 1]);
            sector += n; count -= n; out += (size_t)n * kRawSector;
        }
        return true;
    }

    bool read_file(const std::wstring &path, std::vector<uint8_t> &out, std::wstring &err) override
    {
        uint32_t extent, size;
        if (data_file_ < 0 || !find(path, extent, size)) { err = path + L" not found on the disc image"; return false; }
        out.resize(size);
        uint8_t s[2048];
        for (uint32_t off = 0; off < size; off += 2048) {
            if (!data_sector(extent + off / 2048, s)) { err = L"read error in " + path; return false; }
            memcpy(out.data() + off, s, std::min<uint32_t>(2048, size - off));
        }
        return true;
    }

    ~CueDisc() override { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

private:
    bool data_sector(uint32_t lba, uint8_t *out) override
    {
        const BinFile &f = files_[data_file_];
        uint8_t raw[kRawSector];
        std::wstring err;
        uint32_t first = 0;
        for (const Track &t : tracks_) if (!t.audio) { first = t.start - f.base; break; }   /* data track's start in its file */
        if (!read_at(f.path, (uint64_t)(first + lba) * f.sector_size, raw, f.sector_size, err)) return false;
        memcpy(out, raw + f.data_offset, 2048);
        return true;
    }

    bool read_at(const std::wstring &path, uint64_t off, uint8_t *out, size_t n, std::wstring &err)
    {
        if (path != open_path_) {
            if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
            h_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            open_path_ = h_ == INVALID_HANDLE_VALUE ? L"" : path;
            if (h_ == INVALID_HANDLE_VALUE) { err = L"cannot open " + path; return false; }
        }
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
        SetFilePointerEx(h_, li, nullptr, FILE_BEGIN);
        while (n) {
            DWORD got = 0, want = (DWORD)std::min<size_t>(n, 1 << 24);
            if (!ReadFile(h_, out, want, &got, nullptr) || !got) { err = L"read error in " + path; return false; }
            out += got; n -= got;
        }
        return true;
    }

    std::wstring cue_, open_path_;
    std::vector<BinFile> files_;
    int data_file_ = -1;
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

/* ---------- a CD in a drive ---------- */

#pragma pack(push, 1)
struct TocTrack { UCHAR reserved; UCHAR control_adr; UCHAR number; UCHAR reserved1; UCHAR address[4]; };
struct Toc { UCHAR length[2]; UCHAR first, last; TocTrack track[100]; };
struct RawReadInfo { LARGE_INTEGER disk_offset; ULONG sector_count; ULONG track_mode; };
#pragma pack(pop)
constexpr DWORD kIoctlReadToc = 0x00024000, kIoctlRawRead = 0x0002403E;
constexpr ULONG kModeCdda = 2;
constexpr uint32_t kMaxRawSectors = 20;   /* per request: within every drive's transfer limit */

class DriveDisc final : public Disc {
public:
    std::wstring describe() const override { return std::wstring(L"CD in drive ") + letter_ + L":"; }
    bool needs_double_read() const override { return true; }

    bool load(wchar_t letter, std::wstring &err)
    {
        letter_ = letter;
        wchar_t dev[] = L"\\\\.\\X:";
        dev[4] = letter;
        h_ = CreateFileW(dev, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) { err = std::wstring(L"cannot open drive ") + letter + L": (is a disc in it?)"; return false; }
        Toc toc{};
        DWORD got = 0;
        if (!DeviceIoControl(h_, kIoctlReadToc, nullptr, 0, &toc, sizeof toc, &got, nullptr)) {
            err = std::wstring(L"cannot read the disc's table of contents in drive ") + letter + L":";
            return false;
        }
        int n = toc.last - toc.first + 1;
        if (n < 1 || n > 99) { err = L"the disc's table of contents is not valid"; return false; }
        auto lba = [](const TocTrack &t) { return (uint32_t)((t.address[1] * 60 + t.address[2]) * 75 + t.address[3]) - 150; };
        for (int i = 0; i < n; i++) {
            uint32_t s = lba(toc.track[i]), e = lba(toc.track[i + 1]);   /* entry n is the lead-out */
            tracks_.push_back({ toc.track[i].number, !(toc.track[i].control_adr & 0x04), s, e > s ? e - s : 0 });
        }
        buf_ = (uint8_t *)VirtualAlloc(nullptr, kMaxRawSectors * kRawSector, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        return buf_ != nullptr;
    }

    bool read_audio(uint32_t sector, uint32_t count, uint8_t *out, std::wstring &err) override
    {
        while (count) {
            uint32_t n = std::min(count, kMaxRawSectors);
            RawReadInfo ri{};
            ri.disk_offset.QuadPart = (LONGLONG)sector * 2048;
            ri.sector_count = n;
            ri.track_mode = kModeCdda;
            DWORD got = 0;
            bool ok = false;
            for (int attempt = 0; attempt < 5 && !ok; attempt++)
                ok = DeviceIoControl(h_, kIoctlRawRead, &ri, sizeof ri, buf_, n * kRawSector, &got, nullptr) && got == n * kRawSector;
            if (!ok) { wchar_t b[96]; swprintf(b, 96, L"the drive could not read audio sector %u", sector); err = b; return false; }
            memcpy(out, buf_, (size_t)n * kRawSector);
            sector += n; count -= n; out += (size_t)n * kRawSector;
        }
        return true;
    }

    bool read_file(const std::wstring &path, std::vector<uint8_t> &out, std::wstring &err) override
    {
        return read_whole_file(std::wstring(1, letter_) + L":\\" + path, out, err);
    }

    ~DriveDisc() override
    {
        if (buf_) VirtualFree(buf_, 0, MEM_RELEASE);
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }

private:
    wchar_t letter_ = 0;
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint8_t *buf_ = nullptr;
};

} // namespace

std::unique_ptr<Disc> open_cue(const std::wstring &cue_path, std::wstring &err)
{
    std::unique_ptr<CueDisc> d(new CueDisc);
    if (!d->load(cue_path, err)) return nullptr;
    return std::unique_ptr<Disc>(d.release());
}

std::unique_ptr<Disc> open_drive(wchar_t letter, std::wstring &err)
{
    std::unique_ptr<DriveDisc> d(new DriveDisc);
    if (!d->load(letter, err)) return nullptr;
    return std::unique_ptr<Disc>(d.release());
}

std::vector<wchar_t> cd_drives()
{
    std::vector<wchar_t> r;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = L"A:\\";
        root[0] = (wchar_t)(L'A' + i);
        if (GetDriveTypeW(root) == DRIVE_CDROM) r.push_back(root[0]);
    }
    return r;
}
