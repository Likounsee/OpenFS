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
