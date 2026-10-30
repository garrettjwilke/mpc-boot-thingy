#ifndef SD_CHECK_H
#define SD_CHECK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronize pending writes to flash and invalidate dirty page cache. */
void sd_sync_and_drop_caches(const char *card);

/* Look up the block device and filesystem type (e.g. "exfat", "vfat")
   for the given card path from /proc/mounts. Returns 0 on success. */
int sd_find_device(const char *card, char *dev_out, size_t dev_n,
                   char *fstype_out, size_t fs_n);

/* Run an integrity check and automatic repair (fsck) on the SD card filesystem.
   Returns:
     0: Clean / no errors found
     1: Errors detected and successfully repaired
    <0: Check could not run or repair failed
*/
int sd_check_repair(const char *card);

#ifdef __cplusplus
}
#endif

#endif /* SD_CHECK_H */

