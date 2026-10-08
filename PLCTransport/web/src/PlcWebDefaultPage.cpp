#include "PlcWebDefaultPage.h"

namespace {

const char kIndex[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PLC tags</title>
<style>
  body { font: 15px/1.4 system-ui, sans-serif; margin: 16px; color: #222; background: #fff; }
  h1 { font-size: 20px; margin: 0 0 12px; }
  table { border-collapse: collapse; width: 100%; max-width: 720px; }
  th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid #ddd; }
  td.v { font-family: ui-monospace, monospace; }
  #status { color: #666; margin-top: 10px; }
  @media (prefers-color-scheme: dark) { body { color: #ddd; background: #111; } th, td { border-color: #333; } }
</style>
</head>
<body>
<h1>PLC tags</h1>
<table><thead><tr><th>Name</th><th>Type</th><th>Value</th></tr></thead><tbody id="tags"></tbody></table>
<div id="status">Loading...</div>
<script>
const body = document.getElementById('tags'), status = document.getElementById('status');
async function refresh() {
  try {
    const r = await fetch('/api/tags', { cache: 'no-store' });
    const j = await r.json();
    body.replaceChildren(...j.tags.map(t => {
      const tr = document.createElement('tr');
      for (const v of [t.name, t.type, JSON.stringify(t.value)]) {
        const td = document.createElement('td');
        td.textContent = v;
        tr.appendChild(td);
      }
      tr.lastChild.className = 'v';
      return tr;
    }));
    status.textContent = j.tags.length + ' tags, ' + new Date().toLocaleTimeString();
  } catch (e) {
    status.textContent = 'No answer from the PLC: ' + e;
  }
}
refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
)html";

}  // namespace

const HttpMemoryFiles::File plcWebDefaultFiles[] = {
    {"/index.html", reinterpret_cast<const uint8_t*>(kIndex), sizeof kIndex - 1},
};
const size_t plcWebDefaultFileCount = sizeof plcWebDefaultFiles / sizeof plcWebDefaultFiles[0];
