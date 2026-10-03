# HyGrow IoT audit for v3.1.4

Reviewed on 2026-10-03. Scope: the complete `HyGrow_IoT` working tree, including the uncommitted local discovery, shared telemetry, and Firebase-worker implementation. Base commit: `cc28292667c103ed91e336a14f28e093e68666a6`. Mobile application and AI code were excluded.

This audit includes bounded firmware and embedded-dashboard repairs. Firmware, asset cache versions and the app handoff now identify **3.1.4**; telemetry schema and discovery protocol remain **1**. The original reproductions below are retained for traceability. Use the disposition table to distinguish repaired findings from open work. Hardware effects and provider behavior have not been demonstrated on a connected board.

The local-first direction is sound: one completed sensor snapshot feeds `/status`, dashboard WebSocket data, and Firestore; cloud HTTP runs on a separate worker; pending cloud work stays bounded during outages. The main remaining architectural problem is that configuration, authentication, persistence, and client lifetimes still have several task owners. Fix the P1 findings before treating this release as ready for deployment.

## Current disposition

| Findings | Status | Result / remaining work |
| --- | --- | --- |
| F04, F05, F09, F11–F15, F22–F24 | Repaired for the described reproductions | Independent application dialogs with focus/Escape handling; raw-voltage pH calibration and finite coefficient bounds; canonical sample freshness, simulation provenance and aligned nullable history/CSV; submitted-versus-editable save state; rejected-reset session recovery; current restart reason; consistent 3.1.4 metadata and accessible control names. |
| F01 | Partially repaired | Real driver initialization now checks the sensor's enabled flag, preventing boot-disabled drivers from touching their rejected pins. Complete active pin-set/pair/duplicate validation and a boot GPIO-call harness remain. |
| F02 | Power-pulse defect repaired; wider architecture open | Water-level signal and power pins belong to its initialized driver until reboot. A settings/demo change during or between reads cannot redirect either GPIO or strand the old power pin HIGH. Other drivers and sample provenance still need a coherent immutable active configuration. |
| F03, F06–F08, F10, F16–F21 | Open | Safe client/auth ownership, Firebase certificate verification, demo pin mirrors, DHT readiness/recovery, checked persistence, cloud readiness/generations, fragmented input, pre-auth capacity, backpressure and discovery fairness. |

Terminal improvements retain the existing fonts, icon family and themes: readable uptime/severity/source rows, search and level filters, bounded repeat grouping and replay deduplication, device sequence ordering, buffered Pause/Resume, HTTP clipboard fallback and mobile wrapping. The firmware log ring is protected by a short lock and replays 40 entries in one heap-backed batch instead of filling a 32-message client queue with 40 separate frames. The browser keeps at most 200 grouped rows from 800 events. Core task/client ownership F03 remains open.

The inactive mobile sidebar path was removed from HTML/JS/CSS. The pH wrapper now uses its explicit coefficient parameters. Notice banners occupy layout space rather than covering page headings; compact pin/toggle controls have larger touch areas and visible focus. Existing authenticated credential-display behavior was preserved.

## Priority definitions

- **P1:** can affect GPIO control, crash normal concurrent operation, expose cloud credentials, or prevent a core user flow.
- **P2:** incorrect data, lost state, misleading success, or reproducible reliability/interaction failures.
- **Recommendation:** a design or maintenance gap; physical performance or security consequences may need further validation.

## Original P1 findings and repair directions

### F01 — Disabled sensors initialize forbidden GPIOs (partially repaired)

**Evidence:** [boot pin protection](../HyGrow_IoT.ino#L70), [driver initialization](../src/core/task_sensor.cpp#L364), [water-level GPIO setup](../src/sensors/sensor_water_level.cpp#L31).

Boot disables a sensor with an invalid GPIO but leaves its pin assignment available for correction. Whenever any other real sensor is enabled, initialization still invokes every non-demo driver without checking that driver's enabled state. Example: old NVS contains water-level power GPIO19. Boot disables water level to protect native USB, then its driver still configures GPIO19 as an output and drives it low.

**Repair:** validate the complete active pin set at boot, including duplicates. Initialize a driver only when enabled and all required pins are usable. A disabled or invalid sensor must never touch its pins. Add a regression that records GPIO calls with a forbidden disabled sensor and another enabled sensor.

### F02 — A settings change can leave water-level probe power HIGH (pulse repaired)

**Evidence:** [pin mutation](../src/core/command_handlers.cpp#L366), [power-on and sample loop](../src/sensors/sensor_water_level.cpp#L64), [power-off](../src/sensors/sensor_water_level.cpp#L76), [snapshot publication](../src/core/telemetry.cpp#L42).

The water-level reader raises `currentConfig.pin_wl_power`, waits and samples for about 66 ms, then reads that mutable field again to lower the output. An async pin save changing GPIO5 to GPIO10 during the pulse lowers GPIO10 and leaves GPIO5 high. Demo transitions can replace the field with `-42` during the same window. Physical behavior has not been tested; the mismatched writes are established by the code.

The wider problem is that reboot-required saves immediately change live configuration. Analog drivers consult new pins while DHT/DS18B20 objects retain their initialization pins. A snapshot mutex protects completed telemetry, not these live configuration reads or the provenance of a cycle.

**Repair:** separate persisted/pending configuration from active sensor configuration. The sensor task owns the latter, and reboot-required changes cannot alter it until initialization. Capture both water-level GPIOs at sample entry and guarantee that power-off uses the captured power pin on every exit. Capture calibration and simulation settings consistently for the full cycle.

### F03 — WebSocket clients and authentication are accessed across tasks without safe ownership

**Evidence:** [raw client traversal](../src/core/websocket.cpp#L25), [authentication set](../src/core/auth.cpp#L26), [membership operations](../src/core/auth.cpp#L73), [session reset](../src/core/auth.cpp#L118), [cloud result client lookup](../src/core/firebase.cpp#L561).

Network broadcasts, sensor logs, and cloud logs iterate the raw client list while async callbacks and cleanup can remove clients. The installed ESPAsyncWebServer 3.12.1 returns an unlocked list from `getClients()` while its own mutations use an internal client-list lock. Concurrent removal can invalidate an iterator/reference. The application authentication `std::set` is also read, erased, inserted, and cleared from different tasks without synchronization. A client pointer obtained for a cloud result can disappear after lookup.

**Repair:** give application commands/auth state one owner and queue cross-task log/result events using client IDs. Synchronize membership and copy authenticated IDs safely; send through the pinned library's locked `ws.text(id, payload)` API instead of traversing raw client objects. Do not retain raw client pointers across work or rely on a pointer after an unlocked lookup. Test simultaneous disconnect, broadcast, logout/password reset, and cloud completion on a board.

### F04 — Application dialogs stay hidden after login (repaired)

**Evidence:** [authentication overlay](../data/index.html#L99), [nested reboot dialog](../data/index.html#L189), [nested generic dialogs](../data/index.html#L208), [parent hide](../data/js/app.js#L593), [reboot dialog opener](../data/js/app.js#L671), [alert opener](../data/js/app.js#L702).

All reboot, alert, confirmation, and typed-reset dialogs are descendants of `#auth-overlay`. Successful authentication hides that parent. Opening an application dialog unhides only its child; CSS still hides the entire ancestor subtree. Logout, manual reboot, factory reset, pin reset, post-save reboot prompts, and many errors therefore cannot appear after login.

**Repair:** place application dialogs in an independent overlay or use one controller that manages the parent and every panel correctly. Include focus entry, Escape/cancel behavior, and focus restoration. Test against the actual HTML hierarchy; the existing dashboard test's independent mock elements cannot detect a hidden ancestor. HTML parsing of the current file confirmed this nesting.

### F05 — First-use pH calibration cannot obtain distinct raw readings (repaired)

**Evidence:** [default coefficients](../config.h#L147), [clamped reading](../src/sensors/sensor_ph.cpp#L38), [pH 7 capture](../data/js/app.js#L2699), [pH 4 capture and equality rejection](../data/js/app.js#L2714).

Fresh coefficients are offset `0` and slope `-5.70`. Every positive ADC voltage produces negative pH and is clamped to zero. The wizard tries to reconstruct raw voltage from that clamped displayed pH, so both captures become zero and the two-point fit is rejected. Clamping destroys the information needed to calibrate.

**Repair:** expose/capture actual calibrated ADC millivolts independently of the displayed pH, preferably through an authenticated calibration command with a sample identity and freshness check. Fit the coefficients from two raw buffer captures. Mark uncalibrated state explicitly and test calibration from factory defaults.

### F06 — Firebase TLS does not verify server identity

**Evidence:** [manual sign-in](../src/core/firebase.cpp#L188), [manual Firestore test](../src/core/firebase.cpp#L256), [worker sign-in](../src/core/firebase.cpp#L318), [upload](../src/core/firebase.cpp#L439).

All four HTTPS paths call `setInsecure()`. Encryption without certificate verification allows an active network attacker to impersonate the endpoint, obtain account credentials/tokens, or forge a successful upload result. The installed Arduino ESP32 2.0.17 supports `setCACert()` and `setCACertBundle()`; its implementation switches off insecure mode when a CA is set.

**Repair:** use an appropriate maintained CA/bundle, hostname verification, and valid time for certificate checks. Keep time/cloud failures isolated from local sensing and discovery. Test valid certificates and rejected untrusted/expired/wrong-host certificates. This is separate from the deliberately authenticated password-display policy.

## Original P2 findings and repair directions

| ID | Finding and reproduction | Source | Repair direction |
| --- | --- | --- | --- |
| F07 | Individual Demo ON → global Demo ON overwrites the saved real GPIO with `-42`; global OFF and individual OFF then restore `-42`. Conversely, individual Demo OFF while global mode remains ON is acknowledged, but boot forces all pins back to demo. | [global mirror overwrite](../src/core/command_handlers.cpp#L515), [restore](../src/core/command_handlers.cpp#L546), [individual policy](../src/core/command_handlers.cpp#L580), [boot override](../src/core/state.cpp#L420) | Store real GPIOs independently of simulation mode. Define and persist one explicit policy for global versus individual overrides. Migrate/validate existing mirrors. |
| F08 | DHT startup makes five attempts 250 ms apart. Installed DHT 1.4.7 caches even failed reads for 2,000 ms, so a first failure makes the remaining retries return the same failure. The sensor is then persistently disabled despite the log saying “for this session.” | [retry constants](../src/core/task_sensor.cpp#L45), [retry loop](../src/core/task_sensor.cpp#L305), [persistent disable](../src/core/task_sensor.cpp#L234) | Use sensor-specific readiness/retry timing. Keep user enabled preference separate from runtime failure state; retry recovery without rewriting that preference. |
| F09 | pH validation treats the fitted intercept as a pH reading and limits it to ±14. A mathematically valid pair, pH 7 at 1.5 V and pH 4 at 2.026 V, gives slope about −5.70 and intercept about 15.55 and is rejected. These are illustrative inputs, not measured probe results. | [coefficient validation](../src/core/command_handlers.cpp#L179) | Validate raw buffer voltages, sufficient separation, finite coefficients, fit behavior, and supported probe range; do not impose the displayed pH range on an extrapolated intercept. |
| F10 | Password/token writes ignore persistence failure yet setup/change reports success. Ordinary config commands mutate RAM before storage; on failure, calibration/pins/demo can remain applied although the dashboard was told the save failed. Empty-string writes can also mask failure through `putString(...) > 0 || empty`. | [token write](../src/core/state.cpp#L568), [password write](../src/core/state.cpp#L656), [success acknowledgement](../src/core/auth.cpp#L315), [config writes](../src/core/state.cpp#L460), [calibration failure path](../src/core/command_handlers.cpp#L410) | Return checked persistence outcomes; retain old active/auth state on failure. Stage changes, persist one consistent version, then publish it. Serialize saves and resets. |
| F11 | Dashboard ignores `sampleSequence`, `bootId`, and `sampleAgeMs`. Default 1 s pushes of 2 s samples append duplicates. A stalled sensor task can continue showing old values as live while network frames arrive; calibration remains available. | [snapshot broadcast](../src/core/task_network.cpp#L247), [status](../data/js/app.js#L1263), [history append](../data/js/app.js#L1402) | Deduplicate complete samples by boot/sequence and gate freshness separately from socket connectivity. Use the policy already documented in the [app handoff](APP_LOCAL_TELEMETRY_HANDOFF.md#3-transport-and-freshness-behavior-to-implement). |
| F12 | Simulation is derived from current config pins, not the sample's `sensorStatus.*.simulated`. Turning demo off can relabel a cached simulated value as real before a real sample exists. History persists across boots/mode changes without a provenance boundary. | [status derivation](../data/js/app.js#L1272), [config update](../data/js/app.js#L1816), [cached rerender](../data/js/app.js#L1870), [sample provenance](../src/core/telemetry_json.cpp#L53) | Use sample provenance and require a fresh real sample for calibration. Annotate or separate history across boot/config/mode boundaries. |
| F13 | Sensor buffers independently skip failures; CSV combines equal indices as if they were simultaneous and inserts zero for missing values. TDS being disabled/empty blocks all-sensor export. | [independent buffers](../data/js/app.js#L1402), [TDS-only gate](../data/js/app.js#L2971), [zero-filled export](../data/js/app.js#L2973) | Keep one sample-row ring with IDs/time, nullable fields, health, and simulation metadata. Export missing readings as blanks and permit export when any sample is available. |
| F14 | A second edit during a pending save can be marked saved without being sent. Completion copies the current editable fields, rather than the submitted payload, into confirmed state; dirty handlers can re-enable Save during submission. | [pin payload](../data/js/app.js#L2502), [confirmation from DOM](../data/js/app.js#L2540), [demo save](../data/js/app.js#L3178) | Confirm the submitted version or returned device config. Preserve later edits and include an explicit submitting state in Save gating. |
| F15 | An explicitly rejected pin reset clears the restart spinner and paints the link live, but `deviceAuthenticated` remains false. The existing socket stays open; readings/actions remain offline until reconnect. Hidden confirmation F04 currently masks ordinary reachability. | [restart auth clearing](../data/js/app.js#L798), [rejection handler](../data/js/app.js#L3070) | Restore the known session on explicit rejection or complete a new auth handshake, then refresh status. |
| F16 | Firebase “Test Connection” only performs sign-in plus GET, accepting every 404. It can succeed when uploads are forbidden or the requested database/resource is missing; its comments promise more than it verifies. | [GET](../src/core/firebase.cpp#L247), [404 success](../src/core/firebase.cpp#L276) | Distinguish sign-in, document read, and confirmed upload. Inspect structured 404 errors. A real successful commit is the evidence of write readiness; do not silently write a test document. |
| F17 | Cloud readiness is not tied to settings generation. Saving different credentials preserves old readiness, and an old in-flight commit can mark the new configuration ready or replace its error state. | [settings save](../src/core/firebase.cpp#L126), [ready getter](../src/core/firebase.cpp#L106), [commit completion](../src/core/firebase.cpp#L458) | Clear readiness on settings changes, record attempted/successful generation, and ignore obsolete completion status. |
| F18 | A valid WebSocket command delivered in several callbacks is silently ignored because the dispatcher requires an entire frame in one callback. The installed library delivers TCP segments separately. | [single-callback condition](../src/core/websocket.cpp#L61) | Bounded per-client message reassembly with a deadline, size limit, and explicit rejection of unsupported framing. Add split-frame/split-TCP tests. |
| F19 | Unauthenticated clients have no auth deadline. Installed library cleanup limits ESP32 clients to eight and closes the oldest, so nine pre-auth connections can evict an active authenticated dashboard. | [connect handler](../src/core/websocket.cpp#L36), [cleanup](../src/core/task_network.cpp#L118) | Limit pre-auth slots and enforce an auth deadline; reject excess newcomers without displacing an authenticated session. |
| F20 | Installed WebSocket data queues cap at 32 messages and drop new messages when full by default. Ignored send results mean a slow browser can lose command acknowledgements as well as telemetry. | [unchecked send](../src/core/websocket.cpp#L29), [command acknowledgement](../src/core/command_handlers.cpp#L195) | Coalesce superseded telemetry; preserve bounded command/result handling and explicitly close/recover slow clients. Use request IDs so retries and timeouts are unambiguous. |
| F21 | Discovery consumes its global 200 ms budget before parsing and device filtering. A malformed packet or a request for another device can suppress a valid matching discovery immediately afterward. | [limiter ordering](../src/core/local_network.cpp#L31), [limiter](../src/core/local_protocol.cpp#L86) | Separate cheap inbound abuse control from valid-reply budgeting; parse/filter before consuming a reply token and allow a bounded burst or peer-aware fairness. |
| F22 | Web terminal says “Previous boot ended with” the saved reason for why that previous boot started. After POWERON then PANIC, the terminal can report POWERON while the actual PANIC appears only in current Serial output. | [current reset reason](../HyGrow_IoT.ino#L250), [saved reason label](../HyGrow_IoT.ino#L269) | Put the current `esp_reset_reason()` into the web backlog; label saved history as the previous startup cause. |
| F23 | Release metadata diverges: intended 3.1.4, telemetry/discovery/cloud 1.2.0, dashboard cache query 1.1.3, and handoff examples 1.2.0. No local Git release tags were found. | [firmware version](../src/core/telemetry.h#L7), [asset version](../data/index.html#L79), [handoff](APP_LOCAL_TELEMETRY_HANDOFF.md#L3) | Use one release version source, generate firmware/docs/cache metadata, and independently retain schema/protocol version 1 unless their compatibility changes. |
| F24 | Important pin fields, enabled/demo toggles, and typed-reset input lack associated accessible names. Adjacent text is not programmatically linked to the relevant input. | [reset input](../data/index.html#L246), [sensor toggles](../data/index.html#L478), [pin controls](../data/index.html#L1049) | Connect existing labels with `for`/`id` or `aria-labelledby` and make toggle names sensor-specific. Test keyboard and accessibility-tree behavior. |

Firestore read and write permissions are separate operations; GET cannot establish upload permission. This was checked using current Context7 documentation and [Firebase's granular rules documentation](https://firebase.google.com/docs/firestore/security/rules-structure). TLS recommendations were checked against current Espressif documentation and the actual pinned core rather than assuming newer 3.x APIs.

## Recommended architecture

Keep the current task split and shared telemetry serializer. A large rewrite or extra framework is unnecessary. Make ownership and state transitions explicit:

| Component | Owns | Communicates through |
| --- | --- | --- |
| Command/config owner | Validated config versions, authentication membership, persistence, command outcomes | Bounded command/event queues; client IDs and request IDs |
| Sensor task | Initialized drivers, immutable active hardware config, runtime sensor health, complete sample publication | Configuration applied at a safe boundary/reboot; immutable telemetry snapshots |
| Local network adapters | Discovery parsing/replies and read-only `/status` | Snapshot copies; bounded protocol/input policies |
| Dashboard transport | Safe ID-based client sends and bounded outbound policy | Latest telemetry coalescing; command results with correlation |
| Firebase worker | Credential snapshot/generation, token, retry state, HTTPS lifecycle | Latest-state request notification; result/status events; no direct config saves or raw client traversal |
| Dashboard UI | Submitted versus editable config, sample history, modal/auth flow | Canonical sample metadata and versioned command outcomes |

### Changes worth making after the immediate repairs

1. **Separate user preference, active state, and health.** `enabled`, runtime `ready/failing`, simulated provenance, and pending reboot are different facts. GPIO values should always be GPIO values; simulation should be a separate enum/flag. A transient missing sensor should not permanently rewrite the user's enabled preference.
2. **Serialize and version persistence.** Current `state_save()` writes many independent keys from mutable globals, while commands, sensor startup, cloud auto-disable, and BOOT reset can all call storage routines. Prefer one checked, versioned configuration record with migration and a serialized writer. A save should return the persisted version and whether reboot is required. Keep authentication storage separation for auth-only reset, but give it the same checked lifecycle.
3. **Start sensing independently of dashboard assets.** LittleFS failure currently halts sensor, network, discovery, cloud, and the BOOT watcher before they start ([halt](../HyGrow_IoT.ino#L307)). Preserve the failed filesystem for diagnosis; continue the valid sensor/local JSON services and physical recovery, with a small built-in error page instead of depending on LittleFS for all recovery.
4. **Use Wi-Fi events and explicit network states.** SoftAP fallback is boot-only. After a successful STA-only boot loses Wi-Fi, vitals still call it `ap_mode` although no AP exists ([status](../src/core/task_network.cpp#L161)). Track STA connected/reconnecting, AP active, and cloud health separately. Add runtime fallback only as an explicit recovery policy, rather than assuming no STA means AP.
5. **Make sensor lifecycle asynchronous where useful.** Use sensor-specific warm-up/retry timing and bounded recovery. Check BH1750 first-conversion readiness before accepting its startup sample; its pinned library exposes the timing. Avoid repeated blocking DS18B20 conversion work where scheduling can preserve the same cadence. Measure before changing timing.
6. **Make calibration a raw-data operation.** Capture raw ADC readings, sample identity, coefficients/config version, and buffer label on the sensor owner. Do not reconstruct raw values from clipped values or accept stale/simulated samples. Water-level percentage currently maps voltage to 3,300 mV rather than measured dry/full probe endpoints ([mapping](../src/sensors/sensor_water_level.cpp#L103)); actual accuracy/saturation requires bench calibration.
7. **Add command correlation and bounded transport behavior.** Auth/command deadlines, maximum inbound bytes/nesting, pre-auth capacity, reassembly timeout, outbound backpressure, and command IDs should be explicit. Currently parsing happens before the auth gate and there is no application-level general command rate limit. Do not call the current data queues unbounded: the installed library caps them.
8. **Measure runtime cost before reducing stacks or tuning CPU.** Static RAM/flash size does not measure TLS heap peaks, task stack headroom, slow-client buffers, or long-term fragmentation. Export per-task stack high-water marks, maximum sample duration, snapshot age, minimum heap, reconnect counts, cloud retries, and dropped messages. Use those measurements to tune task cadence/stack size. Check task-creation results; the three sketch task creations currently ignore them ([creation](../HyGrow_IoT.ino#L336)).
9. **Automate release checks.** Add one fail-fast IoT check runner and CI with harmless offline configuration. Generate/verify gzip assets as part of packaging, compare the packaged file list, and check release metadata consistently. Current assets are correct, but running compression remains a separate manual step.

## Deliberate policies and remaining capabilities

- `/status` and discovery are public read-only LAN services. This matches the requested local behavior; discovery is not device authentication. Same-subnet validation, bounded request size, echoed nonce, and unicast replies are useful, but a local announcement should not be treated as cryptographic identity.
- Authenticated dashboard clients intentionally receive plaintext admin/Wi-Fi/AP/Firebase credentials, and the dashboard uses `ws://`. Preserve that chosen behavior unless the trust model is redesigned; document that an authenticated browser and the LAN transport are trusted. The comments now acknowledge that `webLog()` stores the boot message for authenticated backlog replay ([boot comment](../HyGrow_IoT.ino#L290), [config credentials](../src/core/task_network.cpp#L195)).
- Cloud uploads deliberately keep latest state, not history. Do not add a backlog unless requirements change. Consumers must derive offline/stale state from freshness; `status="Online"` alone is not sufficient.
- Changing a saved secured network to an open SSID has no explicit password-clear action. Blank currently means “keep saved password” ([UI](../data/js/app.js#L2313), [firmware](../src/core/command_handlers.cpp#L243)). Add an explicit clearing option if open networks are supported; this is a capability gap rather than a reason to change blank semantics silently.
- Analog input health does not prove a probe is connected. The pinned ADC implementation returns unsigned zero for an invalid pin; the water-level negative-value check cannot detect that error, while zero is intentionally a valid dry reading. Report these limits instead of inventing a disconnect heuristic that rejects real zero measurements.
- This review does not establish live Firebase rules compliance, mobile permission support, Wi-Fi reconnect behavior, sensor accuracy, or physical pin behavior. Those need the acceptance tests below.

## Dead code and maintenance cleanup

| Candidate | Evidence | Suggested action |
| --- | --- | --- |
| Inert mobile menu path | Removed from HTML, JS and CSS | Current bottom navigation and desktop sidebar verified in the browser. |
| Unused radio/heap/IP fields in `VitalsState` | [fields](../src/core/state.h#L119); broadcasts query Wi-Fi/ESP directly instead | Remove the unused fields or populate a coherent vitals snapshot with a clear owner. |
| Unobserved task handles | [task creation outputs](../HyGrow_IoT.ino#L336) | Prefer using them for creation checks/diagnostics; otherwise pass null for unused outputs. |
| Ignored pH API parameters | Repaired in [sensor_ph.cpp](../src/sensors/sensor_ph.cpp) | Explicit offset/slope parameters now drive the calculation; raw ADC voltage is retained independently. |
| Redundant unreachable fallbacks | [network FS-failure branch](../src/core/task_network.cpp#L89), [second reset-sensor fallback](../src/core/command_handlers.cpp#L977), [post-factory-reset release branch](../HyGrow_IoT.ino#L160) | Simplify after lifecycle repairs and keep one explicit error path. |
| Stale narrative comments | Dead mobile-menu/calibration comments and credential-display claims corrected | Other lifecycle/initializer/platform-size comments remain cleanup candidates; retain useful wiring constraints. |

Not dead: `s_kick` is used for the immediate first complete sensor cycle. Driver wrappers are called. No unused font/icon file or stale compressed asset was established; all manifest icons exist and the six fonts are referenced. Do not delete assets based on an incomplete file inventory.

## Validation completed

| Check | Result | What it establishes |
| --- | --- | --- |
| PlatformIO firmware build, `esp32-s3-n16r8` | PASS; static RAM **55,308 / 327,680 bytes (16.9%)**, app flash **1,128,501 / 3,145,728 bytes (35.9%)** | Current firmware compiles/links for ESP32-S3 N16R8. |
| PlatformIO LittleFS build and image listing | PASS; all **25 files** present | Includes both new JS modules, seven gzip files, local fonts, icons and manifest. |
| `tools/test-web-assets.py` | PASS; source asset tree **704,199 bytes** | Local links/manifest/fonts exist; HTML IDs are unique; all seven gzip copies match; firmware/cache/handoff versions agree. |
| `tools/test-local-telemetry.py` | PASS | Production snapshot publication and local/dashboard/Firestore parity, null/provenance semantics, raw pH voltage, protocol and retry helpers. |
| `tools/test-local-network.py` | PASS | Mocked HTTP/cache/CORS, UDP binding recovery, STA/AP replies, DHCP changes, filtering and budgets. |
| `tools/test-cloud-worker.py` | PASS | Mocked coalescing, cached auth, outage backoff/recovery, covered credential races, auto-disable and manual-test results. |
| `tools/test-water-level.py` | PASS | Production reader with ADC/Arduino stubs: settling, glitches, real changes, bounded pulses, and pin/demo changes during and between reads. The new mutable-pin case failed before the repair. |
| `tools/test-dashboard-flows.js` | PASS | Auth, restart, pin/staging and sensor-state host checks. |
| `tools/test-terminal-flows.js` | PASS | Bounded retention, replay deduplication/ordering, repeat grouping, filters, text formatting and uptime beyond 24 hours. |
| `tools/test-telemetry-view.js` | PASS | Cycle deduplication/order, freshness, boot/simulation boundaries, aligned null CSV and raw pH fitting. |
| `tools/test-dashboard-browser.js` | PASS, real DOM in headless Edge/Playwright with mocked WebSocket | Terminal controls and HTTP copy; four dialogs with keyboard/focus/typed guard; all ten tabs in dark/light at 390/1440/1728; missing readings; CSV; pH wizard from clamped defaults; stale/demo gating; edits during pending saves; reset rejection; named form controls; notice placement. |
| JavaScript syntax and Git whitespace checks | PASS | All four dashboard modules parse; no diff whitespace errors. |

Dependencies inspected: Arduino ESP32 **2.0.17**, ESPAsyncWebServer **3.12.1**, AsyncTCP **3.5.0**, ArduinoJson **7.4.3**, DHT **1.4.7**, DallasTemperature **4.0.6**, BH1750 **1.3.0**. Context7 was consulted for library/API behavior; actual pinned source decides version-specific claims. Independent source reviews covered lifecycle/cloud, sensors and dashboard. Follow-up reviewers identified the nullable-buffer tab crash and cross-core log ordering, which were repaired and regression checked; further delegated review was unavailable after the tool reported a usage limit.

Better Design UI/UX, review and copy guidance were used. Local screenshots were inspected and terminal DOM spacing was measured at all three widths in both themes. This is local browser evidence, not a public HTTPS design-approval receipt or a claim of full WCAG/device-browser certification.

Host tests stub network/RTOS/hardware behavior. They do not establish physical sensor accuracy, GPIO electrical behavior, real concurrent disconnects, TLS/provider trust, split-frame recovery or radio performance. General active-config mutation still needs repair despite the water-level pulse fix. No board was flashed, no live Firebase request was sent, and no commit/push was made.

## Repair order and acceptance criteria

1. **Finish GPIO/config and task/client ownership:** open parts of F01–F03. Regression tests must demonstrate no forbidden-pin GPIO call, same captured pin HIGH/LOW under concurrent saves, and safe broadcasts/auth resets with disconnects. Keep pending settings out of current sensor cycles.
2. **Finish remaining sensor/persistence behavior:** F07, F08 and F10. Retain the new regression coverage for repaired F04–F05, F09, F11–F15 and F24. Use actual DOM/browser tests for authenticated dialogs, failed reset recovery, edits during pending saves, stale/simulated calibration gating, and a pH wizard starting from default config. History/CSV should contain aligned nullable sample rows.
3. **Finish cloud trust and generation semantics:** F06, F16–F17. Exercise valid/rejected certificates, read-allowed/write-denied rules, missing database, credential changes during sign-in and commit, WAN loss, and recovery. Local `/status`/UDP/dashboard must remain responsive during those failures.
4. **Harden protocol/recovery and diagnostics:** F18–F21 plus lifecycle recommendations. F22 is repaired. Test fragmented input, slow clients, pre-auth excess, malformed/foreign-device discovery bursts, runtime router reconnect, AP+STA, DHCP address change, missing LittleFS, and physical BOOT recovery.
5. **Complete release/device validation:** F23 metadata now agrees and packaging checks pass. Add automatic checks and a documented board soak. Record runtime heap/stack/sample-age measurements; static build percentages alone do not prove efficiency or long-term reliability.

The implementation handoff for the app developer remains [APP_LOCAL_TELEMETRY_HANDOFF.md](APP_LOCAL_TELEMETRY_HANDOFF.md). It now matches 3.1.4 and documents the additive raw pH field; the IoT dashboard follows the same boot/sequence/freshness/null/provenance contract. No app-side implementation is included in this audit.
