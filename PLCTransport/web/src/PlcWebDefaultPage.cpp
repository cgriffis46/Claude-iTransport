#include "PlcWebDefaultPage.h"

namespace {

const char kIndex[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>PLC tags</title>
<style>
  :root { --fg: #222; --bg: #fff; --line: #ddd; --muted: #666; --bad: #b00020; }
  @media (prefers-color-scheme: dark) { :root { --fg: #ddd; --bg: #111; --line: #333; --muted: #999; --bad: #ff6b81; } }
  body { font: 15px/1.4 system-ui, sans-serif; margin: 16px; color: var(--fg); background: var(--bg); }
  h1 { font-size: 20px; margin: 0 0 12px; }
  table { border-collapse: collapse; width: 100%; max-width: 820px; }
  th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid var(--line); }
  td.v { font-family: ui-monospace, monospace; }
  input { font: inherit; padding: 3px 6px; max-width: 9em; }
  button { font: inherit; padding: 3px 10px; }
  #who, #status { color: var(--muted); margin: 8px 0; }
  .err { color: var(--bad); }
  form { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; margin: 8px 0; }
  [hidden] { display: none !important; }   /* or display: flex above would show a hidden form */
</style>
</head>
<body>
<h1>PLC tags</h1>
<div id="who"></div>
<form id="login" hidden>
  <input id="user" autocomplete="username" placeholder="user" required>
  <input id="password" type="password" autocomplete="current-password" placeholder="password" required>
  <button>Log in</button> <span id="loginMsg" class="err"></span>
</form>
<table><thead><tr><th>Name</th><th>Type</th><th>Value</th><th id="setHead"></th></tr></thead><tbody id="tags"></tbody></table>
<div id="status">Loading...</div>
<script>
const $ = id => document.getElementById(id);
let me = null;          // {user, role, csrf} while logged in
const rows = new Map(); // tag name -> its row's cells
const canWrite = () => me && (me.role === 'operator' || me.role === 'admin');
const numberText = /^-?\d+(\.\d+)?([eE][+-]?\d+)?$/;

function showWho() {
  const who = $('who');
  who.replaceChildren();
  if (me) {
    who.append('Logged in as ' + me.user + ' (' + me.role + ') ');
    const out = document.createElement('button');
    out.textContent = 'Log out';
    out.onclick = async () => { await fetch('/api/logout', { method: 'POST' }); me = null; update(); };
    who.append(out);
  } else if (location.protocol !== 'https:') {
    who.textContent = 'Read only. To change values, open this page over https.';
  }
  $('login').hidden = !!me || location.protocol !== 'https:';
  $('setHead').textContent = canWrite() ? 'Set' : '';
}

async function write(t, input, msg) {
  const v = t.type === 'BOOL' ? (input.checked ? 'true' : 'false') : input.value.trim();
  msg.textContent = '';
  if (t.type !== 'BOOL' && !numberText.test(v)) { msg.textContent = 'not a number'; return; }
  const r = await fetch('/api/tags/' + encodeURIComponent(t.name), {
    method: 'POST',
    headers: { 'Content-Type': 'application/json', 'X-CSRF-Token': me.csrf },
    body: '{"value":' + v + '}'   // the text as typed: 64-bit integers stay exact
  });
  if (!r.ok) {
    const j = await r.json().catch(() => ({}));
    msg.textContent = j.error || ('error ' + r.status);
    if (r.status === 401) { me = null; update(); }
  }
}

function row(t) {
  let r = rows.get(t.name);
  if (!r) {
    const tr = document.createElement('tr');
    r = { name: document.createElement('td'), type: document.createElement('td'),
          value: document.createElement('td'), set: document.createElement('td') };
    r.value.className = 'v';
    r.name.textContent = t.name;
    tr.append(r.name, r.type, r.value, r.set);
    $('tags').appendChild(tr);
    rows.set(t.name, r);
  }
  r.type.textContent = t.type + (t.webWritable && t.min !== undefined ? ' (' + t.min + ' to ' + t.max + ')' : '');
  r.value.textContent = JSON.stringify(t.value);
  const want = t.webWritable && canWrite();
  if (want && !r.set.firstChild) {
    const input = document.createElement('input');
    if (t.type === 'BOOL') input.type = 'checkbox';
    const go = document.createElement('button');
    go.textContent = 'Set';
    const msg = document.createElement('span');
    msg.className = 'err';
    go.onclick = () => write(t, input, msg);
    r.set.append(input, ' ', go, ' ', msg);
  } else if (!want && r.set.firstChild) {
    r.set.replaceChildren();
  }
}

async function refresh() {
  try {
    const r = await fetch('/api/tags', { cache: 'no-store' });
    if (r.status === 401) { $('status').textContent = 'Log in to see the tags.'; return; }
    const j = await r.json();
    j.tags.forEach(row);
    $('status').textContent = j.tags.length + ' tags, ' + new Date().toLocaleTimeString();
  } catch (e) {
    $('status').textContent = 'No answer from the PLC: ' + e;
  }
}

async function update() {
  const r = await fetch('/api/session', { cache: 'no-store' }).catch(() => null);
  me = r && r.ok ? await r.json() : null;
  showWho();
  for (const r of rows.values()) r.set.replaceChildren();
  refresh();
}

$('login').onsubmit = async e => {
  e.preventDefault();
  $('loginMsg').textContent = '';
  const r = await fetch('/api/login', {
    method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user: $('user').value, password: $('password').value })
  });
  $('password').value = '';
  const j = await r.json().catch(() => ({}));
  if (r.ok) { me = j; showWho(); update(); }
  else $('loginMsg').textContent = r.status === 429 ? 'Too many tries. Wait ' + (r.headers.get('Retry-After') || 'a while') + ' s.'
                                                     : (j.error || 'error ' + r.status);
};

update();
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
