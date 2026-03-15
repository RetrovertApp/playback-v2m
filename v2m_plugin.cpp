///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// V2M Playback Plugin
//
// Implements RVPlaybackPlugin interface for V2 Synthesizer Music files using v2m-player.
// V2M is the music format used by Farbrausch's V2 synthesizer, common in demoscene productions.
// Audio output: Stereo F32 at 44100 Hz (native output from v2m-player).
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

#include "sounddef.h"
#include "synth.h"
#include "v2mconv.h"
#include "v2mplayer.h"

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

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();
static bool g_sd_initialized = false;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct V2MReplayerData {
    V2MPlayer* player;
    uint8_t* raw_data;       // Original file data (kept for IO free)
    uint8_t* converted_data; // After ConvertV2M (must be freed with free())
    int converted_len;
    uint32_t length_s;
    bool playing;
    bool scope_enabled;
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

    if (!g_sd_initialized) {
        sdInit();
        g_sd_initialized = true;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* v2m_create(const RVService* service_api) {
    V2MReplayerData* data = (V2MReplayerData*)calloc(1, sizeof(V2MReplayerData));
    if (!data) {
        return nullptr;
    }

    // V2MPlayer has a 3MB internal synth buffer, must be heap-allocated
    data->player = new (std::nothrow) V2MPlayer();
    if (!data->player) {
        free(data);
        return nullptr;
    }

    data->player->Init();

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int v2m_destroy(void* user_data) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (data->playing && data->player) {
        data->player->Close();
    }
    if (data->converted_data) {
        free(data->converted_data);
    }
    if (data->raw_data) {
        rv_io_free_url_to_memory(data->raw_data);
    }
    if (data->player) {
        delete data->player;
    }
    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult v2m_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url, uint64_t total_size) {
    (void)probe_data;
    (void)data_size;
    (void)total_size;

    // Extension-only detection. CheckV2MVersion has no internal bounds checking --
    // it walks 16 channels using offsets parsed from the data without validation,
    // so calling it on non-V2M probe data causes out-of-bounds reads and crashes.
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

static int v2m_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)subsong;
    (void)service_api;

    V2MReplayerData* data = (V2MReplayerData*)user_data;

    // Clean up previous state
    if (data->playing) {
        data->player->Close();
        data->playing = false;
    }
    if (data->converted_data) {
        free(data->converted_data);
        data->converted_data = nullptr;
    }
    if (data->raw_data) {
        rv_io_free_url_to_memory(data->raw_data);
        data->raw_data = nullptr;
    }

    RVIoReadUrlResult read_res;
    if ((read_res = rv_io_read_url_to_memory(url)).data == nullptr) {
        rv_error("V2M: Failed to load %s", url);
        return -1;
    }

    data->raw_data = (uint8_t*)read_res.data;

    // Convert to newest V2M format
    uint8_t* conv_ptr = nullptr;
    int conv_len = 0;
    ConvertV2M(data->raw_data, (int)read_res.data_size, &conv_ptr, &conv_len);
    if (!conv_ptr || conv_len <= 0) {
        rv_error("V2M: ConvertV2M failed for %s", url);
        rv_io_free_url_to_memory(data->raw_data);
        data->raw_data = nullptr;
        return -1;
    }

    data->converted_data = conv_ptr;
    data->converted_len = conv_len;

    // Open with the converted data
    if (!data->player->Open(data->converted_data, V2M_SAMPLE_RATE)) {
        rv_error("V2M: Open failed for %s", url);
        free(data->converted_data);
        data->converted_data = nullptr;
        rv_io_free_url_to_memory(data->raw_data);
        data->raw_data = nullptr;
        return -1;
    }

    data->length_s = data->player->Length();
    data->player->Play(0);
    data->playing = true;

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void v2m_close(void* user_data) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;

    if (data->playing) {
        data->player->Close();
        data->playing = false;
    }
    if (data->converted_data) {
        free(data->converted_data);
        data->converted_data = nullptr;
    }
    if (data->raw_data) {
        rv_io_free_url_to_memory(data->raw_data);
        data->raw_data = nullptr;
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo v2m_read_data(void* user_data, RVReadData dest) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;

    RVAudioFormat format = { RVAudioStreamFormat_F32, V2M_CHANNELS, V2M_SAMPLE_RATE };

    if (!data->playing || !data->player->IsPlaying()) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(float) * V2M_CHANNELS);
    float* output = (float*)dest.channels_output;

    // Zero the output first (Render adds to existing data if a_add=true, but we use default)
    memset(output, 0, max_frames * V2M_CHANNELS * sizeof(float));

    // Render outputs interleaved stereo F32 directly
    data->player->Render(output, max_frames);

    if (!data->player->IsPlaying()) {
        data->playing = false;
        return (RVReadInfo) { format, max_frames, RVReadStatus_Finished};
    }

    return (RVReadInfo) { format, max_frames, RVReadStatus_Ok};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t v2m_seek(void* user_data, int64_t ms) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;

    if (!data->converted_data) {
        return -1;
    }

    // Close and reopen to seek
    data->player->Close();
    if (!data->player->Open(data->converted_data, V2M_SAMPLE_RATE)) {
        data->playing = false;
        return -1;
    }

    data->player->Play((uint32_t)ms);
    data->playing = true;

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

    // Convert to get duration
    uint8_t* conv_ptr = nullptr;
    int conv_len = 0;
    ConvertV2M((const uint8_t*)read_res.data, (int)read_res.data_size, &conv_ptr, &conv_len);
    if (conv_ptr && conv_len > 0) {
        V2MPlayer player;
        player.Init();
        if (player.Open(conv_ptr, V2M_SAMPLE_RATE)) {
            uint32_t length_s = player.Length();
            if (length_s > 0) {
                rv_metadata_set_tag_f64(id, RV_METADATA_LENGTH_TAG, (double)length_s);
            }
            player.Close();
        }
        free(conv_ptr);
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

static uint32_t v2m_get_scope_data(void* user_data, int channel, float* buffer, uint32_t num_samples) {
    V2MReplayerData* data = (V2MReplayerData*)user_data;
    if (!data || !data->player || !data->playing || !buffer) {
        return 0;
    }

    void* synth = data->player->GetSynth();
    if (!synth) {
        return 0;
    }

    if (!data->scope_enabled) {
        synthEnableScopeCapture(synth, 1);
        data->scope_enabled = true;
    }

    return synthGetScopeData(synth, channel, buffer, num_samples);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t v2m_get_scope_channel_names(void* user_data, const char** names, uint32_t max_channels) {
    (void)user_data;
    static char s_name_bufs[64][16];
    int total = synthGetNumChannels();
    uint32_t count = total > 0 ? (uint32_t)total : 0;
    if (count > 64)
        count = 64;
    if (count > max_channels)
        count = max_channels;
    for (uint32_t i = 0; i < count; i++) {
        snprintf(s_name_bufs[i], sizeof(s_name_bufs[i]), "Synth %u", i + 1);
        names[i] = s_name_bufs[i];
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_v2m_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "v2m",
    "0.0.1",
    "v2m-player (jgilje)",
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
    nullptr, // get_tracker_info
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
    v2m_get_scope_data,
    nullptr, // static_destroy
    v2m_get_scope_channel_names,
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

extern "C" RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_v2m_plugin;
}
