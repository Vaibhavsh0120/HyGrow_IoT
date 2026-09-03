# PROGRESS.md

Tracks what's done, what's open, and why things are built the way they are.
Read this before starting new work.

## Current state

Firmware is stable and feature-complete for its current scope: sensor
reading, local dashboard (LittleFS-hosted), calibration wizards, WiFi
provisioning with SoftAP fallback, and optional Firestore cloud sync.
Verified working on real ESP32-S3 N16R8 hardware.

## Done

- Full sensor pipeline for all 6 sensors (water level, light, TDS, DHT22,
  pH, DS18B20) with startup validation and auto-disable on repeated
  failure.
- Local web dashboard (`data/`) served from LittleFS: live tiles, Settings,
  guided pH/TDS calibration, Terminal log view, light/dark/auto theme.
- WiFi provisioning with `HyGrow-Setup` SoftAP fallback when saved
  credentials fail.
- BOOT-button hold-to-reset: 10s clears the admin password only, 20s does
  a full factory reset.
- Optional Firestore cloud sync (`src/core/firebase.cpp`) — one document
  per device, written on a fixed interval, sensor fields sent as `null`
  when a sensor is disabled/failing rather than dropped or stale.
- Material Symbols icon font rebuilt to render by Private Use Area
  codepoint instead of OpenType ligatures, plus defensive CSS containment
  (`width/height: 1em; overflow: hidden`) — confirmed fixed on real
  hardware, not just a local repro.
- Admin password flow consolidated to a single read-only "current
  password" field driven by the live broadcast config.

## Known issues / open items

- **Bottom nav safe-area gap**: a small visual gap between the bottom nav
  bar and the bottom of the screen on some phones. Investigated (checked
  `.liquid-glass` background rules, `#bottomNav` CSS, safe-area-inset
  padding) without a conclusive root cause. Next step if picked back up:
  inspect the actual rendered box model of `#bottomNav` and its
  `initBottomNav()`-injected children in real devtools, not static
  analysis.
- **`tools/build-icon-font.py` is not in this repo snapshot**, though
  `data/css/style.css` and the font's own comments reference it as the
  source of the icon font and the auto-generated `ICON-CODEPOINTS` block.
  If you have this script locally, add it back under `tools/`. If not, it
  needs to be rebuilt before the icon set can be safely regenerated —
  don't hand-edit the codepoint block or the `.woff2` in the meantime.
- **No `docs/` or `firebase/` folders in this repo.** Earlier documentation
  referenced a deeper `docs/` (Firestore/WebSocket/firmware architecture
  writeups) and a `firebase/` folder (security rules + an offline-detection
  Cloud Function). Neither exists here. README.md has been rewritten to
  only describe what's actually in this repo — Firestore setup is
  console-only steps, and there's no bundled security rules or
  offline-detection function. Treat both as new work if you want them, not
  something to restore.
- No `LICENSE` file yet.

## Decisions made

- Icons render by PUA codepoint lookup (single `cmap` hit), not ligature
  substitution — the ligature approach previously shipped completely
  broken (icons fell back to literal text on real hardware).
- Firestore writes only ever set `status: "Online"`; the firmware never
  writes `"Offline"` itself. Detecting a genuinely offline device requires
  something external watching `lastUpdated` staleness — not included here.
- `secrets.h` is required at compile time (no silent empty-string
  fallback) so a board can never get flashed with credentials it doesn't
  actually have.
