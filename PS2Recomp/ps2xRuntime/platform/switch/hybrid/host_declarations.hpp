#pragma once
#include "raylib.h"
// Compile-time API declarations omitted by the Vita stream-only subset.
// These have no implementation here: the Switch host bridge must supply real
// behavior before a full runtime can link. Sound/Wave retain the raylib5.5 ABI.
bool IsSoundPlaying(Sound sound);
void UnloadSound(Sound sound);
void StopSound(Sound sound);
Wave LoadWaveFromMemory(const char *fileType,const unsigned char *fileData,int dataSize);
Sound LoadSoundFromWave(Wave wave);
void UnloadWave(Wave wave);
void SetSoundPitch(Sound sound,float pitch);
void SetSoundVolume(Sound sound,float volume);
void PlaySound(Sound sound);
