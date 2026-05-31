#include "../engine.h"
#include "../common/zone.h"
#include "../common/com_strings.h"
#include "optick.h"
#include "cl_sound.h"
#include "../common/sys_dll.h"
#include <nitroapi/modules/engine/enginemsg.h>
#include <array>

sfx_t* S_PrecacheSound(char* sample)
{
    OPTICK_EVENT();

    return eng()->S_PrecacheSound(sample);
}

void S_UnloadSounds(const std::unordered_set<std::string>& names)
{
    OPTICK_EVENT();

    sfx_t* known_sfx = *p_known_sfx;
    int num_sfx = *p_num_sfx;

    int i;
    sfx_t* sfx;

    for (i = 0; i < num_sfx; i++)
    {
        sfx = &known_sfx[i];

        if (!names.contains(std::format("{}{}", DEFAULT_SOUNDPATH, sfx->name)))
        {
            continue;
        }

        if (Cache_Check(&sfx->cache))
        {
            Cache_Free(&sfx->cache);
        }

        sfx->cache.data = nullptr;
        sfx->name[0] = '\0';
    }
}

// --- Phase-0 mixer spike: engine_mini-owned sfx pool ---
// Mirrors the model precedent (mod_known[1024] + Mod_FindName replace). Bypasses hw.dll's capped
// known_sfx[] (which fatal-errors "S_FindName: out of sfx_t" past ~MAX_SFX). The returned sfx_t
// lives in engine_mini memory; if hw.dll's mixer plays it correctly, foreign sfx_t is validated.
namespace {
    constexpr int kMaxSfxEx = 2048;
    std::array<sfx_t, kMaxSfxEx> g_sfx_ex{};
    int g_num_sfx_ex = 0;
}

void S_FindName_Ex_Reset()
{
    for (int i = 0; i < g_num_sfx_ex; i++)
    {
        if (Cache_Check(&g_sfx_ex[i].cache))
            Cache_Free(&g_sfx_ex[i].cache);
        g_sfx_ex[i].cache.data = nullptr;
        g_sfx_ex[i].name[0] = '\0';
    }
    g_num_sfx_ex = 0;
}

sfx_t* S_FindName_Ex(const char* name, int* pfInCache)
{
    if (!name || !name[0])
        Sys_Error("S_FindName_Ex: NULL/empty name");
    if (Q_strlen(name) >= MAX_QPATH)
        Sys_Error("S_FindName_Ex: sound name too long: %s", name);

    for (int i = 0; i < g_num_sfx_ex; i++)
    {
        if (!Q_stricmp(g_sfx_ex[i].name, name))
        {
            if (pfInCache)
                *pfInCache = (g_sfx_ex[i].cache.data != nullptr) ? 1 : 0;
            return &g_sfx_ex[i];
        }
    }

    if (g_num_sfx_ex >= kMaxSfxEx)
        Sys_Error("S_FindName_Ex: out of sfx_t (ex pool, %d)", kMaxSfxEx);

    sfx_t* sfx = &g_sfx_ex[g_num_sfx_ex++];
    Q_memset(sfx, 0, sizeof(sfx_t));
    Q_strncpy(sfx->name, name, MAX_QPATH - 1);
    sfx->name[MAX_QPATH - 1] = '\0';
    if (pfInCache)
        *pfInCache = 0;
    return sfx;
}

// --- Phase 1: expanded sound precache index table (indices 512..1023) + svc_sound takeover ---
namespace {
    std::array<sfx_t*, MAX_SOUNDS_EX> g_sound_precache_ex{};

    // Reconstruct hw.dll's MSG_ReadBitCoord (mirror of ReHLDS MSG_WriteBitCoord, common.cpp:852).
    float ReadBitCoord_Ex()
    {
        float value = 0.0f;
        int hasInt  = eng()->MSG_ReadBits(1);
        int hasFrac = eng()->MSG_ReadBits(1);
        if (hasInt || hasFrac)
        {
            int sign    = eng()->MSG_ReadBits(1);
            int intval  = hasInt  ? eng()->MSG_ReadBits(12) : 0;
            int fracval = hasFrac ? eng()->MSG_ReadBits(3)  : 0;
            value = intval + fracval * 0.125f;
            if (sign) value = -value;
        }
        return value;
    }

    // Mirror of MSG_ReadBitVec3Coord (common.cpp:871).
    void ReadBitVec3Coord_Ex(vec3_t out)
    {
        out[0] = out[1] = out[2] = 0.0f;
        int xf = eng()->MSG_ReadBits(1);
        int yf = eng()->MSG_ReadBits(1);
        int zf = eng()->MSG_ReadBits(1);
        if (xf) out[0] = ReadBitCoord_Ex();
        if (yf) out[1] = ReadBitCoord_Ex();
        if (zf) out[2] = ReadBitCoord_Ex();
    }
}

void S_SoundEx_Store(int index, sfx_t* sfx)
{
    if (index >= 0 && index < MAX_SOUNDS_EX)
        g_sound_precache_ex[index] = sfx;
}

void S_SoundEx_Reset()
{
    g_sound_precache_ex.fill(nullptr);
}

// Parses svc_sound exactly like the server writer SV_BuildSoundMsg (sv_main.cpp:786-799).
// Returns true if it fully handled a HIGH-index (>=MAX_SOUNDS) sound; returns false (with the
// message read-cursor rewound to the start) for sentences and indices < MAX_SOUNDS, so the caller
// can delegate to hw.dll's original handler.
bool CL_SvcSound_HandleHigh()
{
    const int rc = *pMsg_readcount;
    eng()->MSG_StartBitReading(net_message);

    int field_mask = eng()->MSG_ReadBits(9);

    int volume    = DEFAULT_SOUND_PACKET_VOLUME;
    int atten_raw = 0;
    if (field_mask & SND_FL_VOLUME)      volume    = eng()->MSG_ReadBits(8);
    if (field_mask & SND_FL_ATTENUATION) atten_raw = eng()->MSG_ReadBits(8);

    int channel   = eng()->MSG_ReadBits(3);
    int entity    = eng()->MSG_ReadBits(MAX_EDICT_BITS);
    int sound_num = (field_mask & SND_FL_LARGE_INDEX) ? eng()->MSG_ReadBits(16) : eng()->MSG_ReadBits(8);

    // Delegate sentences (VOX) and the normal 0..511 path to hw.dll (rewind first).
    if ((field_mask & SND_FL_SENTENCE) || sound_num < MAX_SOUNDS)
    {
        eng()->MSG_EndBitReading(net_message);
        *pMsg_readcount = rc;
        return false;
    }

    vec3_t origin;
    ReadBitVec3Coord_Ex(origin);

    int pitch = DEFAULT_SOUND_PACKET_PITCH;
    if (field_mask & SND_FL_PITCH)
        pitch = eng()->MSG_ReadBits(8);

    eng()->MSG_EndBitReading(net_message);

    sfx_t* sfx = (sound_num < MAX_SOUNDS_EX) ? g_sound_precache_ex[sound_num] : nullptr;
    if (sfx)
    {
        float fvol  = volume / 255.0f;
        float atten = (field_mask & SND_FL_ATTENUATION) ? (atten_raw / 64.0f) : DEFAULT_SOUND_PACKET_ATTENUATION;
        if (channel == CHAN_STATIC)
            eng()->S_StartStaticSound(entity, channel, sfx, origin, fvol, atten, field_mask, pitch);
        else
            eng()->S_StartDynamicSound(entity, channel, sfx, origin, fvol, atten, field_mask, pitch);
    }
    return true;
}
