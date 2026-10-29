/* HamClock parks.html page, baked in as a C string.
 * served on the R/W REST port as /parks.html. lets a user upload the Hunted Parks CSV downloaded from pota.app so
 * HamClock can flag parks they have not hunted yet. see huntedparks.cpp.
 * the screenshot is a downscaled 760 px wide PNG of the pota.app Activated/Hunted Parks tabs, as a data: URI.
 */

#include "HamClock.h"

const char parks_html[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>HamClock POTA hunted parks</title>
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
  a { color: var(--accent); }
  .mute { color: var(--mute); }
  .card { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 1rem; margin: 1rem 0; }
  .big { font-size: 1.15rem; font-weight: 600; }
  ol { margin: .5rem 0 0; padding-left: 1.25rem; } li { margin: .5rem 0; }
  img { display: block; width: 100%; height: auto; margin: .5rem 0; border: 1px solid var(--line); border-radius: 6px;
        background: #f5f5f5; }
  input[type=file] { width: 100%; padding: .6rem; border: 1px dashed var(--line); border-radius: 6px;
                     background: var(--bg); color: var(--ink); }
  button { margin-top: .75rem; min-height: 44px; padding: 0 1.25rem; border: 0; border-radius: 6px;
           background: var(--accent); color: #fff; font: inherit; font-weight: 600; cursor: pointer; }
  @media (prefers-color-scheme: dark) { button { color: #00212a; } }
  button:disabled { opacity: .45; cursor: default; }
  #msg { min-height: 1.4em; margin-top: .75rem; }
  .ok { color: var(--ok); } .bad { color: var(--bad); }
  details { margin-top: 1rem; } summary { cursor: pointer; color: var(--mute); }
</style>
</head>
<body>
<main>
  <h1>POTA hunted parks</h1>
  <p class="mute">Upload the list of parks you have hunted so HamClock can flag a park you have not worked yet as
     a <b>NEW PARK</b> in Next Up, and underline it in yellow in the On The Air pane.</p>

  <section class="card" aria-live="polite">
    <div class="mute">Current list</div>
    <div class="big" id="current">Checking&hellip;</div>
  </section>

  <section class="card">
    <div class="big">Step 1: get the file from pota.app</div>
    <ol>
      <li>Open <a href="https://pota.app/#stats/" target="_blank" rel="noopener">https://pota.app/#stats/</a>
          and <b>log in</b>. Your stats are only shown, and the download only works, when you are logged in.</li>
      <li><b>Scroll down to the bottom of the page.</b> You will find the <b>Activated Parks</b> and
          <b>Hunted Parks</b> tabs.</li>
      <li>Click the <b>Hunted Parks</b> tab, then click <b>Export CSV (Hunted Parks)</b>, as shown below.
          Be sure the button says Hunted, not Activated.
          <img alt="pota.app stats page with the Hunted Parks tab selected and the Export CSV (Hunted Parks) button"
               src="data:image/png;base64,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"></li>
      <li>Your browser saves a file named something like <code>hunter_parks.csv</code>.</li>
    </ol>
  </section>

  <form class="card" id="form">
    <div class="big">Step 2: upload it here</div>
    <p class="mute" style="margin-top:.25rem">Click Browse (or Choose File), pick the CSV you just downloaded, then Upload.
       It is stored on this HamClock.</p>
    <input type="file" id="file" accept=".csv,text/csv">
    <button type="submit" id="go" disabled>Upload</button>
    <div id="msg" role="status"></div>
  </form>

  <details>
    <summary>Notes</summary>
    <p class="mute">Uploading replaces the previous list. Download a fresh file and upload it again whenever you
       want to include parks you have hunted since.</p>
    <p class="mute">Only the park reference column is used. The Activated Parks file is refused because parks you
       activated are not parks you hunted.</p>
    <p class="mute">This works alongside your ADIF log: a park counts as worked if it is in either.</p>
  </details>
</main>
<script>
(function () {
  var MAX = 4 * 1024 * 1024;
  var cur = document.getElementById('current'), file = document.getElementById('file'),
      go = document.getElementById('go'), msg = document.getElementById('msg'),
      form = document.getElementById('form');

  function say(text, cls) { msg.textContent = text; msg.className = cls || ''; }

  function refresh() {
    fetch('/get_parks.txt', { cache: 'no-store' }).then(function (r) { return r.text(); }).then(function (t) {
      var m = t.match(/POTA_hunted_parks\s+(\d+)/);
      var n = m ? parseInt(m[1], 10) : 0;
      cur.textContent = n ? n.toLocaleString() + ' parks hunted' : 'No hunted parks loaded';
    }).catch(function () { cur.textContent = 'Could not reach HamClock'; });
  }

  file.addEventListener('change', function () {
    say('');
    var f = file.files[0];
    if (f && f.size > MAX) { say('That file is over 4 MB.', 'bad'); go.disabled = true; return; }
    go.disabled = !f;
  });

  form.addEventListener('submit', function (e) {
    e.preventDefault();
    var f = file.files[0];
    if (!f) return;
    go.disabled = true; say('Uploading\u2026');
    var xhr = new XMLHttpRequest();
    xhr.open('POST', '/set_parks?save');
    xhr.onload = function () {
      var body = (xhr.responseText || '').trim();
      if (xhr.status === 200) { say(body || 'Done.', 'ok'); refresh(); }
      else { say(body || ('Upload failed (' + xhr.status + ')'), 'bad'); }
      go.disabled = !file.files[0];
    };
    xhr.onerror = function () { say('Upload failed: connection lost.', 'bad'); go.disabled = false; };
    xhr.send(f);
  });

  refresh();
})();
</script>
</body>
</html>
)HTML";
