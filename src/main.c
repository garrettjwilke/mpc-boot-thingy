/* mpc-boot-thingy: after the splash, holding Shift opens the menu.
   Waiting starts the saved default, or Stock when none is saved. */
#include "boot_ui.h"
#include "input.h"
#include "menu.h"
#include "screen.h"
#include "surface.h"
#include "theme.h"
#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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

static void restore_stock_if_stopped(void) {
    if (stopped_ac50) systemctl_ac50("start");
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

static void present(struct screen *scr, const uint32_t *front, int rects[][4], int n) {
    for (int i = 0; i < n; i++) {
        int x0 = rects[i][0], y0 = rects[i][1], x1 = rects[i][2], y1 = rects[i][3];
        for (int y = y0; y < y1; y++)
            memcpy(scr->px + y * scr->stride + x0, front + y * UI_W + x0, (size_t)(x1 - x0) * 4);
        screen_flush(scr, x0, y0, x1, y1);
    }
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

static void draw_menu_frame(struct screen *scr, uint32_t *back, uint32_t *front, struct surface *midi,
                            const struct input *in, struct boot_ui *ui) {
    struct ui u = {.px = back};
    boot_frame(&u, in, ui);
    draw_pixels(scr, back, front, midi, &u);
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

/* Run one entry. Stock starts ac50 and the caller should exit. Other apps return here.
   2 means the panel and screen are still ours. */
static int launch(struct surface *midi, struct screen *scr, const char *card, const struct menu_entry *e) {
    if (e->stock) {
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

static int reopen_after_app(struct surface *midi, struct screen *scr) {
    if (screen_open(scr) != 0) return -1;
    if (open_surface_claim(midi) != 0) {
        screen_close(scr);
        return -1;
    }
    return 0;
}

static void print_menu(const struct menu *m, int sel) {
    printf("menu:\n");
    for (int i = 0; i < m->n; i++) printf("%c %s\n", i == sel ? '>' : ' ', m->items[i].title);
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

int main(int argc, char **argv) {
    const char *autorun = argc > 1 ? argv[1] : NULL;
    char card[512];
    card_root_of(autorun, card, sizeof card);

    struct surface midi = {.fd = -1};
    struct screen scr = {.fd = -1};
    if (open_surface_claim(&midi) != 0) {
        restore_stock_if_stopped();
        return 1;
    }
    if (screen_open(&scr) != 0) {
        release_devices(&midi, &scr);
        restore_stock_if_stopped();
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
        restore_stock_if_stopped();
        return 1;
    }

    draw_prompt(&scr, back, front, &midi, theme);
    printf("prompt: hold SHIFT for boot thingy\n");
    fflush(stdout);

    struct parser parser = {0};
    struct input_dev dev = {0};
    int held = wait_shift(&midi, &parser, &dev);
    if (!held) {
        if (def <= 0) {
            release_devices(&midi, &scr);
            restore_stock_if_stopped();
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
                restore_stock_if_stopped();
                free(back);
                free(front);
                return 1;
            }
            parser = (struct parser){0};
            dev = (struct input_dev){0};
        }
    }

    struct boot_ui ui;
    boot_ui_init(&ui, &menu, theme, def);
    struct input quiet;
    input_begin(&quiet, &dev);
    draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
    print_menu(&menu, ui.apps_ls.cursor);

    int rc = 1;
    for (;;) {
        struct pollfd pf = {.fd = midi.fd, .events = POLLIN};
        int pr = poll(&pf, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        struct input in;
        int got = read_frame(&midi, &parser, &dev, &in);
        if (got < 0) break;
        if (!got) continue;
        draw_menu_frame(&scr, back, front, &midi, &in, &ui);

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
            input_begin(&quiet, &dev);
            draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            continue;
        }
        if (ui.action == BOOT_ACT_SET_DEFAULT || ui.action == BOOT_ACT_SAVE_THEME) {
            int failed = persist(&ui, card) != 0;
            if (failed || ui.action == BOOT_ACT_SET_DEFAULT) {
                input_begin(&quiet, &dev);
                draw_menu_frame(&scr, back, front, &midi, &quiet, &ui);
            }
        }
    }

    if (rc != 0) {
        release_devices(&midi, &scr);
        restore_stock_if_stopped();
    }
    free(back);
    free(front);
    return rc;
}
