#ifndef SAFEPULSE_IDENTITY_H
#define SAFEPULSE_IDENTITY_H

/* Private guest identities; these must match the image startup configuration. */
#define SAFEPULSE_UID_RESMGR   420
#define SAFEPULSE_UID_WATCHER  421
#define SAFEPULSE_UID_BRIDGE   422
/* ECU processes share the bridge UID so replay can pause them without an
 * unrestricted cross-user signal ability; their policy types stay separate. */
#define SAFEPULSE_UID_STEERING 422
#define SAFEPULSE_UID_BRAKING  422
#define SAFEPULSE_UID_POWER    422
#define SAFEPULSE_UID_READER   426
#define SAFEPULSE_UID_ROGUE    427
#define SAFEPULSE_GID_DEVICE   428
#define SAFEPULSE_GID_ROGUE    429

#endif
