///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// cpsycle Playback Plugin
//
// Implements RVPlaybackPlugin interface for Psycle tracker files (.psy) using the cpsycle audio engine.
// Supports PSY3 (Psycle 3) and PSY2 (Psycle 2 legacy) formats.
//
// Known limitation: Only built-in machines (Sampler, XM Sampler, Mixer, Master, Duplicator)
// produce audio. PSY files using third-party VST/LADSPA plugins will have those channels
// replaced with silent Dummy machines.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// C11 nullptr compatibility
#ifndef nullptr
#define nullptr ((void*)0)
#endif

#include <retrovert/io.h>
#include <retrovert/log.h>
#include <retrovert/metadata.h>
#include <retrovert/playback.h>
#include <retrovert/service.h>

#include "exclusivelock.h"
#include "machine.h"
#include "machinefactory.h"
#include "player.h"
#include "plugincatcher.h"
#include "sequencer.h"
#include "silentdriver.h"
#include "song.h"
#include "songio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#include <unistd.h>
#endif

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_SAMPLE_RATE 48000
#define RENDER_BLOCK_SIZE 2048

RV_PLUGIN_USE_IO_API();
RV_PLUGIN_USE_METADATA_API();
RV_PLUGIN_USE_LOG_API();

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef struct CpsycleReplayerData {
    psy_audio_Player player;
    psy_audio_Song* song;
    psy_audio_MachineCallback machinecallback;
    psy_audio_MachineFactory machinefactory;
    psy_audio_PluginCatcher plugincatcher;
    int initialized;
    int song_ended;
    char temp_path[512];
} CpsycleReplayerData;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static const char* cpsycle_plugin_supported_extensions(void) {
    return "psy";
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void* cpsycle_plugin_create(const RVService* service_api) {
    CpsycleReplayerData* data = malloc(sizeof(CpsycleReplayerData));
    if (data == nullptr) {
        return nullptr;
    }
    memset(data, 0, sizeof(CpsycleReplayerData));

    return data;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int cpsycle_plugin_destroy(void* user_data) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;
    free(data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVProbeResult cpsycle_plugin_probe_can_play(uint8_t* probe_data, uint64_t data_size, const char* url,
                                                   uint64_t total_size) {
    (void)url;
    (void)total_size;

    // Need at least 8 bytes to check magic
    if (data_size < 8) {
        return RVProbeResult_Unsupported;
    }

    // PSY3 format: "PSY3SONG" magic at offset 0
    if (memcmp(probe_data, "PSY3SONG", 8) == 0) {
        return RVProbeResult_Supported;
    }

    // PSY2 format: "PSY2SONG" magic at offset 0
    if (memcmp(probe_data, "PSY2SONG", 8) == 0) {
        return RVProbeResult_Supported;
    }

    return RVProbeResult_Unsupported;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Write memory buffer to a temporary file and return path.
// cpsycle's song loader requires a filesystem path - this avoids patching its file I/O.

static int write_temp_file(CpsycleReplayerData* data, const uint8_t* buf, uint64_t size) {
#ifdef _WIN32
    char temp_dir[256];
    DWORD len = GetTempPathA(sizeof(temp_dir), temp_dir);
    if (len == 0 || len >= sizeof(temp_dir)) {
        return -1;
    }
    snprintf(data->temp_path, sizeof(data->temp_path), "%scpsycle_XXXXXX", temp_dir);
    int fd = _mktemp_s(data->temp_path, sizeof(data->temp_path));
    if (fd != 0) {
        return -1;
    }
    FILE* f = fopen(data->temp_path, "wb");
    if (f == nullptr) {
        return -1;
    }
#else
    snprintf(data->temp_path, sizeof(data->temp_path), "/tmp/cpsycle_XXXXXX");
    int fd = mkstemp(data->temp_path);
    if (fd < 0) {
        return -1;
    }
    FILE* f = fdopen(fd, "wb");
    if (f == nullptr) {
        close(fd);
        return -1;
    }
#endif

    size_t written = fwrite(buf, 1, (size_t)size, f);
    fclose(f);

    if (written != (size_t)size) {
        unlink(data->temp_path);
        data->temp_path[0] = '\0';
        return -1;
    }

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void cleanup_temp_file(CpsycleReplayerData* data) {
    if (data->temp_path[0] != '\0') {
#ifdef _WIN32
        _unlink(data->temp_path);
#else
        unlink(data->temp_path);
#endif
        data->temp_path[0] = '\0';
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int cpsycle_plugin_open(void* user_data, const char* url, uint32_t subsong, const RVService* service_api) {
    (void)service_api;
    (void)subsong;

    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;

    // Read file into memory via the I/O API
    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        rv_error("cpsycle: Failed to load %s to memory", url);
        return -1;
    }

    // Write to temp file (cpsycle needs a filesystem path)
    if (write_temp_file(data, read_res.data, read_res.data_size) != 0) {
        rv_error("cpsycle: Failed to create temp file for %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    rv_io_free_url_to_memory(read_res.data);

    // Initialize the audio subsystem
    psy_audio_init();

    // Set up machine callback (host interface for plugins)
    psy_audio_machinecallback_init(&data->machinecallback);

    // Initialize plugin catcher (minimal - no scanning for external plugins)
    psy_audio_plugincatcher_init(&data->plugincatcher);

    // Initialize machine factory
    psy_audio_machinefactory_init(&data->machinefactory, &data->machinecallback, &data->plugincatcher);

    // Initialize player with no song initially
    psy_audio_player_init(&data->player, nullptr, nullptr);
    psy_audio_machinecallback_setplayer(&data->machinecallback, &data->player);

    // Allocate and load song
    data->song = psy_audio_song_allocinit(&data->machinefactory);
    if (data->song == nullptr) {
        rv_error("cpsycle: Failed to allocate song for %s", url);
        psy_audio_player_dispose(&data->player);
        psy_audio_machinefactory_dispose(&data->machinefactory);
        psy_audio_plugincatcher_dispose(&data->plugincatcher);
        psy_audio_dispose();
        cleanup_temp_file(data);
        return -1;
    }

    psy_audio_machinecallback_set_song(&data->machinecallback, data->song);

    // Load the song file
    psy_audio_SongFile songfile;
    psy_audio_songfile_init(&songfile);
    songfile.song = data->song;
    int err = psy_audio_songfile_load(&songfile, data->temp_path);
    psy_audio_songfile_dispose(&songfile);

    if (err != PSY_OK) {
        rv_error("cpsycle: Failed to load song %s (error %d)", url, err);
        psy_audio_song_deallocate(data->song);
        data->song = nullptr;
        psy_audio_player_dispose(&data->player);
        psy_audio_machinefactory_dispose(&data->machinefactory);
        psy_audio_plugincatcher_dispose(&data->plugincatcher);
        psy_audio_dispose();
        cleanup_temp_file(data);
        return -1;
    }

    // Connect song to player
    psy_audio_exclusivelock_enter();
    psy_audio_player_setsong(&data->player, data->song);
    psy_audio_player_setbpm(&data->player, data->song->properties.bpm);
    psy_audio_player_set_lpb(&data->player, data->song->properties.lpb);
    psy_audio_exclusivelock_leave();

    // Set sample rate to 48kHz
    psy_audio_sequencer_setsamplerate(&data->player.sequencer, (psy_dsp_big_hz_t)OUTPUT_SAMPLE_RATE);

    // Start playback from the beginning
    psy_audio_sequencer_stop_loop(&data->player.sequencer);
    psy_audio_player_setposition(&data->player, 0.0);
    psy_audio_player_start(&data->player);

    data->initialized = 1;
    data->song_ended = 0;

    // Clean up temp file now that song is loaded
    cleanup_temp_file(data);

    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void cpsycle_plugin_close(void* user_data) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;

    if (!data->initialized) {
        return;
    }

    psy_audio_player_stop(&data->player);
    psy_audio_player_dispose(&data->player);

    if (data->song != nullptr) {
        psy_audio_song_deallocate(data->song);
        data->song = nullptr;
    }

    psy_audio_machinefactory_dispose(&data->machinefactory);
    psy_audio_plugincatcher_dispose(&data->plugincatcher);
    psy_audio_dispose();

    cleanup_temp_file(data);
    data->initialized = 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVReadInfo cpsycle_plugin_read_data(void* user_data, RVReadData dest) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;
    RVAudioFormat format = { RVAudioStreamFormat_F32, 2, OUTPUT_SAMPLE_RATE };

    if (!data->initialized || data->song == nullptr) {
        return (RVReadInfo) { format, 0, RVReadStatus_Error};
    }

    if (data->song_ended) {
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    // Calculate how many frames we can generate
    uint32_t max_frames = dest.channels_output_max_bytes_size / (sizeof(float) * 2);
    if (max_frames > RENDER_BLOCK_SIZE) {
        max_frames = RENDER_BLOCK_SIZE;
    }

    // Drive the cpsycle player to render audio
    int numsamples = (int)max_frames;
    int hostisplaying = 1;

    // Call psy_audio_player_work which returns interleaved float stereo
    psy_dsp_amp_t* rendered = psy_audio_player_work(&data->player, &numsamples, &hostisplaying);

    if (rendered == nullptr || numsamples <= 0) {
        data->song_ended = 1;
        return (RVReadInfo) { format, 0, RVReadStatus_Finished};
    }

    // Copy rendered audio to output buffer and normalize from native range [-32768, 32768] to [-1, 1]
    // cpsycle internally uses PSY_DSP_AMP_RANGE_NATIVE (integer-scale floats)
    int total_samples = numsamples * 2; // stereo interleaved
    float* output = (float*)dest.channels_output;
    const float scale = 1.0f / 32768.0f;
    for (int i = 0; i < total_samples; i++) {
        output[i] = rendered[i] * scale;
    }

    if (!hostisplaying) {
        data->song_ended = 1;
        return (RVReadInfo) { format, (uint32_t)numsamples, RVReadStatus_Finished};
    }

    return (RVReadInfo) { format, (uint32_t)numsamples, RVReadStatus_Ok};
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int64_t cpsycle_plugin_seek(void* user_data, int64_t ms) {
    (void)user_data;
    (void)ms;
    // Seeking is not easily supported in cpsycle's sequencer model
    return -1;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static int cpsycle_plugin_metadata(const char* url, const RVService* service_api) {
    (void)service_api;

    RVIoReadUrlResult read_res = rv_io_read_url_to_memory(url);
    if (read_res.data == nullptr) {
        return -1;
    }

    RVMetadataId index = rv_metadata_create_url(url);

    // Extract title from PSY3 header if possible
    // PSY3 format: 8 bytes magic "PSY3SONG", then chunks
    // For now, just register the URL - full metadata extraction would require
    // loading the full song which is expensive
    if (read_res.data_size >= 8) {
        if (memcmp(read_res.data, "PSY3SONG", 8) == 0) {
            rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, "Psycle 3");
        } else if (memcmp(read_res.data, "PSY2SONG", 8) == 0) {
            rv_metadata_set_tag(index, RV_METADATA_SONGTYPE_TAG, "Psycle 2");
        }
    }

    rv_io_free_url_to_memory(read_res.data);
    return 0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void cpsycle_plugin_event(void* user_data, uint8_t* event_data, uint64_t len) {
    (void)user_data;
    (void)event_data;
    (void)len;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static void cpsycle_plugin_static_init(const RVService* service_api) {
    rv_init_log_api(service_api);
    rv_init_io_api(service_api);
    rv_init_metadata_api(service_api);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static RVPlaybackPlugin g_cpsycle_plugin = {
    RV_PLAYBACK_PLUGIN_API_VERSION,
    "cpsycle",
    "0.0.1",
    "cpsycle (Psycle audio engine)",
    cpsycle_plugin_probe_can_play,
    cpsycle_plugin_supported_extensions,
    cpsycle_plugin_create,
    cpsycle_plugin_destroy,
    cpsycle_plugin_event,
    cpsycle_plugin_open,
    cpsycle_plugin_close,
    cpsycle_plugin_read_data,
    cpsycle_plugin_seek,
    cpsycle_plugin_metadata,
    cpsycle_plugin_static_init,
    nullptr, // settings_updated
    nullptr, // get_tracker_info
    nullptr, // get_pattern_cell
    nullptr, // get_pattern_num_rows
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_cpsycle_plugin;
}
