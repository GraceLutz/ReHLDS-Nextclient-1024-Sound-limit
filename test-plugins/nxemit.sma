// emit_sound test: emits a sound to the calling player via pfnEmitSound -> SV_StartSound ->
// svc_sound (the real server->client path).
//   nx_snd 55                    -> emits dummy at index 55  (nexontest/snd0055.wav), LOW  (<512)
//   nx_snd 555                   -> emits dummy at index 555 (nexontest/snd0555.wav), HIGH (>511, needs client Phase 1)
//   nx_snd weapons/ak47-1.wav    -> emits any precached sound by name
//   nx_snd                       -> default (index 100)
// NOTE: emit_sound takes a NAME, not an index. With the sndmax plugin, dummy number == sound index
// (snd0001=idx1 .. snd0720=idx720), so a numeric arg is mapped to nexontest/snd<NNNN>.wav.
#include <amxmodx>
#include <fakemeta>

public plugin_init() {
    register_plugin("EmitSoundTest", "1.1", "nexon");
    register_clcmd("nx_snd", "cmd_snd");
    register_clcmd("say /snd", "cmd_snd");
}

public cmd_snd(id) {
    if (!is_user_connected(id))
        return PLUGIN_HANDLED;

    new arg[128];
    read_argv(1, arg, charsmax(arg));

    new snd[128];
    if (!arg[0]) {
        copy(snd, charsmax(snd), "nexontest/snd0100.wav");
    } else {
        new bool:numeric = true;
        for (new i = 0; arg[i]; i++) {
            if (arg[i] < '0' || arg[i] > '9') { numeric = false; break; }
        }
        if (numeric)
            formatex(snd, charsmax(snd), "nexontest/snd%04d.wav", str_to_num(arg));
        else
            copy(snd, charsmax(snd), arg);
    }

    // ATTN_NONE (0.0) = audible regardless of distance; CHAN_AUTO=0, pitch 100
    engfunc(EngFunc_EmitSound, id, CHAN_AUTO, snd, 1.0, 0.0, 0, 100);

    client_print(id, print_chat, "[nx] emit_sound -> %s", snd);
    server_print("[nx] emit_sound to player %d: %s", id, snd);
    return PLUGIN_HANDLED;
}
