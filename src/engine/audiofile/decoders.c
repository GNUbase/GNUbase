// SPDX-License-Identifier: GPL-3.0-or-later
// Compiles the single-file decoders from ext/ once: dr_wav (WAV, RF64, Wave64 and AIFF), dr_flac, dr_mp3 and
// stb_vorbis. Built as a library of its own without GNUbase's warning flags, since this is third-party code.
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"
