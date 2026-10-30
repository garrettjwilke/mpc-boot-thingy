#define _GNU_SOURCE
#include "format_sd.h"
#include "sd_check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int s_mock_format = 0;
static int s_mock_format_fail = 0;

void format_sd_set_mock(int enable, int fail) {
  s_mock_format = enable;
  s_mock_format_fail = fail;
}

int format_sd_is_mock(void) {
#ifdef __linux__
  return s_mock_format;
#else
  (void)s_mock_format;
  return 1;
#endif
}

static int run_cmd_quiet(char *const argv[]) {
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

static void report_progress(format_progress_fn progress, void *userdata,
                            const char *msg) {
  if (progress && msg)
    progress(msg, userdata);
}

int format_sd_card(const char *card, const char *label,
                   format_progress_fn progress, void *userdata) {
  const char *lbl = (label && label[0]) ? label : "MPC-Sample";

  if (format_sd_is_mock()) {
    report_progress(progress, userdata, "Staging clean boot files in RAM...");
    usleep(50 * 1000);
    report_progress(progress, userdata, "Stopping background services...");
    usleep(50 * 1000);
    report_progress(progress, userdata, "Unmounting SD card...");
    usleep(50 * 1000);
    report_progress(progress, userdata, "Formatting exFAT filesystem...");
    usleep(50 * 1000);
    if (s_mock_format_fail) {
      report_progress(progress, userdata, "Error: format failed!");
      return -1;
    }
    report_progress(progress, userdata, "Restoring clean boot thingy...");
    usleep(50 * 1000);
    report_progress(progress, userdata, "Synchronizing flash storage...");
    usleep(50 * 1000);
    report_progress(progress, userdata, "Format complete! Rebooting...");
    return 0;
  }

  char dev[256] = {0}, fstype[64] = {0};
  if (sd_find_device(card, dev, sizeof dev, fstype, sizeof fstype) != 0 ||
      strncmp(dev, "/dev/", 5) != 0) {
    report_progress(progress, userdata, "Error: Could not locate SD device!");
    return -1;
  }

  report_progress(progress, userdata, "Staging clean boot files in RAM...");
  char *mkdir_rec_argv[] = {"mkdir", "-p", "/run/mpc-boot-recovery", NULL};
  run_cmd_quiet(mkdir_rec_argv);

  char clean_src[512];
  snprintf(clean_src, sizeof clean_src, "%s/autorun/system/boot-clean.tar.gz",
           card);

  int have_archive = 0;
  if (access(clean_src, R_OK) == 0) {
    char *cp_argv[] = {
        "cp", clean_src, "/run/mpc-boot-recovery/boot-clean.tar.gz", NULL};
    if (run_cmd_quiet(cp_argv) == 0)
      have_archive = 1;
  }

  if (!have_archive) {
    char *tar_argv[] = {"tar",
                        "--exclude=autorun/apps",
                        "--exclude=autorun/boot-edits",
                        "-czf",
                        "/run/mpc-boot-recovery/boot-clean.tar.gz",
                        "-C",
                        (char *)card,
                        "autorun",
                        NULL};
    if (run_cmd_quiet(tar_argv) == 0)
      have_archive = 1;
  }

  if (!have_archive) {
    report_progress(progress, userdata, "Error: Failed to stage boot archive!");
    return -2;
  }

  report_progress(progress, userdata, "Stopping background services...");
  char *stop_web[] = {"systemctl", "stop", "mpc-boot-web.service", NULL};
  run_cmd_quiet(stop_web);
  char *stop_ssh[] = {"systemctl", "stop", "mpc-boot-ssh.service", NULL};
  run_cmd_quiet(stop_ssh);

  if (chdir("/run") != 0) {
    (void)chdir("/");
  }

  report_progress(progress, userdata, "Unmounting SD card...");
  sync();
  usleep(50 * 1000);
  sync();

  if (access("/usr/bin/edisksctl", X_OK) == 0) {
    char *edisks_argv[] = {"/usr/bin/edisksctl", "unmount", dev, NULL};
    run_cmd_quiet(edisks_argv);
  }

  char *umount_argv[] = {"umount", dev, NULL};
  if (run_cmd_quiet(umount_argv) != 0) {
    char *umount_l_argv[] = {"umount", "-l", dev, NULL};
    run_cmd_quiet(umount_l_argv);
  }
  usleep(100 * 1000);

  report_progress(progress, userdata, "Formatting exFAT filesystem...");
  char *mkfs_tool = NULL;
  if (access("/usr/sbin/mkfs.exfat", X_OK) == 0)
    mkfs_tool = "/usr/sbin/mkfs.exfat";
  else if (access("/sbin/mkfs.exfat", X_OK) == 0)
    mkfs_tool = "/sbin/mkfs.exfat";
  else
    mkfs_tool = "mkfs.exfat";

  char *fmt_argv[] = {mkfs_tool, "-L", (char *)lbl, dev, NULL};
  if (run_cmd_quiet(fmt_argv) != 0) {
    /* Fallback to mkfs.vfat if exfat failed */
    char *vfat_argv[] = {"mkfs.vfat", "-F", "32", "-n", (char *)lbl, dev, NULL};
    if (run_cmd_quiet(vfat_argv) != 0) {
      report_progress(progress, userdata, "Error: mkfs failed!");
      return -3;
    }
  }

  report_progress(progress, userdata, "Restoring clean boot thingy...");
  char *mkdir_mnt[] = {"mkdir", "-p", "/run/mnt_sd", NULL};
  run_cmd_quiet(mkdir_mnt);

  char *mount_argv[] = {"mount", "-o", "rw,dirsync", dev, "/run/mnt_sd", NULL};
  if (run_cmd_quiet(mount_argv) != 0) {
    char *mount_fb[] = {"mount", dev, "/run/mnt_sd", NULL};
    if (run_cmd_quiet(mount_fb) != 0) {
      report_progress(progress, userdata, "Error: Mounting formatted card failed!");
      return -4;
    }
  }

  char *unpack_argv[] = {"tar", "-xzf",
                         "/run/mpc-boot-recovery/boot-clean.tar.gz", "-C",
                         "/run/mnt_sd", NULL};
  if (run_cmd_quiet(unpack_argv) != 0) {
    char *unpack_fb[] = {"tar", "-xf",
                         "/run/mpc-boot-recovery/boot-clean.tar.gz", "-C",
                         "/run/mnt_sd", NULL};
    run_cmd_quiet(unpack_fb);
  }

  char *chmod_ar[] = {"chmod", "755", "/run/mnt_sd/autorun/autorun.sh", NULL};
  run_cmd_quiet(chmod_ar);
  char *chmod_bin[] = {"chmod", "755",
                       "/run/mnt_sd/autorun/system/mpc-boot-thingy", NULL};
  run_cmd_quiet(chmod_bin);

  report_progress(progress, userdata, "Synchronizing flash storage...");
  sync();
  int cfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if (cfd >= 0) {
    if (write(cfd, "3\n", 2) < 0) {
    }
    close(cfd);
  }
  sync();

  char *umount_run[] = {"umount", "/run/mnt_sd", NULL};
  run_cmd_quiet(umount_run);
  sync();

  report_progress(progress, userdata, "Format complete! Rebooting...");
  usleep(1000 * 1000);
  return 0;
}

