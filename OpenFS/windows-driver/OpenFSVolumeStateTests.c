#include <stdio.h>
#include "OpenFSVolumeState.h"

static unsigned int g_Checks;
static unsigned int g_Failures;

static void
Check(int Condition, const char* Name)
{
    ++g_Checks;
    if (!Condition) {
        fprintf(stderr, "FAIL: %s\n", Name);
        ++g_Failures;
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

    /*
     * Exhaustively verify the transition matrix. This catches accidental
     * lifecycle shortcuts when new states or teardown paths are introduced.
     */
    {
        static const int expected[6][6] = {
            { 0, 1, 0, 0, 0, 0 },
            { 0, 0, 1, 0, 0, 1 },
            { 0, 0, 0, 1, 0, 1 },
            { 0, 0, 0, 0, 1, 0 },
            { 0, 0, 0, 0, 0, 1 },
            { 0, 0, 0, 0, 0, 0 }
        };
        int current;
        int next;
        for (current = OpenFsVolumeUninitialized;
             current <= OpenFsVolumeDead; ++current) {
            for (next = OpenFsVolumeUninitialized;
                 next <= OpenFsVolumeDead; ++next) {
                Check(
                    OpenFsVolumeStateCanTransition(
                        (OPENFS_VOLUME_STATE)current,
                        (OPENFS_VOLUME_STATE)next) ==
                        expected[current][next],
                    "exhaustive lifecycle transition matrix"
                    );
            }
        }
    }

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
    Check(
        !OpenFsVolumeStateCanDestroy(OpenFsVolumeMounted, 0),
        "mounted volume cannot use private destroy"
        );

    state = OpenFsVolumeInitializing;
    Check(
        OpenFsVolumeStateTryTransition(
            &state,
            OpenFsVolumeInitializing,
            OpenFsVolumeMounted) &&
            state == OpenFsVolumeMounted,
        "mount commit transitions initializing -> mounted"
        );
    Check(
        OpenFsVolumeStateTryTransition(
            &state,
            OpenFsVolumeMounted,
            OpenFsVolumeDismounting) &&
            state == OpenFsVolumeDismounting,
        "dismount begins only from mounted"
        );
    Check(
        OpenFsVolumeStateTryTransition(
            &state,
            OpenFsVolumeDismounting,
            OpenFsVolumeDead) &&
            state == OpenFsVolumeDead,
        "dismount teardown reaches dead"
        );


    Check(OpenFsVolumeStateCanLock(OpenFsVolumeMounted, 1, 1, 0, 1, 0),
        "only volume handle can lock mounted volume");
    Check(!OpenFsVolumeStateCanLock(OpenFsVolumeMounted, 2, 1, 0, 1, 0),
        "lock is rejected while another handle is open");
    Check(!OpenFsVolumeStateCanLock(OpenFsVolumeMounted, 1, 0, 0, 1, 0),
        "ordinary file handle cannot lock the volume");
    Check(!OpenFsVolumeStateCanLock(OpenFsVolumeMounted, 1, 1, 1, 1, 0),
        "second lock owner is rejected");
    Check(!OpenFsVolumeStateCanLock(OpenFsVolumeMounted, 1, 1, 0, 0, 0),
        "unmounted VPB cannot be locked");
    Check(!OpenFsVolumeStateCanLock(OpenFsVolumeMounted, 1, 1, 0, 1, 1),
        "remove-pending volume cannot be locked");
    Check(!OpenFsVolumeStateCanLock(OpenFsVolumeInitializing, 1, 1, 0, 1, 0),
        "initializing volume cannot be locked");

    Check(OpenFsVolumeStateCanDismount(OpenFsVolumeMounted, 1, 1, 1, 0),
        "locked sole volume handle can request dismount");
    Check(!OpenFsVolumeStateCanDismount(OpenFsVolumeMounted, 2, 1, 1, 0),
        "dismount rejected while another handle is open");
    Check(!OpenFsVolumeStateCanDismount(OpenFsVolumeMounted, 1, 0, 1, 0),
        "dismount rejected without caller lock ownership");
    Check(!OpenFsVolumeStateCanDismount(OpenFsVolumeMounted, 1, 1, 0, 0),
        "dismount rejected when VPB is not mounted");
    Check(!OpenFsVolumeStateCanDismount(OpenFsVolumeMounted, 1, 1, 1, 1),
        "dismount rejected while removal is pending");
    Check(!OpenFsVolumeStateCanDismount(OpenFsVolumeDismounting, 1, 1, 1, 0),
        "duplicate dismount request is rejected");
    Check(!OpenFsVolumeStateCanDismount(OpenFsVolumeInitializing, 1, 1, 1, 0),
        "unpublished volume cannot be dismounted");

    printf("OpenFS volume-state tests: %u checks, %u failures\n",
        g_Checks, g_Failures);
    return g_Checks == 70 && g_Failures == 0 ? 0 : 1;
}
