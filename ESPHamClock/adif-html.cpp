/* HamClock adif.html page, baked in as a C string.
 * served on the R/W REST port as /adif.html. lets a user upload their log from a browser instead of
 * having to put a file on the HamClock machine and type its name into Setup.
 */

#include "HamClock.h"

const char adif_html[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HamClock ADIF log</title>
<style>
  :root {
    --bg: #f5f5f5; --card: #ffffff; --ink: #1a1a1a; --mute: #5a6270; --line: #d5d9de;
    --accent: #0b6e80; --ok: #17733f; --bad: #b3261e;
  }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #14171c; --card: #1c2027; --ink: #e8eaed; --mute: #a3aab5; --line: #2f353d;
            --accent: #4dd0e1; --ok: #3ddc84; --bad: #ff8a80; }
  }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 1.5rem 1rem; background: var(--bg); color: var(--ink);
         font: 16px/1.45 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  main { max-width: 34rem; margin: 0 auto; }
  h1 { font-size: 1.4rem; margin: 0 0 .5rem; }
  p { margin: .5rem 0; }
  .mute { color: var(--mute); }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 1rem; margin: 1rem 0; }
  .big { font-size: 1.15rem; font-weight: 600; }
  input[type=file] { width: 100%; padding: .6rem; border: 1px dashed var(--line); border-radius: 6px;
                     background: var(--bg); color: var(--ink); }
  button { margin-top: .75rem; min-height: 44px; padding: 0 1.25rem; border: 0; border-radius: 6px;
           background: var(--accent); color: #fff; font: inherit; font-weight: 600; cursor: pointer; }
  @media (prefers-color-scheme: dark) { button { color: #00212a; } }
  button:disabled { opacity: .45; cursor: default; }
  progress { width: 100%; margin-top: .75rem; }
  #msg { min-height: 1.4em; margin-top: .75rem; }
  .ok { color: var(--ok); } .bad { color: var(--bad); }
  details { margin-top: 1rem; } summary { cursor: pointer; color: var(--mute); }
</style>
</head>
<body>
<main>
  <h1>ADIF log</h1>
  <p class="mute">Upload your logbook so HamClock can tell which DXCC entities, bands and grids you still need.
     Export it from your logging program as an ADIF file (.adi). It is kept on this HamClock only.</p>

  <section class="card" aria-live="polite">
    <div class="mute">Current log</div>
    <div class="big" id="current">Checking&hellip;</div>
  </section>

  <form class="card" id="form">
    <label for="file">Choose an ADIF file</label>
    <input type="file" id="file" accept=".adi,.adif">
    <button type="submit" id="go" disabled>Upload</button>
    <progress id="bar" max="100" value="0" hidden></progress>
    <div id="msg" role="status"></div>
  </form>

  <details>
    <summary>Notes</summary>
    <p class="mute">Uploading replaces the log HamClock was using. Each QSO needs a call, a band or frequency,
       a date and time, and a grid square (or a call HamClock can place). Files up to 64 MB are accepted.
       HamClock may pause briefly while a large log loads.</p>
    <p class="mute">Next Up and the ADIF pane use this log. You can still pick a file in Setup instead.</p>
  </details>
</main>
<script>
(function () {
  var MAX = 64 * 1024 * 1024;
  var cur = document.getElementById('current'), file = document.getElementById('file'),
      go = document.getElementById('go'), bar = document.getElementById('bar'),
      msg = document.getElementById('msg'), form = document.getElementById('form');

  function say(text, cls) { msg.textContent = text; msg.className = cls || ''; }

  function refresh() {
    fetch('/get_adif.txt', { cache: 'no-store' }).then(function (r) { return r.text(); }).then(function (t) {
      var kv = {};
      t.split('\n').forEach(function (l) {
        var m = l.match(/^(\S+)\s+(.*)$/);
        if (m) kv[m[1]] = m[2].trim();
      });
      var n = parseInt(kv.ADIF_qsos || '0', 10);
      if (kv.ADIF_file === 'none') cur.textContent = 'No log set';
      else if (!n) cur.textContent = 'Log set, but no QSOs loaded yet';
      else cur.textContent = n.toLocaleString() + ' QSOs, ' + kv.ADIF_dxcc + ' DXCC entities';
    }).catch(function () { cur.textContent = 'Could not reach HamClock'; });
  }

  file.addEventListener('change', function () {
    say('');
    var f = file.files[0];
    if (f && f.size > MAX) { say('That file is over 64 MB.', 'bad'); go.disabled = true; return; }
    go.disabled = !f;
  });

  form.addEventListener('submit', function (e) {
    e.preventDefault();
    var f = file.files[0];
    if (!f) return;
    go.disabled = true; bar.hidden = false; bar.value = 0; say('Uploading…');
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/set_adif?save');
    xhr.upload.onprogress = function (ev) { if (ev.lengthComputable) bar.value = ev.loaded / ev.total * 100; };
    xhr.onload = function () {
      bar.hidden = true;
      var body = (xhr.responseText || '').trim();
      if (xhr.status === 200) { say(body || 'Done.', 'ok'); refresh(); }
      else { say(body || ('Upload failed (' + xhr.status + ')'), 'bad'); }
      go.disabled = !file.files[0];
    };
    xhr.onerror = function () { bar.hidden = true; say('Upload failed: connection lost.', 'bad'); go.disabled = false; };
    xhr.send(f);
  });

  refresh();
})();
</script>
</body>
</html>
)HTML";
