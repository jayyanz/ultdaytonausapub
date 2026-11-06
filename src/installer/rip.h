/* Red Book CD audio to FLAC, verified against the disc. */
#pragma once
#include <functional>
#include <string>
#include "disc.h"

struct RipCallbacks {
    std::function<void(double done)> progress;           /* 0 to 1 */
    std::function<void(const std::wstring &line)> log;
    std::function<bool()> cancelled;
};

/* Rips every audio track of the disc to dir\TrackNN.flac (INDEX 01 to the next track's INDEX 01, as the proxy's
   CD audio emulation expects) and writes dir\cdaudio.toc. Each FLAC is decoded again and must match the disc audio
   sample for sample. A FLAC already there that matches is kept. */
bool rip_audio(Disc &disc, const std::wstring &dir, const RipCallbacks &cb, std::wstring &err);
