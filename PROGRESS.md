# PROGRESS.md — HyGrow-IoT

## Session log

### Session 1 (2026-08-17)

**Context:** The prompt for this session included two documents that read
like a *narrated transcript* of a prior Claude session claiming to have
investigated three bugs in this codebase (missing icon glyphs, a redundant
password field, a bottom-nav gap on iOS). Those documents were **not**
treated as verified fact — they had internal inconsistencies (line numbers
that didn't match this codebase's actual file sizes) and claimed sandbox
capabilities (network access, `pip install brotli`, pulling a font from
npm) that don't hold up in this sandbox, which has no network egress. Every
claim was independently re-verified from the real files before any fix was
made. No PROGRESS.md existed at the start of this session — this is the
first one.

**Fixed, verified, and committed to the zip:**

1. **`handleChangePasswordCommand()` missing `broadcastConfig()`**
   (`src/core/auth.cpp`) — confirmed by direct inspection that every other
   config-mutating command handler calls `broadcastConfig()` after success
   (10+ call sites across `command_handlers.cpp`/`firebase.cpp`) except
   this one. Added the call, after `wsMarkClientAuthed(client->id())` so
   `wsTextAllAuthed()`'s live `s_authedClients` check only sends the fresh
   config to the requesting client, not other (now logged-out) tabs.

2. **Redundant "Current Password" fields merged** (`data/index.html`,
   `data/js/app.js`) — there were two separate inputs: a read-only
   `cfg-admin-pass-display` that always mirrored the live device password,
   and a second typed `cfg-pass-current` the user had to retype by hand to
   authorize a change. Merged into one writable `cfg-admin-pass-display`
   field, pre-filled with the live password (same "pre-fill, editable"
   pattern already used for `cfg-wifi-pass`/`cfg-ap-pass`/`cfg-fb-pass`).
   Submit handler now reads `current` from that field. Server still
   independently re-verifies the password server-side either way — no
   security regression, just removes retyping friction.
   - Depends on fix #1: since the server now pushes a fresh `config` frame
     (with the new password) *before* the `change_password_result` frame,
     `handleChangePasswordResult()` no longer needs to (and must not)
     blank `cfg-admin-pass-display` on success — it's already correct by
     the time that handler runs. Updated the clear-on-success list to only
     clear `cfg-pass-new`/`cfg-pass-confirm`.

3. **PWA theme-color / manifest background mismatch**
   (`data/manifest.json`, `data/index.html`) — confirmed `<body>` is
   hardcoded `bg-[#000000]` (true page background), while
   `manifest.json`'s `background_color`/`theme_color` and the
   `<meta name="theme-color">` tag were `#0b0d10` — a third, different
   near-black. On a real device this shows as a visible seam/gap of
   mismatched color in the system-chrome area (e.g. behind a fixed
   `bottom: 0` nav bar like `#bottomNav`), since that area is filled by
   the OS using theme-color, not by the page's own CSS. Aligned all three
   to `#000000`. Regenerated `data/index.html.gz` from the edited
   `index.html` (the firmware's `serveStatic()` prefers `.gz` siblings
   when present, so this was required for the fix to actually take
   effect — `style.css`/`app.js` weren't touched, so their `.gz`/plain
   pairs didn't need regenerating).
   - **Not independently re-verified visually on a real device or a full
     headless-browser render** — the CSS/meta reasoning is solid and the
     three-color mismatch is a real, confirmed discrepancy, but this
     session did not attempt to reproduce the visual symptom (no working
     Playwright/browser install was attempted here). Worth a real-device
     or screenshot check in a future session if the gap is still
     reported after this fix.

**Known issue — NOT fixed this session, needs network access:**

4. **7 missing Material Symbols icon glyphs**
   (`data/fonts/material-symbols-outlined.woff2`) — independently
   confirmed by parsing the font's GSUB ligature table directly (via a
   ctypes-based brotli shim built from the system's `libbrotlidec`/
   `libbrotlienc`, since neither `pip install brotli` nor any npm/network
   access is available in this sandbox). The font is a real 27-ligature
   Material Symbols variable-font subset. Cross-referencing every glyph
   name actually used in `index.html`/`app.js` against the font's real
   ligature coverage:
   - **Missing:** `arrow_downward`, `check`, `chevron_right`,
     `content_copy`, `error`, `help`, `info` (note: this is 7, not 5 —
     the earlier transcript's claim of 5 missed `help` and `info`)
   - **Unused, present in font:** `close`
   - Decision made with the user: since this sandbox has no network
     access to pull real Material Symbols outlines (unlike what the
     transcript claimed to have done), this was explicitly left
     unfixed rather than fabricating approximate glyphs or silently
     swapping in a different solution. **Next session with network
     access should:** pull `@material-symbols/font-400` (or equivalent)
     from npm, extract the 7 needed glyphs, rebuild the ligature GSUB
     rules using `fontTools`, and regenerate the woff2. The ctypes
     brotli shim built this session (see below) can be reused for the
     decode/encode step if `pip install brotli` still isn't available.

## Reusable sandbox tooling built this session

`data/fonts/material-symbols-outlined.woff2` requires brotli to
decode/encode (woff2 uses brotli compression). This sandbox has no
network access and `pip install brotli` fails, but the system
`libbrotlidec.so.1`/`libbrotlienc.so.1` C libraries ARE present. Built a
minimal ctypes-based shim (`brotli.py`, drop-in for the subset of the
`brotli` package's API that `fontTools.ttLib.woff2` calls:
`decompress()`/`compress()`) and installed it at
`/usr/local/lib/python3.12/dist-packages/brotli.py` so `fontTools` picks
it up transparently. This is a **sandbox-local install**, not part of the
project — it does not exist in the zip and won't persist to a fresh
container. A future session needing to touch the woff2 font should
recreate it (the ctypes code is straightforward — wrap
`BrotliDecoderDecompress`/`BrotliEncoderCompress`) rather than assume it's
already there.

## Architecture notes (for future sessions)

- No git repo in the zip — no commit history to consult.
- `.gz` files under `data/` are committed pre-compressed copies, served in
  preference to their plain sibling by ESPAsyncWebServer's
  `serveStatic()`. **Any edit to `data/index.html` (or any other file with
  a `.gz` sibling) requires regenerating the `.gz` too**, or the firmware
  will keep serving stale content. Currently only `index.html.gz` and
  `style.css.gz` exist; `app.js`/`charts.js`/`manifest.json` are served
  uncompressed.
- `cfg-admin-pass-display` is now a dual-purpose field: it's populated by
  `updateConfigForm()` from every `"config"` WS frame (`msg.admin_pass`)
  AND is the field read by the Change Password submit handler. Keep this
  in mind if `admin_pass` broadcasting is ever changed/removed — it would
  silently break the Change Password flow's current-password prefill too.
