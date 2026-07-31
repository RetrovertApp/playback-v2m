///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// V2M pattern extraction
//
// A .v2m file stores its music as 16 channels of delta-coded MIDI-ish event
// streams (notes, program changes, pitch bends, 7 controllers) plus a global
// tempo/time-signature track. That is enough to lay the song out as a tracker
// grid: one row per 32nd note, one column group per channel.
//
// This header parses ONLY the event section, which sits at the very start of
// the file and has the same layout in every format version (0..6) -- the
// version-dependent parts (globals, patch map) come after it, so no
// canonicalization is needed here.
//
// Reference for the layout: V2MPlayer::InitBase / V2MPlayer::Tick in v2seq.cpp.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <stdint.h>
#include <string.h>

#include <vector>

namespace v2mpat {

enum { CHANNELS = 16, MAX_ROWS = 1 << 18 };

// One cell of the grid. `note` and `pgm` are stored +1 so 0 means "empty";
// `vel` is the raw MIDI velocity (0 = note off).
struct Cell {
    uint8_t note;
    uint8_t vel;
    uint8_t pgm;
};

struct Pattern {
    uint32_t rows = 0;
    uint32_t used_channels = 0;      // bitmask of channels that carry notes
    std::vector<Cell> cells;         // rows * CHANNELS, row-major
    std::vector<uint64_t> row_sample; // rows, sample index at the start of each row
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

namespace detail {

inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Event times are 24-bit deltas stored column-wise: byte n of the delta for
// event i lives at ptr[n * count + i].
inline uint32_t delta3(const uint8_t* p, uint32_t count) {
    return (uint32_t)p[0] | ((uint32_t)p[count] << 8) | ((uint32_t)p[2 * count] << 16);
}

// Cursor with bounds checking -- this parses untrusted file data.
struct Cursor {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;

    uint32_t u32() {
        if (!ok || (uint64_t)(end - p) < 4) {
            ok = false;
            return 0;
        }
        uint32_t v = rd32(p);
        p += 4;
        return v;
    }

    // Claims `count * stride` bytes and returns the start, or null on overflow.
    const uint8_t* take(uint64_t count, uint64_t stride) {
        uint64_t n = count * stride;
        if (!ok || count > 0x10000000ull || (uint64_t)(end - p) < n) {
            ok = false;
            return nullptr;
        }
        const uint8_t* s = p;
        p += n;
        return s;
    }
};

struct Stream {
    const uint8_t* ptr = nullptr;
    uint32_t count = 0;
};

} // namespace detail

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Parses `data` and fills `out`. Returns false if the file is malformed or has
// no events at all.
inline bool extract(const uint8_t* data, size_t size, uint32_t samplerate, Pattern* out) {
    using namespace detail;

    Cursor c = { data, data + size };
    uint32_t timediv = c.u32();
    uint32_t maxtime = c.u32();
    uint32_t gdnum = c.u32();
    if (!c.ok || timediv == 0) {
        return false;
    }

    const uint8_t* gptr = c.take(gdnum, 10);

    Stream notes[CHANNELS];
    Stream pgms[CHANNELS];
    for (int ch = 0; ch < CHANNELS && c.ok; ch++) {
        uint32_t notenum = c.u32();
        if (!notenum) {
            continue;
        }
        notes[ch].count = notenum;
        notes[ch].ptr = c.take(notenum, 5);

        pgms[ch].count = c.u32();
        pgms[ch].ptr = c.take(pgms[ch].count, 4);

        uint32_t pbnum = c.u32();
        c.take(pbnum, 5);

        for (int cc = 0; cc < 7; cc++) {
            uint32_t ccnum = c.u32();
            c.take(ccnum, 4);
        }
    }
    if (!c.ok) {
        return false;
    }

    uint64_t rows = (uint64_t)maxtime * 8 / timediv + 1;
    if (rows > MAX_ROWS) {
        rows = MAX_ROWS;
    }

    out->rows = (uint32_t)rows;
    out->used_channels = 0;
    out->cells.assign(rows * CHANNELS, Cell { 0, 0, 0 });
    out->row_sample.assign(rows, 0);

    // --- events -> cells ------------------------------------------------------
    for (int ch = 0; ch < CHANNELS; ch++) {
        const Stream& s = notes[ch];
        uint32_t time = 0;
        uint8_t note = 0, vel = 0;
        for (uint32_t i = 0; i < s.count; i++) {
            time += delta3(s.ptr + i, s.count);
            note = (uint8_t)(note + s.ptr[3 * s.count + i]);
            vel = (uint8_t)(vel + s.ptr[4 * s.count + i]);
            uint64_t row = (uint64_t)time * 8 / timediv;
            if (row < rows) {
                Cell& cell = out->cells[row * CHANNELS + ch];
                cell.note = (uint8_t)(note + 1);
                cell.vel = vel;
                out->used_channels |= 1u << ch;
            }
        }

        const Stream& p = pgms[ch];
        time = 0;
        uint8_t pgm = 0;
        for (uint32_t i = 0; i < p.count; i++) {
            time += delta3(p.ptr + i, p.count);
            pgm = (uint8_t)(pgm + p.ptr[3 * p.count + i]);
            uint64_t row = (uint64_t)time * 8 / timediv;
            if (row < rows) {
                out->cells[row * CHANNELS + ch].pgm = (uint8_t)(pgm + 1);
            }
        }
    }

    // --- tempo track -> row start times ---------------------------------------
    // Sample delta per tick is usecs / (10000 * timediv), with usecs updated by
    // the global track; see V2MPlayer::Tick. Doubles are fine here: this table
    // only drives the playhead highlight, never audio.
    const double timediv2 = 10000.0 * (double)timediv;
    double usecs = 5000.0 * (double)samplerate; // 120 BPM, V2MPlayer::Reset default
    double sample = 0.0;
    uint64_t tick = 0;
    uint32_t gnr = 0;
    uint64_t gtick = gdnum ? delta3(gptr, gdnum) : UINT64_MAX;

    for (uint64_t r = 0; r < rows; r++) {
        out->row_sample[r] = (uint64_t)sample;
        uint64_t target = (r + 1) * timediv / 8;
        while (tick < target) {
            uint64_t next = gtick < target ? gtick : target;
            sample += (double)(next - tick) * usecs / timediv2;
            tick = next;
            if (tick == gtick) {
                usecs = (double)rd32(gptr + 3 * gdnum + 4 * gnr) * ((double)samplerate / 100.0);
                gnr++;
                gtick = gnr < gdnum ? gtick + delta3(gptr + gnr, gdnum) : UINT64_MAX;
            }
        }
    }

    return out->used_channels != 0;
}

} // namespace v2mpat
