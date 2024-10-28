/*
 * bis_storage.h
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

#pragma once

#ifndef __BIS_STORAGE_H__
#define __BIS_STORAGE_H__

#ifdef __cplusplus
extern "C" {
#endif

/// Mounts an eMMC BIS partition using its ID and provides a pointer to a string that holds its mount name, which can be used to carry out FS operations.
/// Only eMMC BIS partition IDs `CalibrationFile` (28) through `System` (31) are supported.
bool bisStorageMountPartition(u8 bis_partition_id, const char **out_mount_name);

/// Unmounts a previously mounted eMMC BIS partition.
/// Only eMMC BIS partition IDs `CalibrationFile` (28) through `System` (31) are supported.
void bisStorageUnmountPartition(u8 bis_partition_id);

/// Unmounts all previously mounted eMMC BIS partitions.
void bisStorageUnmountAllPartitions(void);

/// Returns a pointer to a FsStorage object that matches the provided FatFs drive number, or NULL if it hasn't been mounted.
/// Only used by FatFs's diskio operations.
FsStorage *bisStorageGetFsStorageByFatFsDriveNumber(u8 drive_number);

/// (Un)locks the BIS storage mutex. Can be used to block other threads and prevent them from altering the internal status of this interface.
/// Use with caution.
void bisStorageControlMutex(bool lock);

#ifdef __cplusplus
}
#endif

#endif /* __BIS_STORAGE_H__ */
