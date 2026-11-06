/* The installer's view of a Daytona USA disc: a .cue / .bin image or a CD in a drive. */
#pragma once
#include <windows.h>
#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

constexpr int kRawSector = 2352;   /* bytes per CD sector of audio (44,100 Hz, 16-bit, stereo: 588 samples) */

struct Track {
    int number;
    bool audio;
    uint32_t start;    /* first sector (INDEX 01), counted from the start of the program area (no 150-sector lead-in) */
    uint32_t length;   /* sectors, to the next track's INDEX 01 (or the end of the disc) */
};

class Disc {
public:
    virtual ~Disc() {}
    const std::vector<Track> &tracks() const { return tracks_; }
    virtual std::wstring describe() const = 0;
    /* raw audio sectors, little-endian 16-bit stereo */
    virtual bool read_audio(uint32_t sector, uint32_t count, uint8_t *out, std::wstring &err) = 0;
    /* a file from the data track, path like L"GAME\\MINI\\DAYTONA.EXE" */
    virtual bool read_file(const std::wstring &path, std::vector<uint8_t> &out, std::wstring &err) = 0;
    /* true if reads can differ between attempts (a real drive): the ripper then reads every track twice */
    virtual bool needs_double_read() const { return false; }
protected:
    std::vector<Track> tracks_;
};

std::unique_ptr<Disc> open_cue(const std::wstring &cue_path, std::wstring &err);
std::unique_ptr<Disc> open_drive(wchar_t letter, std::wstring &err);
std::vector<wchar_t> cd_drives();   /* drive letters of CD / DVD drives */
