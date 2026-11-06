// Pomodoro sounds: short lo-fi cues for start and finish, and (parked for now)
// background music, generative lo-fi hip hop or an internet lo-fi station.
#pragma once

enum Sound { SOUND_POMODORO_START, SOUND_POMODORO_FINISH };
void soundPlay(Sound s);      // in the background
bool soundBusy();             // a cue or music is coming out of the speaker

void musicBegin();            // starts the audio task; call after audioBegin()
void musicStart();            // fades in
void musicStop(bool finish);  // finish: a closing chord, then fade; otherwise a quick fade
bool musicPlaying();          // sound is coming out, fades included
