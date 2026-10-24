/* Host self-test: Stock first, drop-in apps and older boot-menu files, bad files skipped. */
#include "menu.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_file(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
}

static void mkdir_p(const char *path) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    assert(mkdir(buf, 0755) == 0 || access(buf, F_OK) == 0);
}

int main(void) {
    char dir[] = "/tmp/mpc-boot-thingy-XXXXXX";
    assert(mkdtemp(dir));
    char path[512];

    snprintf(path, sizeof path, "%s/autorun/boot-menu", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/autorun/boot-menu/10-bad.entry", dir);
    write_file(path, "title=Bad\n# no run line\n");
    snprintf(path, sizeof path, "%s/autorun/boot-menu/05-skip.entry", dir);
    write_file(path, "run=apps/only\n");
    snprintf(path, sizeof path, "%s/autorun/boot-menu/30-snake.entry", dir);
    write_file(path, "\n# a game\ntitle=Snake\nrun=apps/snake\nargs=\n");

    snprintf(path, sizeof path, "%s/apps/20-lab", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/apps/20-lab/boot.entry", dir);
    write_file(path, "title=Lab firmware\nrun=mpcfw\nargs=--demo one\n");
    snprintf(path, sizeof path, "%s/apps/20-lab/notes.txt", dir);
    write_file(path, "title=Not an entry\nrun=nope\n");
    snprintf(path, sizeof path, "%s/apps/loose-binary", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/apps/loose-binary/mpcfw", dir);
    write_file(path, "no boot.entry here\n");
    snprintf(path, sizeof path, "%s/apps/90-skip", dir);
    mkdir_p(path);
    snprintf(path, sizeof path, "%s/apps/90-skip/boot.entry", dir);
    write_file(path, "title=Missing run\n");

    char lab[512];
    snprintf(lab, sizeof lab, "%s/apps/20-lab", dir);

    struct menu m;
    assert(menu_load(&m, dir) == 3);
    assert(m.items[0].stock);
    assert(strcmp(m.items[0].title, "Stock") == 0);
    assert(m.items[0].run[0] == 0);
    assert(strcmp(m.items[1].title, "Lab firmware") == 0);
    assert(strcmp(m.items[1].run, "mpcfw") == 0);
    assert(strcmp(m.items[1].base, lab) == 0);
    assert(strcmp(m.items[1].args, "--demo one") == 0);
    assert(!m.items[1].stock);
    assert(strcmp(m.items[2].title, "Snake") == 0);
    assert(strcmp(m.items[2].run, "apps/snake") == 0);
    assert(strcmp(m.items[2].base, dir) == 0);

    char buf[64];
    char *av[8];
    int n = menu_split_args(m.items[1].args, buf, sizeof buf, av, 8);
    assert(n == 2);
    assert(strcmp(av[0], "--demo") == 0 && strcmp(av[1], "one") == 0);
    assert(menu_split_args("", buf, sizeof buf, av, 8) == 0);
    assert(menu_split_args(NULL, buf, sizeof buf, av, 8) == 0);

    struct menu empty;
    assert(menu_load(&empty, NULL) == 1 && empty.items[0].stock);
    assert(menu_load(&empty, "/no/such/boot-menu") == 1 && empty.items[0].stock);

    puts("selftest ok");
    return 0;
}
