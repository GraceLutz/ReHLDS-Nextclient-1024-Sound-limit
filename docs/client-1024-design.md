# Design: Client-side support for >512 (up to 1024) precached sounds in NextClient

**Date:** 2026-05-31
**Status:** Phase-0 spike DONE (2026-05-31) → **Q2 = NO**; approach revised (see "Phase 0 Results" below)
**Scope:** NextClient `engine_mini` only. Pairs with the already-patched ReHLDS server
(`MAX_SOUND_INDEX_BITS = 10`, sound limit 1024).

## 1. Problem

The patched ReHLDS server can precache up to 1024 sounds and emits `svc_sound` with a
16-bit "large index" (`SND_FL_LARGE_INDEX`) when the sound number > 255. The CS 1.6 client
must store and resolve those sounds. The proprietary `hw.dll` engine is hardcoded to
`MAX_SOUNDS = 512`, and NextClient's `engine_mini` accesses the engine's `client_state_t`
global `cl` through a bare pointer whose layout is fixed by `hw.dll`.

**Why we can't just bump the constant:** `sfx_t* sound_precache[MAX_SOUNDS]` sits *mid-struct*
in `client_state_t` (`ncl-hl1-source-sdk/engine/client.h:304`), before `consistency_list`
(`:305`), `maxclients` (`:309`), `worldmodel` (`:312`). Changing `MAX_SOUNDS` to 1024 in
`qlimits.h` shifts every later field; `hw.dll` keeps reading `worldmodel`/`maxclients` at the
old offsets → the client disconnects with **"Client world model is NULL"**
(`cl_parse.cpp:123-124`). Confirmed empirically.

## 2. Goal & success criteria

- The client can precache and play **1024 sounds simultaneously live per map** (confirmed use case).
- **No `client_state_t` resize** — the `hw.dll` ABI stays intact (`cl->sound_precache[0..511]` only).
- Indices `0..511` behave exactly as today (no regression).
- Indices `512..1023` are stored and resolved by `engine_mini` and played through `hw.dll`'s mixer.
- End-to-end: a >511-index sound emitted by the ReHLDS server is audible on the client.

## 3. Architecture verdict (from the architecture probe)

The client sound pipeline is almost entirely `hw.dll`-owned; `engine_mini` owns exactly one node:

| Step | Where | Owner |
|------|-------|-------|
| A. resource list → assign `nIndex` | `CL_ParseResourceList` | hw.dll (hook point unused) |
| B. precache write `cl->sound_precache[nIndex] = S_PrecacheSound(name)` | `cl_main.cpp:243-263` | **engine_mini** |
| C. `S_PrecacheSound` → `sfx_t*` from `known_sfx[]` pool | `cl_sound.cpp:6-11` (wrapper) | hw.dll body |
| D. runtime `svc_sound` parse (read index) | hw.dll `CL_ParseStartSoundPacket` | hw.dll (hook point unused) |
| E. **index → sfx** `cl->sound_precache[num]` | inside hw.dll svc_sound handler | hw.dll 🔒 |
| F. `S_StartDynamicSound(…, sfx_t*, …)` | hw.dll | hw.dll (hookable; index already gone) |
| G. mix/playback | hw.dll | hw.dll |

The numeric index → `sfx_t*` resolution (E) is locked inside `hw.dll` and the first hookable
boundary after it (F) only sees an already-resolved `sfx_t*`. **However**, the `svc_sound`
message handler (`SVC_Sound`, `EngineData.h:332`, registered `EngineModule.cpp:330`) is a
**fully replaceable** hook (`|=` / `InvokeChained`). `engine_mini` already replaces other engine
message handlers this way (`SVC_TimeScale`, `SVC_SendCvarValue` at `engine.cpp:389-390`;
`SVC_StuffText`/`SVC_ResourceLocation` read-then-replay the netbuffer at `engine.cpp:446-462`).
So `engine_mini` *can* take over `svc_sound`, move the index→sfx resolution into open code, and
reuse `hw.dll`'s mixer for the final `sfx_t*`.

## 4. Chosen approach (A): own table + `svc_sound` takeover

Mirror the existing **model precedent** (`mod_known[MAX_KNOWN_MODELS=1024]` + `Mod_FindName`
`|=`-replaced — `model.cpp:17`, `model.h:13` extern, `MAX_KNOWN_MODELS` in
`ncl-hl1-source-sdk/engine/qlimits.h:31`, replace at `engine.cpp:399`), extended to the sound
*index* space. (Verified against source 2026-05-31.)

### Components
1. **`g_sound_precache_ex` (size 1024)** — an `engine_mini`-owned `sfx_t*` table; the real storage.
   Indices `0..511` may mirror `cl->sound_precache` or stay empty (see lookup helper); `512..1023`
   live only here.
2. **Precache branch** (`cl_main.cpp:243-263`): for `nIndex < 512` keep
   `cl->sound_precache[nIndex] = S_PrecacheSound(name)` (unchanged). For `nIndex >= 512` write to
   `g_sound_precache_ex[nIndex]` instead, **never** to `cl->sound_precache` (this also closes the
   currently-latent out-of-bounds write at `cl_main.cpp:252`). Add a hard bounds guard `< 1024`.
3. **`SVC_Sound` takeover** (`|=` replace, same pattern as `SVC_TimeScale` `engine.cpp:389`):
   `engine_mini` parses the message itself.
   - **Decision is on the parsed `num`, NOT on the flag.** `SND_FL_LARGE_INDEX` is set by the
     server whenever `sound_num > 255` (`sv_main.cpp:783-784`), so indices 256–511 *also* use the
     16-bit encoding yet still belong to the hw.dll table. So we must read far enough to obtain
     `num`, then branch.
   - `num < 512` → **delegate to hw.dll's original handler**: rewind `*pMsg_readcount` to the
     message start (saved before parsing) and `next->Invoke()` — the exact read-then-rewind-then-
     replay pattern used for `SVC_StuffText`/`SVC_ResourceLocation` (`engine.cpp:446-462`). Minimal
     surface, regression-safe. *(Chosen over routing everything through the ex-path.)*
   - `num >= 512` → resolve `sfx = g_sound_precache_ex[num]`, finish parsing the remaining fields,
     and call `eng()->S_StartDynamicSound(...)` / `S_StartStaticSound(...)` (`EngineData.h:536-537`,
     both take `sfx_t*`) directly. Save/restore `*pMsg_readcount` carefully so a delegated message
     re-parses cleanly.
   - Bit reads use `eng()->MSG_StartBitReading/MSG_ReadBits/MSG_EndBitReading` (no local wrappers
     exist yet — add thin ones in `net_msg.cpp` for clarity). `S_BeginPrecaching/S_EndPrecaching`
     and `S_PrecacheSound` wrappers already exist in engine_mini (`cl_sound.cpp`).
4. **Lookup helper** `index → sfx_t*` spanning both tables, so the boundary lives in one place.

### The `svc_sound` parse spec (de-risked)
The client read must mirror, byte-for-byte, the server writer **`SV_BuildSoundMsg`**
(`ReHLDS/rehlds/engine/sv_main.cpp:786-799`), which we control:
```
MSG_WriteByte(svc_sound)
MSG_StartBitWriting
field_mask        : 9 bits
if VOLUME         : volume      8 bits
if ATTENUATION    : attenuation 8 bits   (atten*64)
channel           : 3 bits
entity            : MAX_EDICT_BITS
sound_num         : 8 bits, or 16 bits if (field_mask & SND_FL_LARGE_INDEX)
origin            : MSG_WriteBitVec3Coord
if PITCH          : pitch       8 bits
MSG_EndBitWriting
```
The client uses `MSG_StartBitReading`/`MSG_ReadBits` (delegated to hw.dll, `net_msg.cpp:45`).
`SND_FL_LARGE_INDEX = BIT(2)` (`enginemsg.h:10`). For sentence sounds (`SND_FL_SENTENCE`) and
the `num < 512` case we delegate, so only the large-index dynamic/static path is custom.

### ABI-safety
`cl->sound_precache[512]` is never written past index 511; all expansion is in `engine_mini`
memory. No struct resize. Safe.

## 5. Data flow
- **Precache (map load):** resource list → `nIndex` → `S_PrecacheSound(name)` → store in
  `cl->sound_precache` (`<512`) or `g_sound_precache_ex` (`>=512`).
- **Playback (runtime):** `svc_sound` → `engine_mini` parse → `num` → table lookup → `sfx_t*` →
  `S_StartDynamicSound` (hw.dll mixer, unchanged).

## 6. Phases (spike-first)

**Phase 0 — Spike (DECISIVE, do first):** Build the `g_sound_precache_ex` table + the precache
branch (component 2) only. On the client, precache **>512 distinct real sounds** and verify
`eng()->S_PrecacheSound` returns a non-null `sfx_t*` for the 513th+ and that the entries are
retained. This confirms **hw.dll's internal `known_sfx[]` / `MAX_SFX` pool can hold >512 sounds**
— the single most decisive unknown.
- If pool ≥ 1024 → proceed to Phase 1.
- If pool caps at ~512 → STOP and re-design (approach C: reimplement the sfx pool/mixer, or patch
  the pool). This invalidates A.
- *Run by the user* (needs Steam + graphics); harness provided.

**Phase 1 — `svc_sound` takeover:** Implement component 3 (replace `SVC_Sound`, delegate `<512`,
custom-resolve `>=512`) + the lookup helper (component 4).

**Phase 2 — End-to-end verification:** ReHLDS server emits a >511-index sound; confirm audible on
the client; confirm `<512` sounds unchanged.

## 7. Risks & open questions
- **R1 (decisive):** hw.dll `known_sfx`/`MAX_SFX` pool cap. Resolved by Phase-0 spike.
- **R2:** `nIndex >= 512` arriving at `cl_main.cpp:252` is *already* a latent OOB write today;
  the Phase-0 guard fixes it regardless of the rest.
- **R3:** Bit-exact `svc_sound` parse — mitigated by having the server writer as the spec; only the
  `>=512` path is custom, `<512` delegates.
- **R4:** Build/version coupling — the parse is tied to the 8684 hw.dll svc_sound layout
  (`EngineAddressProvider8684Windows.cpp`). Document supported build(s).
- **R5 (server side, already OK):** ReHLDS already sends 16-bit large index for `sound_num>255`
  (`sv_main.cpp:795`) and precaches 1024 — verified.

## 8. Testing
- Phase-0 spike: >512 distinct precached sounds load + play on the client.
- Bit-parse parity: a >511-index sound round-trips server→client and plays; `<512` regression-free.
- Negative: ensure no write to `cl->sound_precache[>=512]` ever (assert/guard).

## 9. Key file references
- `ncl-hl1-source-sdk/engine/client.h:304` — the fixed `sound_precache[512]` array
- `ncl-hl1-source-sdk/common/qlimits.h:32-34` — `MAX_SOUND_INDEX_BITS`/`MAX_SOUNDS` (keep at 9)
- `engine_mini/src/client/cl_main.cpp:243-263` — engine_mini's only precache write (unguarded)
- `engine_mini/src/client/cl_parse.cpp:123-132` — NULL-worldmodel disconnect; server-msg pass-through
- `engine_mini/src/engine.cpp:389-462` — `|=` replace + netbuffer-replay pattern to copy for `SVC_Sound`
- `engine_mini/src/engine.cpp:399` + `model.cpp:17` + `model.h:13` + `qlimits.h:31` (MAX_KNOWN_MODELS=1024) — the model precedent
- `NclNitroApi/.../EngineData.h:332,536-537` — `SVC_Sound` hook + `sfx_t*`-only `S_StartDynamicSound`
- `NclNitroApi/.../EngineModule.cpp:330` — `SVC_Sound` registered (available, unused)
- `NclNitroApi/.../enginemsg.h:10` — `SND_FL_LARGE_INDEX`
- server spec: `ReHLDS/rehlds/engine/sv_main.cpp:786-799` — `SV_BuildSoundMsg` (the parse spec)

---

## Phase 0 Results & Revised Approach (2026-05-31)

**Spike outcome: Q2 = NO (decisive).** Connecting the (guarded) client to a server precaching
1018 sounds produces a **`Fatal Error: S_FindName: out of sfx_t`** during "Precaching resources".
So the binding client ceiling is NOT only the `cl->sound_precache[512]` array — it is **hw.dll's
internal `known_sfx[MAX_SFX]` sfx pool**, which is `< 1018` and fails *fatally* (Sys_Error) when
exceeded. This would break **any** client (even vanilla) on a `>MAX_SFX`-sound server. The
original Approach A (index table + svc_sound takeover) alone is therefore **insufficient**.

**Good news — the pool is fixable via the model precedent.** All needed symbols are exposed by
nitro_api:
- `S_FindName` — hookable: `EngineData.h:535` (`NitroFunction<sfx_t*, const char*, int*>`),
  `EngineModule.cpp:282`, addr `EngineAddressProvider8684Windows.cpp:192`.
- `S_PrecacheSound` — hookable: `EngineData.h:296`, `EngineModule.cpp:177`.
- `known_sfx` (`sfx_t**`) + `num_sfx` (`int*`) — exposed vars: `EngineData.h:73-74`,
  `EngineModule.cpp:52-53`.

This is exactly the model precedent (`mod_known[1024]` + `Mod_FindName` `|=`-replace) applied to
the sfx pool. `funchook` patches the function entry, so hw.dll's *internal* `S_FindName` calls
(from `S_PrecacheSound`) get redirected too — same mechanism that makes `Mod_FindName` work.

### Revised approach (A + sfx-pool ownership)
1. **(NEW) Own the sfx pool:** engine_mini allocates `known_sfx_ex[]` (≥1024 `sfx_t`, layout per
   `sound.h:36,45-50`) and `|=`-replaces `S_FindName` to find/allocate from the own pool instead of
   hw.dll's capped `known_sfx[]`. (Optionally also handle `S_PrecacheSound`.) This removes the
   fatal ceiling.
2. **Own the index table:** `g_sound_precache_ex[1024]` (as before).
3. **`svc_sound` takeover:** resolve indices ≥512 from the ex-table, dispatch via
   `S_StartDynamicSound` (as before).
4. **Reuse hw.dll's mixer** (`S_LoadSound`/channels) on the resulting `sfx_t*`.

### New decisive risk (next spike)
Does hw.dll's `S_LoadSound`/mixer/cache work correctly with **engine_mini-owned `sfx_t` objects**
(foreign pool)? The model precedent (hw.dll renders `mod_known[1024]` entries) is strong evidence
YES, but sounds use hw.dll's `cache` system on the `sfx_t` — must be confirmed by a spike before
committing to the full build-out. If the mixer rejects foreign `sfx_t`, escalate to full Approach
C (reimplement `S_LoadSound`/mixing).

**Scope impact:** larger than the originally-planned Approach A (adds a pool-ownership subsystem),
but follows two proven in-codebase precedents (model pool + `|=` message replace). Plan
`docs/superpowers/plans/2026-05-31-client-sound-1024.md` must be extended with the pool-ownership
phase ahead of the svc_sound takeover.
