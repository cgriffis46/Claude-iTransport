#pragma once
#include <cstddef>
#include "HttpFiles.h"

// A built-in UI, served when the file source has no index.html of its
// own: a table of every tag, refreshed each second from /api/tags, with a
// login (over https) and a Set control for each tag an operator may
// write. Also
// a starting point for a custom page (copy it to the SD card or folder
// the server reads, as index.html, and change it there).
//
//     static HttpMemoryFiles builtIn(plcWebDefaultFiles, plcWebDefaultFileCount);
//     static HttpStaticFiles site(disk, &builtIn);
extern const HttpMemoryFiles::File plcWebDefaultFiles[];
extern const size_t plcWebDefaultFileCount;
