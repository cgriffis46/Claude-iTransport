# PLCTransport

The PLC tag database and the ways to reach it from the network.

- `inc/PlcTagRegistry.h`: tags by name, pointing at the program's own
  variables. `readTag`/`writeTag` under one mutex, and `snapshot()` to
  copy tags out under it (for code that must not hold the lock while it
  works).
- `inc/CipTagMessageCodec.h`, `hw/freertos/CipTagTcpServer`: CIP
  Get/Set_Attribute_Single by tag name, over lwIP's raw TCP API. This is
  for machines: HMIs, other PLCs.
- `web/`: the tags as JSON, for web pages and scripts, on iNetTransport's
  `xHttpServer`. Runs on the F207 over lwIP (`SocketNetDevice`), or on a
  board with a W5500 or an ESP module.

## The web API (read-only)

```
GET /api/tags              {"tags":[{"name":"Line1.Temp","type":"REAL","value":21.5,"writable":true}, ...]}
GET /api/tags?names=a,b    those, in order; an unknown one: {"name":"x","error":"no such tag"}
GET /api/tags/<name>       one; 404 if there is no such tag
```

```cpp
static PlcTagWebApi api(registry);
static HttpStdioFiles disk("0:/www");                               // the user's UI
static HttpMemoryFiles builtIn(plcWebDefaultFiles, plcWebDefaultFileCount);
static HttpStaticFiles site(disk, &builtIn);                        // built-in page if none
api.attach(web);                                                    // before the catch-all
web.get("/*", HttpStaticFiles::handler, &site);
```

- Values are read when a request comes in, a few tags per hold of the
  registry's lock, and written out after it's released. A slow client
  never holds up the control program or the CIP server.
- Types: BOOL as true/false; the integer types and REAL/LREAL as numbers
  (NaN and infinity as null; 64-bit integers past 2^53 lose precision in
  JavaScript); STRUCT as a hex string of its first 64 bytes, with `size`
  and `truncated`.
- The built-in page (`PlcWebDefaultPage`) is a table of every tag,
  refreshed each second. Copy it to the UI folder as `index.html` and
  change it there to make your own.
- Writes aren't routed: POST and PUT get 405.
- REAL formatting uses `snprintf("%g")`. With newlib-nano, link with
  `-u _printf_float`.

## Security (planned, not built)

Before tag writes are allowed from the web:

- **Authentication:** a login that issues a session (a random token in an
  `HttpOnly` cookie), checked on every write. Users and roles: read, write.
- **Writes:** an allow-list of tags the web may write, with limits
  (min/max). Never a tag that `safeTransport` relies on.
- **Transport:** plain HTTP shows the password and the session to anyone
  on the network. Either TLS (mbedTLS: about 40-60 KB of flash, and 30+ KB
  of RAM per connection; tight on the F207, easy on an ESP32), or, without
  TLS, a challenge-response login and signed write requests. Those protect
  the password and stop forged or replayed writes, but don't hide the
  values.
- **Captive portal:** for setting up Wi-Fi on a device in access-point
  mode. Every DNS name answers with the device's address, so the setup
  page opens by itself. It suits an ESP32 or ESP-AT module, not a wired
  F207.

## Known limits

- The registry's lock covers `readTag`/`writeTag`/`snapshot`, not the
  control program's own writes to its variables. 64-bit values and
  structs can be read half-updated. That needs the program to publish its
  values under the lock, or a copy made at the end of each scan.
- Lookups by name are linear: fine for hundreds of tags.
- Nothing here has run on an STM32. The F207 build was compile-checked
  only (see CHANGELOG).

## Building and tests

```
cmake -S PLCTransport -B build -DSENSOR_FW_BUILD_TESTS=ON   # plc_tags, plc_web
cmake --build build && ctest --test-dir build
```

`PlcTagWebApi_test` checks the JSON for every type, `?names=`, 404 and
405, and that the lock is free while a response is written.
`PlcWebServer_test` runs it end to end over real sockets with curl and
Python's JSON parser, while a thread keeps changing a tag.
