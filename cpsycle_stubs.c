///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Stubs for excluded cpsycle source files
//
// These provide no-op implementations for symbols referenced by compiled code (e.g. songio.c)
// but defined in source files we excluded (exporters, EXS24 loader, keyboard driver).
// This prevents dlopen(RTLD_NOW) from failing on undefined symbols.
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

// Stub types matching cpsycle's conventions
typedef int psy_err_t;
typedef struct {
    char dummy;
} psy_audio_SongFile;
typedef struct {
    char dummy;
} psy_audio_Song;
typedef struct {
    char dummy;
} psy_audio_Psy3Saver;
typedef struct {
    char dummy;
} psy_audio_LYSongExport;
typedef struct {
    char dummy;
} psy_audio_MidiSongExport;
typedef struct {
    char dummy;
} psy_audio_EXS24Loader;

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// psy3saver.c stubs (PSY3 format export - we only need loading)

void psy_audio_psy3saver_init(psy_audio_Psy3Saver* self, psy_audio_SongFile* songfile) {
    (void)self;
    (void)songfile;
}

void psy_audio_psy3saver_dispose(psy_audio_Psy3Saver* self) {
    (void)self;
}

psy_err_t psy_audio_psy3saver_save(psy_audio_Psy3Saver* self) {
    (void)self;
    return 1; // error
}

psy_err_t psy_audio_psy3saver_saveinstrument(psy_audio_Psy3Saver* self, int slot) {
    (void)self;
    (void)slot;
    return 1; // error
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// lysongexport.c stubs (LY format export)

void psy_audio_lysongexport_init(psy_audio_LYSongExport* self, psy_audio_SongFile* songfile) {
    (void)self;
    (void)songfile;
}

void psy_audio_lysongexport_dispose(psy_audio_LYSongExport* self) {
    (void)self;
}

psy_err_t psy_audio_lysongexport_save(psy_audio_LYSongExport* self) {
    (void)self;
    return 1; // error
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// midisongexport.c stubs (MIDI format export)

void psy_audio_midisongexport_init(psy_audio_MidiSongExport* self, psy_audio_SongFile* songfile) {
    (void)self;
    (void)songfile;
}

void psy_audio_midisongexport_dispose(psy_audio_MidiSongExport* self) {
    (void)self;
}

psy_err_t psy_audio_midisongexport_save(psy_audio_MidiSongExport* self) {
    (void)self;
    return 1; // error
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// exs24loader.c stubs (Apple Logic EXS24 sample format)

void psy_audio_exs24loader_init(psy_audio_EXS24Loader* self) {
    (void)self;
}

void psy_audio_exs24loader_dispose(psy_audio_EXS24Loader* self) {
    (void)self;
}

psy_err_t psy_audio_exs24loader_load(psy_audio_EXS24Loader* self, psy_audio_Song* song, const char* path) {
    (void)self;
    (void)song;
    (void)path;
    return 1; // error
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// kbddriver.c stub (keyboard input driver - UI only)

void* psy_audio_kbddriver_create(void) {
    return (void*)0;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// xmsongexport.c stubs (XM format export - note: no psy_audio_ prefix in original)

typedef struct {
    char dummy;
} XMSongExport;

void xmsongexport_init(XMSongExport* self, psy_audio_SongFile* songfile) {
    (void)self;
    (void)songfile;
}

void xmsongexport_dispose(XMSongExport* self) {
    (void)self;
}

psy_err_t xmsongexport_exportsong(XMSongExport* self) {
    (void)self;
    return 1; // error
}
