#ifndef SNAPVITA_DIAGNOSTICS_H
#define SNAPVITA_DIAGNOSTICS_H

/* Release-safe defaults, including builds outside CMake. Diagnostics must be
 * explicitly enabled; the playback thread never performs file logging. */
#ifndef SNAPVITA_FILE_LOGGING
#define SNAPVITA_FILE_LOGGING 0
#endif
#ifndef SNAPVITA_AUDIO_DUMP
#define SNAPVITA_AUDIO_DUMP 0
#endif

#endif
