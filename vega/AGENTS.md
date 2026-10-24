# Vega client

- The design spec is ARCHITECTURE.md in this folder. Read it before changing
  app logic, protocol handling, or the lifecycle state machine. Note: the
  approach has since changed to an embedded engine + WebView; see
  PHASE0_RESULTS.md, which takes precedence where they conflict.
- The Android app (../android) may be used as a UX reference (screens,
  settings, wording). Don't copy its internal design.
- Use the system Node from dnf (/usr/bin/node) for npm/tsc/eslint; the `vega`
  CLI uses its own bundled Node.
- For testing, use callsign N0CALL. Test device: 192.168.1.237:5555 (armv7).
- Phase 0 spike code is in ../tmp/vega-spike (untracked scratch).
