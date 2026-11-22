# HyGrow implementation rules

- Work only on existing `main`. No new branches/worktrees or repository rename.
- Scope is this repository. Do not modify sibling projects, including `HyGrow_Software`.
- Firmware lives in `firmware/`. Preserve existing Firebase latest-sensor payload,
  paths, identity, cadence and behavior. Oracle is an independent parallel uplink.
- Keep the embedded Doctor and SoftAP until native replacements are verified.
- Use small tested versioned commits, push each milestone, and check Actions.
- Run `Dev.ps1 -Help` for supported commands. Firmware target: `esp32-s3-n16r8`.
- Never commit plaintext credentials. The secret mapping is
  `config/secrets.manifest.json`; private age identities are terminal input only.
- Mobile target is Flutter Android/iPhone/iPad; website is cloud only.
- Backend: Fastify 5 + TypeScript + PostgreSQL + Redis; background jobs use BullMQ.
  Keep durable job records in PostgreSQL and secrets out of Redis job payloads/logs.
- Build traceable diagnostics: stable error codes, request IDs, versioned logs,
  health checks and reproducible regression tests. Never swallow failures to pass CI.
- UI should load cached content promptly, refresh in the background with accurate
  freshness labels, provide immediate feedback and respect reduced motion.
- Flutter lives directly in `app/`; deliver Android APK and unsigned iOS device
  release IPA. No desktop app. A separate PC/iPad-landscape web intro comes later.
- App identifier is `dev.vaibhavsh0120.hygrow` on Android/iOS. Use the single
  canonical brand asset in `app/assets/branding/`; do not keep duplicate light/dark
  logo PNGs. Screen artwork belongs under `app/assets/artwork/<feature>/`.
- Follow supplied native screen references. Work outside to inside and complete
  one screen/item with rendered verification before advancing. Use one reusable
  floating liquid-glass navbar with equal selection geometry, spring feedback,
  long-press drag preview and reduced-motion behavior. Doctor has no Pair tab.
- End-user setup offers BLE and automatic LAN discovery, never manual IP entry.
  Account/global settings are not an extra Farmer tab. Avoid developer headers.
- Pairing is BLE/LAN with authorization, never QR. UID alone is not proof.
- Dark surfaces use neutral charcoal. Never send water-level anomaly alerts.
- Fetch current library/platform documentation through Context7 before API work.
- Use Better Design principles before UI styling/behavior and its review tools
  plus rendered spacing evidence before presenting UI. Preserve product styling.
- No feature is working merely because source/workflow exists: run its tests and
  distinguish local, CI, deployed and physical-device evidence.
