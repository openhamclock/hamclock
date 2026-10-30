# Vega client

- Start with README.md (build, install, tests). ARCHITECTURE.md is the original
  native-app spec and is superseded: the app embeds the HamClock engine and shows
  its page in a WebView. The PHASEn_RESULTS.md files describe what was actually built.
- The Android app (../android) may be used as a UX reference (screens,
  settings, wording). Don't copy its internal design.
- Use the system Node from dnf (/usr/bin/node) for npm/tsc/eslint; the `vega`
  CLI uses its own bundled Node.
- For testing, use callsign N0CALL. Test device: 192.168.1.237:5555 (armv7).
- Phase 0 spike code is in ../tmp/vega-spike (untracked scratch).
- The phase plan is PLAN.md in this folder (keep it up to date); results per phase are in
  PHASEn_RESULTS.md, the latest taking precedence.
