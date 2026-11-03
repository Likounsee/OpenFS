#pragma once

/*
 * Portable volume lifecycle state machine shared by the native driver and
 * host tests. Callers must serialize transitions (the future VCB uses its
 * resource for this); this helper deliberately does not provide a lock.
 */
typedef enum _OPENFS_VOLUME_STATE {
    OpenFsVolumeUninitialized = 0,
    OpenFsVolumeProbing,
    OpenFsVolumeInitializing,
    OpenFsVolumeMounted,
    OpenFsVolumeDismounting,
    OpenFsVolumeDead
} OPENFS_VOLUME_STATE;

int
OpenFsVolumeStateCanTransition(
    OPENFS_VOLUME_STATE Current,
    OPENFS_VOLUME_STATE Next
    );

int
OpenFsVolumeStateTryTransition(
    OPENFS_VOLUME_STATE* State,
    OPENFS_VOLUME_STATE Expected,
    OPENFS_VOLUME_STATE Next
    );

/* A private VDO can be destroyed only before VPB ownership or mount. */
int
OpenFsVolumeStateCanDestroy(
    OPENFS_VOLUME_STATE State,
    int HasVpb
    );
