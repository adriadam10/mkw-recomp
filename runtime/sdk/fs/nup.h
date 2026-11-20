// Read-only access to a Switch 2 .nup package (the dump format that packs a title's exefs/ and romfs/ into one file),
// so the RomFS can be read in place instead of from a 23 GB extracted folder. Callers go through pack.h.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "pack.h"
#include "platform/platform.h"

enum { NUP_EXEFS = PACK_EXEFS, NUP_ROMFS = PACK_ROMFS };

typedef struct Nup Nup;

int nup_is(const char *path);                             // a regular file that starts with the NUP magic
// The package at path, parsed once and kept until exit (NULL and a log line when it is not a valid NUP).
// Its game's titles are layered (the base game and its updates; DLC, another title id, left out): the ExeFS is the
// newest title's (by version), a RomFS path the newest title's that has it, as an update may hold only what it adds or
// changes (Eglantine's --pack-nup). A package of one title reads as that title.
Nup *nup_open(const char *path);
// The files of a part (NUP_EXEFS, NUP_ROMFS), sorted by name; the array belongs to the Nup. A zstd file is PACK_WHOLE
// (one frame per file), the rest PACK_RAW.
size_t nup_entries(Nup *n, int part, const PackEntry **out);
PlatFile *nup_file(Nup *n);                               // the package, shared; positional reads only
// A whole file of the package, decompressed when stored compressed; malloc'd (free() it), NULL on a read or format error.
uint8_t *nup_read(Nup *n, const PackEntry *e);
