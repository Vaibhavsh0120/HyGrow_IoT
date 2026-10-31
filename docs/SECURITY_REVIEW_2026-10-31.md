# IoT review - 2026-10-31

Work stayed on main; no flash, deployment, commit, push or secret-file edits.
Firmware version and telemetry schema remain 3.1.4 / 1.

Changes: four Firebase HTTPS paths verify Google GTS R1/R3/R4 CA chains;
nonblocking SNTP gates cloud requests until wall-clock initialization.
Config and boot logs no longer disclose saved passwords. Removed password
getter APIs; dashboard requires typed current password and clears typed fields
on successful admin change. A mutex protects authenticated-client set access
across tasks. Permanent cloud auto-disable persistence/config broadcasts move
to the network loop and reject stale settings generations. DHT rejects
nonfinite/out-of-range temperature (-40..80 C) and humidity (0..100%);
DS18B20 rejects nonfinite/out-of-range temperature (-55..125 C) and disconnect
sentinels; BH1750 rejects nonfinite/negative values. Valid zero stays valid.
Dashboard gzip copies regenerated.

Canonical /status schema 1, UDP 39400 discovery, completed sample identity,
Firestore fields/nulls and server lastUpdated transform are unchanged.
Firestore writes no owner metadata. See APP_LOCAL_TELEMETRY_HANDOFF.md.

Regression-first tests reproduced password disclosure, uncleared current
password, concurrent auth set access violation, invalid sensor acceptance and
untrusted cloud requests. All pass after the fixes. Existing local telemetry,
local network, cloud worker, water-level, dashboard flows, terminal flows,
telemetry view and asset checks also pass. Actual firmware build outcome is
reported separately; initial attempt could not write global PlatformIO lock. The installed global
framework/toolchain were newer than the pinned platform; matching cached
archives were subsequently extracted into an isolated writable build core.

Limits: host HTTPS simulation checks trust configuration, not real certificate
exchange. Public local /status is intentionally unauthenticated; HTTP/WebSocket
login needs a trusted LAN. Secrets remain plaintext in NVS/ignored secrets.h.
Secure boot, flash encryption, authenticated time and local HTTPS are outside
this change. Google roots need maintenance before rotation/June 2036 expiry.
No live Firestore, sensor accuracy, radio or hardware soak test was performed.
The auth set/cloud persistence fixes do not establish all shared global
configuration/NVS state is race-free; hardware concurrency validation remains.

Final local build attempt resolved the pinned packages in an isolated core,
but Windows refused the Xtensa compiler with Illegal System DLL Relocation.
Firmware compilation is not claimed as passing. The new Linux CI compile uses
the pinned platform and public example configuration; it does not flash hardware.
