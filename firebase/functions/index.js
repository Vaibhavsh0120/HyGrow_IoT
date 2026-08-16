/**
 * ============================================================================
 * HyGrow IoT — Firebase Functions
 * ============================================================================
 * This is the ONLY backend-side piece of the "devices/{deviceId}" Firestore
 * architecture (see ../../docs/FIRESTORE_ARCHITECTURE.md for the full design).
 * It owns exactly one job: deciding when a device's `status` field flips
 * from "Online" to "Offline". The ESP32 firmware NEVER writes
 * status: "Offline" itself — that decision lives here, and only here, so a
 * device that has simply lost power or Wi-Fi (and therefore can't tell
 * anyone it's down) still eventually gets marked Offline.
 *
 * Two functions:
 *
 *   1. checkDeviceHeartbeats (scheduled, every 1 minute)
 *      Scans every document in the `devices` collection and marks any whose
 *      `lastUpdated` is older than STALE_THRESHOLD_MS as Offline.
 *
 *   2. markDeviceOnlineOnWrite (Firestore trigger, onDocumentWritten)
 *      The instant a device's document is updated by a genuine device
 *      write (i.e. status arrives as "Online" in that write), this is a
 *      no-op fast path — the ESP32 already set status: "Online" itself.
 *      This trigger exists for the OPPOSITE case: if function #1 marked a
 *      device Offline and it then reconnects, the device's own next write
 *      already carries status: "Online" — so in practice #2 has very
 *      little to do. It is kept intentionally minimal (see the comment on
 *      the function itself) rather than removed, so the reasoning for NOT
 *      needing more logic here is on record instead of silently absent.
 *
 * ----------------------------------------------------------------------------
 * IMPORTANT — the 30-second threshold vs. Cloud Scheduler's real floor:
 * ----------------------------------------------------------------------------
 * The product requirement is "Offline within 30 seconds of the last
 * heartbeat". Cloud Scheduler (which drives every Firebase scheduled
 * function) cannot run more often than once per minute — there is no way to
 * get a genuine 30-second polling loop out of `onSchedule` at all; running
 * more often requires a always-on process, which contradicts "serverless
 * backend function" and would cost meaningfully more. This implementation:
 *
 *   - Keeps STALE_THRESHOLD_MS at the requested 30 seconds — a device is
 *     considered stale the instant its lastUpdated is >30s old, exactly as
 *     specified.
 *   - Runs the CHECK every 60 seconds (Cloud Scheduler's floor).
 *
 * Net effect: a device that goes dark is marked Offline somewhere between
 * ~30s and ~90s after its last real heartbeat, depending on exactly where
 * in the 1-minute polling cycle it stopped — not a hard 30s guarantee. This
 * is a real, documented limitation of building this on Cloud
 * Scheduler/onSchedule, not an oversight — see docs/FIRESTORE_ARCHITECTURE.md
 * section 4 for the full discussion and the alternative (a paid always-on
 * instance) if a tighter bound is ever required.
 * ============================================================================
 */

const { initializeApp } = require("firebase-admin/app");
const { getFirestore, FieldValue, Timestamp } = require("firebase-admin/firestore");
const { onSchedule } = require("firebase-functions/scheduler");
const { onDocumentWritten } = require("firebase-functions/firestore");
const { logger } = require("firebase-functions");

initializeApp();
const db = getFirestore();

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
// Matches the ESP32 firmware's contract exactly (see src/core/firebase.cpp):
// a device is "stale" once lastUpdated is more than this many ms old.
const STALE_THRESHOLD_MS = 30 * 1000;

// How often Cloud Scheduler invokes checkDeviceHeartbeats. 1 minute is
// Cloud Scheduler's minimum granularity — see the file header for why this
// can't be tightened to exactly match STALE_THRESHOLD_MS.
const CHECK_SCHEDULE = "every 1 minutes";

const DEVICES_COLLECTION = "devices";

// ---------------------------------------------------------------------------
// 1. checkDeviceHeartbeats — the actual Offline detector
// ---------------------------------------------------------------------------
// Runs on Cloud Scheduler's cadence (CHECK_SCHEDULE). Safe to run with any
// number of devices under devices/{deviceId} — it queries only documents
// that currently claim status == "Online" (so a device already marked
// Offline is never re-examined every tick for no reason), then filters
// those down to the ones whose lastUpdated has actually gone stale, and
// batches every resulting write into a single commit.
exports.checkDeviceHeartbeats = onSchedule(
  {
    schedule: CHECK_SCHEDULE,
    timeZone: "Etc/UTC",
    retryCount: 0, // a missed tick is superseded by the next one 60s later — no need to retry a stale check
  },
  async (event) => {
    const now = Timestamp.now();
    const cutoffMs = now.toMillis() - STALE_THRESHOLD_MS;

    // Only devices currently believed Online are candidates for going
    // stale — this keeps the query (and the write volume) proportional to
    // "devices that are actually up", not the total historical device
    // count, and is what makes this safe to run with many devices present.
    const onlineSnapshot = await db
      .collection(DEVICES_COLLECTION)
      .where("status", "==", "Online")
      .get();

    if (onlineSnapshot.empty) {
      logger.info("checkDeviceHeartbeats: no devices currently Online — nothing to check.");
      return;
    }

    const batch = db.batch();
    let staleCount = 0;

    onlineSnapshot.forEach((doc) => {
      const data = doc.data();
      const lastUpdated = data.lastUpdated; // Firestore Timestamp | undefined

      // A device with no lastUpdated at all (e.g. a document created by
      // something other than the firmware's own commit, or a partially
      // provisioned device) is treated as stale rather than silently
      // skipped — "unknown freshness" should never read as "assumed fresh".
      const lastUpdatedMs = lastUpdated ? lastUpdated.toMillis() : -Infinity;

      if (lastUpdatedMs < cutoffMs) {
        staleCount++;
        batch.update(doc.ref, {
          status: "Offline",
          // lastUpdated is NOT touched here — it must keep reflecting the
          // last time the DEVICE itself actually reported in, so the app
          // can still show "last seen 4 minutes ago" accurately. Only
          // `status` (a backend-owned field) changes in this write.
          offlineDetectedAt: FieldValue.serverTimestamp(),
        });
      }
    });

    if (staleCount === 0) {
      logger.info(
        `checkDeviceHeartbeats: ${onlineSnapshot.size} device(s) Online, all within the ${STALE_THRESHOLD_MS / 1000}s freshness window.`
      );
      return;
    }

    await batch.commit();
    logger.info(
      `checkDeviceHeartbeats: marked ${staleCount} of ${onlineSnapshot.size} previously-Online device(s) as Offline (stale lastUpdated).`
    );
  }
);

// ---------------------------------------------------------------------------
// 2. markDeviceOnlineOnWrite — defensive backstop, NOT the primary path
// ---------------------------------------------------------------------------
// The ESP32 already sets status: "Online" on every single successful
// upload (src/core/firebase.cpp) — that write reaching Firestore at all IS
// the device announcing "I'm alive right now". So in the overwhelmingly
// common case, this trigger fires, sees status is already "Online", and
// does nothing.
//
// It's kept as a real (tiny) function rather than omitted for one reason:
// it's the natural place to clear a stale `offlineDetectedAt` marker left
// behind by checkDeviceHeartbeats once a device is confirmed reporting
// again, so that field never lingers as misleading history after a
// reconnect. Deliberately does NOT flip status itself — the ESP32's own
// write already did that; re-deciding status here would be a second source
// of truth for a field that docs/FIRESTORE_ARCHITECTURE.md section 4
// explicitly says must have exactly one.
exports.markDeviceOnlineOnWrite = onDocumentWritten(
  `${DEVICES_COLLECTION}/{deviceId}`,
  async (event) => {
    const after = event.data && event.data.after;
    if (!after || !after.exists) {
      return; // document deleted — nothing to reconcile
    }

    const data = after.data();
    if (data.status === "Online" && data.offlineDetectedAt) {
      await after.ref.update({ offlineDetectedAt: FieldValue.delete() });
    }
  }
);
