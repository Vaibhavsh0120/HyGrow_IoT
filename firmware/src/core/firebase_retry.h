#ifndef HYGROW_FIREBASE_RETRY_H
#define HYGROW_FIREBASE_RETRY_H
// DNS/socket/timeouts, rate limiting and service outages are retryable.
// They must never permanently turn off the user's upload preference.
inline bool firebaseFailureIsPermanent(int httpCode)
{
    return httpCode >= 400 && httpCode < 500 && httpCode != 408 && httpCode != 429;
}
#endif
