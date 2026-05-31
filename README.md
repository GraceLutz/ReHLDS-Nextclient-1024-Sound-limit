# ReHLDS + NextClient — 1024 Sound Limit

Raises the Counter-Strike 1.6 / GoldSrc engine **sound precache limit from 512 to 1024**, end to
end: a patched **ReHLDS** dedicated server *and* a patched **CS-NextClient** client that can
precache and play sounds at indices **512–1023**.

> Prebuilt binaries are **Win32 / x86**. The changes themselves are platform-independent C++, so
> they build for **Linux** too (see *Building from source*). Server: ReHLDS. Client: CS-NextClient
> (engine build **8684**).

## Repository layout

| Path | Contents |
|------|----------|
| `release/` | Prebuilt **Win32** binaries — `release/server/swds.dll`, `release/client/next_engine_mini.dll` |
| `source/` | The **modified source files**, mirrored at their upstream paths (ReHLDS + NextClient) |
| `linux/` | The **same modified source files** again, as a Linux working copy / build set |
| `patches/` | `git apply`-able diffs against upstream ReHLDS and CS-NextClient |
| `docs/` | Full design document, architecture probe and the mixer spike write-up |
| `test-plugins/` | AMX Mod X plugins used to validate the build (`sndmax.sma`, `nxemit.sma`) |

> `source/` and `linux/` contain identical files — the changes are platform-independent. They are
> kept separate so each target has its own clearly-labelled working copy.

---

## The problem

Stock GoldSrc caps precached sounds at **512** (`MAX_SOUND_INDEX_BITS = 9`). Pushing past it shows
up as a flood of:

```
SV_BuildSoundMsg: weapons/knife_deploy1.wav not precached (0)
```

and, on naive attempts to raise the limit, a client crash during *"Precaching resources…"*:

```
Fatal Error: S_FindName: out of sfx_t
```

or a disconnect with **`Client world model is NULL`**.

Raising the limit correctly requires changes on **both** sides, and the client side is *not* a
simple constant bump (see below).

---

## What changed

### Server — ReHLDS (`patches/rehlds-1024-sound.patch`, sources in `source/rehlds/`)

1. **`rehlds/common/qlimits.h`** — `MAX_SOUND_INDEX_BITS 9 → 10`.
   This makes `MAX_SOUNDS = 1024` and `MAX_SOUNDS_HASHLOOKUP_SIZE = 2047`.
2. **`rehlds/engine/sv_main.cpp`** (`SV_LookupSoundIndex`) — the hashed sound-lookup used a
   **hardcoded `1023`** as the hash modulus while the *insert* side
   (`SV_AddSampleToHashedLookupTable`) used `MAX_SOUNDS_HASHLOOKUP_SIZE`. At 9 bits the macro
   happened to equal `1023`, so the bug was invisible; at 10 bits the two sides hash into different
   buckets and **almost every** post-load sound lookup fails (`not precached (0)`).
   **Fix:** use `MAX_SOUNDS_HASHLOOKUP_SIZE` on the lookup side too.

The network protocol already supports large sound indices (`svc_sound` writes a 16-bit index via
`SND_FL_LARGE_INDEX` when the sound number exceeds 255), so no protocol change is needed.

### Client — CS-NextClient `engine_mini` (`patches/nextclient-1024-sound.patch`, sources in `source/nextclient/`)

The client is the hard part. NextClient's `engine_mini` accesses the proprietary `hw.dll` engine's
`client_state_t` (`cl`) through a bare pointer, and `sfx_t* sound_precache[MAX_SOUNDS]` sits
*mid-struct*. **Bumping `MAX_SOUNDS` in the client header shifts every later field** (`maxclients`,
`worldmodel`, …) and corrupts them → `Client world model is NULL`. So the client `qlimits.h` is
**deliberately left at 9** and the support is added without resizing the engine struct, mirroring
the way NextClient already extends the model pool (`mod_known[1024]` + `Mod_FindName` replacement):

1. **`cl_sound.cpp` / `cl_sound.h`**
   - **Own `sfx_t` pool + `S_FindName` replacement.** `hw.dll`'s internal `known_sfx[]` pool is the
     real ceiling (`S_FindName: out of sfx_t` fires during precache). `engine_mini` now owns a
     larger `sfx_t` pool and `|=`-replaces `S_FindName`, so precaching >512 distinct sounds no
     longer overflows `hw.dll`. (`funchook` redirects `hw.dll`'s internal `S_FindName` calls too.)
   - **Expanded sound-precache index table** `g_sound_precache_ex[1024]` for indices 512–1023
     (`cl->sound_precache[0..511]` is never written past index 511 → ABI-safe).
   - **`svc_sound` takeover** (`CL_SvcSound_HandleHigh`): parses the `svc_sound` bitfield exactly
     like the server writer `SV_BuildSoundMsg`; for sound numbers `>= 512` it resolves the `sfx_t*`
     from the expanded table and dispatches through `hw.dll`'s `S_StartDynamicSound` /
     `S_StartStaticSound`; sentences and indices `< 512` are delegated unchanged to `hw.dll`.
2. **`cl_main.cpp`** — the precache loop stores indices `>= 512` into the expanded table and adds a
   bounds guard so the 512-element `hw.dll` array is never written out of bounds.
3. **`engine.cpp`** — registers the two `|=` replacements (`S_FindName`, `SVC_Sound`).

### Compatibility with ReGameDLL / ReAPI / Metamod — verified safe

ReGameDLL (`mp.dll`), ReAPI (`reapi_amxx.dll`) and Metamod-R were audited against the server-side
`server_t` layout shift. **No rebuild is required for correctness.** None of them dereference
`server_t`/`g_psv` directly; all server-state access goes through ReHLDS's pure-virtual
`IRehldsServerData` accessor methods, whose offsets are resolved *inside* `swds.dll` (the 1024
build). Their own `qlimits.h` staying at 9 is a cosmetic mismatch, not an active bug.

---

## Installation (precompiled binaries)

Both binaries in `release/` are **Win32/x86** drop-in replacements.

### Server
```
release/server/swds.dll          →   <hlds>/swds.dll
```
(Deploy the same way you deploy any ReHLDS `swds.dll` over a HLDS dedicated install.)

### Client
```
release/client/next_engine_mini.dll   →   <NextClient>/next_engine_mini.dll
```

### ⚠️ Deploy as a matched pair
A patched (1024) server talking to an **unpatched (512) client** will overflow the client when it
sends a sound index ≥ 512. Update **both** sides together. For maps/servers that precache ≤ 512
sounds, everything behaves exactly as before.

---

## Building from source

The modifications are plain C++ source changes (in `source/`, also as `patches/`). They apply to
upstream **ReHLDS** and **CS-NextClient** unchanged and build on both Windows and Linux. Either copy
the files from `source/` over a fresh upstream checkout, or `git apply` the patch from the repo
root.

### ReHLDS → server engine

**Windows** (`swds.dll`, VS2022 BuildTools + v143):
```
git apply rehlds-1024-sound.patch
msbuild msvc/ReHLDS.sln /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=v143 /p:XPDeprecationWarning=false /t:Build /m
```
Output: `msvc/Release/swds.dll`.

**Linux** (engine `.so`, GCC/CMake — multilib 32-bit):
```
git apply rehlds-1024-sound.patch
mkdir build && cd build
cmake .. && cmake --build . --config Release
```
(Or use ReHLDS's own `build.sh` / Docker flow.) Produces the Linux engine shared object.

### CS-NextClient → `next_engine_mini.dll` / `.so`

> **Do not** change the client `qlimits.h` — `MAX_SOUND_INDEX_BITS` must stay **9** there, or you
> reintroduce the `Client world model is NULL` ABI break.

**Windows** (CMake VS2022 preset, vcpkg `x86-windows-static`):
```
git apply nextclient-1024-sound.patch
cmake --preset vs2022 -DNEXTCLIENT_INSTALL_DIR=<your CS install>
cmake --build --preset vs2022-release --target BUILD_ALL
```

**Linux:** apply the same patch and build with CS-NextClient's Linux build process (the bundled
NclNitroApi ships an 8684 Linux address provider). The changed files are identical to the Windows
ones — no platform-specific code was added.

---

## Testing

`test-plugins/` contains two AMX Mod X plugins used to validate the build (server side):

- **`sndmax.sma`** — precaches `DUMMY_COUNT` extra sounds (`nexontest/sndNNNN.wav`) to push the
  total toward the 1024 ceiling, then runs `rescount` so you can see `sound : <total> 1023`.
- **`nxemit.sma`** — `nx_snd <index>` emits the dummy at that sound index to the calling player via
  `EmitSound` (the real server→client `svc_sound` path), e.g. `nx_snd 555` plays the sound at
  index 555.

Procedure: precache > 512 sounds on the server, connect a patched client, and confirm a high-index
sound (`nx_snd 555`, `nx_snd 1000`) is audible while sounds < 512 and normal gameplay are
unaffected.

---

## Notes & limitations

- **HLTV / demos:** sounds at indices > 255 already use the 16-bit `SND_FL_LARGE_INDEX` encoding; a
  HLTV/relay or demo player must read that conditional width to handle high indices.
- The client work reuses `hw.dll`'s mixer (it only replaces sound *resolution*, not mixing), so it
  is tied to the exact `svc_sound` bitfield layout of the supported engine build (8684).
- `docs/client-1024-design.md` documents the full design, the architecture probe, and the spike
  that proved `hw.dll`'s mixer accepts an `engine_mini`-owned `sfx_t` pool.

## Credits / upstream

- [ReHLDS](https://github.com/dreamstalker/rehlds) — reverse-engineered HLDS engine (server).
- [CS-NextClient](https://github.com/CS-NextClient/NextClient) — Counter-Strike 1.6 client.
- [ReGameDLL_CS](https://github.com/s1lentq/ReGameDLL_CS), [ReAPI](https://github.com/s1lentq/reapi),
  [Metamod-R](https://github.com/rehlds/Metamod-R) — verified compatible.

Changes here are provided as source (`source/`) and patches (`patches/`) in keeping with the
upstream licenses; the prebuilt DLLs are modified versions of the above projects.
