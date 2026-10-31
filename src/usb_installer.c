/* usb_installer.c
   On-device USB Installer for mpc-boot-thingy.
   Triggered over USB via POST /testapp on port 80.
   Runs completely in RAM, draws a confirmation screen, installs/updates
   boot-thingy on the SD card, and reboots. An existing autoboot can be
   kept as a named boot entry or deleted. */

#define _GNU_SOURCE
#include "input.h"
#include "screen.h"
#include "sd_check.h"
#include "surface.h"
#include "theme.h"
#include "ui.h"
#include "widgets.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void log_msg(const char *fmt, ...) {
  FILE *f = fopen("/tmp/usb_installer.log", "a");
  if (!f)
    return;
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  fprintf(f, "[%ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fprintf(f, "\n");
  fclose(f);
}

enum install_mode { MODE_UNKNOWN, MODE_INSTALL, MODE_UPDATE, MODE_MIGRATE };

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

static void do_reboot(void) {
  log_msg("Executing reboot sequence");
  sync();
  int fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
  if (fd >= 0) {
    write(fd, "3\n", 2);
    close(fd);
  }
  /* Clear bootcount so recovery mode does not trip */
  fd = open("/sys/devices/platform/fdc20000.syscon/fdc20000.syscon:bootcount/"
            "bootcount",
            O_WRONLY);
  if (fd >= 0) {
    write(fd, "0\n", 2);
    close(fd);
  }
  sync();

  log_msg("Invoking reboot commands");
  pid_t pid = fork();
  if (pid == 0) {
    char *const r_argv[] = {"reboot", NULL};
    execvp(r_argv[0], r_argv);
    _exit(0);
  }
  usleep(500 * 1000);
  pid = fork();
  if (pid == 0) {
    char *const s_argv[] = {"systemctl", "reboot", "-i", NULL};
    execvp(s_argv[0], s_argv);
    _exit(0);
  }
  usleep(500 * 1000);
  sync();
  log_msg("Calling kernel reboot syscall");
  reboot(RB_AUTOBOOT);
}

/* Locate mounted SD card under /media, skipping internal eMMC partitions */
static int find_sd_card(char *out, size_t n) {
  /* Primary: check /proc/mounts for /dev/mmcblk1 (removable SD card slot) */
  FILE *f = fopen("/proc/mounts", "r");
  if (f) {
    char line[1024];
    while (fgets(line, sizeof line, f)) {
      char dev[256], mnt[512], type[64];
      if (sscanf(line, "%255s %511s %63s", dev, mnt, type) == 3) {
        if (strncmp(dev, "/dev/mmcblk1", 12) == 0) {
          snprintf(out, n, "%s", mnt);
          fclose(f);
          log_msg("Found SD card via /proc/mounts: %s (dev=%s)", out, dev);
          return 0;
        }
      }
    }
    fclose(f);
  }

  /* Fallback: scan /media, skipping UUIDs and az01-internal */
  DIR *d = opendir("/media");
  if (d) {
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
      if (de->d_name[0] == '.')
        continue;
      if (strncmp(de->d_name, "az01-internal", 13) == 0)
        continue;
      if (strchr(de->d_name, '-') && strlen(de->d_name) > 30)
        continue;
      char cand[512];
      struct stat st;
      if (snprintf(cand, sizeof cand, "/media/%s", de->d_name) >=
          (int)sizeof cand)
        continue;
      if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, n, "%s", cand);
        closedir(d);
        log_msg("Found SD card via /media scan: %s", out);
        return 0;
      }
    }
    closedir(d);
  }
  log_msg("No SD card found");
  return -1;
}

static enum install_mode detect_mode(const char *card) {
  char mark[512], autorun[512];
  snprintf(mark, sizeof mark, "%s/autorun/system/mpc-boot-thingy.mark", card);
  snprintf(autorun, sizeof autorun, "%s/autorun", card);

  struct stat st;
  if (stat(mark, &st) == 0) {
    log_msg("Detect mode: UPDATE");
    return MODE_UPDATE;
  }
  if (stat(autorun, &st) == 0 && S_ISDIR(st.st_mode)) {
    log_msg("Detect mode: MIGRATE");
    return MODE_MIGRATE;
  }
  log_msg("Detect mode: FRESH INSTALL");
  return MODE_INSTALL;
}

/* Locate staged autorun folder alongside binary, in /tmp, or in
 * /run/staged_autorun */
static int find_staged_autorun(const char *argv0, char *out, size_t n) {
  char cand[512];
  char exe[512];
  ssize_t elen = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (elen > 0) {
    exe[elen] = 0;
    char path_copy[512];
    snprintf(path_copy, sizeof path_copy, "%s", exe);
    char *dir = dirname(path_copy);
    snprintf(cand, sizeof cand, "%s/autorun", dir);
    struct stat st;
    if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
      snprintf(out, n, "%s", cand);
      log_msg("Found staged autorun relative to exe: %s", out);
      return 0;
    }
  }

  if (argv0) {
    char path_copy[512];
    snprintf(path_copy, sizeof path_copy, "%s", argv0);
    char *dir = dirname(path_copy);
    snprintf(cand, sizeof cand, "%s/autorun", dir);
    struct stat st;
    if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
      snprintf(out, n, "%s", cand);
      log_msg("Found staged autorun relative to argv0: %s", out);
      return 0;
    }
  }

  struct stat st;
  if (stat("/tmp/autorun", &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, n, "/tmp/autorun");
    log_msg("Found staged autorun at /tmp/autorun");
    return 0;
  }
  if (stat("/run/staged_autorun", &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, n, "/run/staged_autorun");
    log_msg("Found staged autorun at /run/staged_autorun");
    return 0;
  }
  if (stat("./autorun", &st) == 0 && S_ISDIR(st.st_mode)) {
    snprintf(out, n, "./autorun");
    log_msg("Found staged autorun at ./autorun");
    return 0;
  }
  log_msg("Staged autorun not found");
  return -1;
}

static int remount_card_exec(const char *card) {
  char *argv[] = {"mount", "-o", "remount,exec", (char *)card, NULL};
  return run_cmd(argv);
}

/* Copy tree using standard cp -a */
static int copy_tree(const char *src, const char *dst) {
  char *argv[] = {"cp", "-a", (char *)src, (char *)dst, NULL};
  return run_cmd(argv);
}

/* Menu titles are MENU_TITLE bytes, including the NUL. */
#define APP_NAME_MAX 64
#define DEFAULT_APP_NAME "old autoboot"
#define DEFAULT_APP_FOLDER "old-autoboot"

static void clean_title(char *dst, size_t n, const char *src) {
  size_t j = 0;
  if (!dst || n == 0)
    return;
  dst[0] = 0;
  if (!src)
    return;
  while (*src == ' ' || *src == '\t')
    src++;
  for (const unsigned char *p = (const unsigned char *)src; *p && j + 1 < n;
       p++) {
    if (*p < 32 || *p >= 127)
      break;
    dst[j++] = (char)*p;
  }
  while (j && (dst[j - 1] == ' ' || dst[j - 1] == '\t'))
    j--;
  dst[j] = 0;
}

/* Lowercase slug, matching setup.sh. Stops at 48 characters so a numeric
   suffix still fits in the folder name. */
static int slug_of(const char *title, char *out, size_t n) {
  size_t j = 0;
  size_t cap;
  int dash = 1;
  if (!out || n < 2) {
    if (out && n)
      out[0] = 0;
    return -1;
  }
  cap = n - 1;
  if (cap > 48)
    cap = 48;
  for (const unsigned char *p = (const unsigned char *)(title ? title : "");
       *p; p++) {
    unsigned char c = *p;
    int alnum;
    if (c >= 'A' && c <= 'Z')
      c = (unsigned char)(c - 'A' + 'a');
    alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (alnum) {
      if (j >= cap)
        break;
      out[j++] = (char)c;
      dash = 0;
    } else if (!dash && j < cap) {
      out[j++] = '-';
      dash = 1;
    }
  }
  if (j && out[j - 1] == '-')
    j--;
  out[j] = 0;
  return j ? 0 : -1;
}

static int path_exists(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0;
}

/* Folder under autorun/apps/. A name already in the staged tree gets -2, -3, ... */
static void app_folder_for(const char *staged, const char *title, char *out,
                           size_t n) {
  char base[64];
  if (!out || n == 0)
    return;
  if (slug_of(title, base, sizeof base) != 0)
    snprintf(base, sizeof base, "%s", DEFAULT_APP_FOLDER);
  for (int i = 0; i < 50; i++) {
    char cand[64];
    char path[512];
    int wr;
    if (i == 0)
      wr = snprintf(cand, sizeof cand, "%s", base);
    else
      wr = snprintf(cand, sizeof cand, "%s-%d", base, i + 1);
    if (wr < 0 || (size_t)wr >= sizeof cand)
      continue;
    if (staged && staged[0]) {
      wr = snprintf(path, sizeof path, "%s/apps/%s", staged, cand);
      if (wr < 0 || (size_t)wr >= sizeof path)
        continue;
      if (path_exists(path))
        continue;
    }
    snprintf(out, n, "%s", cand);
    return;
  }
  snprintf(out, n, "%s", DEFAULT_APP_FOLDER);
}

static void ellipsize(char *dst, size_t n, const char *src, int max_chars) {
  int len = 0;
  if (!dst || n == 0)
    return;
  if (!src)
    src = "";
  if (max_chars < 0)
    max_chars = 0;
  if ((size_t)max_chars >= n)
    max_chars = (int)n - 1;
  while (src[len])
    len++;
  if (len <= max_chars) {
    memcpy(dst, src, (size_t)len);
    dst[len] = 0;
    return;
  }
  if (max_chars <= 3) {
    memset(dst, '.', (size_t)max_chars);
    dst[max_chars] = 0;
    return;
  }
  memcpy(dst, src, (size_t)(max_chars - 3));
  dst[max_chars - 3] = '.';
  dst[max_chars - 2] = '.';
  dst[max_chars - 1] = '.';
  dst[max_chars] = 0;
}

static int perform_install(const char *card, const char *staged,
                           enum install_mode mode, int keep_old,
                           const char *migrate_title) {
  log_msg("perform_install started: card=%s staged=%s mode=%d", card, staged,
          mode);
  remount_card_exec(card);

  char dest_autorun[512];
  snprintf(dest_autorun, sizeof dest_autorun, "%s/autorun", card);

  if (mode == MODE_MIGRATE && keep_old) {
    log_msg("Migrating existing autoboot from %s", dest_autorun);
    /* Stage old autorun to RAM */
    char ram_stage[512];
    snprintf(ram_stage, sizeof ram_stage, "/run/old_autoboot");
    char *rm_argv[] = {"rm", "-rf", ram_stage, NULL};
    run_cmd(rm_argv);

    char *mv_argv[] = {"mv", dest_autorun, ram_stage, NULL};
    if (run_cmd(mv_argv) != 0) {
      char *cp_argv[] = {"cp", "-a", dest_autorun, ram_stage, NULL};
      run_cmd(cp_argv);
      char *rm_dest[] = {"rm", "-rf", dest_autorun, NULL};
      run_cmd(rm_dest);
    }

    /* Install clean boot-thingy */
    char *mkdir_argv[] = {"mkdir", "-p", dest_autorun, NULL};
    run_cmd(mkdir_argv);
    copy_tree(staged, (char *)card);

    /* Move old autorun into apps/<folder> and name its boot entry. */
    char title[APP_NAME_MAX];
    char folder[64];
    char app_dir[512], app_autorun[512];
    clean_title(title, sizeof title, migrate_title);
    if (!title[0])
      snprintf(title, sizeof title, "%s", DEFAULT_APP_NAME);
    app_folder_for(staged, title, folder, sizeof folder);
    log_msg("Saving existing autoboot as '%s' in apps/%s", title, folder);
    if (snprintf(app_dir, sizeof app_dir, "%s/autorun/apps/%s", card, folder) >=
        (int)sizeof app_dir) {
      log_msg("App path too long, using %s", DEFAULT_APP_FOLDER);
      snprintf(folder, sizeof folder, "%s", DEFAULT_APP_FOLDER);
      snprintf(app_dir, sizeof app_dir, "%s/autorun/apps/%s", card, folder);
    }
    snprintf(app_autorun, sizeof app_autorun, "%s/autorun", app_dir);

    char *mk_app[] = {"mkdir", "-p", app_dir, NULL};
    run_cmd(mk_app);

    char *mv_old[] = {"mv", ram_stage, app_autorun, NULL};
    if (run_cmd(mv_old) != 0) {
      char *cp_old[] = {"cp", "-a", ram_stage, app_autorun, NULL};
      run_cmd(cp_old);
    }

    char entry_file[512];
    snprintf(entry_file, sizeof entry_file, "%s/boot.entry", app_dir);
    FILE *ef = fopen(entry_file, "w");
    if (ef) {
      fprintf(ef, "title=%s\nrun=autorun/autorun.sh\n", title);
      fclose(ef);
      log_msg("Wrote %s", entry_file);
    } else {
      log_msg("Failed to write %s", entry_file);
    }
  } else if (mode == MODE_UPDATE) {
    log_msg("Updating existing boot-thingy installation");
    /* Preserve user apps, configs, and host key */
    char hold_dir[512];
    snprintf(hold_dir, sizeof hold_dir, "/run/mpc_update_hold");
    char *rm_hold[] = {"rm", "-rf", hold_dir, NULL};
    run_cmd(rm_hold);
    char *mk_hold[] = {"mkdir", "-p", hold_dir, NULL};
    run_cmd(mk_hold);

    char path[512], hold_target[512];
    /* Hold SSH key */
    log_msg("Preserving SSH host key if present");
    snprintf(path, sizeof path, "%s/autorun/system/ssh/ssh_host_ed25519_key",
             card);
    struct stat st;
    if (stat(path, &st) == 0) {
      snprintf(hold_target, sizeof hold_target, "%s/ssh_key", hold_dir);
      char *cp_k[] = {"cp", "-a", path, hold_target, NULL};
      run_cmd(cp_k);
      log_msg("SSH host key preserved");
    }

    /* Hold os_tools rc files */
    log_msg("Preserving shell rc files if present");
    const char *rc_files[] = {".bashrc", ".bash_profile", NULL};
    for (int i = 0; rc_files[i]; i++) {
      const char *rc = rc_files[i];
      snprintf(path, sizeof path, "%s/autorun/os_tools/%s", card, rc);
      if (stat(path, &st) == 0) {
        snprintf(hold_target, sizeof hold_target, "%s/%s", hold_dir, rc);
        char *cp_rc[] = {"cp", "-a", path, hold_target, NULL};
        run_cmd(cp_rc);
        log_msg("Preserved rc file: %s", rc);
      }
    }

    /* Copy staged payload over autorun */
    log_msg("Copying staged tree from %s to %s", staged, card);
    copy_tree(staged, (char *)card);
    log_msg("Copy staged tree finished");

    /* Restore SSH key if was preserved */
    snprintf(hold_target, sizeof hold_target, "%s/ssh_key", hold_dir);
    if (stat(hold_target, &st) == 0) {
      snprintf(path, sizeof path, "%s/autorun/system/ssh/ssh_host_ed25519_key",
               card);
      char *cp_back[] = {"cp", "-f", hold_target, path, NULL};
      run_cmd(cp_back);
      log_msg("Restored SSH host key");
    }

    /* Restore os_tools rc */
    for (int i = 0; rc_files[i]; i++) {
      const char *rc = rc_files[i];
      snprintf(hold_target, sizeof hold_target, "%s/%s", hold_dir, rc);
      if (stat(hold_target, &st) == 0) {
        snprintf(path, sizeof path, "%s/autorun/os_tools/%s", card, rc);
        char *cp_back[] = {"cp", "-f", hold_target, path, NULL};
        run_cmd(cp_back);
        log_msg("Restored rc file: %s", rc);
      }
    }
    char *rm_tmp[] = {"rm", "-rf", hold_dir, NULL};
    run_cmd(rm_tmp);
  } else {
    if (mode == MODE_MIGRATE) {
      log_msg("Deleting existing autoboot at %s", dest_autorun);
      char *rm_old[] = {"rm", "-rf", dest_autorun, NULL};
      run_cmd(rm_old);
    }
    log_msg("Fresh install to %s", dest_autorun);
    char *mkdir_argv[] = {"mkdir", "-p", dest_autorun, NULL};
    run_cmd(mkdir_argv);
    copy_tree(staged, (char *)card);
  }

  sync();
  remount_card_exec(card);
  log_msg("perform_install complete");
  return 0;
}

static void present(struct screen *scr, const uint32_t *front, int rects[][4],
                    int n) {
  for (int i = 0; i < n; i++) {
    int x0 = rects[i][0], y0 = rects[i][1], x1 = rects[i][2], y1 = rects[i][3];
    for (int y = y0; y < y1; y++)
      memcpy(scr->px + y * scr->stride + x0, front + y * UI_W + x0,
             (size_t)(x1 - x0) * 4);
  }
  screen_flush(scr, rects, n);
}

static void draw_frame(struct screen *scr, uint32_t *back, uint32_t *front,
                       struct surface *midi, struct ui *u) {
  int rects[UI_H][4];
  int n = ui_changes(back, front, rects);
  if (scr->px)
    present(scr, front, rects, n);
  for (int i = 0; i < u->nled; i++)
    surface_pad(midi, u->led[i].index, u->led[i].r, u->led[i].g, u->led[i].b);
}

static void draw_status_screen(struct screen *scr, uint32_t *back,
                               uint32_t *front, struct surface *midi,
                               const char *title, const char *status) {
  struct ui u = {.px = back};
  ui_clear(&u, 0x121418);

  /* Top bar */
  struct ui_paint top_bar = {.fill_set = 1, .fill = 0x22262c};
  ui_box(&u, 0, 0, UI_W, 28, top_bar);
  ui_text(&u, 14, 8, "USB INSTALLER", 0xffb000, 1);

  /* Center box */
  struct ui_paint box = {.fill_set = 1,
                         .fill = 0x1b1f24,
                         .border_set = 1,
                         .border = 0x333a44,
                         .border_px = 1};
  ui_box(&u, 16, 50, UI_W - 32, 140, box);

  ui_text(&u, 32, 85, title, 0xffffff, 2);
  ui_text(&u, 32, 130, status, 0x8892a0, 1);

  draw_frame(scr, back, front, midi, &u);
}

/* Left is choice 0, right is choice 1. Returns -1 while nothing is chosen. */
static int installer_yes_no(struct ui *u, const struct input *in,
                            struct ui_soft_state *soft, int *selected,
                            long elapsed_ms, int button_y, const char *left,
                            const char *right) {
  int choice = -1;
  if (in) {
    if (in->encoder > 0)
      *selected = 1;
    else if (in->encoder < 0)
      *selected = 0;
  }

  int active_key = *selected == 0 ? 0 : 2;
  const char *labels[3] = {left, "", right};
  struct ui_soft_style soft_st = {
      .fill = 0x2a303a,
      .text = 0xd0d8e0,
      .active_text = 0xffffff,
      .highlight = 0xffb000,
      .gap = 4,
      .feedback = UI_SOFT_FLASH,
      .flash_ms = 120,
  };
  int key_pressed =
      ui_soft_keys(u, 8, 4, 304, 26, labels, &active_key, soft_st, in, soft);

  int armed = in && elapsed_ms >= 350;
  if (armed) {
    if (input_down(in, IN_B1) || input_down(in, IN_STOP))
      choice = 0;
    else if (input_down(in, IN_B3) || input_down(in, IN_PLAY))
      choice = 1;
    else if (in->encoder_press)
      choice = *selected;
    if (key_pressed == 0)
      choice = 0;
    else if (key_pressed == 2)
      choice = 1;
  }

  struct ui_button b_no = {
      .fill = (*selected == 0 ? 0x442222 : 0x22262c),
      .text = (*selected == 0 ? 0xffffff : 0x8892a0),
      .highlighted = (*selected == 0),
      .highlight = 0xff4444,
      .highlight_px = 2,
  };
  if (ui_button(u, 24, button_y, 126, 42, left, b_no, armed ? in : NULL))
    choice = 0;

  struct ui_button b_yes = {
      .fill = (*selected == 1 ? 0x224422 : 0x22262c),
      .text = (*selected == 1 ? 0xffffff : 0x8892a0),
      .highlighted = (*selected == 1),
      .highlight = 0x44ff44,
      .highlight_px = 2,
  };
  if (ui_button(u, 170, button_y, 126, 42, right, b_yes, armed ? in : NULL))
    choice = 1;

  for (int p = 0; p < 16; p++)
    ui_led(u, p, 0, 0, 0);
  ui_led(u, 0, *selected == 0 ? 120 : 15, 0, 0);
  ui_led(u, 3, 0, *selected == 1 ? 120 : 15, 0);
  return choice;
}

int main(int argc, char **argv) {
  (void)argc;
  log_msg("=== USB Installer started ===");

  /* Stop ac50 if running so we own the display and surface */
  char *stop_argv[] = {"systemctl", "stop", "ac50.service", NULL};
  run_cmd(stop_argv);
  char *kill_argv[] = {"killall", "-9", "MPC Sample", NULL};
  run_cmd(kill_argv);
  usleep(100 * 1000);

  char card[512] = {0};
  if (find_sd_card(card, sizeof card) != 0) {
    log_msg("Warning: No SD card found");
  }

  char staged[512] = {0};
  find_staged_autorun(argv[0], staged, sizeof staged);

  enum install_mode mode = card[0] ? detect_mode(card) : MODE_UNKNOWN;
  log_msg("Target card: '%s', staged: '%s', mode: %d", card, staged, mode);

  struct screen scr = {.fd = -1};
  if (screen_open(&scr) != 0) {
    log_msg("Fatal: failed to open screen");
    return 1;
  }
  log_msg("Screen opened successfully: px=%p stride=%d", (void *)scr.px,
          scr.stride);

  struct surface midi = {.fd = -1};
  surface_open(&midi);
  log_msg("Surface MIDI opened: fd=%d", midi.fd);

  /* Drain any pending/stale bytes in MIDI queue */
  if (midi.fd >= 0) {
    unsigned char drain[512];
    while (read(midi.fd, drain, sizeof drain) > 0) {
    }
  }

  uint32_t *back = calloc((size_t)UI_W * UI_H, sizeof(uint32_t));
  uint32_t *front = calloc((size_t)UI_W * UI_H, sizeof(uint32_t));
  if (!back || !front) {
    log_msg("Fatal: calloc failed for screen buffers");
    screen_close(&scr);
    surface_close(&midi);
    return 1;
  }

  if (!card[0] || !staged[0]) {
    log_msg("Showing error screen: card[0]=%d staged[0]=%d", card[0],
            staged[0]);
    draw_status_screen(&scr, back, front, &midi, "Installer Error",
                       !card[0] ? "No SD card detected. Insert card and retry."
                                : "Staged autorun payload missing.");
    sleep(4);
    free(back);
    free(front);
    screen_close(&scr);
    surface_close(&midi);
    do_reboot();
    return 1;
  }

  struct input_dev dev = {0};
  struct parser parser = {0};
  struct input in = {0};
  struct ui_soft_state soft_state = {0};

  char app_title[APP_NAME_MAX];
  char edit_buf[APP_NAME_MAX];
  struct ui_text_editor editor;
  snprintf(app_title, sizeof app_title, "%s", DEFAULT_APP_NAME);
  memset(&editor, 0, sizeof editor);

  enum { STEP_KEEP, STEP_EDIT, STEP_CONFIRM };
  int step = mode == MODE_MIGRATE ? STEP_KEEP : STEP_CONFIRM;
  int selected = 1; /* 0 = left, 1 = right. Keep and Yes start selected. */
  int keep_old = 0;
  int choice = -1; /* install: -1 pending, 0 = No, 1 = Yes */

  const char *question =
      (mode == MODE_UPDATE) ? "Update boot thingy?" : "Install boot thingy?";

  struct timespec start_ts;
  clock_gettime(CLOCK_MONOTONIC, &start_ts);

  log_msg("Entering UI loop: mode=%d question='%s'", mode, question);
  if (mode == MODE_MIGRATE)
    log_msg("Existing autoboot found");

  while (choice < 0) {
    input_begin(&in, &dev);

    /* Read midi input */
    if (midi.fd >= 0) {
      unsigned char buf[256];
      for (;;) {
        ssize_t rd = read(midi.fd, buf, sizeof buf);
        if (rd <= 0)
          break;
        for (ssize_t i = 0; i < rd; i++) {
          if (parser_feed(&parser, buf[i]))
            input_push(&dev, &in, parser.msg);
        }
      }
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - start_ts.tv_sec) * 1000 +
                      (now.tv_nsec - start_ts.tv_nsec) / 1000000;

    /* 5 minute timeout: cancel & reboot */
    if (elapsed_ms > 300000) {
      log_msg("UI loop timeout -> cancelling");
      choice = 0;
      break;
    }

    if (step == STEP_EDIT) {
      struct ui u = {.px = back};
      ui_clear(&u, 0x121418);
      struct ui_text_editor_style ed_st = ui_text_editor_style_default();
      ed_st.fill = 0x121418;
      ed_st.text_box = 0x1b1f24;
      ed_st.title = 0xffb000;
      ed_st.highlight = 0xffb000;
      int res = ui_text_editor(&u, 0, 0, UI_W, UI_H, &editor, ed_st, &in);
      if (res == UI_TEXT_OK) {
        char cleaned[APP_NAME_MAX];
        clean_title(cleaned, sizeof cleaned, edit_buf);
        if (!cleaned[0]) {
          log_msg("Empty app name ignored");
          snprintf(edit_buf, sizeof edit_buf, "%s", app_title);
          ui_text_editor_init(&editor, edit_buf, (int)sizeof edit_buf,
                              "NAME ENTRY");
        } else {
          snprintf(app_title, sizeof app_title, "%s", cleaned);
          log_msg("Boot entry name set to '%s'", app_title);
          keep_old = 1;
          selected = 1;
          step = STEP_CONFIRM;
        }
      } else if (res == UI_TEXT_CANCEL) {
        log_msg("Entry naming cancelled");
        keep_old = 0;
        selected = 1;
        step = STEP_KEEP;
      }
      draw_frame(&scr, back, front, &midi, &u);
    } else {
      struct ui u = {.px = back};
      ui_clear(&u, 0x121418);

      struct ui_paint box = {.fill_set = 1,
                             .fill = 0x1b1f24,
                             .border_set = 1,
                             .border = 0x333a44,
                             .border_px = 1};
      ui_box(&u, 8, 36, 304, 196, box);
      ui_text(&u, 20, 48, "USB INSTALLER", 0xffb000, 1);

      int button_y = 134;
      const char *footer = "Top buttons: No / Yes       Wheel: Select";
      const char *left = "No";
      const char *right = "Yes";
      if (step == STEP_KEEP) {
        ui_text(&u, 16, 66, "An existing autoboot is already on this SD card.",
                0xffffff, 1);
        ui_text(&u, 16, 88, "boot thingy will add it as a boot entry", 0x9aa4b0,
                1);
        ui_text(&u, 16, 104, "if you want to keep it.", 0x9aa4b0, 1);
        ui_text(&u, 16, 128, "Would you like to keep or delete", 0xffffff, 1);
        ui_text(&u, 16, 144, "the existing autoboot?", 0xffffff, 1);
        button_y = 164;
        footer = "Top: Delete / Keep        Wheel: Select";
        left = "Delete";
        right = "Keep";
      } else {
        char shown[40];
        char subtitle_buf[80];
        const char *subtitle;
        if (mode == MODE_MIGRATE && keep_old) {
          ellipsize(shown, sizeof shown, app_title, 36);
          snprintf(subtitle_buf, sizeof subtitle_buf, "Saved as %s", shown);
          subtitle = subtitle_buf;
        } else if (mode == MODE_MIGRATE) {
          subtitle = "Existing autoboot will be removed";
        } else if (mode == MODE_UPDATE) {
          subtitle = "Update boot thingy files on SD card";
        } else {
          subtitle = "Install boot thingy to SD card";
        }
        ui_text(&u, 20, 72, question, 0xffffff, 1);
        ui_text(&u, 20, 104, subtitle, 0x9aa4b0, 1);
      }

      int pick = installer_yes_no(&u, &in, &soft_state, &selected, elapsed_ms,
                                  button_y, left, right);
      int footer_y = step == STEP_KEEP ? 214 : 198;
      ui_text(&u, 16, footer_y, footer, 0x707c8c, 1);

      if (pick == 0 || pick == 1)
        log_msg("%s -> %s", step == STEP_KEEP ? "Keep prompt" : "Install prompt",
                step == STEP_KEEP ? (pick ? "Keep" : "Delete")
                                  : (pick ? "Yes" : "No"));
      if (step == STEP_KEEP && pick == 1) {
        snprintf(app_title, sizeof app_title, "%s", DEFAULT_APP_NAME);
        snprintf(edit_buf, sizeof edit_buf, "%s", app_title);
        ui_text_editor_init(&editor, edit_buf, (int)sizeof edit_buf,
                            "NAME ENTRY");
        step = STEP_EDIT;
      } else if (step == STEP_KEEP && pick == 0) {
        keep_old = 0;
        selected = 1;
        step = STEP_CONFIRM;
      } else if (pick >= 0) {
        choice = pick;
      }

      draw_frame(&scr, back, front, &midi, &u);
    }

    usleep(25 * 1000);
  }

  log_msg("Choice resolved to: %d", choice);

  if (choice == 1) {
    draw_status_screen(&scr, back, front, &midi, "Installing...",
                       "Copying boot thingy to SD card...");
    log_msg("Calling perform_install");
    perform_install(card, staged, mode, keep_old, app_title);
    log_msg("Drawing Complete! status screen");
    draw_status_screen(&scr, back, front, &midi, "Complete!",
                       "Boot thingy is installed. Rebooting...");
    sleep(2);
  } else {
    log_msg("Drawing Cancelled status screen");
    draw_status_screen(&scr, back, front, &midi, "Cancelled",
                       "No changes made. Rebooting...");
    sleep(1);
  }

  log_msg("Closing screen and surface");
  free(back);
  free(front);
  screen_close(&scr);
  surface_close(&midi);

  log_msg("Triggering do_reboot");
  do_reboot();
  return 0;
}
