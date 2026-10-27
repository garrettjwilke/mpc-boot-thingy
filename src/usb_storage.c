#define _GNU_SOURCE
#include "usb_storage.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mount.h>
#endif

#define LUN_PATH "/sys/kernel/config/usb_gadget/ac50/functions/mass_storage.block/lun.0/file"
#define GADGET_TOOL "/usr/bin/az0x-usb-gadget"
#define EDISKSCTL "/usr/bin/edisksctl"

static int s_mock_enabled = 0;
static int s_mock_fail = 0;

void usb_storage_set_mock(int enable, int fail_start) {
    s_mock_enabled = enable;
    s_mock_fail = fail_start;
}

int usb_storage_is_mock(void) {
#ifndef __linux__
    return 1;
#else
    return s_mock_enabled;
#endif
}

static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null_fd = open("/dev/null", O_RDWR);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void set_err(char *buf, size_t len, const char *msg) {
    if (buf && len > 0) {
        snprintf(buf, len, "%s", msg ? msg : "");
    }
}

int usb_storage_find_sd_device(char *out, size_t n) {
    if (usb_storage_is_mock()) {
        snprintf(out, n, "/dev/mock_mmcblk1");
        return 0;
    }

    /* Check for external MMC controller node (RK3566 SDMMC at fe2b0000.mmc) */
    DIR *d = opendir("/sys/bus/platform/devices/fe2b0000.mmc/mmc_host");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strncmp(de->d_name, "mmc", 3) != 0) continue;
            char path[512];
            snprintf(path, sizeof path, "/sys/bus/platform/devices/fe2b0000.mmc/mmc_host/%s", de->d_name);
            DIR *host_d = opendir(path);
            if (!host_d) continue;
            struct dirent *hde;
            while ((hde = readdir(host_d)) != NULL) {
                if (strncmp(hde->d_name, "mmc", 3) == 0 && strchr(hde->d_name, ':')) {
                    /* Found card e.g. mmc1:0001 */
                    char blk_path[512];
                    snprintf(blk_path, sizeof blk_path, "%s/%s/block", path, hde->d_name);
                    DIR *blk_d = opendir(blk_path);
                    if (blk_d) {
                        struct dirent *bde;
                        while ((bde = readdir(blk_d)) != NULL) {
                            if (strncmp(bde->d_name, "mmcblk", 6) == 0) {
                                snprintf(out, n, "/dev/%s", bde->d_name);
                                closedir(blk_d);
                                closedir(host_d);
                                closedir(d);
                                return 0;
                            }
                        }
                        closedir(blk_d);
                    }
                }
            }
            closedir(host_d);
        }
        closedir(d);
    }

    /* Standard fallback on MPC Sample */
    if (access("/dev/mmcblk1", F_OK) == 0) {
        snprintf(out, n, "/dev/mmcblk1");
        return 0;
    }

    return -1;
}

static void unmount_sd_partitions(const char *sd_dev) {
    sync();

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return;

    char line[1024];
    char mounts[32][512];
    int nmounts = 0;

    size_t dev_len = strlen(sd_dev);
    while (fgets(line, sizeof line, f) && nmounts < 32) {
        char dev[256], mnt[512];
        if (sscanf(line, "%255s %511s", dev, mnt) == 2) {
            if (strncmp(dev, sd_dev, dev_len) == 0) {
                /* Decode any \040 escapes in mount path */
                char decoded[512];
                size_t di = 0;
                for (size_t si = 0; mnt[si] && di + 1 < sizeof decoded; si++) {
                    if (mnt[si] == '\\' && mnt[si + 1] >= '0' && mnt[si + 1] <= '7' &&
                        mnt[si + 2] >= '0' && mnt[si + 2] <= '7' &&
                        mnt[si + 3] >= '0' && mnt[si + 3] <= '7') {
                        int code = (mnt[si + 1] - '0') * 64 + (mnt[si + 2] - '0') * 8 + (mnt[si + 3] - '0');
                        decoded[di++] = (char)code;
                        si += 3;
                    } else {
                        decoded[di++] = mnt[si];
                    }
                }
                decoded[di] = 0;
                snprintf(mounts[nmounts++], sizeof mounts[0], "%s", decoded);
            }
        }
    }
    fclose(f);

    /* Unmount identified mount points */
    for (int i = 0; i < nmounts; i++) {
        char *av_edisks[] = {EDISKSCTL, "unmount", mounts[i], NULL};
        if (access(EDISKSCTL, X_OK) == 0) {
            run_cmd(av_edisks);
        }
#ifdef __linux__
        if (umount2(mounts[i], MNT_DETACH) != 0) {
            umount(mounts[i]);
        }
#endif
        char *av_umount[] = {"umount", "-l", mounts[i], NULL};
        run_cmd(av_umount);
    }
}

int usb_storage_start(char *err_buf, size_t err_len) {
    if (usb_storage_is_mock()) {
        if (s_mock_fail) {
            set_err(err_buf, err_len, "Mock failure starting USB storage");
            return -1;
        }
        return 0;
    }

    char sd_dev[64];
    if (usb_storage_find_sd_device(sd_dev, sizeof sd_dev) != 0) {
        set_err(err_buf, err_len, "SD card not detected in slot");
        return -1;
    }

    /* Safely unmount host filesystems on this SD card */
    unmount_sd_partitions(sd_dev);

    /* Switch gadget to mass storage */
    char *av_switch[] = {GADGET_TOOL, "switch", "sd-access", NULL};
    if (run_cmd(av_switch) != 0) {
        set_err(err_buf, err_len, "Failed to switch USB gadget to sd-access");
        return -2;
    }

    /* Wait up to 5 seconds for configfs lun.0/file to become available */
    int ready = 0;
    for (int retry = 0; retry < 50; retry++) {
        if (access(LUN_PATH, W_OK) == 0) {
            ready = 1;
            break;
        }
        usleep(100 * 1000);
    }

    if (!ready) {
        set_err(err_buf, err_len, "Timeout waiting for USB mass storage gadget sysfs");
        return -3;
    }

    /* Bind SD block device into LUN file */
    int fd = open(LUN_PATH, O_WRONLY);
    if (fd < 0) {
        set_err(err_buf, err_len, "Cannot open USB storage LUN configfs node");
        return -4;
    }

    size_t dev_str_len = strlen(sd_dev);
    char write_buf[128];
    int n = snprintf(write_buf, sizeof write_buf, "%s\n", sd_dev);
    ssize_t written = write(fd, write_buf, (size_t)n);
    close(fd);

    if (written < (ssize_t)dev_str_len) {
        set_err(err_buf, err_len, "Failed to attach SD device to USB storage LUN");
        return -5;
    }

    return 0;
}

int usb_storage_stop(char *err_buf, size_t err_len) {
    if (usb_storage_is_mock()) {
        return 0;
    }

    sync();

    /* Unbind backing device from LUN */
    int fd = open(LUN_PATH, O_WRONLY);
    if (fd >= 0) {
        write(fd, "\n", 1);
        close(fd);
    }

    /* Switch gadget back to default composite mode */
    char *av_switch[] = {GADGET_TOOL, "switch", NULL};
    if (run_cmd(av_switch) != 0) {
        set_err(err_buf, err_len, "Warning: failed to restore default USB gadget");
    }

    usleep(300 * 1000);
    sync();

    /* Drop VM caches so new files written by computer are seen fresh */
    int cfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
    if (cfd >= 0) {
        write(cfd, "3\n", 2);
        close(cfd);
    }

    /* Trigger udev block scan so edisksd remounts the SD card */
    char *av_udev[] = {"udevadm", "trigger", "--subsystem-match=block", NULL};
    run_cmd(av_udev);

    /* Also attempt explicit edisksctl mount if partition is known */
    char part[64];
    if (usb_storage_find_sd_device(part, sizeof part) == 0) {
        char part1[80];
        snprintf(part1, sizeof part1, "%sp1", part);
        if (access(part1, F_OK) == 0 && access(EDISKSCTL, X_OK) == 0) {
            char *av_mount[] = {EDISKSCTL, "mount", part1, NULL};
            run_cmd(av_mount);
        }
    }

    return 0;
}
