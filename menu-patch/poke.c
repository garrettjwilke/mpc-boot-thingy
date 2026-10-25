/* Write stdin over a file at a byte offset. Does not truncate.
   The device dd has no conv=notrunc, so it cannot do this. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int fd;
    off_t off;
    char *end;
    char buf[4096];
    ssize_t n;
    if (argc != 3) {
        fprintf(stderr, "usage: poke FILE OFFSET\n");
        return 2;
    }
    off = strtoll(argv[2], &end, 10);
    if (end == argv[2] || *end || off < 0) {
        fprintf(stderr, "bad offset\n");
        return 2;
    }
    fd = open(argv[1], O_WRONLY);
    if (fd < 0) {
        perror(argv[1]);
        return 1;
    }
    if (lseek(fd, off, SEEK_SET) != off) {
        perror("lseek");
        close(fd);
        return 1;
    }
    while ((n = read(STDIN_FILENO, buf, sizeof buf)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(fd, buf + w, (size_t)(n - w));
            if (k < 0) {
                if (errno == EINTR) continue;
                perror("write");
                close(fd);
                return 1;
            }
            w += k;
        }
    }
    if (n < 0) {
        perror("read");
        close(fd);
        return 1;
    }
    if (close(fd) != 0) {
        perror("close");
        return 1;
    }
    return 0;
}
