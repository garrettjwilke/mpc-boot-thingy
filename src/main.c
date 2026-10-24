/* mpc-boot-thingy: after the splash, SHIFT opens the menu. Waiting starts the stock app. */
#include "draw.h"
#include "menu.h"
#include "screen.h"
#include "surface.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NOTE_SHIFT 49
#define NOTE_PADBANK 35
#define NOTE_ENCODER 111
#define CC_ENCODER 100
#define WINDOW_MS 1000

static int stopped_ac50;

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

static int systemctl_ac50(const char *verb) {
    char *argv[] = {"systemctl", (char *)verb, "ac50.service", NULL};
    return run_cmd(argv);
}

static int open_midi_raw(char *path, size_t path_n) {
    path[0] = 0;
    int card = card_by_path("/dev/snd/by-path/platform-fe670000.serial");
    if (card < 0) {
        fputs("control surface not found\n", stderr);
        return -1;
    }
    snprintf(path, path_n, "/dev/snd/midiC%dD0", card);
    return open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

/* Stop the stock app once so the panel port can be opened. */
static int open_midi_claim(void) {
    char path[40];
    int fd = open_midi_raw(path, sizeof path);
    if (fd >= 0) return fd;
    if (!path[0]) return -1;
    if (errno != EBUSY) {
        fprintf(stderr, "%s: %s\n", path, strerror(errno));
        return -1;
    }
    if (systemctl_ac50("stop") != 0) return -1;
    stopped_ac50 = 1;
    for (int i = 0; i < 40; i++) {
        usleep(50 * 1000);
        fd = open_midi_raw(path, sizeof path);
        if (fd >= 0) return fd;
        if (errno != EBUSY) {
            fprintf(stderr, "%s: %s\n", path, strerror(errno));
            return -1;
        }
    }
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return -1;
}

static void restore_stock_if_stopped(void) {
    if (stopped_ac50) systemctl_ac50("start");
}

struct listen {
    int shift;
    int combo;
    int sel;
    int n;
    int confirm;
    int moved;
};

static void on_shift(const unsigned char *m, struct listen *k) {
    int st = m[0] & 0xF0, ch = (m[0] & 0x0F) + 1;
    if (ch != 1) return;
    if (st == 0x90 && m[1] == NOTE_SHIFT && m[2] > 0) k->combo = 1;
}

static void on_menu(const unsigned char *m, struct listen *k) {
    int st = m[0] & 0xF0, ch = (m[0] & 0x0F) + 1;
    if (ch != 1 || k->n < 1) return;
    if (st == 0xB0 && m[1] == CC_ENCODER) {
        int step = m[2] > 64 ? (int)m[2] - 128 : (int)m[2];
        if (step) {
            int dir = step > 0 ? 1 : -1;
            k->sel = (k->sel + dir + k->n) % k->n;
            k->moved = 1;
        }
    } else if (st == 0x90 && m[1] == NOTE_ENCODER && m[2] > 0) {
        k->confirm = 1;
    }
}

static int pump(int fd, struct parser *p, struct listen *k, void (*fn)(const unsigned char *, struct listen *)) {
    unsigned char buf[128];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) return errno == EAGAIN || errno == EINTR ? 0 : -1;
        if (n == 0) return 0;
        for (ssize_t i = 0; i < n; i++)
            if (feed(p, buf[i])) fn(p->msg, k);
    }
}

static void show_prompt(void) {
    if (!hw_fb) return;
    draw_prompt(hw_fb, hw_stride, 320, 240);
    hw_screen_flush(0, 0, 320, 240);
}

static int wait_shift(int fd) {
    struct parser p = {0};
    struct listen k = {0};
    led(fd, NOTE_SHIFT, 8);
    double start = mono_ms();
    while (mono_ms() - start < WINDOW_MS) {
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        int left = (int)(WINDOW_MS - (mono_ms() - start));
        if (left < 1) left = 1;
        if (poll(&pf, 1, left > 50 ? 50 : left) < 0 && errno != EINTR) break;
        if (pump(fd, &p, &k, on_shift) < 0) break;
        if (k.combo) break;
    }
    led(fd, NOTE_SHIFT, 0);
    return k.combo;
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

static void show(const struct menu *m, int sel) {
    if (hw_fb) {
        draw_menu(hw_fb, hw_stride, 320, 240, m, sel);
        hw_screen_flush(0, 0, 320, 240);
    }
    printf("menu:\n");
    for (int i = 0; i < m->n; i++) printf("%c %s\n", i == sel ? '>' : ' ', m->items[i].title);
    fflush(stdout);
}

/* Run one entry. Stock starts ac50 and the caller should exit. Other apps return here. */
static int launch(int *fd, const char *card, const struct menu_entry *e) {
    if (e->stock) {
        if (*fd >= 0) {
            all_off(*fd);
            close(*fd);
            *fd = -1;
        }
        hw_screen_close();
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
            return 2; /* panel and screen are still ours */
        }
        snprintf(execpath, sizeof execpath, "/run/mpc-boot-app");
    }

    char argbuf[MENU_ARGS];
    char *av[18];
    av[0] = execpath;
    int na = menu_split_args(e->args, argbuf, sizeof argbuf, av + 1, 16);
    av[1 + na] = NULL;

    if (*fd >= 0) {
        all_off(*fd);
        close(*fd);
        *fd = -1;
    }
    hw_screen_close();

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 0;
    }
    if (pid == 0) {
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
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    int code = -1;
    if (WIFEXITED(st)) code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) code = 128 + WTERMSIG(st);
    if (code != 0) app_log("exit %d\n", code);
    return 0;
}

static int reopen_after_app(void) {
    hw_screen_open();
    int fd = open_midi_claim();
    return fd;
}

int main(int argc, char **argv) {
    const char *autorun = argc > 1 ? argv[1] : NULL;
    char card[512];
    card_root_of(autorun, card, sizeof card);

    int fd = open_midi_claim();
    if (fd < 0) {
        restore_stock_if_stopped();
        return 1;
    }
    hw_screen_open();
    show_prompt();
    printf("prompt: hold SHIFT for boot thingy\n");
    fflush(stdout);
    if (!wait_shift(fd)) {
        all_off(fd);
        close(fd);
        hw_screen_close();
        restore_stock_if_stopped();
        return 0;
    }

    struct menu menu;
    menu_load(&menu, card);
    struct parser p = {0};
    struct listen k = {.n = menu.n};
    show(&menu, k.sel);

    for (;;) {
        k.n = menu.n;
        k.moved = 0;
        k.confirm = 0;
        struct pollfd pf = {.fd = fd, .events = POLLIN};
        if (poll(&pf, 1, 200) < 0 && errno != EINTR) break;
        if (pump(fd, &p, &k, on_menu) < 0) break;
        if (k.moved) show(&menu, k.sel);
        if (!k.confirm) continue;
        int ran = launch(&fd, card, &menu.items[k.sel]);
        if (ran == 1) return 0;
        if (ran == 2) {
            show(&menu, k.sel);
            continue;
        }
        fd = reopen_after_app();
        if (fd < 0) {
            hw_screen_close();
            restore_stock_if_stopped();
            return 1;
        }
        p = (struct parser){0};
        k.confirm = 0;
        show(&menu, k.sel);
    }
    if (fd >= 0) close(fd);
    hw_screen_close();
    restore_stock_if_stopped();
    return 1;
}
