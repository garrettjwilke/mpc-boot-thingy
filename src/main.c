/* mpc-boot-thingy. At power-on, holding Shift during the splash opens the menu.
   Waiting starts the saved default, or Stock when none is saved.
   --menu skips the splash and opens the menu. The stock Shift+Pad 16 row uses that. */
#include "boot_edit.h"
#include "boot_ui.h"
#include "input.h"
#include "menu.h"
#include "screen.h"
#include "surface.h"
#include "theme.h"
#include "ui.h"
#include "usb_storage.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WINDOW_MS 1000

static int stopped_ac50;
static volatile sig_atomic_t g_reload_requested;
static volatile sig_atomic_t g_boot_now_requested;
static pid_t g_running_child_pid = -1;

static void handle_reload_signal(int sig) {
    (void)sig;
    g_reload_requested = 1;
    if (g_running_child_pid > 0) {
        kill(-g_running_child_pid, SIGTERM);
    }
}

static void handle_boot_now_signal(int sig) {
    (void)sig;
    g_boot_now_requested = 1;
    if (g_running_child_pid > 0) {
        kill(-g_running_child_pid, SIGTERM);
    }
}

static double mono_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static int run_cmd(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int st;
    while (waitpid(pid, &st, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int remount_exec(const char *card);

static int systemctl_ac50(const char *verb) {
    if (verb && strcmp(verb, "stop") == 0) {
        char *k_argv[] = {"systemctl", "kill", "-s", "KILL", "ac50.service", NULL};
        run_cmd(k_argv);
        char *ka_argv[] = {"killall", "-9", "MPC Sample", NULL};
        run_cmd(ka_argv);
    }
    char *argv[] = {"systemctl", (char *)verb, "ac50.service", NULL};
    return run_cmd(argv);
}


/* Mount the in-memory Shift+Pad 16 loader, then ac50 can start the stock app.
   The script lives next to this menu. A missing script still starts stock. */
static void draw_pixels(struct screen *scr, uint32_t *back, uint32_t *front, struct surface *midi, struct ui *u);

/* 1 when a bind mount already covers this path. mountinfo field 5 is the point. */
static int mount_point_is(const char *target) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    char line[4096];
    size_t n;
    int found = 0;
    if (!f || !target) {
        if (f) fclose(f);
        return 0;
    }
    n = strlen(target);
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        int i;
        for (i = 0; i < 4; i++) {
            while (*p && *p != ' ') p++;
            if (*p == ' ') p++;
        }
        if (strncmp(p, target, n) == 0 && (p[n] == ' ' || p[n] == '\n' || p[n] == 0)) {
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static int read_stamp(char *out, size_t n) {
    FILE *f = fopen("/run/mpc-patch-stamp", "r");
    size_t len;
    if (!f || n < 2) {
        if (f) fclose(f);
        return -1;
    }
    if (!fgets(out, (int)n, f)) {
        fclose(f);
        return -1;
    }
    fclose(f);
    len = strlen(out);
    while (len && (out[len - 1] == '\n' || out[len - 1] == '\r')) out[--len] = 0;
    return out[0] ? 0 : -1;
}

static int stamp_equals(const char *dir) {
    char got[512];
    if (!dir || !dir[0] || read_stamp(got, sizeof got) != 0) return 0;
    return strcmp(got, dir) == 0;
}

/* 1 when Stock's patch directory is the one mounted on launch-ac50.
   A Boot Edit entry leaves a different stamp, so Stock has to replace it. */
static int stock_ready(const char *card) {
    char script[512], dir[512];
    int ns, nd;
    if (!card || !card[0]) return 1;
    ns = snprintf(script, sizeof script, "%s/autorun/system/boot-menu-apply.sh", card);
    if (ns < 0 || (size_t)ns >= sizeof script || access(script, R_OK) != 0) return 1;
    if (!mount_point_is("/usr/bin/launch-ac50")) return 0;
    nd = snprintf(dir, sizeof dir, "%s/autorun/system/patches", card);
    if (nd < 0 || (size_t)nd >= sizeof dir) return 0;
    return stamp_equals(dir);
}

static void show_loading(struct screen *scr, struct surface *midi, int theme) {
    uint32_t *back = calloc((size_t)UI_W * UI_H, sizeof(uint32_t));
    uint32_t *front = calloc((size_t)UI_W * UI_H, sizeof(uint32_t));
    if (!back || !front) {
        free(back);
        free(front);
        return;
    }
    if (midi && midi->fd >= 0) surface_all_off(midi);
    struct ui u = {.px = back};
    boot_draw_loading(&u, theme);
    draw_pixels(scr, back, front, midi, &u);
    free(back);
    free(front);
}

static void mount_patched_stock(const char *card) {
    char script[512];
    if (!card || !card[0]) return;
    int n = snprintf(script, sizeof script, "%s/autorun/system/boot-menu-apply.sh", card);
    if (n < 0 || (size_t)n >= sizeof script) return;
    if (access(script, R_OK) != 0) return;
    remount_exec(card);
    char *argv[] = {"sh", script, NULL};
    int rc = run_cmd(argv);
    if (rc != 0) fprintf(stderr, "stock patch failed (%d)\n", rc);
}

static void restore_stock_if_stopped(const char *card) {
    struct screen scr = {.fd = -1};
    int opened = 0;
    if (!stopped_ac50) return;
    if (!stock_ready(card) && screen_open(&scr) == 0) {
        show_loading(&scr, NULL, theme_load(card));
        opened = 1;
    }
    mount_patched_stock(card);
    if (opened) screen_close(&scr);
    systemctl_ac50("start");
    stopped_ac50 = 0;
}

/* Quiet check used while ac50 is stopping. surface_open logs every failure. */
static int probe_midi(void) {
    char link[64];
    ssize_t n = readlink("/dev/snd/by-path/platform-fe670000.serial", link, sizeof link - 1);
    if (n <= 0) return -1;
    link[n] = 0;
    char *c = strstr(link, "controlC");
    if (!c) return -1;
    int card = 0;
    for (c += 8; *c >= '0' && *c <= '9'; c++) card = card * 10 + (*c - '0');
    char path[40];
    snprintf(path, sizeof path, "/dev/snd/midiC%dD0", card);
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

static int open_surface_claim(struct surface *midi) {
    if (surface_open(midi) == 0) return 0;
    /* surface_open logs, and that can clear errno, so probe the port again. */
    if (probe_midi() == 0) return surface_open(midi);
    if (errno != EBUSY) return -1;
    if (systemctl_ac50("stop") != 0) return -1;
    stopped_ac50 = 1;
    for (int i = 0; i < 40; i++) {
        usleep(50 * 1000);
        if (probe_midi() == 0) return surface_open(midi);
        if (errno != EBUSY) return -1;
    }
    return -1;
}

static void button_led(struct surface *s, int note, int value) {
    unsigned char m[3] = {0xB0, (unsigned char)(note & 0x7f), (unsigned char)(value & 0x7f)};
    if (!s || s->fd < 0) return;
    size_t off = 0;
    while (off < sizeof m) {
        ssize_t w = write(s->fd, m + off, sizeof m - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (w == 0) return;
        off += (size_t)w;
    }
}

static int read_frame(struct surface *midi, struct parser *p, struct input_dev *dev, struct input *in) {
    input_begin(in, dev);
    unsigned char buf[256];
    int any = 0;
    for (;;) {
        ssize_t n = read(midi->fd, buf, sizeof buf);
        if (n < 0) return errno == EAGAIN || errno == EINTR ? any : -1;
        if (n == 0) return any;
        any = 1;
        for (ssize_t i = 0; i < n; i++)
            if (parser_feed(p, buf[i])) input_push(dev, in, p->msg);
    }
}

static int poll_ms(int wake, double started) {
    if (wake <= 0) return 200;
    int spent = (int)(mono_ms() - started);
    if (spent < 0) spent = 0;
    if (spent >= wake) return 0;
    return wake - spent;
}

static void present(struct screen *scr, const uint32_t *front, int rects[][4], int n) {
    for (int i = 0; i < n; i++) {
        int x0 = rects[i][0], y0 = rects[i][1], x1 = rects[i][2], y1 = rects[i][3];
        for (int y = y0; y < y1; y++)
            memcpy(scr->px + y * scr->stride + x0, front + y * UI_W + x0, (size_t)(x1 - x0) * 4);
    }
    screen_flush(scr, rects, n);
}

static void draw_pixels(struct screen *scr, uint32_t *back, uint32_t *front, struct surface *midi, struct ui *u) {
    int rects[UI_H][4];
    int n = ui_changes(back, front, rects);
    if (scr->px) present(scr, front, rects, n);
    for (int i = 0; i < u->nled; i++)
        surface_pad(midi, u->led[i].index, u->led[i].r, u->led[i].g, u->led[i].b);
}

static void draw_prompt(struct screen *scr, uint32_t *back, uint32_t *front, struct surface *midi, int theme) {
    struct ui u = {.px = back};
    boot_draw_prompt(&u, theme);
    draw_pixels(scr, back, front, midi, &u);
}

static int draw_menu_frame(struct screen *scr, uint32_t *back, uint32_t *front, struct surface *midi,
                           const struct input *in, struct boot_ui *ui) {
    struct ui u = {.px = back};
    boot_frame(&u, in, ui);
    draw_pixels(scr, back, front, midi, &u);
    return u.wake_ms;
}

static int wait_shift(struct surface *midi, struct parser *p, struct input_dev *dev) {
    button_led(midi, INPUT_BUTTON_NOTE[IN_SHIFT], 8);
    double start = mono_ms();
    int held = 0;
    while (mono_ms() - start < WINDOW_MS) {
        struct pollfd pf = {.fd = midi->fd, .events = POLLIN};
        int left = (int)(WINDOW_MS - (mono_ms() - start));
        if (left < 1) left = 1;
        if (poll(&pf, 1, left > 50 ? 50 : left) < 0 && errno != EINTR) break;
        struct input in;
        if (read_frame(midi, p, dev, &in) < 0) break;
        if (dev->shift) {
            held = 1;
            break;
        }
    }
    button_led(midi, INPUT_BUTTON_NOTE[IN_SHIFT], 0);
    return held;
}

static void card_root_of(const char *autorun, char *out, size_t n) {
    snprintf(out, n, "%s", autorun ? autorun : "");
    char *slash = strrchr(out, '/');
    if (!slash) {
        snprintf(out, n, ".");
        return;
    }
    if (slash == out) out[1] = 0;
    else *slash = 0;
}

/* Longest mount point that contains path. Spaces in /proc/mounts are octal escapes. */
static int mount_point(const char *path, char *out, size_t n) {
    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return -1;
    char line[1024];
    size_t best = 0;
    out[0] = 0;
    while (fgets(line, sizeof line, f)) {
        char *fs = line, *mnt = strchr(line, ' ');
        if (!mnt) continue;
        *mnt++ = 0;
        char *end = strchr(mnt, ' ');
        if (!end) continue;
        *end = 0;
        (void)fs;
        char plain[512];
        size_t j = 0;
        for (size_t i = 0; mnt[i] && j + 1 < sizeof plain; i++) {
            if (mnt[i] == '\\' && mnt[i + 1] >= '0' && mnt[i + 1] <= '7') {
                int v = 0, k;
                for (k = 0; k < 3 && mnt[i + 1 + k] >= '0' && mnt[i + 1 + k] <= '7'; k++)
                    v = v * 8 + (mnt[i + 1 + k] - '0');
                plain[j++] = (char)v;
                i += k;
            } else plain[j++] = mnt[i];
        }
        plain[j] = 0;
        size_t L = strlen(plain);
        if (L < best || L > strlen(path)) continue;
        if (strncmp(path, plain, L) != 0) continue;
        if (path[L] != 0 && path[L] != '/') continue;
        best = L;
        snprintf(out, n, "%s", plain);
    }
    fclose(f);
    return best ? 0 : -1;
}

static int remount_exec(const char *card) {
    char target[512];
    if (mount_point(card, target, sizeof target) != 0) return -1;
    char *argv[] = {"mount", "-o", "remount,exec", target, NULL};
    return run_cmd(argv);
}

/* One file, replaced each launch. Only a failed run appends its exit line. */
static void app_log(const char *fmt, ...) {
    FILE *f = fopen("/tmp/mpc-boot-app.log", "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
}

static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char buf[8192];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            rc = -1;
            break;
        }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    fclose(out);
    if (rc) unlink(dst);
    else chmod(dst, 0755);
    return rc;
}

static void resolve_run(const char *card, const char *run, char *out, size_t n) {
    if (run[0] == '/') snprintf(out, n, "%s", run);
    else snprintf(out, n, "%s/%s", card, run);
}

static int entry_patch_dir(const char *card, const struct menu_entry *e, char *out, size_t n) {
    char script[512];
    char *slash;
    int wr;
    if (e->run[0] == '/') wr = snprintf(script, sizeof script, "%s", e->run);
    else wr = snprintf(script, sizeof script, "%s/%s", e->base[0] ? e->base : card, e->run);
    if (wr < 0 || (size_t)wr >= sizeof script) return -1;
    slash = strrchr(script, '/');
    if (!slash || slash == script) return -1;
    *slash = 0;
    wr = snprintf(out, n, "%s/patches", script);
    return wr < 0 || (size_t)wr >= n ? -1 : 0;
}

static int run_boot_script(const char *card, const struct menu_entry *e) {
    char path[512];
    char *argv[3];
    resolve_run(e->base[0] ? e->base : card, e->run, path, sizeof path);
    remount_exec(card);
    argv[0] = "sh";
    argv[1] = path;
    argv[2] = NULL;
    return run_cmd(argv);
}

/* Run one entry. Stock and Boot Edit entries start ac50 and the caller should exit.
   Other apps return here. 2 means the panel and screen are still ours. */
static int launch(struct surface *midi, struct screen *scr, const char *card, const struct menu_entry *e) {
    if (e->stock || e->boot_ac50) {
        int ready = 0;
        if (e->stock) ready = stock_ready(card);
        else {
            char dir[512];
            ready = entry_patch_dir(card, e, dir, sizeof dir) == 0 && stamp_equals(dir);
        }
        if (!ready) show_loading(scr, midi, theme_load(card));
        if (e->stock) mount_patched_stock(card);
        else if (run_boot_script(card, e) != 0) {
            fprintf(stderr, "boot entry failed\n");
            return 2;
        }
        surface_all_off(midi);
        surface_close(midi);
        screen_close(scr);
        systemctl_ac50("start");
        stopped_ac50 = 0;
        return 1;
    }
    if (!stopped_ac50 && systemctl_ac50("stop") == 0) stopped_ac50 = 1;

    char path[512];
    resolve_run(e->base[0] ? e->base : card, e->run, path, sizeof path);
    char execpath[512];
    snprintf(execpath, sizeof execpath, "%s", path);
    if (remount_exec(card) != 0) {
        if (copy_file(path, "/run/mpc-boot-app") != 0) {
            fprintf(stderr, "cannot run %s\n", path);
            app_log("cannot run %s\n", path);
            return 2;
        }
        snprintf(execpath, sizeof execpath, "/run/mpc-boot-app");
    }

    char argbuf[MENU_ARGS];
    char *av[18];
    av[0] = execpath;
    int na = menu_split_args(e->args, argbuf, sizeof argbuf, av + 1, 16);
    av[1 + na] = NULL;

    surface_all_off(midi);
    surface_close(midi);
    screen_close(scr);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
        setpgid(0, 0);
        int logfd = open("/tmp/mpc-boot-app.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (logfd >= 0) {
            dup2(logfd, STDOUT_FILENO);
            dup2(logfd, STDERR_FILENO);
            if (logfd > STDERR_FILENO) close(logfd);
        }
        if (e->base[0]) chdir(e->base);
        execv(execpath, av);
        perror(execpath);
        _exit(127);
    }
    g_running_child_pid = pid;
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
        if (g_reload_requested || g_boot_now_requested) {
            kill(-pid, SIGTERM);
            usleep(20000);
            kill(-pid, SIGKILL);
        }
    }
    g_running_child_pid = -1;
    kill(-pid, SIGTERM);
    while (waitpid(-pid, NULL, WNOHANG) > 0) {}

    int code = -1;
    if (WIFEXITED(st)) code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) code = 128 + WTERMSIG(st);
    if (code != 0) app_log("exit %d\n", code);
    return 0;
}

static int reopen_after_app(struct surface *midi, struct screen *scr) {
    if (screen_open(scr) != 0) return -1;
    if (open_surface_claim(midi) != 0) {
        screen_close(scr);
        return -1;
    }
    if (midi->fd >= 0) {
        unsigned char drain[256];
        while (read(midi->fd, drain, sizeof drain) > 0) {}
    }
    return 0;
}

static void print_menu(const struct menu *m, int sel) {
    printf("menu:\n");
    printf("%c %s\n", sel == 0 ? '>' : ' ', "USB storage (SD card)");
    for (int i = 0; i < m->n; i++) printf("%c %s\n", (1 + i) == sel ? '>' : ' ', m->items[i].title);
    fflush(stdout);
}

static int persist(struct boot_ui *ui, const char *card) {
    if (ui->action == BOOT_ACT_SET_DEFAULT) {
        if (menu_set_default(card, ui->action_id) != 0) {
            ui->default_index = menu_default_index(ui->menu, card);
            return -1;
        }
    } else if (ui->action == BOOT_ACT_SAVE_THEME) {
        if (theme_save(card, ui->action_index) != 0) {
            int t = theme_load(card);
            ui->saved_theme = t;
            ui->theme = t;
            ui->theme_ls.cursor = t;
            return -1;
        }
    }
    return 0;
}

static void release_devices(struct surface *midi, struct screen *scr) {
    if (midi->fd >= 0) {
        surface_all_off(midi);
        surface_close(midi);
    }
    if (scr->px || scr->fd >= 0) screen_close(scr);
}

static const char *boot_now_file_path(void) {
    const char *env = getenv("MPC_BOOT_NOW_FILE");
    return (env && env[0]) ? env : "/run/mpc-boot-now";
}

static const char *boot_pid_file_path(void) {
    const char *env = getenv("MPC_BOOT_PID_FILE");
    return (env && env[0]) ? env : "/run/mpc-boot-thingy.pid";
}

static void write_pid_file(void) {
    FILE *f = fopen(boot_pid_file_path(), "w");
    if (f) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
}

static void remove_pid_file(void) {
    unlink(boot_pid_file_path());
}

static int check_boot_now_target(char *out_id, size_t n) {
    out_id[0] = 0;
    const char *path = boot_now_file_path();
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    if (fgets(out_id, (int)n, f)) {
        size_t l = strlen(out_id);
        while (l > 0 && (out_id[l - 1] == '\r' || out_id[l - 1] == '\n' || out_id[l - 1] == ' ')) {
            out_id[--l] = 0;
        }
    }
    fclose(f);
    unlink(path);
    return out_id[0] ? 1 : 0;
}

static void flush_and_drop_caches(const char *card) {
    sync();
    FILE *f = fopen("/proc/sys/vm/drop_caches", "w");
    if (f) {
        fputs("3\n", f);
        fclose(f);
    }
    remount_exec(card);
}

static void update_run_binary(const char *card) {
    char src[512];
    if (snprintf(src, sizeof src, "%s/autorun/mpc-boot-thingy", card) >= (int)sizeof src) return;
    if (access(src, R_OK) == 0) {
        copy_file(src, "/run/mpc-boot-thingy");
    }
}

static void reload_and_reexec(struct surface *midi, struct screen *scr, const char *card, int argc, char **argv) {
    flush_and_drop_caches(card);
    update_run_binary(card);
    release_devices(midi, scr);
    remove_pid_file();

    char *new_av[32];
    int na = 0;
    new_av[na++] = argv[0];
    new_av[na++] = "--menu";
    for (int i = 1; i < argc && na < 30; i++) {
        if (strcmp(argv[i], "--menu") != 0) {
            new_av[na++] = argv[i];
        }
    }
    new_av[na] = NULL;

    execv(new_av[0], new_av);
    if (strcmp(new_av[0], "/run/mpc-boot-thingy") != 0 && access("/run/mpc-boot-thingy", X_OK) == 0) {
        new_av[0] = "/run/mpc-boot-thingy";
        execv(new_av[0], new_av);
    }
    perror("execv reload");
}

static int wants_menu(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--menu") == 0) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handle_reload_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);

    struct sigaction sa_boot;
    memset(&sa_boot, 0, sizeof sa_boot);
    sa_boot.sa_handler = handle_boot_now_signal;
    sigemptyset(&sa_boot.sa_mask);
    sigaction(SIGUSR2, &sa_boot, NULL);

    write_pid_file();

    int open_menu = wants_menu(argc, argv);
    const char *autorun = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--menu") == 0) continue;
        autorun = argv[i];
        break;
    }
    char card[512];
    card_root_of(autorun, card, sizeof card);

    struct surface midi = {.fd = -1};
    struct screen scr = {.fd = -1};
    if (open_surface_claim(&midi) != 0) {
        restore_stock_if_stopped(card);
        return 1;
    }
    if (screen_open(&scr) != 0) {
        release_devices(&midi, &scr);
        restore_stock_if_stopped(card);
        return 1;
    }

    struct menu menu;
    menu_load(&menu, card);
    int theme = theme_load(card);
    int def = menu_default_index(&menu, card);

    uint32_t *back = calloc((size_t)UI_W * UI_H, sizeof(uint32_t));
    uint32_t *front = calloc((size_t)UI_W * UI_H, sizeof(uint32_t));
    if (!back || !front) {
        free(back);
        free(front);
        release_devices(&midi, &scr);
        restore_stock_if_stopped(card);
        return 1;
    }

    struct parser parser = {0};
    struct input_dev dev = {0};
    char boot_now_id[MENU_ID] = {0};
    int has_boot_now = check_boot_now_target(boot_now_id, sizeof boot_now_id);
    int held = 1;
    if (!open_menu && !has_boot_now) {
        draw_prompt(&scr, back, front, &midi, theme);
        printf("prompt: hold SHIFT for boot thingy\n");
        fflush(stdout);
        held = wait_shift(&midi, &parser, &dev);
    }
    if (has_boot_now) {
        int bsel = -1;
        for (int i = 0; i < menu.n; i++) {
            if (strcmp(menu.items[i].id, boot_now_id) == 0) {
                bsel = i;
                break;
            }
        }
        if (bsel >= 0) {
            int ran = launch(&midi, &scr, card, &menu.items[bsel]);
            if (ran == 1) {
                free(back);
                free(front);
                return 0;
            }
            if (ran != 2) {
                memset(front, 0, (size_t)UI_W * UI_H * sizeof(uint32_t));
                if (reopen_after_app(&midi, &scr) != 0) {
                    release_devices(&midi, &scr);
                    restore_stock_if_stopped(card);
                    free(back);
                    free(front);
                    return 1;
                }
                parser = (struct parser){0};
                dev = (struct input_dev){0};
            }
        }
    } else if (!held) {
        if (def <= 0) {
            if (!stock_ready(card)) show_loading(&scr, &midi, theme);
            mount_patched_stock(card);
            release_devices(&midi, &scr);
            if (stopped_ac50) {
                systemctl_ac50("start");
                stopped_ac50 = 0;
            }
            free(back);
            free(front);
            return 0;
        }
        int ran = launch(&midi, &scr, card, &menu.items[def]);
        if (ran == 1) {
            free(back);
            free(front);
            return 0;
        }
        if (ran != 2) {
            memset(front, 0, (size_t)UI_W * UI_H * sizeof(uint32_t));
            if (reopen_after_app(&midi, &scr) != 0) {
                release_devices(&midi, &scr);
                restore_stock_if_stopped(card);
                free(back);
                free(front);
                return 1;
            }
            parser = (struct parser){0};
            dev = (struct input_dev){0};
        }
    }

    struct boot_ui ui;
    struct boot_patch catalog[BOOT_PATCH_MAX];
    int npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
    boot_ui_init(&ui, &menu, theme, def);
    boot_ui_set_catalog(&ui, catalog, npatches);
    struct input quiet;
    input_begin(&quiet, &dev);
    double started = mono_ms();
    int wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
    print_menu(&menu, ui.apps_ls.cursor);

    int rc = 1;
    for (;;) {
        if (g_reload_requested) {
            g_reload_requested = 0;
            show_loading(&scr, &midi, theme_load(card));
            reload_and_reexec(&midi, &scr, card, argc, argv);
            if (reopen_after_app(&midi, &scr) != 0) break;
            menu_load(&menu, card);
            theme = theme_load(card);
            def = menu_default_index(&menu, card);
            boot_ui_init(&ui, &menu, theme, def);
            npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
            boot_ui_set_catalog(&ui, catalog, npatches);
            boot_ui_notice(&ui, "Reloaded menu");
        }
        if (g_boot_now_requested || access(boot_now_file_path(), F_OK) == 0) {
            g_boot_now_requested = 0;
            char b_id[MENU_ID] = {0};
            if (check_boot_now_target(b_id, sizeof b_id)) {
                menu_load(&menu, card);
                int sel = -1;
                for (int i = 0; i < menu.n; i++) {
                    if (strcmp(menu.items[i].id, b_id) == 0) {
                        sel = i;
                        break;
                    }
                }
                if (sel >= 0) {
                    int ran = launch(&midi, &scr, card, &menu.items[sel]);
                    if (ran == 1) {
                        rc = 0;
                        break;
                    }
                    if (ran != 2) {
                        memset(front, 0, (size_t)UI_W * UI_H * sizeof(uint32_t));
                        if (reopen_after_app(&midi, &scr) != 0) break;
                        parser = (struct parser){0};
                        dev = (struct input_dev){0};
                    }
                    flush_and_drop_caches(card);
                    menu_load(&menu, card);
                    npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
                    boot_ui_set_catalog(&ui, catalog, npatches);
                    ui.default_index = menu_default_index(&menu, card);
                    int dsel = (ui.default_index >= 0 && ui.default_index < menu.n) ? ui.default_index : 0;
                    ui.apps_ls.cursor = 1 + dsel;

                    input_begin(&quiet, &dev);
                    started = mono_ms();
                    wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
                    continue;
                }
            }
        }
        struct pollfd pf = {.fd = midi.fd, .events = POLLIN};
        int pr = poll(&pf, 1, poll_ms(wake, started));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) {
            if (wake <= 0) continue;
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        struct input in;
        int got = read_frame(&midi, &parser, &dev, &in);
        if (got < 0) break;
        if (!got) continue;
        started = mono_ms();
        wake = draw_menu_frame(&scr, back, front, &midi, &in, &ui);

        if (ui.action == BOOT_ACT_LAUNCH) {
            int sel = ui.action_index;
            if (sel < 0 || sel >= menu.n) continue;
            int ran = launch(&midi, &scr, card, &menu.items[sel]);
            if (ran == 1) {
                rc = 0;
                break;
            }
            if (ran != 2) {
                memset(front, 0, (size_t)UI_W * UI_H * sizeof(uint32_t));
                if (reopen_after_app(&midi, &scr) != 0) break;
                parser = (struct parser){0};
                dev = (struct input_dev){0};
            }
            flush_and_drop_caches(card);
            menu_load(&menu, card);
            npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
            boot_ui_set_catalog(&ui, catalog, npatches);
            ui.default_index = menu_default_index(&menu, card);
            int dsel = (ui.default_index >= 0 && ui.default_index < menu.n) ? ui.default_index : 0;
            ui.apps_ls.cursor = 1 + dsel;

            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_RELOAD) {
            ui.action = BOOT_ACT_NONE;
            show_loading(&scr, &midi, theme_load(card));
            reload_and_reexec(&midi, &scr, card, argc, argv);
            if (reopen_after_app(&midi, &scr) != 0) break;
            menu_load(&menu, card);
            theme = theme_load(card);
            def = menu_default_index(&menu, card);
            boot_ui_init(&ui, &menu, theme, def);
            npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
            boot_ui_set_catalog(&ui, catalog, npatches);
            boot_ui_notice(&ui, "Reloaded menu");
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_SAVE_BOOT) {
            const char *ids[BOOT_PATCH_MAX];
            char out_id[MENU_ID];
            int i, rc;
            for (i = 0; i < ui.save_npatches && i < BOOT_PATCH_MAX; i++) ids[i] = ui.save_patches[i];
            rc = boot_edit_save(card, ui.save_id[0] ? ui.save_id : NULL, ui.save_title[0] ? ui.save_title : NULL, ids,
                                ui.save_npatches, out_id, sizeof out_id);
            if (rc == 0) {
                menu_load(&menu, card);
                ui.default_index = menu_default_index(&menu, card);
                boot_ui_saved(&ui);
            } else if (rc == BOOT_EDIT_BAD_PATCH) {
                boot_ui_notice(&ui, "That patch is not on the card");
            } else if (rc == BOOT_EDIT_BAD_TITLE) {
                boot_ui_notice(&ui, "Title is not valid");
            } else if (rc == BOOT_EDIT_BAD_ID) {
                boot_ui_notice(&ui, "That entry cannot be edited");
            } else {
                boot_ui_notice(&ui, boot_edit_last_error());
            }
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_UPDATE_ENTRY) {
            ui.action = BOOT_ACT_NONE;
            int urc = boot_entry_update(card, ui.action_id, ui.save_title[0] ? ui.save_title : NULL, ui.save_args);
            if (urc == 0) {
                flush_and_drop_caches(card);
                menu_load(&menu, card);
                ui.default_index = menu_default_index(&menu, card);
                boot_ui_notice(&ui, "Entry updated");
            } else {
                boot_ui_notice(&ui, "Could not update entry");
            }
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_DELETE_ENTRY) {
            ui.action = BOOT_ACT_NONE;
            int drc = boot_entry_delete(card, ui.action_id);
            if (drc == 0) {
                flush_and_drop_caches(card);
                menu_load(&menu, card);
                npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
                boot_ui_set_catalog(&ui, catalog, npatches);
                ui.default_index = menu_default_index(&menu, card);
                boot_ui_notice(&ui, "Deleted");
            } else {
                boot_ui_notice(&ui, "Could not delete");
            }
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_START_USB_STORAGE) {
            ui.action = BOOT_ACT_NONE;
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);

            char err_buf[128] = {0};
            int rc_st = usb_storage_start(err_buf, sizeof err_buf);
            if (rc_st == 0) {
                ui.storage_status = USB_STORAGE_STATUS_ACTIVE;
                ui.storage_err[0] = 0;
            } else {
                ui.storage_status = USB_STORAGE_STATUS_ERROR;
                snprintf(ui.storage_err, sizeof ui.storage_err, "%s", err_buf[0] ? err_buf : "Failed to start");
            }
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_STOP_USB_STORAGE) {
            ui.action = BOOT_ACT_NONE;
            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);

            char err_buf[128] = {0};
            usb_storage_stop(err_buf, sizeof err_buf);
            flush_and_drop_caches(card);
            menu_load(&menu, card);
            npatches = boot_edit_list(card, catalog, BOOT_PATCH_MAX);
            boot_ui_set_catalog(&ui, catalog, npatches);
            ui.default_index = menu_default_index(&menu, card);
            ui.storage_status = USB_STORAGE_STATUS_IDLE;
            ui.storage_err[0] = 0;
            int dsel = (ui.default_index >= 0 && ui.default_index < menu.n) ? ui.default_index : 0;
            ui.apps_ls.cursor = 1 + dsel;
            ui.screen = BOOT_SCREEN_APPS;

            input_begin(&quiet, &dev);
            started = mono_ms();
            wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_SET_DEFAULT || ui.action == BOOT_ACT_SAVE_THEME) {
            int failed = persist(&ui, card) != 0;
            if (failed || ui.action == BOOT_ACT_SET_DEFAULT) {
                input_begin(&quiet, &dev);
                started = mono_ms();
                wake = draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            }
        }
    }

    if (ui.storage_status == USB_STORAGE_STATUS_ACTIVE) {
        usb_storage_stop(NULL, 0);
    }

    remove_pid_file();
    if (rc != 0) {
        release_devices(&midi, &scr);
        restore_stock_if_stopped(card);
    }
    free(back);
    free(front);
    return rc;
}
