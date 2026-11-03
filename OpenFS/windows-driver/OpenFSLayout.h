#pragma once

#include "OpenFSSuperblock.h"

/*
 * Canonical reserved-region layout shared by superblock validation and the
 * kernel bitmap reader. RingEndBlock is exclusive.
 */
typedef struct _OPENFS_VOLUME_LAYOUT {
    ULONGLONG BitmapStartBlock;
    ULONGLONG BitmapBlocks;
    ULONGLONG BitmapEndBlock;
    ULONGLONG JournalStartBlock;
    ULONGLONG CheckpointA;
    ULONGLONG CheckpointB;
    ULONGLONG RingStartBlock;
    ULONGLONG RingEndBlock;
    ULONGLONG DataStartBlock;
    ULONGLONG BackupSuperblockBlock;
} OPENFS_VOLUME_LAYOUT, *POPENFS_VOLUME_LAYOUT;

NTSTATUS
OpenFsCalculateVolumeLayout(
    _In_ ULONG BlockSize,
    _In_ ULONGLONG TotalBlocks,
    _In_ USHORT FormatVersion,
    _Out_ POPENFS_VOLUME_LAYOUT Layout
    );
