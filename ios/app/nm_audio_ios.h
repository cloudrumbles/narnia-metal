#ifndef NM_AUDIO_IOS_H
#define NM_AUDIO_IOS_H

#include "nm_audio.h"

/* Game audio sink for the iPad: jitter buffer + Opus decoder
 * (nm_audio_player.c) feeding an AudioUnit RemoteIO, in an AVAudioSession
 * with the playback category so it sounds with the silent switch on.
 */
extern const struct nm_audio_sink_ops nm_audio_sink_ios;

#endif
