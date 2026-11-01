#include "testapp.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *env_path(const char *key, const char *fallback) {
  const char *env = getenv(key);
  return (env && env[0]) ? env : fallback;
}

static const char *request_path(void) {
  return env_path("MPC_BOOT_TESTAPP_REQUEST", "/run/mpc-boot-testapp.request");
}

static const char *ready_path(void) {
  return env_path("MPC_BOOT_TESTAPP_READY", "/run/mpc-boot-testapp.ready");
}

int testapp_request_pending(void) { return access(request_path(), F_OK) == 0; }

int testapp_mark_ready(void) {
  FILE *f = fopen(ready_path(), "w");
  if (!f)
    return -1;
  if (fputs("1\n", f) < 0) {
    fclose(f);
    return -1;
  }
  return fclose(f) == 0 ? 0 : -1;
}

int testapp_comm_matches(const char *comm) {
  if (!comm || !comm[0])
    return 0;
  if (strncmp(comm, "test-app-launch", 15) == 0)
    return 1;
  if (strncmp(comm, "usb_installer", 13) == 0)
    return 1;
  return 0;
}

int testapp_proc_has_launcher(const char *proc_root) {
  DIR *d;
  struct dirent *de;
  if (!proc_root || !proc_root[0])
    return 0;
  d = opendir(proc_root);
  if (!d)
    return 0;
  while ((de = readdir(d)) != NULL) {
    char path[512];
    FILE *f;
    char comm[64];
    size_t n;
    const char *name = de->d_name;
    if (!isdigit((unsigned char)name[0]))
      continue;
    if (snprintf(path, sizeof path, "%s/%s/comm", proc_root, name) >=
        (int)sizeof path)
      continue;
    f = fopen(path, "r");
    if (!f)
      continue;
    if (!fgets(comm, (int)sizeof comm, f)) {
      fclose(f);
      continue;
    }
    fclose(f);
    n = strlen(comm);
    while (n && (comm[n - 1] == '\n' || comm[n - 1] == '\r'))
      comm[--n] = 0;
    if (testapp_comm_matches(comm)) {
      closedir(d);
      return 1;
    }
  }
  closedir(d);
  return 0;
}
