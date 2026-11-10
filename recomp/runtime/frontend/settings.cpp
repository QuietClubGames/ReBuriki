// Player settings (settings.ini).
//
// Adapted from ReLayer (port/ui/settings.c), Copyright (c) 2026 The ReLayer project, MIT License
// (see LICENSE-ReLayer.txt in this folder).
#include "settings.h"
#include <cstddef>
#include <cstdio>
#include <cstring>

Settings g_settings;

void settings_defaults(Settings* s) {
    memset(s, 0, sizeof *s);
    s->window_size = 2;            // 1195x896: two screen lines per arcade line
    s->enhanced = 1;
    s->volume = 100;
    s->unlock_silber = 1;          // like ReLayer's "unlock all characters": on unless the player wants to earn him
    s->region = REGION_USA;        // English by default; Japanese stays selectable
}

int settings_quality_scale(int q) {
    static const int k[] = {0, 2, 3, 4, 6, 8};
    return q >= 0 && q < 6 ? k[q] : 0;
}

const char* settings_region_bios(int region) {
    switch (region) {
    case REGION_JAPAN: return "brom1.bin";
    case REGION_WORLD: return "bios_export.bin";
    default: return "bios_us.bin";
    }
}

static const struct { const char* key; size_t ofs; int lo, hi; } k_fields[] = {
    {"display_mode", offsetof(Settings, display_mode), 0, 2},
    {"window_size", offsetof(Settings, window_size), 0, 5},
    {"monitor", offsetof(Settings, monitor), 0, 16},
    {"picture_shape", offsetof(Settings, aspect), 0, 3},
    {"widescreen_hud_edges", offsetof(Settings, hud_edges), 0, 1},
    {"enhanced_graphics", offsetof(Settings, enhanced), 0, 1},
    {"quality", offsetof(Settings, quality), 0, 5},
    {"screen_filter", offsetof(Settings, filter), 0, 2},
    {"frame_pacing", offsetof(Settings, pacing), 0, 1},
    {"graphics_card", offsetof(Settings, gpu), 0, 16},
    {"show_fps", offsetof(Settings, show_fps), 0, 1},
    {"volume", offsetof(Settings, volume), 0, 100},
    {"mute", offsetof(Settings, mute), 0, 1},
    {"unlock_silber", offsetof(Settings, unlock_silber), 0, 1},
    {"region", offsetof(Settings, region), 0, REGION_COUNT - 1},
};

static int set_field(Settings* s, const char* key, int v) {
    for (auto& f : k_fields)
        if (!strcmp(key, f.key)) {
            if (v < f.lo) v = f.lo;
            if (v > f.hi) v = f.hi;
            *(int*)((char*)s + f.ofs) = v;
            return 1;
        }
    return 0;
}

int settings_set(Settings* s, const char* kv) {
    char key[64];
    int v;
    if (sscanf(kv, " %63[^= ] = %d", key, &v) != 2) return 0;
    return set_field(s, key, v);
}

int settings_load(Settings* s, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        settings_set(s, line);
    }
    fclose(f);
    return 1;
}

int settings_save(const Settings* s, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "# Buriki One settings (written by the in-game menu: Tab / Esc, or Back + Start on a pad)\n"
               "# region: 0 Japan (Japanese), 1 USA (English), 2 World (English)\n");
    for (auto& k : k_fields) fprintf(f, "%s = %d\n", k.key, *(const int*)((const char*)s + k.ofs));
    fclose(f);
    return 1;
}
