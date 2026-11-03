#include <stdio.h>
#include "OpenFSVolumeState.h"

static unsigned int g_Checks;

static void
Check(int Condition, const char* Name)
{
    ++g_Checks;
    if (!Condition) {
        fprintf(stderr, "FAIL: %s\n", Name);
    }
}

int
main(void)
{
    OPENFS_VOLUME_STATE state;

    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeUninitialized,
            OpenFsVolumeProbing),
        "uninitialized -> probing"
        );
    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeProbing,
            OpenFsVolumeInitializing),
        "probing -> initializing"
        );
    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeProbing,
            OpenFsVolumeDead),
        "probing -> dead on probe failure"
        );
    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeInitializing,
            OpenFsVolumeMounted),
        "initializing -> mounted"
        );
    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeInitializing,
            OpenFsVolumeDead),
        "initializing -> dead on rollback"
        );
    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeMounted,
            OpenFsVolumeDismounting),
        "mounted -> dismounting"
        );
    Check(
        OpenFsVolumeStateCanTransition(
            OpenFsVolumeDismounting,
            OpenFsVolumeDead),
        "dismounting -> dead"
        );
    Check(
        !OpenFsVolumeStateCanTransition(
            OpenFsVolumeMounted,
            OpenFsVolumeProbing),
        "mounted cannot return to probing"
        );
    Check(
        !OpenFsVolumeStateCanTransition(
            OpenFsVolumeDead,
            OpenFsVolumeMounted),
        "dead cannot be resurrected"
        );

    state = OpenFsVolumeUninitialized;
    Check(
        OpenFsVolumeStateTryTransition(
            &state,
            OpenFsVolumeUninitialized,
            OpenFsVolumeProbing) &&
            state == OpenFsVolumeProbing,
        "transition updates state"
        );
    Check(
        !OpenFsVolumeStateTryTransition(
            &state,
            OpenFsVolumeUninitialized,
            OpenFsVolumeInitializing) &&
            state == OpenFsVolumeProbing,
        "stale expected state is rejected without mutation"
        );
    Check(
        !OpenFsVolumeStateTryTransition(
            &state,
            OpenFsVolumeProbing,
            OpenFsVolumeMounted) &&
            state == OpenFsVolumeProbing,
        "illegal transition is rejected without mutation"
        );
    Check(
        !OpenFsVolumeStateTryTransition(
            0,
            OpenFsVolumeProbing,
            OpenFsVolumeInitializing),
        "null state pointer is rejected"
        );

    Check(
        OpenFsVolumeStateCanDestroy(OpenFsVolumeInitializing, 0),
        "private initializing volume can be destroyed"
        );
    Check(
        !OpenFsVolumeStateCanDestroy(OpenFsVolumeInitializing, 1),
        "volume with VPB association cannot use private destroy"
        );
    Check(
        !OpenFsVolumeStateCanDestroy(OpenFsVolumeDead, 0),
        "dead volume cannot be destroyed twice"
        );

    printf("OpenFS volume-state tests: %u checks\n", g_Checks);
    return g_Checks == 16 ? 0 : 1;
}
