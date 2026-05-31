# v1.0 — 1024 Sound Limit (ReHLDS + CS-NextClient)

Raises the Counter-Strike 1.6 / GoldSrc engine **sound precache limit from 512 to 1024**, end to
end: a patched **ReHLDS** server and a patched **CS-NextClient** client that precache and play
sounds at indices **512–1023**. Platform: **Win32 / x86**.

## Downloads
- `swds.dll` — patched ReHLDS server engine (drop-in over your ReHLDS `swds.dll`).
- `next_engine_mini.dll` — patched CS-NextClient engine module (engine build **8684**).

The repository is organized as `release/` (prebuilt Win32 binaries), `source/` + `linux/`
(modified source files), `patches/` (git-apply diffs), `docs/` and `test-plugins/`. See the README
for Windows **and Linux** build instructions.

## Server (ReHLDS)
- `MAX_SOUND_INDEX_BITS` 9 → 10 (`MAX_SOUNDS` = 1024).
- Fixes `SV_LookupSoundIndex`: the hashed lookup used a hardcoded `1023` modulus while the insert
  side used `MAX_SOUNDS_HASHLOOKUP_SIZE`; at 10 bits this produced the
  `SV_BuildSoundMsg: ... not precached (0)` flood. Now both sides are consistent.

## Client (CS-NextClient `engine_mini`)
- Owns its own `sfx_t` pool (`S_FindName` replacement) to bypass `hw.dll`'s internal `known_sfx`
  cap (which otherwise fatals with `S_FindName: out of sfx_t`).
- Expanded sound-precache index table for indices 512–1023.
- Takes over `svc_sound` to resolve high indices and dispatch through `hw.dll`'s mixer.
- The client `qlimits.h` intentionally stays at 9 — resizing it shifts `client_state_t` and
  corrupts `hw.dll` (`Client world model is NULL`).

## Compatibility
ReGameDLL_CS, ReAPI and Metamod-R were audited and are **compatible with no rebuild** (they reach
server state only through ReHLDS `IRehldsServerData` accessor methods, never the raw `server_t`).

## Install
1. **Server:** replace your ReHLDS `swds.dll` with the one in this release.
2. **Client:** replace `next_engine_mini.dll` in your CS-NextClient install.
3. **Deploy as a matched pair.** A 1024 server with an unpatched 512 client overflows the client on
   sound indices ≥ 512. Maps/servers that precache ≤ 512 sounds behave exactly as before.

See the [README](README.md) for building from source.
