// Near-maximum sound precache test.
//   plugin_precache: precaches DUMMY_COUNT copies of a real dummy .wav
//   (sound/nexontest/snd0001.wav ...). Each distinct filename = a distinct
//   engine sound slot, so this fills the sound precache list toward the
//   MAX_SOUNDS ceiling (1024 on the patched server).
//   After load it runs "rescount" so you can see model/sound/total counts.
//
// Set DUMMY_COUNT so that (base game sounds + DUMMY_COUNT) stays <= MAX_SOUNDS-1,
// otherwise the engine Host_Errors during map load.
#include <amxmodx>

#define DUMMY_COUNT 720        // base de_dust2 sounds = 298; 298+720 = 1018 (<= 1023 limit)
#define SND_DIR     "nexontest"

new g_maxIdx;

public plugin_precache() {
    new s[64];
    for (new i = 1; i <= DUMMY_COUNT; i++) {
        formatex(s, charsmax(s), "%s/snd%04d.wav", SND_DIR, i);
        new idx = precache_sound(s);
        if (idx > g_maxIdx) g_maxIdx = idx;
    }
}

public plugin_init() {
    register_plugin("SoundMaxTest", "1.0", "nexon");
    set_task(6.0, "report");
}

public report() {
    server_print("[SNDMAX] DUMMY_COUNT=%d  max dummy index=%d", DUMMY_COUNT, g_maxIdx);
    server_print("[SNDMAX] ---- rescount (Total sound vs Limit) ----");
    server_cmd("rescount");
    server_exec();
}
