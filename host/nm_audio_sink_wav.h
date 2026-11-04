#ifndef NM_AUDIO_SINK_WAV_H
#define NM_AUDIO_SINK_WAV_H

#include "nm_audio.h"

/* Test sink for the CLI host: runs the jitter buffer against a simulated
 * 10 ms device clock, logs buffer stats and the capture-to-playout latency
 * (valid when the daemon runs on the same machine) once a second, and writes
 * what it played to $NM_AUDIO_WAV if set.
 */
extern const struct nm_audio_sink_ops nm_audio_sink_wav;

#endif
