#include "OpenFSVolumeState.h"

int
OpenFsVolumeStateCanTransition(
    OPENFS_VOLUME_STATE Current,
    OPENFS_VOLUME_STATE Next
    )
{
    switch (Current) {
    case OpenFsVolumeUninitialized:
        return Next == OpenFsVolumeProbing;

    case OpenFsVolumeProbing:
        return Next == OpenFsVolumeInitializing ||
            Next == OpenFsVolumeDead;

    case OpenFsVolumeInitializing:
        return Next == OpenFsVolumeMounted ||
            Next == OpenFsVolumeDead;

    case OpenFsVolumeMounted:
        return Next == OpenFsVolumeDismounting;

    case OpenFsVolumeDismounting:
        return Next == OpenFsVolumeDead;

    case OpenFsVolumeDead:
    default:
        return 0;
    }
}

int
OpenFsVolumeStateTryTransition(
    OPENFS_VOLUME_STATE* State,
    OPENFS_VOLUME_STATE Expected,
    OPENFS_VOLUME_STATE Next
    )
{
    if (State == 0 || *State != Expected) {
        return 0;
    }

    if (!OpenFsVolumeStateCanTransition(Expected, Next)) {
        return 0;
    }

    *State = Next;
    return 1;
}

int
OpenFsVolumeStateCanDestroy(
    OPENFS_VOLUME_STATE State,
    int HasVpb
    )
{
    return State == OpenFsVolumeInitializing && !HasVpb;
}


int
OpenFsVolumeStateCanLock(
    OPENFS_VOLUME_STATE State,
    unsigned long long TotalOpenHandles,
    int CallerIsVolumeHandle,
    int LockAlreadyHeld,
    int VpbMounted,
    int RemovePending
    )
{
    return State == OpenFsVolumeMounted &&
        TotalOpenHandles == 1 &&
        CallerIsVolumeHandle &&
        !LockAlreadyHeld &&
        VpbMounted &&
        !RemovePending;
}

int
OpenFsVolumeStateCanDismount(
    OPENFS_VOLUME_STATE State,
    unsigned long long TotalOpenHandles,
    int CallerOwnsVolumeLock,
    int VpbMounted,
    int RemovePending
    )
{
    /*
     * Dismount is only admissible after an exclusive lock has excluded new
     * opens and all other file/volume handles have been closed. This is a
     * policy gate, not permission to tear down the VDO from the FSCTL stack.
     */
    return State == OpenFsVolumeMounted &&
        TotalOpenHandles == 1 &&
        CallerOwnsVolumeLock &&
        VpbMounted &&
        !RemovePending;
}
