/* usb_installer.c
   On-device USB Installer for mpc-boot-thingy.
   Triggered over USB via POST /testapp on port 80.
   Runs completely in RAM, draws a confirmation screen, installs/updates
   boot-thingy on the SD card, migrates any existing autoboot to autorun/apps/,
   and reboots. */

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

  char *argv1[] = {"/bin/systemctl", "reboot", "-i", NULL};
  run_cmd(argv1);
  char *argv2[] = {"/sbin/reboot", "-f", NULL};
  run_cmd(argv2);
  char *argv3[] = {"reboot", "-f", NULL};
  run_cmd(argv3);
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

static int perform_install(const char *card, const char *staged,
                           enum install_mode mode) {
  log_msg("perform_install started: card=%s staged=%s mode=%d", card, staged,
          mode);
  remount_card_exec(card);

  char dest_autorun[512];
  snprintf(dest_autorun, sizeof dest_autorun, "%s/autorun", card);

  if (mode == MODE_MIGRATE) {
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

    /* Move old autorun into apps/previous-autoboot */
    char app_dir[512], app_autorun[512];
    snprintf(app_dir, sizeof app_dir, "%s/autorun/apps/previous-autoboot",
             card);
    snprintf(app_autorun, sizeof app_autorun, "%s/autorun", app_dir);

    char *mk_app[] = {"mkdir", "-p", app_dir, NULL};
    run_cmd(mk_app);

    char *mv_old[] = {"mv", ram_stage, app_autorun, NULL};
    if (run_cmd(mv_old) != 0) {
      char *cp_old[] = {"cp", "-a", ram_stage, app_autorun, NULL};
      run_cmd(cp_old);
    }

    /* Write boot.entry for previous autoboot */
    char entry_file[512];
    snprintf(entry_file, sizeof entry_file, "%s/boot.entry", app_dir);
    FILE *ef = fopen(entry_file, "w");
    if (ef) {
      fprintf(ef, "title=Previous Autoboot\nrun=autorun/autorun.sh\n");
      fclose(ef);
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
    snprintf(path, sizeof path, "%s/autorun/system/ssh/ssh_host_ed25519_key",
             card);
    struct stat st;
    if (stat(path, &st) == 0) {
      snprintf(hold_target, sizeof hold_target, "%s/ssh_key", hold_dir);
      char *cp_k[] = {"cp", "-a", path, hold_target, NULL};
      run_cmd(cp_k);
    }

    /* Hold os_tools rc files */
    const char *rc_files[] = {".bashrc", ".bash_profile", NULL};
    for (int i = 0; rc_files[i]; i++) {
      const char *rc = rc_files[i];
      snprintf(path, sizeof path, "%s/autorun/os_tools/%s", card, rc);
      if (stat(path, &st) == 0) {
        snprintf(hold_target, sizeof hold_target, "%s/%s", hold_dir, rc);
        char *cp_rc[] = {"cp", "-a", path, hold_target, NULL};
        run_cmd(cp_rc);
      }
    }

    /* Copy staged payload over autorun */
    copy_tree(staged, (char *)card);

    /* Restore SSH key if was preserved */
    snprintf(hold_target, sizeof hold_target, "%s/ssh_key", hold_dir);
    if (stat(hold_target, &st) == 0) {
      snprintf(path, sizeof path, "%s/autorun/system/ssh/ssh_host_ed25519_key",
               card);
      char *cp_back[] = {"cp", "-f", hold_target, path, NULL};
      run_cmd(cp_back);
    }

    /* Restore os_tools rc */
    for (int i = 0; rc_files[i]; i++) {
      const char *rc = rc_files[i];
      snprintf(hold_target, sizeof hold_target, "%s/%s", hold_dir, rc);
      if (stat(hold_target, &st) == 0) {
        snprintf(path, sizeof path, "%s/autorun/os_tools/%s", card, rc);
        char *cp_back[] = {"cp", "-f", hold_target, path, NULL};
        run_cmd(cp_back);
      }
    }
    char *rm_tmp[] = {"rm", "-rf", hold_dir, NULL};
    run_cmd(rm_tmp);
  } else {
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

  int selected = 1; /* 0 = No, 1 = Yes */
  int choice = -1;  /* -1 pending, 0 = No, 1 = Yes */

  struct ui_soft_style soft_st = {
      .fill = 0x2a303a,
      .text = 0xd0d8e0,
      .active_text = 0xffffff,
      .highlight = 0xffb000,
      .gap = 4,
      .feedback = UI_SOFT_FLASH,
      .flash_ms = 120,
  };

  const char *question =
      (mode == MODE_UPDATE) ? "Update boot thingy?" : "Install boot thingy?";
  const char *subtitle = "";
  if (mode == MODE_MIGRATE)
    subtitle = "Existing autoboot will be saved to Apps";
  else if (mode == MODE_UPDATE)
    subtitle = "Update boot thingy files on SD card";
  else
    subtitle = "Install boot thingy to SD card";

  const char *labels[3] = {"No", "", "Yes"};

  struct timespec start_ts;
  clock_gettime(CLOCK_MONOTONIC, &start_ts);

  log_msg("Entering UI loop: question='%s'", question);

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

    /* Check buttons and encoder */
    if (in.encoder > 0) {
      selected = 1;
      in.encoder = 0;
    } else if (in.encoder < 0) {
      selected = 0;
      in.encoder = 0;
    }

    if (elapsed_ms >= 350) {
      if (input_down(&in, IN_B1) || input_down(&in, IN_STOP)) {
        log_msg("Button B1/STOP pressed -> choice=No");
        choice = 0;
      } else if (input_down(&in, IN_B3) || input_down(&in, IN_PLAY)) {
        log_msg("Button B3/PLAY pressed -> choice=Yes");
        choice = 1;
      } else if (in.encoder_press) {
        log_msg("Encoder pressed -> choice=%s", selected ? "Yes" : "No");
        choice = selected;
      }
    }

    /* 5 minute timeout: cancel & reboot */
    if (elapsed_ms > 300000) {
      log_msg("UI loop timeout -> cancelling");
      choice = 0;
    }

    struct ui u = {.px = back};
    ui_clear(&u, 0x121418);

    /* Soft buttons on the top spanning full width right below hardware buttons
     */
    int active_key = selected == 0 ? 0 : 2;
    int key_pressed = ui_soft_keys(&u, 8, 4, 304, 26, labels, &active_key,
                                   soft_st, &in, &soft_state);
    if (elapsed_ms >= 350) {
      if (key_pressed == 0) {
        log_msg("Soft key 0 pressed -> choice=No");
        choice = 0;
      } else if (key_pressed == 2) {
        log_msg("Soft key 2 pressed -> choice=Yes");
        choice = 1;
      }
    }

    /* Center prompt dialog box */
    struct ui_paint box = {.fill_set = 1,
                           .fill = 0x1b1f24,
                           .border_set = 1,
                           .border = 0x333a44,
                           .border_px = 1};
    ui_box(&u, 8, 36, 304, 196, box);

    /* Header tag */
    ui_text(&u, 20, 48, "USB INSTALLER", 0xffb000, 1);

    /* Question */
    ui_text(&u, 20, 72, question, 0xffffff, 2);

    /* Subtitle */
    ui_text(&u, 20, 104, subtitle, 0x9aa4b0, 1);

    /* Visual selection buttons */
    struct ui_button b_no = {
        .fill = (selected == 0 ? 0x442222 : 0x22262c),
        .text = (selected == 0 ? 0xffffff : 0x8892a0),
        .highlighted = (selected == 0),
        .highlight = 0xff4444,
        .highlight_px = 2,
    };
    if (ui_button(&u, 24, 134, 126, 42, "No", b_no, &in) && elapsed_ms >= 350) {
      choice = 0;
    }

    struct ui_button b_yes = {
        .fill = (selected == 1 ? 0x224422 : 0x22262c),
        .text = (selected == 1 ? 0xffffff : 0x8892a0),
        .highlighted = (selected == 1),
        .highlight = 0x44ff44,
        .highlight_px = 2,
    };
    if (ui_button(&u, 170, 134, 126, 42, "Yes", b_yes, &in) &&
        elapsed_ms >= 350) {
      choice = 1;
    }

    /* Footer hints */
    ui_text(&u, 20, 198, "Top buttons: No / Yes       Wheel: Select", 0x707c8c,
            1);

    /* Hardware pad illumination: Pad 0 red for No, Pad 3 green for Yes */
    ui_led(&u, 0, selected == 0 ? 120 : 15, 0, 0);
    ui_led(&u, 3, 0, selected == 1 ? 120 : 15, 0);

    draw_frame(&scr, back, front, &midi, &u);

    usleep(25 * 1000);
  }

  log_msg("Choice resolved to: %d", choice);

  if (choice == 1) {
    draw_status_screen(&scr, back, front, &midi, "Installing...",
                       "Copying boot thingy to SD card...");
    perform_install(card, staged, mode);
    draw_status_screen(&scr, back, front, &midi, "Complete!",
                       "Boot thingy is installed. Rebooting...");
    sleep(2);
  } else {
    draw_status_screen(&scr, back, front, &midi, "Cancelled",
                       "No changes made. Rebooting...");
    sleep(1);
  }

  free(back);
  free(front);
  screen_close(&scr);
  surface_close(&midi);

  do_reboot();
  return 0;
}
