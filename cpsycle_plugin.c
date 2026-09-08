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
#include "notestab.h"
#include "sequenceselection.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define OUTPUT_SAMPLE_RATE 48000
#define RENDER_BLOCK_SIZE 2048

// Note, instrument, machine, volume, command, parameter -- the six fields a
// Psycle pattern cell carries.
#define CPSYCLE_COLUMN_COUNT 6
// The host caps a pattern channel count at 64 and, by default, a captured
// window at 64 rows. Both are clamped here rather than reported past the limit,
// which the host treats as an error rather than as something to truncate.
#define CPSYCLE_MAX_PATTERN_CHANNELS 64
#define CPSYCLE_ROW_WINDOW 64
// Psycle plays a multi-sequence; only the first sequence track is followed.
#define CPSYCLE_SEQUENCE_TRACK 0

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
    if (read_res.data_size > (uint64_t)UINTPTR_MAX) {
        rv_error("cpsycle: Song is too large to load on this platform: %s", url);
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

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
        rv_io_free_url_to_memory(read_res.data);
        return -1;
    }

    psy_audio_machinecallback_set_song(&data->machinecallback, data->song);

    // Load the song file
    psy_audio_SongFile songfile;
    psy_audio_songfile_init(&songfile);
    songfile.song = data->song;
    int err = psy_audio_songfile_load_memory(
        &songfile, read_res.data, (uintptr_t)read_res.data_size, url);
    psy_audio_songfile_dispose(&songfile);
    rv_io_free_url_to_memory(read_res.data);

    if (err != PSY_OK) {
        rv_error("cpsycle: Failed to load song %s (error %d)", url, err);
        psy_audio_song_deallocate(data->song);
        data->song = nullptr;
        psy_audio_player_dispose(&data->player);
        psy_audio_machinefactory_dispose(&data->machinefactory);
        psy_audio_plugincatcher_dispose(&data->plugincatcher);
        psy_audio_dispose();
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
    uint32_t capacity_frames = dest.channels_output_max_bytes_size / (sizeof(float) * 2);
    uint32_t max_frames = dest.info.frame_count < capacity_frames ? dest.info.frame_count : capacity_frames;
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
// Visualization.
//
// Psycle is a tracker, so what it has to show is the pattern grid rather than a
// scope. Its patterns are not row arrays: a pattern is a list of entries each
// carrying a beat offset and a track, so a row is recovered by multiplying the
// offset by the sequencer's lines per beat. The playing pattern is found by
// asking the sequence which order entry covers the current beat position.

static psy_audio_Sequence* cpsycle_sequence(CpsycleReplayerData* data) {
    if (data == nullptr || data->song == nullptr) {
        return nullptr;
    }
    return psy_audio_song_sequence(data->song);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Resolve the order entry, pattern and row the player is currently on.
// Returns false when the song is not positioned on a pattern.

static bool cpsycle_current_location(CpsycleReplayerData* data, uintptr_t* out_order, psy_audio_Pattern** out_pattern,
                                     uintptr_t* out_pattern_index, uint32_t* out_row, uint32_t* out_rows) {
    psy_audio_Sequence* sequence = cpsycle_sequence(data);
    if (sequence == nullptr) {
        return false;
    }

    uintptr_t lpb = psy_audio_sequencer_lpb(&data->player.sequencer);
    if (lpb == 0) {
        return false;
    }

    psy_dsp_big_beat_t position = psy_audio_player_position(&data->player);
    uintptr_t order = psy_audio_sequence_order(sequence, CPSYCLE_SEQUENCE_TRACK, position);
    psy_audio_OrderIndex index = psy_audio_orderindex_make(CPSYCLE_SEQUENCE_TRACK, order);

    psy_audio_Pattern* pattern = psy_audio_sequence_pattern(sequence, index);
    if (pattern == nullptr) {
        return false;
    }

    psy_dsp_big_beat_t entry_offset = psy_audio_sequence_offset(sequence, index);
    psy_dsp_big_beat_t in_pattern = position - entry_offset;
    if (in_pattern < (psy_dsp_big_beat_t)0.0) {
        in_pattern = (psy_dsp_big_beat_t)0.0;
    }

    double rows = (double)psy_audio_pattern_length(pattern) * (double)lpb;
    if (rows < 1.0) {
        rows = 1.0;
    }

    uint32_t row = (uint32_t)((double)in_pattern * (double)lpb);
    if ((double)row >= rows) {
        row = (uint32_t)rows - 1;
    }

    *out_order = order;
    *out_pattern = pattern;
    *out_pattern_index = psy_audio_sequence_patternindex(sequence, index);
    *out_row = row;
    *out_rows = (uint32_t)rows;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t cpsycle_pattern_channels(CpsycleReplayerData* data) {
    if (data == nullptr || data->song == nullptr) {
        return 0;
    }

    uintptr_t tracks = psy_audio_song_numsongtracks(data->song);
    if (tracks > CPSYCLE_MAX_PATTERN_CHANNELS) {
        tracks = CPSYCLE_MAX_PATTERN_CHANNELS;
    }
    return (uint32_t)tracks;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool cpsycle_plugin_get_structure(void* user_data, RVVizInfo* out) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return false;
    }

    uint32_t channels = cpsycle_pattern_channels(data);
    if (channels == 0) {
        return false;
    }

    // The whole song is loaded up front and its rows never change as it plays.
    out->caps = RVVizCaps_PatternCells | RVVizCaps_WholeSongKnown | RVVizCaps_SeekablePreview | RVVizCaps_FutureKnown;
    out->scroll_mode = RVScrollMode_Synchronized;
    out->pattern_channel_count = channels;
    out->scope_channel_count = 0;
    out->column_count = CPSYCLE_COLUMN_COUNT;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t cpsycle_plugin_get_columns(void* user_data, RVColumnDesc* out, uint32_t cap) {
    (void)user_data;
    static const struct {
        const char* label;
        uint8_t width;
        RVColumnKind kind;
    } s_columns[CPSYCLE_COLUMN_COUNT] = {
        { "Note", 3, RVColumnKind_Note },  { "Inst", 2, RVColumnKind_Instrument },
        { "Mac", 2, RVColumnKind_Custom }, { "Vol", 2, RVColumnKind_Volume },
        { "Cmd", 2, RVColumnKind_Effect }, { "Prm", 2, RVColumnKind_Param },
    };

    if (out == nullptr) {
        return 0;
    }

    uint32_t count = cap < CPSYCLE_COLUMN_COUNT ? cap : CPSYCLE_COLUMN_COUNT;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].label, 0, sizeof(out[i].label));
        snprintf((char*)out[i].label, sizeof(out[i].label), "%s", s_columns[i].label);
        out[i].char_width = s_columns[i].width;
        out[i].kind = s_columns[i].kind;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t cpsycle_plugin_get_pattern_channels(void* user_data, RVChannelDesc* out, uint32_t cap) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return 0;
    }

    uint32_t channels = cpsycle_pattern_channels(data);
    uint32_t count = channels < cap ? channels : cap;
    for (uint32_t i = 0; i < count; i++) {
        memset(out[i].name, 0, sizeof(out[i].name));
        snprintf((char*)out[i].name, sizeof(out[i].name), "Track %u", i + 1);
        out[i].scope_width = 0;
    }
    return count;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static bool cpsycle_plugin_get_position(void* user_data, RVTrackerPosition* out) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;
    if (data == nullptr || out == nullptr) {
        return false;
    }

    uintptr_t order = 0;
    uintptr_t pattern_index = 0;
    psy_audio_Pattern* pattern = nullptr;
    uint32_t row = 0;
    uint32_t rows = 0;
    if (!cpsycle_current_location(data, &order, &pattern, &pattern_index, &row, &rows)) {
        return false;
    }

    // A pattern can be longer than the host's row budget, so the window follows
    // the playhead instead of spanning the whole pattern.
    uint32_t lo = 0;
    uint32_t hi = rows;
    if (rows > CPSYCLE_ROW_WINDOW) {
        lo = row > (CPSYCLE_ROW_WINDOW / 2) ? row - (CPSYCLE_ROW_WINDOW / 2) : 0;
        if (lo + CPSYCLE_ROW_WINDOW > rows) {
            lo = rows - CPSYCLE_ROW_WINDOW;
        }
        hi = lo + CPSYCLE_ROW_WINDOW;
    }

    out->order = (uint32_t)order;
    out->pattern = (uint32_t)pattern_index;
    out->row = row;
    out->window_lo = lo;
    out->window_hi = hi;
    return true;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Psycle names its own notes, including "off" and the tweak commands, in a
// 256-entry table covering every value a cell can hold -- so the grid reads the
// way it does in Psycle rather than in a convention invented here. The table
// spells an empty note as spaces; the host's grid uses dots for that.

static void cpsycle_format_note(char* text, size_t size, uint8_t note) {
    const char* name = psy_dsp_notetostr(note, psy_dsp_NOTESTAB_DEFAULT);
    if (name == nullptr || name[0] == ' ' || name[0] == '\0') {
        snprintf(text, size, "...");
        return;
    }
    snprintf(text, size, "%s", name);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Find the event on `track` at `row`, or null. Pattern entries are a list
// ordered by beat offset, so the row is matched by rounding each offset back to
// a row index rather than by looking a row up directly.

static const psy_audio_PatternEvent* cpsycle_event_at(psy_audio_Pattern* pattern, uintptr_t lpb, uintptr_t track,
                                                      uint32_t row) {
    for (psy_audio_PatternNode* node = psy_audio_pattern_begin(pattern); node != nullptr; node = node->next) {
        psy_audio_PatternEntry* entry = (psy_audio_PatternEntry*)node->entry;
        if (entry == nullptr || entry->track != track) {
            continue;
        }

        uint32_t entry_row = (uint32_t)((double)entry->offset * (double)lpb + 0.5);
        if (entry_row == row) {
            return psy_audio_patternentry_front_const(entry);
        }
        if (entry_row > row) {
            // Entries are ordered by offset, so nothing later can match.
            break;
        }
    }
    return nullptr;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

static uint32_t cpsycle_plugin_get_cells(void* user_data, int32_t channel, uint32_t row_lo, uint32_t row_hi,
                                         RVPatternCell* out, uint32_t cap) {
    CpsycleReplayerData* data = (CpsycleReplayerData*)user_data;
    if (data == nullptr || out == nullptr || channel < -1) {
        return 0;
    }

    uintptr_t order = 0;
    uintptr_t pattern_index = 0;
    psy_audio_Pattern* pattern = nullptr;
    uint32_t row = 0;
    uint32_t rows = 0;
    if (!cpsycle_current_location(data, &order, &pattern, &pattern_index, &row, &rows)) {
        return 0;
    }

    uintptr_t lpb = psy_audio_sequencer_lpb(&data->player.sequencer);
    uint32_t channels = cpsycle_pattern_channels(data);
    if (channels == 0 || lpb == 0) {
        return 0;
    }

    if (row_hi > rows) {
        row_hi = rows;
    }
    if (row_lo >= row_hi) {
        return 0;
    }

    uint32_t channel_start = channel < 0 ? 0 : (uint32_t)channel;
    uint32_t channel_end = channel < 0 ? channels : (uint32_t)channel + 1;
    if (channel_start >= channels) {
        return 0;
    }

    uint32_t written = 0;
    for (uint32_t r = row_lo; r < row_hi; r++) {
        for (uint32_t c = channel_start; c < channel_end; c++) {
            const psy_audio_PatternEvent* event = cpsycle_event_at(pattern, lpb, c, r);

            for (uint32_t column = 0; column < CPSYCLE_COLUMN_COUNT; column++) {
                if (written >= cap) {
                    return written;
                }

                RVPatternCell* cell = &out[written++];
                memset(cell, 0, sizeof(*cell));

                char* text = (char*)cell->text;
                size_t size = sizeof(cell->text);
                if (event == nullptr) {
                    snprintf(text, size, column == 0 ? "..." : "..");
                    continue;
                }

                switch (column) {
                    case 0:
                        cell->raw = event->note;
                        cpsycle_format_note(text, size, event->note);
                        break;
                    case 1:
                        cell->raw = event->inst;
                        if (event->inst == psy_audio_NOTECOMMANDS_INST_EMPTY) {
                            snprintf(text, size, "..");
                        } else {
                            snprintf(text, size, "%02X", (unsigned)(event->inst & 0xFF));
                        }
                        break;
                    case 2:
                        cell->raw = event->mach;
                        if (event->mach == psy_audio_NOTECOMMANDS_EMPTY) {
                            snprintf(text, size, "..");
                        } else {
                            snprintf(text, size, "%02X", (unsigned)event->mach);
                        }
                        break;
                    case 3:
                        cell->raw = event->vol;
                        if (event->vol == psy_audio_NOTECOMMANDS_VOL_EMPTY) {
                            snprintf(text, size, "..");
                        } else {
                            snprintf(text, size, "%02X", (unsigned)(event->vol & 0xFF));
                        }
                        break;
                    case 4:
                        cell->raw = event->cmd;
                        snprintf(text, size, "%02X", (unsigned)event->cmd);
                        break;
                    default:
                        cell->raw = event->parameter;
                        snprintf(text, size, "%02X", (unsigned)event->parameter);
                        break;
                }
            }
        }
    }
    return written;
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
    nullptr, // static_destroy

    // Visualization: the tracker pattern grid. No scope -- Psycle mixes through
    // a machine graph rather than a fixed set of channels.
    cpsycle_plugin_get_structure,
    cpsycle_plugin_get_columns,
    cpsycle_plugin_get_pattern_channels,
    nullptr, // get_scope_channels
    cpsycle_plugin_get_position,
    nullptr, // get_channel_rows
    cpsycle_plugin_get_cells,
    nullptr, // set_scope_enabled
    nullptr, // get_scope_samples
    nullptr, // get_vu
};

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

RV_EXPORT RVPlaybackPlugin* rv_playback_plugin(void) {
    return &g_cpsycle_plugin;
}
