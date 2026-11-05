// SPDX-License-Identifier: GPL-3.0-or-later
// Compiles the single-file decoders from ext/ once: dr_wav (WAV, RF64, Wave64 and AIFF), dr_flac, dr_mp3 and
// stb_vorbis. Built as a library of its own without GNUbase's warning flags, since this is third-party code.
//
// Without dr_libs' stdio functions: the readers open the files and hand them over through callbacks. The decoders
// close a file at the end if their read callback is the one for files they open themselves, which they tell by its
// address, and MSVC's identical code folding gives the readers' callback, with the same code, that address in Release
// builds. The reader then closed the file a second time, by then maybe another thread's. The defines that configure
// the decoders (DR_WAV_NO_STDIO and the others) are public compile definitions of gnubase-decoders
// (src/engine/CMakeLists.txt), so that every file that includes these headers sees the same configuration.
#if !defined(DR_WAV_NO_STDIO) || !defined(DR_FLAC_NO_STDIO) || !defined(DR_MP3_NO_STDIO) ||                             \
	!defined(STB_VORBIS_NO_PUSHDATA_API)
#error "the decoders' configuration comes from the compile definitions of gnubase-decoders"
#endif
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#include "stb_vorbis.c"
