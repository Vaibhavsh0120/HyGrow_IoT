# HyGrow IoT — Technical Documentation

This folder holds implementation-level reference material for developers
and AI assistants working on the HyGrow IoT codebase. If you're setting up
or using a HyGrow device, start with the main
[project README](../README.md) instead — these documents assume you're
reading source code alongside them.

| Document | Covers |
| --- | --- |
| [`FIRESTORE_ARCHITECTURE.md`](FIRESTORE_ARCHITECTURE.md) | The `devices/{deviceId}` Firestore data model, Online/Offline detection, security rules, and Cloud Functions backend — the full technical design behind the cloud sync feature. |
| [`WEBSOCKET_API.md`](WEBSOCKET_API.md) | The local `/ws` protocol between the ESP32 and its on-board dashboard: auth handshake, every command and its payload, and every broadcast frame type. |
| [`FIRMWARE_ARCHITECTURE.md`](FIRMWARE_ARCHITECTURE.md) | Firmware internals: dual-core task split, pin-safety layers, calibration bounds, save/crash reliability, sensor auto-disable, and per-sensor Firestore upload gating. |

Each document names the exact source files it describes, so changes to
behavior should update both the code and the relevant doc in the same
change.
