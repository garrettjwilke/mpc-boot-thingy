#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif

#include "app_install.h"
#include "unzip.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_err(char *err, size_t err_n, const char *msg) {
  if (err && err_n > 0)
    snprintf(err, err_n, "%s", msg ? msg : "install failed");
}

static int safe_component(const char *s) {
  if (!s || !s[0] || strlen(s) >= 200)
    return 0;
  if (strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
    return 0;
  if (strstr(s, ".."))
    return 0;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\')
      return 0;
  }
  return 1;
}

static int ensure_dir(const char *path) {
  struct stat st;
  if (lstat(path, &st) == 0) {
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
      return -1;
    return 0;
  }
  return mkdir(path, 0755);
}

static int rm_tree(const char *path, int depth) {
  struct stat st;
  DIR *d;
  struct dirent *de;
  if (depth > 16 || lstat(path, &st) != 0)
    return -1;
  if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))
    return unlink(path);
  d = opendir(path);
  if (!d)
    return -1;
  while ((de = readdir(d)) != NULL) {
    char child[PATH_MAX];
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >=
        (int)sizeof child) {
      closedir(d);
      return -1;
    }
    if (rm_tree(child, depth + 1) != 0) {
      closedir(d);
      return -1;
    }
  }
  closedir(d);
  return rmdir(path);
}

static int write_full(int fd, const char *buf, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = write(fd, buf + off, n - off);
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

static int one_line(const char *s) {
  if (!s)
    return 0;
  for (const char *p = s; *p; p++) {
    if (*p == '\n' || *p == '\r')
      return 0;
  }
  return 1;
}

int app_install_slug(const char *title, char *out, size_t n) {
  size_t j = 0;
  int dash = 1;
  if (!title || !out || n == 0)
    return -1;
  for (const unsigned char *p = (const unsigned char *)title; *p; p++) {
    unsigned char c = *p;
    int alnum;
    if (c >= 'A' && c <= 'Z')
      c = (unsigned char)(c - 'A' + 'a');
    alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    if (alnum) {
      if (j + 1 >= n)
        return -1;
      out[j++] = (char)c;
      dash = 0;
    } else if (!dash && j + 1 < n) {
      out[j++] = '-';
      dash = 1;
    }
  }
  if (j && out[j - 1] == '-')
    j--;
  out[j] = 0;
  if (!j || j > 64)
    return -1;
  return 0;
}

int app_install_ensure_dir(const char *card) {
  char autorun[PATH_MAX], apps[PATH_MAX];
  struct stat st;
  if (!card || !card[0])
    return -1;
  if (snprintf(autorun, sizeof autorun, "%s/autorun", card) >=
          (int)sizeof autorun ||
      snprintf(apps, sizeof apps, "%s/autorun/apps", card) >=
          (int)sizeof apps)
    return -1;
  if (stat(autorun, &st) != 0 || !S_ISDIR(st.st_mode))
    return -1;
  return ensure_dir(apps);
}

static int write_card_package_entry(const char *card, const char *slug,
                                    const char *title, const char *args,
                                    const char *launcher, const char *dirs,
                                    char *out_id, size_t out_id_n, char *err,
                                    size_t err_n) {
  char menudir[PATH_MAX], entry[PATH_MAX], body[1024];
  int bn, entryfd = -1;
  if (snprintf(menudir, sizeof menudir, "%s/autorun/boot-menu", card) >=
          (int)sizeof menudir ||
      snprintf(entry, sizeof entry, "%s/autorun/boot-menu/%s.entry", card,
               slug) >= (int)sizeof entry) {
    set_err(err, err_n, "path is too long");
    return APP_INSTALL_ERR_BAD;
  }
  if (ensure_dir(menudir) != 0) {
    set_err(err, err_n, "could not create boot menu");
    return APP_INSTALL_ERR_IO;
  }
  bn = snprintf(body, sizeof body, "title=%s\nrun=%s\ncard=1\ndirs=%s\n", title,
                launcher, dirs);
  if (bn < 0 || (size_t)bn >= sizeof body) {
    set_err(err, err_n, "could not write boot.entry");
    return APP_INSTALL_ERR_IO;
  }
  if (args && args[0]) {
    int w = snprintf(body + bn, sizeof body - (size_t)bn, "args=%s\n", args);
    if (w < 0 || (size_t)(bn + w) >= sizeof body) {
      set_err(err, err_n, "could not write boot.entry");
      return APP_INSTALL_ERR_IO;
    }
    bn += w;
  }
  entryfd = open(entry, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (entryfd < 0 || write_full(entryfd, body, (size_t)bn) != 0 ||
      close(entryfd) != 0) {
    if (entryfd >= 0)
      close(entryfd);
    set_err(err, err_n, "could not write boot.entry");
    return APP_INSTALL_ERR_IO;
  }
  if (out_id && out_id_n > 0)
    snprintf(out_id, out_id_n, "entry:%s.entry", slug);
  return APP_INSTALL_OK;
}

int app_install_zip(const char *card, const char *zip_path, const char *slug,
                    const char *title, const char *args, char *out_id,
                    size_t out_id_n, char *err, size_t err_n) {
  char apps[PATH_MAX], dir[PATH_MAX], stage[PATH_MAX], backup[PATH_MAX],
      entry[PATH_MAX];
  char launcher[MENU_RUN], dirs[MENU_RUN], run[MENU_RUN], body[1024];
  const char *safe_args = args ? args : "";
  struct stat st;
  int kind, bn, entryfd = -1, had_dir = 0;

  if (out_id && out_id_n > 0)
    out_id[0] = 0;
  set_err(err, err_n, "could not store the package");

  if (!card || !card[0] || !zip_path || !zip_path[0] || !slug ||
      !safe_component(slug) || !title || !title[0] || !one_line(title) ||
      strlen(title) >= MENU_TITLE) {
    set_err(err, err_n, "invalid app metadata");
    return APP_INSTALL_ERR_BAD;
  }
  if (safe_args[0] && (!one_line(safe_args) || strlen(safe_args) >= MENU_ARGS)) {
    set_err(err, err_n, "args must be one short line");
    return APP_INSTALL_ERR_BAD;
  }

  if (snprintf(apps, sizeof apps, "%s/autorun/apps", card) >= (int)sizeof apps ||
      snprintf(dir, sizeof dir, "%s/autorun/apps/%s", card, slug) >=
          (int)sizeof dir ||
      snprintf(stage, sizeof stage, "%s/autorun/apps/.%s.incoming", card,
               slug) >= (int)sizeof stage ||
      snprintf(backup, sizeof backup, "%s/autorun/apps/.%s.bak", card, slug) >=
          (int)sizeof backup ||
      snprintf(entry, sizeof entry, "%s/boot.entry", stage) >=
          (int)sizeof entry) {
    set_err(err, err_n, "path is too long");
    return APP_INSTALL_ERR_BAD;
  }

  if (ensure_dir(apps) != 0) {
    set_err(err, err_n, "could not create the app folder");
    return APP_INSTALL_ERR_IO;
  }

  launcher[0] = 0;
  dirs[0] = 0;
  kind = unzip_card_package(zip_path, card, launcher, sizeof launcher, dirs,
                            sizeof dirs);
  if (kind < 0) {
    set_err(err, err_n, unzip_error());
    return APP_INSTALL_ERR_BAD;
  }
  if (kind == 1) {
    /* A previous plain-autorun install of the same name lives in apps/<slug>.
       The card package replaces it. */
    rm_tree(dir, 0);
    return write_card_package_entry(card, slug, title, safe_args, launcher,
                                    dirs, out_id, out_id_n, err, err_n);
  }

  rm_tree(stage, 0);
  rm_tree(backup, 0);
  if (mkdir(stage, 0755) != 0) {
    set_err(err, err_n, "could not create the app folder");
    return APP_INSTALL_ERR_IO;
  }
  if (unzip_autorun(zip_path, stage, run, sizeof run) != 0) {
    rm_tree(stage, 0);
    set_err(err, err_n, unzip_error());
    return APP_INSTALL_ERR_BAD;
  }
  dirs[0] = 0;
  if (unzip_sibling_dirs(zip_path, card, dirs, sizeof dirs) != 0) {
    rm_tree(stage, 0);
    set_err(err, err_n, unzip_error());
    return APP_INSTALL_ERR_BAD;
  }

  {
    char mod_launcher[PATH_MAX];
    int takeover = 0;
    if (snprintf(mod_launcher, sizeof mod_launcher, "%s/autorun/mod-launcher.sh",
                 stage) < (int)sizeof mod_launcher &&
        access(mod_launcher, F_OK) == 0) {
      takeover = 1;
    }
    bn = snprintf(body, sizeof body, "title=%s\nrun=%s\n%s%s%s%s%s%s%s",
                  title, run, safe_args[0] ? "args=" : "",
                  safe_args[0] ? safe_args : "", safe_args[0] ? "\n" : "",
                  dirs[0] ? "dirs=" : "", dirs[0] ? dirs : "",
                  dirs[0] ? "\n" : "", takeover ? "takeover=1\n" : "");
  }
  if (bn < 0 || (size_t)bn >= sizeof body) {
    rm_tree(stage, 0);
    set_err(err, err_n, "could not write boot.entry");
    return APP_INSTALL_ERR_IO;
  }
  entryfd = open(entry, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (entryfd < 0 || write_full(entryfd, body, (size_t)bn) != 0 ||
      close(entryfd) != 0) {
    if (entryfd >= 0)
      close(entryfd);
    rm_tree(stage, 0);
    set_err(err, err_n, "could not write boot.entry");
    return APP_INSTALL_ERR_IO;
  }

  if (lstat(dir, &st) == 0) {
    had_dir = 1;
    if (rename(dir, backup) != 0) {
      rm_tree(stage, 0);
      set_err(err, err_n, "could not store the program");
      return APP_INSTALL_ERR_IO;
    }
  }
  if (rename(stage, dir) != 0) {
    if (had_dir)
      rename(backup, dir);
    rm_tree(stage, 0);
    set_err(err, err_n, "could not store the program");
    return APP_INSTALL_ERR_IO;
  }
  if (had_dir)
    rm_tree(backup, 0);

  if (out_id && out_id_n > 0)
    snprintf(out_id, out_id_n, "app:%s", slug);
  return APP_INSTALL_OK;
}

static int has_zip_ext(const char *name) {
  size_t n = strlen(name);
  if (n <= 4)
    return 0;
  const char *ext = name + n - 4;
  return ext[0] == '.' && (ext[1] == 'z' || ext[1] == 'Z') &&
         (ext[2] == 'i' || ext[2] == 'I') &&
         (ext[3] == 'p' || ext[3] == 'P');
}

static int str_ptr_cmp(const void *a, const void *b) {
  const char *const *sa = a;
  const char *const *sb = b;
  return strcmp(*sa, *sb);
}

static void title_from_zip_name(const char *filename, char *out, size_t n) {
  size_t len = strlen(filename);
  size_t stem_len = len > 4 ? len - 4 : len;
  size_t j = 0;
  for (size_t i = 0; i < stem_len && j + 1 < n; i++) {
    unsigned char c = (unsigned char)filename[i];
    if (c < 0x20 || c == 0x7f)
      c = ' ';
    if (j == 0 && (c == ' ' || c == '\t'))
      continue;
    out[j++] = (char)c;
  }
  while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t'))
    j--;
  out[j] = 0;
  if (!out[0])
    snprintf(out, n, "App");
}

static void write_error_note(const char *zip_path, const char *err) {
  char err_path[PATH_MAX];
  int fd;
  char msg[256];
  int mn;
  if (snprintf(err_path, sizeof err_path, "%s.error.txt", zip_path) >=
      (int)sizeof err_path)
    return;
  mn = snprintf(msg, sizeof msg, "%s\n",
                (err && err[0]) ? err : "could not install package");
  if (mn <= 0 || (size_t)mn >= sizeof msg)
    return;
  fd = open(err_path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
  if (fd >= 0) {
    (void)write_full(fd, msg, (size_t)mn);
    close(fd);
  }
}

int app_install_pending_zips(const char *card,
                             app_install_progress_fn progress, void *userdata,
                             struct app_install_summary *summary) {
  char apps[PATH_MAX];
  DIR *d;
  struct dirent *de;
  char **names = NULL;
  size_t count = 0, cap = 0;

  if (summary)
    memset(summary, 0, sizeof *summary);
  if (!card || !card[0])
    return -1;

  (void)app_install_ensure_dir(card);

  if (snprintf(apps, sizeof apps, "%s/autorun/apps", card) >= (int)sizeof apps)
    return -1;
  d = opendir(apps);
  if (!d)
    return 0;

  while ((de = readdir(d)) != NULL) {
    char full[PATH_MAX];
    struct stat st;
    if (de->d_name[0] == '.')
      continue;
    if (!has_zip_ext(de->d_name))
      continue;
    if (snprintf(full, sizeof full, "%s/%s", apps, de->d_name) >=
        (int)sizeof full)
      continue;
    if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode))
      continue;
    if (count == cap) {
      size_t next_cap = cap ? cap * 2 : 8;
      char **next = realloc(names, next_cap * sizeof *names);
      if (!next)
        break;
      names = next;
      cap = next_cap;
    }
    char *copy = strdup(de->d_name);
    if (!copy)
      break;
    names[count++] = copy;
  }
  closedir(d);

  if (count > 1)
    qsort(names, count, sizeof names[0], str_ptr_cmp);

  for (size_t i = 0; i < count; i++) {
    char zip_path[PATH_MAX], failed_path[PATH_MAX];
    char title[MENU_TITLE], slug[80], out_id[MENU_ID], err[128];
    if (progress)
      progress(names[i], userdata);
    if (snprintf(zip_path, sizeof zip_path, "%s/%s", apps, names[i]) >=
        (int)sizeof zip_path) {
      free(names[i]);
      continue;
    }
    title_from_zip_name(names[i], title, sizeof title);
    if (app_install_slug(title, slug, sizeof slug) != 0)
      snprintf(slug, sizeof slug, "app");

    if (app_install_zip(card, zip_path, slug, title, "", out_id, sizeof out_id,
                        err, sizeof err) == APP_INSTALL_OK) {
      unlink(zip_path);
      if (summary) {
        summary->installed++;
        snprintf(summary->last_title, sizeof summary->last_title, "%s", title);
      }
    } else {
      if (snprintf(failed_path, sizeof failed_path, "%s.failed", zip_path) <
          (int)sizeof failed_path) {
        unlink(failed_path);
        (void)rename(zip_path, failed_path);
      }
      write_error_note(zip_path, err);
      if (summary) {
        summary->failed++;
        snprintf(summary->last_failed_name, sizeof summary->last_failed_name,
                 "%s", names[i]);
        snprintf(summary->last_error, sizeof summary->last_error, "%s", err);
      }
    }
    free(names[i]);
  }
  free(names);
  return 0;
}

