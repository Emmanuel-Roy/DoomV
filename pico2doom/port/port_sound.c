// Replaces doomgeneric's s_sound.c, sounds.c and i_sound.c: silence.
//
// The Pico 2 build has no audio hardware, and the packed WAD has no sound
// or music lumps (tools/wadpack.py). Stock doomgeneric still keeps the
// whole sound layer -- channel bookkeeping, the 5 KB S_sfx table, music
// state -- just to throw it all away. These no-ops cost nothing.
//
// None of the replaced code touched game state or either random number
// generator, so DOOM computes exactly what it did before (the sim's
// -hashes comparison against stock doomgeneric confirms it).

#include "doomtype.h"
#include "s_sound.h"
#include "i_sound.h"

int sfxVolume = 8;
int musicVolume = 8;
int snd_channels = 8;

void S_Init(int sfx_volume, int music_volume) { (void)sfx_volume; (void)music_volume; }
void S_Shutdown(void) {}
void S_Start(void) {}
void S_StartSound(void *origin, int sound_id) { (void)origin; (void)sound_id; }
void S_StopSound(mobj_t *origin) { (void)origin; }
void S_StartMusic(int music_id) { (void)music_id; }
void S_ChangeMusic(int music_id, int looping) { (void)music_id; (void)looping; }
boolean S_MusicPlaying(void) { return false; }
void S_StopMusic(void) {}
void S_PauseSound(void) {}
void S_ResumeSound(void) {}
void S_UpdateSounds(mobj_t *listener) { (void)listener; }
void S_SetMusicVolume(int volume) { (void)volume; }
void S_SetSfxVolume(int volume) { (void)volume; }

void I_InitSound(boolean use_sfx_prefix) { (void)use_sfx_prefix; }
void I_InitMusic(void) {}
void I_BindSoundVariables(void) {}
