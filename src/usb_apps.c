#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "usb_apps.h"
#include "app_install.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mount.h>
#endif

#define LUN_PATH                                                               \
  "/sys/kernel/config/usb_gadget/ac50/functions/mass_storage.block/lun.0/file"
#define GADGET_TOOL "/usr/bin/az0x-usb-gadget"
#define DROP_IMG_PATH "/tmp/mpc-apps-drop.img"
#define DROP_MNT_PATH "/tmp/mpc-apps-mnt"
#define DROP_VOL_LABEL "MPC-APPS"
#define DROP_IMG_TARGET_BYTES (512ULL * 1024ULL * 1024ULL)
#define DROP_IMG_MIN_BYTES (64ULL * 1024ULL * 1024ULL)

static int s_mock_enabled = 0;
static int s_mock_fail = 0;
static char s_mock_dir[PATH_MAX] = {0};

void ui_usb_apps_set_mock(int enable, int fail_start) {
  s_mock_enabled = enable;
  s_mock_fail = fail_start;
}

void ui_usb_apps_set_mock_dir(const char *dir) {
  if (dir && dir[0])
    snprintf(s_mock_dir, sizeof s_mock_dir, "%s", dir);
  else
    s_mock_dir[0] = 0;
}

static int usb_apps_is_mock(void) {
#ifdef __linux__
  return s_mock_enabled;
#else
  (void)s_mock_enabled;
  return 1;
#endif
}

static void set_err(char *buf, size_t len, const char *msg) {
  if (buf && len > 0)
    snprintf(buf, len, "%s", msg ? msg : "");
}

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

static int write_full_fd(int fd, const void *buf, size_t n) {
  const char *p = (const char *)buf;
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (w == 0)
      return -1;
    off += (size_t)w;
  }
  return 0;
}

static int copy_file(const char *src, const char *dst) {
  int in_fd = open(src, O_RDONLY | O_NOFOLLOW);
  if (in_fd < 0)
    return -1;
  int out_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (out_fd < 0) {
    close(in_fd);
    return -1;
  }

  char buf[65536];
  int rc = 0;
  for (;;) {
    ssize_t r = read(in_fd, buf, sizeof buf);
    if (r < 0) {
      if (errno == EINTR)
        continue;
      rc = -1;
      break;
    }
    if (r == 0)
      break;
    if (write_full_fd(out_fd, buf, (size_t)r) != 0) {
      rc = -1;
      break;
    }
  }
  if (close(out_fd) != 0)
    rc = -1;
  close(in_fd);
  if (rc != 0)
    unlink(dst);
  return rc;
}

static int ends_with_ci(const char *s, const char *suf) {
  size_t sl = strlen(s);
  size_t fl = strlen(suf);
  if (sl <= fl)
    return 0;
  const char *p = s + sl - fl;
  for (size_t i = 0; i < fl; i++) {
    unsigned char a = (unsigned char)p[i];
    unsigned char b = (unsigned char)suf[i];
    if (a >= 'A' && a <= 'Z')
      a = (unsigned char)(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z')
      b = (unsigned char)(b - 'A' + 'a');
    if (a != b)
      return 0;
  }
  return 1;
}

static int safe_filename(const char *name) {
  if (!name || !name[0] || name[0] == '.' || strlen(name) >= 200)
    return 0;
  if (strstr(name, ".."))
    return 0;
  for (const unsigned char *p = (const unsigned char *)name; *p; p++) {
    if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\')
      return 0;
  }
  return 1;
}

static void unmount_drop_mnt(void) {
  sync();
#ifdef __linux__
  if (umount(DROP_MNT_PATH) != 0) {
    char *av_umount[] = {"umount", DROP_MNT_PATH, NULL};
    run_cmd(av_umount);
  }
#else
  char *av_umount[] = {"umount", DROP_MNT_PATH, NULL};
  run_cmd(av_umount);
#endif
  rmdir(DROP_MNT_PATH);
}

static void cleanup_drop_files(void) {
  unmount_drop_mnt();
  unlink(DROP_IMG_PATH);
}

static int mount_drop_img(int rw) {
  mkdir(DROP_MNT_PATH, 0755);
  char *av_modprobe[] = {"modprobe", "exfat", NULL};
  run_cmd(av_modprobe);

  const char *opts = rw ? "loop,rw" : "loop,ro";
  char *av_mount[] = {"mount", "-t", "exfat", "-o", (char *)opts,
                      DROP_IMG_PATH, DROP_MNT_PATH, NULL};
  if (run_cmd(av_mount) == 0)
    return 0;

  char *av_fb[] = {"mount", "-o", (char *)opts, DROP_IMG_PATH, DROP_MNT_PATH,
                   NULL};
  return run_cmd(av_fb);
}

static const char *find_mkfs_exfat(void) {
  if (access("/usr/sbin/mkfs.exfat", X_OK) == 0)
    return "/usr/sbin/mkfs.exfat";
  if (access("/sbin/mkfs.exfat", X_OK) == 0)
    return "/sbin/mkfs.exfat";
  return "mkfs.exfat";
}

static int pick_image_size(uint64_t *out_bytes) {
  struct statvfs sv;
  if (statvfs("/tmp", &sv) != 0)
    return -1;
  uint64_t avail = (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
  if (avail < DROP_IMG_MIN_BYTES * 2ULL)
    return -1;
  uint64_t cap = (avail / 2ULL) & ~((1024ULL * 1024ULL) - 1ULL);
  if (cap > DROP_IMG_TARGET_BYTES)
    cap = DROP_IMG_TARGET_BYTES;
  if (cap < DROP_IMG_MIN_BYTES)
    return -1;
  *out_bytes = cap;
  return 0;
}

static void seed_drop_contents(const char *card) {
  const char *readme_path = DROP_MNT_PATH "/Drop .zip apps here.txt";
  const char *msg =
      "Drag and drop .zip app packages into this USB drive.\n"
      "When finished, safely eject this drive on your computer\n"
      "and press Exit USB Mode on your MPC Sample.\n";
  int fd = open(readme_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (fd >= 0) {
    (void)write_full_fd(fd, msg, strlen(msg));
    close(fd);
  }

  if (!card || !card[0])
    return;

  char apps_dir[PATH_MAX];
  if (snprintf(apps_dir, sizeof apps_dir, "%s/autorun/apps", card) >=
      (int)sizeof apps_dir)
    return;

  DIR *d = opendir(apps_dir);
  if (!d)
    return;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (!safe_filename(de->d_name) || !ends_with_ci(de->d_name, ".error.txt"))
      continue;
    char src[PATH_MAX], dst[PATH_MAX];
    struct stat st;
    if (snprintf(src, sizeof src, "%s/%s", apps_dir, de->d_name) >=
            (int)sizeof src ||
        snprintf(dst, sizeof dst, "%s/%s", DROP_MNT_PATH, de->d_name) >=
            (int)sizeof dst)
      continue;
    if (lstat(src, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))
      continue;
    (void)copy_file(src, dst);
  }
  closedir(d);
}

static int start_backend(const char *card, char *err_buf, size_t err_len) {
  if (usb_apps_is_mock()) {
    if (s_mock_fail) {
      set_err(err_buf, err_len, "Mock failure starting USB app installer");
      return -1;
    }
    return 0;
  }

  cleanup_drop_files();

  uint64_t img_bytes = 0;
  if (pick_image_size(&img_bytes) != 0) {
    set_err(err_buf, err_len, "Not enough RAM in /tmp for USB drop drive");
    return -1;
  }

  int img_fd =
      open(DROP_IMG_PATH, O_RDWR | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  if (img_fd < 0) {
    set_err(err_buf, err_len, "Could not create USB drop image");
    return -1;
  }
  if (ftruncate(img_fd, (off_t)img_bytes) != 0) {
    close(img_fd);
    unlink(DROP_IMG_PATH);
    set_err(err_buf, err_len, "Could not size USB drop image");
    return -1;
  }
  close(img_fd);

  const char *mkfs = find_mkfs_exfat();
  char *av_mkfs[] = {(char *)mkfs, "-L", DROP_VOL_LABEL, DROP_IMG_PATH, NULL};
  if (run_cmd(av_mkfs) != 0) {
    unlink(DROP_IMG_PATH);
    set_err(err_buf, err_len, "Failed to format USB drop image");
    return -1;
  }

  if (mount_drop_img(1) != 0) {
    cleanup_drop_files();
    set_err(err_buf, err_len, "Failed to mount virtual exFAT image");
    return -1;
  }
  seed_drop_contents(card);
  unmount_drop_mnt();

  char *av_switch[] = {GADGET_TOOL, "switch", "sd-access", NULL};
  if (run_cmd(av_switch) != 0) {
    unlink(DROP_IMG_PATH);
    set_err(err_buf, err_len, "Failed to switch USB gadget");
    return -2;
  }

  int ready = 0;
  for (int retry = 0; retry < 50; retry++) {
    if (access(LUN_PATH, W_OK) == 0) {
      ready = 1;
      break;
    }
    usleep(100 * 1000);
  }
  if (!ready) {
    char *av_restore[] = {GADGET_TOOL, "switch", NULL};
    run_cmd(av_restore);
    unlink(DROP_IMG_PATH);
    set_err(err_buf, err_len, "Timeout waiting for USB storage gadget");
    return -3;
  }

  int fd = open(LUN_PATH, O_WRONLY);
  if (fd < 0) {
    char *av_restore[] = {GADGET_TOOL, "switch", NULL};
    run_cmd(av_restore);
    unlink(DROP_IMG_PATH);
    set_err(err_buf, err_len, "Cannot open USB storage LUN node");
    return -4;
  }

  char write_buf[128];
  int n = snprintf(write_buf, sizeof write_buf, "%s\n", DROP_IMG_PATH);
  ssize_t written = write(fd, write_buf, (size_t)n);
  close(fd);

  if (written < (ssize_t)strlen(DROP_IMG_PATH)) {
    char *av_restore[] = {GADGET_TOOL, "switch", NULL};
    run_cmd(av_restore);
    unlink(DROP_IMG_PATH);
    set_err(err_buf, err_len, "Failed to attach drop image to USB LUN");
    return -5;
  }
  return 0;
}

static int stop_backend(char *err_buf, size_t err_len) {
  if (usb_apps_is_mock())
    return 0;

  sync();

  int fd = open(LUN_PATH, O_WRONLY);
  if (fd >= 0) {
    ssize_t w = write(fd, "\n", 1);
    (void)w;
    close(fd);
  }

  char *av_switch[] = {GADGET_TOOL, "switch", NULL};
  if (run_cmd(av_switch) != 0)
    set_err(err_buf, err_len, "Warning: failed to restore default USB gadget");

  usleep(300 * 1000);
  sync();
  return 0;
}

int usb_apps_copy_zips_from_dir(const char *src_dir, const char *card,
                                int *copied_out, char *err, size_t err_n) {
  if (copied_out)
    *copied_out = 0;
  if (!src_dir || !src_dir[0] || !card || !card[0]) {
    set_err(err, err_n, "invalid card path");
    return -1;
  }
  if (app_install_ensure_dir(card) != 0) {
    set_err(err, err_n, "could not prepare autorun/apps");
    return -1;
  }

  DIR *d = opendir(src_dir);
  if (!d) {
    set_err(err, err_n, "could not read dropped files");
    return -1;
  }

  int copied = 0;
  int copy_err = 0;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    char src[PATH_MAX], tmp_dst[PATH_MAX], dst[PATH_MAX];
    struct stat st;
    if (!safe_filename(de->d_name) || !ends_with_ci(de->d_name, ".zip"))
      continue;
    if (snprintf(src, sizeof src, "%s/%s", src_dir, de->d_name) >=
        (int)sizeof src)
      continue;
    if (lstat(src, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))
      continue;
    if (snprintf(tmp_dst, sizeof tmp_dst, "%s/autorun/apps/.%s.part", card,
                 de->d_name) >= (int)sizeof tmp_dst ||
        snprintf(dst, sizeof dst, "%s/autorun/apps/%s", card, de->d_name) >=
            (int)sizeof dst) {
      copy_err = 1;
      continue;
    }
    if (copy_file(src, tmp_dst) != 0 || rename(tmp_dst, dst) != 0) {
      unlink(tmp_dst);
      copy_err = 1;
      continue;
    }
    copied++;
  }
  closedir(d);

  if (copied_out)
    *copied_out = copied;
  if (copy_err && copied == 0) {
    set_err(err, err_n, "failed copying .zip onto SD card");
    return -1;
  }
  return 0;
}

int usb_apps_collect_zips(const char *card, int *copied_out, char *err,
                          size_t err_n) {
  if (copied_out)
    *copied_out = 0;
  if (usb_apps_is_mock()) {
    if (!s_mock_dir[0])
      return 0;
    return usb_apps_copy_zips_from_dir(s_mock_dir, card, copied_out, err,
                                       err_n);
  }

  if (access(DROP_IMG_PATH, F_OK) != 0)
    return 0;

  if (mount_drop_img(0) != 0) {
    cleanup_drop_files();
    set_err(err, err_n, "could not mount USB drop image");
    return -1;
  }

  int rc = usb_apps_copy_zips_from_dir(DROP_MNT_PATH, card, copied_out, err,
                                       err_n);
  cleanup_drop_files();
  return rc;
}

void ui_usb_apps_init(struct ui_usb_apps *s, const char *card) {
  if (!s)
    return;
  memset(s, 0, sizeof *s);
  s->phase = UI_USB_APPS_PHASE_PREPARING;
  if (card && card[0])
    snprintf(s->card, sizeof s->card, "%s", card);
}

int ui_usb_apps_is_active(const struct ui_usb_apps *s) {
  return s && s->bound;
}

int ui_usb_apps_stop(struct ui_usb_apps *s) {
  if (!s || !s->bound)
    return 0;
  char err[160];
  int rc = stop_backend(err, sizeof err);
  if (!usb_apps_is_mock())
    cleanup_drop_files();
  s->bound = 0;
  s->presented = 0;
  s->phase = UI_USB_APPS_PHASE_DONE;
  return rc;
}

static void draw_card(struct ui *u, struct ui_usb_apps_style st, const char *a,
                      const char *b, const char *c, const char *d) {
  struct ui_paint card = {
      .fill_set = 1,
      .fill = st.box,
      .border_set = 1,
      .border = st.border,
      .border_px = 1,
  };
  ui_box(u, 16, 76, 288, 144, card);
  if (a)
    ui_text(u, 28, 92, a, st.hl, 1);
  if (b)
    ui_text(u, 28, 114, b, st.text, 1);
  if (c)
    ui_text(u, 28, 132, c, st.dim, 1);
  if (d)
    ui_text(u, 28, 146, d, st.dim, 1);
}

static struct ui_button exit_button(struct ui_usb_apps_style st) {
  struct ui_button btn = {
      .fill = st.bar,
      .text = st.active_text,
      .border_set = 1,
      .border = st.hl,
      .border_px = 1,
      .highlighted = 1,
      .highlight = st.hl,
      .highlight_px = 2,
  };
  return btn;
}

int ui_usb_apps(struct ui *u, struct ui_usb_apps *s,
                struct ui_usb_apps_style st, const struct input *in) {
  if (!u || !s)
    return UI_USB_APPS_FAILED;
  if (s->phase == UI_USB_APPS_PHASE_DONE)
    return UI_USB_APPS_DONE;
  if (s->phase == UI_USB_APPS_PHASE_FAILED)
    return UI_USB_APPS_FAILED;

  if (s->phase == UI_USB_APPS_PHASE_PREPARING && s->presented) {
    char err[160] = {0};
    if (start_backend(s->card, err, sizeof err) == 0) {
      s->bound = 1;
      s->err[0] = 0;
      s->phase = UI_USB_APPS_PHASE_ACTIVE;
    } else {
      snprintf(s->err, sizeof s->err, "%s", err[0] ? err : "Failed to start");
      s->phase = UI_USB_APPS_PHASE_ERROR;
    }
    s->presented = 0;
  } else if (s->phase == UI_USB_APPS_PHASE_EXITING && s->presented) {
    char err[160] = {0};
    stop_backend(err, sizeof err);
    s->bound = 0;
    s->presented = 0;
    s->phase = UI_USB_APPS_PHASE_DONE;
    return UI_USB_APPS_DONE;
  }

  if (in && s->phase == UI_USB_APPS_PHASE_ACTIVE) {
    if (input_down(in, IN_B1) || in->encoder_press) {
      s->phase = UI_USB_APPS_PHASE_EXITING;
      s->presented = 0;
    }
  } else if (in && s->phase == UI_USB_APPS_PHASE_ERROR) {
    if (input_down(in, IN_B1) || in->encoder_press) {
      s->phase = UI_USB_APPS_PHASE_FAILED;
      return UI_USB_APPS_FAILED;
    }
  }

  ui_clear(u, st.bg);
  const char *labels[3] = {"Exit", "", ""};
  struct ui_soft_style soft = {
      .fill = st.box,
      .text = st.text,
      .active_text = st.active_text,
      .highlight = st.hl,
      .gap = 4,
      .feedback = UI_SOFT_FLASH,
      .flash_ms = 120,
  };
  ui_soft_keys(u, 8, 4, 304, 28, labels, &s->soft_active, soft, in, &s->soft);

  const char *hint = "";
  if (s->phase == UI_USB_APPS_PHASE_PREPARING)
    hint = "creating USB drop drive...";
  else if (s->phase == UI_USB_APPS_PHASE_ACTIVE)
    hint = "connected via USB";
  else if (s->phase == UI_USB_APPS_PHASE_EXITING)
    hint = "installing dropped apps...";
  else if (s->phase == UI_USB_APPS_PHASE_ERROR)
    hint = "USB install error";
  ui_text(u, 12, 40, UI_USB_APPS_LABEL, st.text, 1);
  if (hint[0])
    ui_text(u, 12, 56, hint, st.dim, 1);

  if (s->phase == UI_USB_APPS_PHASE_ACTIVE) {
    draw_card(u, st, "USB APP INSTALLER",
              "Drive 'MPC-APPS' mounted on PC.",
              "Drag .zip apps in, eject drive,",
              "then press Exit USB Mode.");
    ui_button(u, 64, 172, 192, 32, "Exit USB Mode", exit_button(st), in);
  } else if (s->phase == UI_USB_APPS_PHASE_PREPARING) {
    draw_card(u, st, "Preparing USB drop drive...",
              "Creating isolated MPC-APPS drive", "Please wait...", NULL);
  } else if (s->phase == UI_USB_APPS_PHASE_EXITING) {
    draw_card(u, st, "Exiting USB mode...",
              "Restoring USB & scanning apps...", "Please wait...", NULL);
  } else if (s->phase == UI_USB_APPS_PHASE_ERROR) {
    draw_card(u, st, "Could not start app installer",
              s->err[0] ? s->err : "Unknown error occurred", NULL, NULL);
    ui_button(u, 80, 172, 160, 32, "Back", exit_button(st), in);
  }

  if (s->phase == UI_USB_APPS_PHASE_PREPARING ||
      s->phase == UI_USB_APPS_PHASE_EXITING) {
    s->presented = 1;
    ui_wake(u, 1);
  }
  return UI_USB_APPS_ACTIVE;
}

