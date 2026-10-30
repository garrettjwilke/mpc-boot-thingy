#define _GNU_SOURCE
#include "sd_check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_cmd(char *const argv[]) {
  pid_t pid = fork();
  if (pid < 0)
    return -1;
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
    if (errno != EINTR)
      return -1;
  }
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int get_mount_info(const char *path, char *dev_out, size_t dev_n,
                          char *mnt_out, size_t mnt_n, char *fs_out,
                          size_t fs_n) {
  FILE *f = fopen("/proc/mounts", "r");
  if (!f)
    return -1;

  char line[1024];
  size_t best_len = 0;
  if (dev_out && dev_n > 0)
    dev_out[0] = 0;
  if (mnt_out && mnt_n > 0)
    mnt_out[0] = 0;
  if (fs_out && fs_n > 0)
    fs_out[0] = 0;

  while (fgets(line, sizeof line, f)) {
    char dev[256], mnt[512], type[64];
    if (sscanf(line, "%255s %511s %63s", dev, mnt, type) != 3)
      continue;

    /* Decode octal escape sequences in mount point (e.g. \040 for space) */
    char decoded[512];
    size_t j = 0;
    for (size_t i = 0; mnt[i] && j + 1 < sizeof decoded; i++) {
      if (mnt[i] == '\\' && mnt[i + 1] >= '0' && mnt[i + 1] <= '7' &&
          mnt[i + 2] >= '0' && mnt[i + 2] <= '7' &&
          mnt[i + 3] >= '0' && mnt[i + 3] <= '7') {
        int v = (mnt[i + 1] - '0') * 64 + (mnt[i + 2] - '0') * 8 +
                (mnt[i + 3] - '0');
        decoded[j++] = (char)v;
        i += 3;
      } else {
        decoded[j++] = mnt[i];
      }
    }
    decoded[j] = 0;

    size_t dlen = strlen(decoded);
    if (dlen < best_len || dlen > strlen(path))
      continue;
    if (strncmp(path, decoded, dlen) != 0)
      continue;
    if (path[dlen] != 0 && path[dlen] != '/')
      continue;

    best_len = dlen;
    if (dev_out && dev_n > 0)
      snprintf(dev_out, dev_n, "%s", dev);
    if (mnt_out && mnt_n > 0)
      snprintf(mnt_out, mnt_n, "%s", decoded);
    if (fs_out && fs_n > 0)
      snprintf(fs_out, fs_n, "%s", type);
  }
  fclose(f);
  return best_len > 0 ? 0 : -1;
}

void sd_sync_and_drop_caches(const char *card) {
  sync();
  int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if (fd >= 0) {
    if (write(fd, "3\n", 2) < 0) {
      /* ignore write error */
    }
    close(fd);
  }
  sync();

  if (card && card[0]) {
    char mnt[512];
    if (get_mount_info(card, NULL, 0, mnt, sizeof mnt, NULL, 0) == 0) {
      char *argv[] = {"mount", "-o", "remount,exec", mnt, NULL};
      run_cmd(argv);
    }
  }
}

int sd_find_device(const char *card, char *dev_out, size_t dev_n,
                   char *fstype_out, size_t fs_n) {
  if (!card || !card[0])
    return -1;
  return get_mount_info(card, dev_out, dev_n, NULL, 0, fstype_out, fs_n);
}

static const char *find_tool(const char *const candidates[], size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (access(candidates[i], X_OK) == 0)
      return candidates[i];
  }
  return NULL;
}

int sd_check_repair(const char *card) {
  if (!card || !card[0])
    return -1;

  char dev[256] = {0}, fstype[64] = {0};
  if (get_mount_info(card, dev, sizeof dev, NULL, 0, fstype,
                     sizeof fstype) != 0) {
    return -1;
  }

  /* Make sure dev is a block device or actual device path */
  if (strncmp(dev, "/dev/", 5) != 0)
    return -1;

  /* Flush pending writes before checking */
  sync();
  int cfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if (cfd >= 0) {
    if (write(cfd, "3\n", 2) < 0) {
    }
    close(cfd);
  }

  int rc = -1;
  if (strstr(fstype, "exfat") != NULL) {
    static const char *const exfat_tools[] = {
        "/usr/sbin/fsck.exfat", "/sbin/fsck.exfat", "fsck.exfat"};
    const char *tool = find_tool(exfat_tools, sizeof exfat_tools / sizeof exfat_tools[0]);
    if (tool) {
      char *av[] = {(char *)tool, "-p", dev, NULL};
      rc = run_cmd(av);
    }
  } else if (strstr(fstype, "fat") != NULL || strstr(fstype, "msdos") != NULL) {
    static const char *const fat_tools[] = {
        "/usr/sbin/fsck.fat", "/sbin/fsck.fat", "fsck.fat",
        "/usr/sbin/dosfsck",  "/sbin/dosfsck",  "dosfsck",
        "/usr/sbin/fsck.vfat", "fsck.vfat"};
    const char *tool = find_tool(fat_tools, sizeof fat_tools / sizeof fat_tools[0]);
    if (tool) {
      char *av[] = {(char *)tool, "-a", dev, NULL};
      rc = run_cmd(av);
    }
  }

  /* If specific tool wasn't found or matched, try generic fsck */
  if (rc < 0) {
    static const char *const generic_tools[] = {
        "/usr/sbin/fsck", "/sbin/fsck", "fsck"};
    const char *tool = find_tool(generic_tools, sizeof generic_tools / sizeof generic_tools[0]);
    if (tool) {
      char *av[] = {(char *)tool, "-a", dev, NULL};
      rc = run_cmd(av);
    }
  }

  /* Invalidate page cache again so Linux sees any on-disk repairs */
  sync();
  int cfd2 = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if (cfd2 >= 0) {
    if (write(cfd2, "3\n", 2) < 0) {
    }
    close(cfd2);
  }

  /* Exit code 0 = clean, 1 = corrected, >1 = uncorrected error */
  if (rc == 0 || rc == 1)
    return rc;
  return -1;
}

