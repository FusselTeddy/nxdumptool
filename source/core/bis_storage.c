/*
 * bis_storage.c
 *
 * Copyright (c) 2020-2024, DarkMatterCore <pabloacurielz@gmail.com>.
 *
 * This file is part of nxdumptool (https://github.com/DarkMatterCore/nxdumptool).
 *
 * nxdumptool is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * nxdumptool is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <core/nxdt_utils.h>
#include <core/bis_storage.h>
#include <core/devoptab/nxdt_devoptab.h>

#define BIS_STORAGE_INDEX(type)         ((type) - FsBisPartitionId_CalibrationFile)
#define BIS_STORAGE_FATFS_CTX(type)     g_bisStorageContexts[BIS_STORAGE_INDEX(type)]
#define BIS_STORAGE_MOUNT_NAME(type)    VolumeStr[BIS_STORAGE_INDEX(type)]

/* Type definitions. */

typedef struct {
    u8 bis_partition_id;    ///< FsBisPartitionId.
    FsStorage bis_storage;
    FATFS fatfs;
} BisStorageFatFsContext;

/* Global variables. */

static Mutex g_bisStorageMutex = 0;

static BisStorageFatFsContext *g_bisStorageContexts[FF_VOLUMES] = {0};

/// Required by FatFs.
const char *VolumeStr[FF_VOLUMES] = {
    "prodinfof",
    "safe",
    "user",
    "system",
};

/* Function prototypes. */

static void _bisStorageUnmountPartition(u8 bis_partition_id);

static BisStorageFatFsContext *bisStorageInitializeFatFsContext(u8 bis_partition_id);
static void bisStorageFreeFatFsContext(BisStorageFatFsContext **bis_fatfs_ctx);

bool bisStorageMountPartition(u8 bis_partition_id, const char **out_mount_name)
{
    if (bis_partition_id < FsBisPartitionId_CalibrationFile || bis_partition_id > FsBisPartitionId_System || !out_mount_name)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return false;
    }

    bool ret = false;

    SCOPED_LOCK(&g_bisStorageMutex)
    {
        BisStorageFatFsContext *bis_fatfs_ctx = NULL;

        /* Check if we have already mounted this eMMC partition. */
        bis_fatfs_ctx = BIS_STORAGE_FATFS_CTX(bis_partition_id);
        if (bis_fatfs_ctx)
        {
            *out_mount_name = BIS_STORAGE_MOUNT_NAME(bis_partition_id);
            ret = true;
            break;
        }

        /* Initialize BIS FatFs context. */
        bis_fatfs_ctx = bisStorageInitializeFatFsContext(bis_partition_id);
        if (!bis_fatfs_ctx) break;

        /* Update output. */
        *out_mount_name = BIS_STORAGE_MOUNT_NAME(bis_partition_id);
        ret = true;
    }

    return ret;
}

void bisStorageUnmountPartition(u8 bis_partition_id)
{
    if (bis_partition_id < FsBisPartitionId_CalibrationFile || bis_partition_id > FsBisPartitionId_System)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return;
    }

    SCOPED_LOCK(&g_bisStorageMutex) _bisStorageUnmountPartition(bis_partition_id);
}

void bisStorageUnmountAllPartitions(void)
{
    SCOPED_LOCK(&g_bisStorageMutex)
    {
        for(u8 i = 0; i < FF_VOLUMES; i++) _bisStorageUnmountPartition(i + FsBisPartitionId_CalibrationFile);
    }
}

FsStorage *bisStorageGetFsStorageByFatFsDriveNumber(u8 drive_number)
{
    FsStorage *bis_storage = NULL;

    SCOPED_LOCK(&g_bisStorageMutex)
    {
        for(u8 i = 0; i < FF_VOLUMES; i++)
        {
            if (!g_bisStorageContexts[i] || g_bisStorageContexts[i]->fatfs.pdrv != drive_number) continue;
            bis_storage = &(g_bisStorageContexts[i]->bis_storage);
            break;
        }
    }

    return bis_storage;
}

void bisStorageControlMutex(bool lock)
{
    bool locked = mutexIsLockedByCurrentThread(&g_bisStorageMutex);

    if (!locked && lock)
    {
        mutexLock(&g_bisStorageMutex);
    } else
    if (locked && !lock)
    {
        mutexUnlock(&g_bisStorageMutex);
    }
}

static void _bisStorageUnmountPartition(u8 bis_partition_id)
{
    /* Check if we have already mounted this eMMC partition. */
    BisStorageFatFsContext *bis_fatfs_ctx = BIS_STORAGE_FATFS_CTX(bis_partition_id);
    if (!bis_fatfs_ctx) return;

    /* Free BIS FatFs context. This will take care of unmounting the partition. */
    bisStorageFreeFatFsContext(&bis_fatfs_ctx);

    /* Update context array. */
    BIS_STORAGE_FATFS_CTX(bis_partition_id) = NULL;
}

static BisStorageFatFsContext *bisStorageInitializeFatFsContext(u8 bis_partition_id)
{
    if (bis_partition_id < FsBisPartitionId_CalibrationFile || bis_partition_id > FsBisPartitionId_System)
    {
        LOG_MSG_ERROR("Invalid parameters!");
        return NULL;
    }

    BisStorageFatFsContext *bis_fatfs_ctx = NULL;

    Result rc = 0;

    FRESULT fr = FR_OK;
    const char *name = BIS_STORAGE_MOUNT_NAME(bis_partition_id);

    bool success = false;

    /* Allocate memory for our output context. */
    bis_fatfs_ctx = calloc(1, sizeof(BisStorageFatFsContext));
    if (!bis_fatfs_ctx)
    {
        LOG_MSG_ERROR("Failed to allocate memory for BIS FatFs context! (partition ID %u).", bis_partition_id);
        goto end;
    }

    /* Set BIS partition ID. */
    bis_fatfs_ctx->bis_partition_id = bis_partition_id;

    /* Open BIS storage. */
    rc = fsOpenBisStorage(&(bis_fatfs_ctx->bis_storage), bis_fatfs_ctx->bis_partition_id);
    if (R_FAILED(rc))
    {
        LOG_MSG_ERROR("Failed to open eMMC BIS partition storage! (0x%X, partition ID %u).", rc, bis_partition_id);
        goto end;
    }

    /* Update context array. */
    /* FatFs diskio demands we do this here. */
    BIS_STORAGE_FATFS_CTX(bis_partition_id) = bis_fatfs_ctx;

    /* Mount BIS partition using FatFs. */
    fr = f_mount(&(bis_fatfs_ctx->fatfs), name, 1);
    if (fr != FR_OK)
    {
        LOG_MSG_ERROR("Failed to mount eMMC BIS partition via FatFs! (%u, partition ID %u).", fr, bis_partition_id);
        goto end;
    }

    /* Mount devoptab device. */
    if (!devoptabMountFatFsDevice(&(bis_fatfs_ctx->fatfs), name))
    {
        LOG_MSG_ERROR("Failed to mount devoptab device for eMMC BIS partition %u!", bis_partition_id);
        goto end;
    }

    /* Update flag. */
    success = true;

end:
    if (!success)
    {
        bisStorageFreeFatFsContext(&bis_fatfs_ctx);
        BIS_STORAGE_FATFS_CTX(bis_partition_id) = NULL;
    }

    return bis_fatfs_ctx;
}

static void bisStorageFreeFatFsContext(BisStorageFatFsContext **bis_fatfs_ctx)
{
    if (!bis_fatfs_ctx || !*bis_fatfs_ctx) return;

    if ((*bis_fatfs_ctx)->fatfs.fs_type)
    {
        const char *name = BIS_STORAGE_MOUNT_NAME((*bis_fatfs_ctx)->bis_partition_id);
        devoptabUnmountDevice(name);
        f_unmount(name);
    }

    if (serviceIsActive(&((*bis_fatfs_ctx)->bis_storage.s))) fsStorageClose(&((*bis_fatfs_ctx)->bis_storage));

    free(*bis_fatfs_ctx);
    *bis_fatfs_ctx = NULL;
}
