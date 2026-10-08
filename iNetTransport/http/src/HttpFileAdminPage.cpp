#include "HttpFileAdmin.h"

namespace {

const char kPage[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Files</title>
<style>
  :root { --fg: #222; --bg: #fff; --line: #ddd; --muted: #666; --bad: #b00020; }
  @media (prefers-color-scheme: dark) { :root { --fg: #ddd; --bg: #111; --line: #333; --muted: #999; --bad: #ff6b81; } }
  body { font: 15px/1.4 system-ui, sans-serif; margin: 16px; color: var(--fg); background: var(--bg); }
  h1 { font-size: 20px; margin: 0 0 12px; }
  table { border-collapse: collapse; width: 100%; max-width: 720px; }
  th, td { text-align: left; padding: 6px 10px; border-bottom: 1px solid var(--line); }
  td.n { text-align: right; font-family: ui-monospace, monospace; }
  input, button { font: inherit; padding: 3px 8px; }
  form, .row { display: flex; gap: 6px; flex-wrap: wrap; align-items: center; margin: 8px 0; }
  [hidden] { display: none !important; }
  #who, #space, #log { color: var(--muted); }
  .err, #log.err { color: var(--bad); }
</style>
</head>
<body>
<h1>Files</h1>
<div id="who"></div>
<form id="login" hidden>
  <input id="user" autocomplete="username" placeholder="user" required>
  <input id="password" type="password" autocomplete="current-password" placeholder="password" required>
  <button>Log in</button> <span id="loginMsg" class="err"></span>
</form>
<div id="admin" hidden>
  <div class="row">
    <label>Folder <input id="dir" value="/" size="12"></label>
    <input id="pick" type="file" multiple>
    <button id="send">Upload</button>
  </div>
  <div id="log"></div>
  <table><thead><tr><th>File</th><th>Bytes</th><th></th></tr></thead><tbody id="files"></tbody></table>
  <div id="space"></div>
</div>
<script>
const $ = id => document.getElementById(id);
let me = null, piece = 1024;
const url = p => '/api/files' + p.split('/').map(encodeURIComponent).join('/');

async function api(method, path, body, query) {
  let r;
  // 503: the file is being sent to someone; it is replaced once they have it.
  for (let tries = 0; ; tries++) {
    r = await fetch(url(path) + (query || ''), {
      method, body, headers: { 'X-CSRF-Token': me ? me.csrf : '', 'Content-Type': 'application/octet-stream' } });
    if (r.status !== 503 || tries >= 60) break;
    $('log').textContent = path + ': waiting while it is being read';
    await new Promise(done => setTimeout(done, 500));
  }
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || ('error ' + r.status));
  return j;
}

async function load() {
  const r = await fetch('/api/files', { cache: 'no-store' });
  if (!r.ok) { $('space').textContent = r.status === 403 ? 'An admin login is needed.' : 'error ' + r.status; return; }
  const j = await r.json();
  piece = j.piece || piece;
  $('files').replaceChildren(...j.files.map(f => {
    const tr = document.createElement('tr');
    const a = document.createElement('a');
    a.href = f.path; a.textContent = f.path;
    const name = document.createElement('td'); name.appendChild(a);
    const size = document.createElement('td'); size.className = 'n'; size.textContent = f.size;
    const del = document.createElement('button'); del.textContent = 'Delete';
    del.onclick = async () => {
      if (!confirm('Delete ' + f.path + '?')) return;
      try { await api('DELETE', f.path); } catch (e) { $('log').textContent = e.message; }
      load();
    };
    const act = document.createElement('td'); act.appendChild(del);
    tr.append(name, size, act);
    return tr;
  }));
  $('space').textContent = j.free + ' of ' + j.total + ' bytes free; files up to ' + j.max + ' bytes';
}

async function upload(file, dir) {
  const path = (dir.endsWith('/') ? dir : dir + '/') + file.name;
  // The browser would resolve "..", sending the request somewhere else.
  if (!path.startsWith('/') || path.split('/').slice(1).some(p => p === '' || p.startsWith('.')))
    throw new Error('not a file path: ' + path + ' (no empty names, none starting with ".")');
  const data = new Uint8Array(await file.arrayBuffer());
  for (let at = 0; at < data.length || at === 0; at += piece) {
    $('log').textContent = path + ': ' + Math.min(at, data.length) + ' / ' + data.length;
    await api('PUT', path, data.subarray(at, at + piece), '?offset=' + at);
    if (data.length === 0) break;
  }
  await api('POST', path, null, '?size=' + data.length);
  $('log').textContent = path + ': ' + data.length + ' bytes, done';
}

$('send').onclick = async () => {
  const files = [...$('pick').files];
  try { for (const f of files) await upload(f, $('dir').value.trim() || '/'); }
  catch (e) { $('log').textContent = 'failed: ' + e.message; $('log').className = 'err'; return; }
  $('log').className = '';
  $('pick').value = '';
  load();
};

async function session() {
  const r = await fetch('/api/session', { cache: 'no-store' }).catch(() => null);
  me = r && r.ok ? await r.json() : null;
  const who = $('who');
  who.replaceChildren();
  if (me) {
    who.append('Logged in as ' + me.user + ' (' + me.role + ') ');
    const out = document.createElement('button');
    out.textContent = 'Log out';
    out.onclick = async () => { await fetch('/api/logout', { method: 'POST' }); session(); };
    who.append(out);
  } else if (location.protocol !== 'https:') {
    who.textContent = 'Open this page over https to log in.';
  }
  $('login').hidden = !!me || location.protocol !== 'https:';
  $('admin').hidden = !me;
  if (me) load();
}

$('login').onsubmit = async e => {
  e.preventDefault();
  const r = await fetch('/api/login', { method: 'POST', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ user: $('user').value, password: $('password').value }) });
  $('password').value = '';
  const j = await r.json().catch(() => ({}));
  $('loginMsg').textContent = r.ok ? '' : (j.error || 'error ' + r.status);
  if (r.ok) session();
};

session();
</script>
</body>
</html>
)html";

}  // namespace

const HttpMemoryFiles::File httpFileAdminPage = {"/files.html", reinterpret_cast<const uint8_t*>(kPage), sizeof kPage - 1};
