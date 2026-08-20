///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// V2M Playback Plugin
//
// Implements RVPlaybackPlugin interface for V2 Synthesizer Music files using v2redux.
// V2M is the music format used by Farbrausch's V2 synthesizer, common in demoscene productions.
// Audio output: Stereo F32 at 44100 Hz (native output from v2redux).
//
// v2redux plays every v2m format version (0..6) at its own era's behaviour, so
// there is no "convert to newest" step -- the original file bytes are handed
// straight to the player.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include "v2m_pattern.h"
#include "v2redux.h"

extern "C" {
#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define V2M_SAMPLE_RATE 44100
#define V2M_CHANNELS 2
#define V2M_COLUMN_COUNT 3

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct V2MReplayerData {
    v2redux::Player player;
    v2mpat::Pattern pattern;
    uint64_t frames_played = 0;
    uint32_t viz_channels = 0; // channels shown in the grid / scope
    bool playing = false;
    bool scope_enabled = false;
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* v2m_supported_extensions(void) {
    return "v2m";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void v2m_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* v2m_create(const RVService* service_api) {
    (void)service_api;
    // Player embeds a 3 MB synth instance, so this must live on the heap.
    return new (std::nothrow) V2MReplayerData();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int v2m_destroy(void* user_data) {
    delete (V2MReplayerData*)user_data;
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult v2m_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url, uint64_t total_size) {
    (void)probe_data;
    (void)data_size;
    (void)total_size;

    // Extension-only detection: a v2m has no magic, it is recognized by the
    // structural consistency of its patch section, which needs the whole file.
    // V2M is a niche demoscene format; no other format uses the .v2m extension.
    if (url != nullptr) {
        const char* dot = strrchr(url, '.');
        if (dot != nullptr && strcasecmp(dot, ".v2m") == 0) {
            return RVProbeResult_Supported;
        }
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// v2redux resets the synth on every play(), which clears the scope capture flag.
static void v2m_start(V2MReplayerData* data, uint32_t ms) {
    data->player.play(ms);
    data->player.setScopeEnabled(data->scope_enabled);
    data->frames_played = (uint64_t)ms * V2M_SAMPLE_RATE / 1000;
    data->playing = true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int v2m_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    V2MReplayerData* data = (V2MReplayerData*)user_data;

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("V2M: Failed to load %s", url);
        return -1;
    }

    // open() copies the data, so the file buffer is ours to release below.
    v2redux::Result res = data->player.open(read_res.data, (size_t)read_res.data_size);
    if (res != v2redux::Result::OK) {
        rv_error("V2M: open failed for %s (result %d)", url, (int)res);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    data->pattern = v2mpat::Pattern();
    data->viz_channels = 0;
    if (v2mpat::extract((const uint8_t*)read_res.data, (size_t)read_res.data_size, V2M_SAMPLE_RATE, &data->pattern)) {
        for (uint32_t ch = 0; ch < v2mpat::CHANNELS; ch++) {
            if (data->pattern.used_channels & (1u << ch)) {
                data->viz_channels = ch + 1;
            }
        }
    }

    rv_io_free_url_to_memory(read_res.data);

    v2m_start(data, 0);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void v2m_close(void* user_data) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    data->playing = false;
    data->pattern = v2mpat::Pattern();
    data->viz_channels = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo v2m_read_data(void* user_data, RVReadData dest) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;

    RVAudioFormat format = { RVAudioStreamFormat_F32, V2M_CHANNELS, V2M_SAMPLE_RATE };

    if (!data->playing || !data->player.isPlaying()) {
        data->playing = false;
        return (RVReadInfo) { format, 0, RVReadStatus_Finished };
    }

    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(float) * V2M_CHANNELS);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;
    data->player.render((float*)dest.channels_output, max_frames);
    data->frames_played += max_frames;

    if (!data->player.isPlaying()) {
        data->playing = false;
        return (RVReadInfo) { format, max_frames, RVReadStatus_Finished };
    }

    return (RVReadInfo) { format, max_frames, RVReadStatus_Ok };
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t v2m_seek(void* user_data, int64_t ms) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (ms < 0) {
        ms = 0;
    }
    v2m_start(data, (uint32_t)ms);
    return ms;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int v2m_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        return -1;
    }

    RVMetadataId id = rv_metadata_create_url(url);
    rv_metadata_set_tag(id, RV_METADATA_SONGTYPE_TAG, "V2M");

    v2redux::Player* player = new (std::nothrow) v2redux::Player();
    if (player != nullptr) {
        if (player->open(read_res.data, (size_t)read_res.data_size) == v2redux::Result::OK) {
            long long length_ms = player->lengthMs();
            if (length_ms > 0) {
                rv_metadata_set_tag_f64(id, RV_METADATA_LENGTH_TAG, (double)length_ms / 1000.0);
            }
        }
        delete player;
    }

    rv_io_free_url_to_memory(read_res.data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void v2m_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Visualization: the v2m event streams are laid out as a tracker grid of 32nd
// note rows (see v2m_pattern.h), plus a per-channel scope and VU meter.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool v2m_get_structure(void* user_data, RVVizInfo* out) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr || out == nullptr || data->viz_channels == 0) {
        return false;
    }

    out->caps = RVVizCaps_PatternCells | RVVizCaps_Scope | RVVizCaps_Vu | RVVizCaps_WholeSongKnown
                | RVVizCaps_SeekablePreview | RVVizCaps_FutureKnown;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = data->viz_channels;
    out->scope_channel_count = data->viz_channels;
    out->column_count = V2M_COLUMN_COUNT;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } cols[V2M_COLUMN_COUNT] = {
        { "Note", 3, RVColumnKind_Note },
        { "Vel", 2, RVColumnKind_Volume },
        { "Pgm", 2, RVColumnKind_Instrument },
    };

    uint32_t n = cap < V2M_COLUMN_COUNT ? cap : V2M_COLUMN_COUNT;
    for (uint32_t i = 0; i < n; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        strncpy((char*)out[i].label, cols[i].label, sizeof(out[i].label) - 1);
        out[i].char_width = cols[i].width;
        out[i].kind = cols[i].kind;
    }
    return n;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_fill_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    uint32_t count = data->viz_channels;
    if (count > cap) {
        count = cap;
    }
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Ch %u", i + 1);
        out[i].scope_width = 1;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool v2m_get_position(void* user_data, RVTrackerPosition* out) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr || out == nullptr || data->pattern.rows == 0) {
        return false;
    }

    // Rows are timed, not counted: find the last row that has already started.
    const std::vector<uint64_t>& times = data->pattern.row_sample;
    uint32_t lo = 0;
    uint32_t hi = data->pattern.rows - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi + 1) / 2;
        if (times[mid] <= data->frames_played) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    // v2m has no order list or patterns; report bars (32 rows) in their place.
    out->order = lo / 32;
    out->pattern = lo / 32;
    out->row = lo;
    out->window_lo = 0;
    out->window_hi = data->pattern.rows;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_get_channel_rows(void* user_data, uint32_t* out, uint32_t cap) {
    (void)user_data;
    (void)out;
    (void)cap;
    return 0; // Synchronized: the window comes from get_position
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void v2m_render_note(uint8_t note, uint8_t vel, char* dest, size_t dest_size) {
    static const char* s_names[12] = { "C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-" };
    if (vel == 0) {
        snprintf(dest, dest_size, "===");
    } else {
        snprintf(dest, dest_size, "%s%u", s_names[note % 12], note / 12);
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi, RVPatternCell* out,
                              uint32_t cap) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr || out == nullptr || data->pattern.rows == 0) {
        return 0;
    }

    uint32_t num_ch = data->viz_channels;
    if (row_hi > data->pattern.rows) {
        row_hi = data->pattern.rows;
    }

    uint32_t ch_start = channel < 0 ? 0 : (uint32_t)channel;
    uint32_t ch_end = channel < 0 ? num_ch : ch_start + 1;
    if (ch_start >= num_ch) {
        return 0;
    }

    uint32_t written = 0;
    for (uint32_t row = row_lo; row < row_hi; row++) {
        for (uint32_t ch = ch_start; ch < ch_end; ch++) {
            const v2mpat::Cell& src = data->pattern.cells[(size_t)row * v2mpat::CHANNELS + ch];
            uint32_t raws[V2M_COLUMN_COUNT] = { src.note ? (uint32_t)(src.note - 1) : 0u, src.vel,
                                                src.pgm ? (uint32_t)(src.pgm - 1) : 0u };

            for (uint32_t c = 0; c < V2M_COLUMN_COUNT; c++) {
                if (written >= cap) {
                    return written;
                }
                RVPatternCell* cell = &out[written++];
                cell->raw = raws[c];
                memset(cell->text, 0, sizeof(cell->text));
                char* txt = (char*)cell->text;
                switch (c) {
                    case 0:
                        if (src.note) {
                            v2m_render_note((uint8_t)(src.note - 1), src.vel, txt, sizeof(cell->text));
                        }
                        break;
                    case 1:
                        if (src.note && src.vel) {
                            snprintf(txt, sizeof(cell->text), "%02X", src.vel);
                        }
                        break;
                    case 2:
                        if (src.pgm) {
                            snprintf(txt, sizeof(cell->text), "%02X", (uint32_t)(src.pgm - 1));
                        }
                        break;
                    default:
                        break;
                }
            }
        }
    }

    return written;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void v2m_set_scope_enabled(void* user_data, bool on) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr) {
        return;
    }
    data->scope_enabled = on;
    data->player.setScopeEnabled(on);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_get_scope_samples(void* user_data, int32_t channel, float* out, uint32_t cap) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr || out == nullptr || !data->playing || !data->scope_enabled) {
        return 0;
    }
    return data->player.getScopeSamples(channel, out, cap);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_get_vu(void* user_data, float* out, uint32_t cap) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data == nullptr || out == nullptr || !data->playing) {
        return 0;
    }

    float levels[v2mpat::CHANNELS];
    data->player.getChannelLevels(levels);

    uint32_t count = data->viz_channels;
    if (count > cap) {
        count = cap;
    }
    memcpy(out, levels, count * sizeof(float));
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_v2m_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "v2m",
    "0.0.1",
    "v2redux",
    v2m_probe_can_play,
    v2m_supported_extensions,
    v2m_create,
    v2m_destroy,
    v2m_event,
    v2m_open,
    v2m_close,
    v2m_read_data,
    v2m_seek,
    v2m_metadata,
    v2m_static_init,
    nullptr, // settings_updated
    nullptr, // static_destroy

    v2m_get_structure,
    v2m_get_columns,
    v2m_fill_channels, // pattern channels
    v2m_fill_channels, // scope channels
    v2m_get_position,
    v2m_get_channel_rows,
    v2m_get_cells,
    v2m_set_scope_enabled,
    v2m_get_scope_samples,
    v2m_get_vu,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_v2m_plugin;
}
