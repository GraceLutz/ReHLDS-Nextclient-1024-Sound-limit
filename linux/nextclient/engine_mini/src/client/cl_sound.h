#pragma once
#include <string>
#include <unordered_set>

void S_UnloadSounds(const std::unordered_set<std::string>& names);

// --- Phase-0 mixer spike: engine_mini-owned sfx pool (replaces hw.dll's capped known_sfx[]) ---
struct sfx_t;
sfx_t* S_FindName_Ex(const char* name, int* pfInCache);
void   S_FindName_Ex_Reset();

// --- Phase 1: expanded sound precache index table (indices 512..1023) + svc_sound takeover ---
#define MAX_SOUNDS_EX 1024
void S_SoundEx_Store(int index, sfx_t* sfx);
void S_SoundEx_Reset();
bool CL_SvcSound_HandleHigh(); // true if it handled a >=512 sound; false => caller delegates
