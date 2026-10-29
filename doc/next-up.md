# Next Up pane

A pane (choose **Next_Up** in a pane's rotation menu) that ranks the contacts you would most like to make right now.
The best is shown large; tap it, or any row, to point the DX at the station and tune the radio, exactly as tapping a
row in the DX Cluster pane does.

## How a spot is scored

`score = Need x Reach x Freshness x 100`. Multiplying means any factor near zero sinks the spot. Colors: green 80+,
amber 60-79, gray below.

| factor | meaning | source |
|---|---|---|
| Need | new DXCC 1.0, new band for a worked DXCC 0.7, new grid 0.5, new prefix 0.4, worked 0.05; 0.5 if no log | your ADIF log |
| Reach | evidence the path is open (see below); 0.5 and flagged if there is none | Live Spots, other spots |
| Freshness | 1.0 for 5 minutes, falling to 0.2 at 30 minutes | spot age |

Reach blends two kinds of evidence, never a guess:

* **heard you** -- Live Spots reports *of DE* (Live Spots set to "of DE") on the spot's band: how many stations heard you and
  how far, compared with the distance to the spot. If you have been heard on other bands but not this one the row is
  marked `?` / "unconfirmed".
* **path open** -- other spots on the same band whose spotter is within 3000 km of DE and whose station is within 3000 km
  of the candidate. WSJT-X decodes count strongly because DE is the spotter.

There is no VOACAP prediction term yet.

One row is shown per DXCC entity, so three stations from the same new country take one slot.

## Giving it your log

Open `http://<your HamClock>:8080/adif.html` in a browser, choose your ADIF export (.adi) and press Upload. The log is
saved on the HamClock, survives restarts, and is used right away. From a script:
`curl --data-binary '@mylog.adi' 'http://<your HamClock>:8080/set_adif?save'`. You can still pick a file in Setup instead.
Until a log is loaded the pane shows "no log: /adif.html" and every score uses a neutral Need, so the ranking is not
about what you need.

## What it needs

Next Up reads what other modules already collect, and for the spot sources they only collect while their own pane is in
some pane's rotation set. (Your ADIF log is the exception: Next Up loads it itself.) The footer shows which are feeding it: `CLU` DX Cluster (also carries WSJT-X), `HA` HamAlert, `OTA` On The Air,
`ADIF` your log, `PSK` Live Spots of DE. Put the ones you have in a rotation set (they do not need to be on screen).
Without ADIF, Need is neutral and shown as "need ?". With no spot source the pane says so.

Row tags: `C` cluster, `H` HamAlert, `O` On The Air.
