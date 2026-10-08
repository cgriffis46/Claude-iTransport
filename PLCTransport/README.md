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
  `xHttpServer`, and allow-listed writes for logged-in operators over
  HTTPS. Runs on the F207 over lwIP (`SocketNetDevice`), or on a board
  with a W5500 or an ESP module.

## The web API

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
- Without a WebAuth, writes aren't routed: POST and PUT get 405.
- REAL formatting uses `snprintf("%g")`. With newlib-nano, link with
  `-u _printf_float`.

## Writes, over HTTPS with logins

```cpp
static const PlcWebWritable writable[] = {   // what the web may set, and its limits
    {"Oven.Setpoint", 20, 250},
    {"Oven.Pump", 0, 0},                     // BOOL: limits ignored
};
PlcTagWebApi::Config c;
c.auth = &auth;                              // iNetTransport's WebAuth
c.writable = writable;  c.writableCount = 2;
c.audit = logWrite;  c.auditCtx = &log;      // who, the tag before and after
static PlcTagWebApi api(registry, c);
api.attach(web);                             // adds POST /api/tags/<name>
```

```
POST /api/tags/Oven.Setpoint   {"value":180}   with the session cookie and X-CSRF-Token
```

- The server should be HTTPS (iNetTransport/README.md, "HTTPS and
  logins"). Logins are refused over plain HTTP.
- A write needs `writeRole` (operator by default) and the CSRF token. The
  tag must be on the allow-list and writable in the registry, and not a
  STRUCT. The value must fit the tag's type (integers exactly, all 64
  bits) and the list's limits. Each refusal has its own status and
  message (401, 403, 404, 400, 422).
- Every write goes to the audit callback.
- Never put a safety tag on the list: a web login is not a safety
  function.
- `readRole`: set it to Viewer to need a login for reading too.
- The built-in page shows a login over HTTPS and a Set control for each
  listed tag, to operators. It was checked in Chromium with
  `web/test/ui_test.cjs` (Playwright; not part of ctest).
- Not built: a captive portal (Wi-Fi setup on an ESP), and changing
  users at run time (they're compiled in, from `tools/web_user.py`).

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
cmake -S PLCTransport -B build -DSENSOR_FW_BUILD_TESTS=ON \
      -DMBEDTLS_DIR=<mbedTLS 3.6 source>                    # plc_tags, plc_web; HTTPS test
cmake --build build && ctest --test-dir build
```

`PlcTagWebApi_test` checks the JSON for every type, `?names=`, 404 and
405, and that the lock is free while a response is written.
`PlcWebServer_test` runs it end to end over real sockets with curl and
Python's JSON parser, while a thread keeps changing a tag.
`PlcWebSecure_test` (with `MBEDTLS_DIR`) does it over HTTPS with logins:
a viewer refused, an operator's writes with limits, the E-stop
untouchable, the audit log. `--serve <s>` keeps it up for a browser
(`ui_test.cjs`).
