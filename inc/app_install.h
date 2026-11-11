#pragma once
#include "menu.h"
#include <stddef.h>

#define APP_INSTALL_OK 0
#define APP_INSTALL_ERR_BAD -1
#define APP_INSTALL_ERR_IO -2

struct app_install_summary {
  int installed;
  int failed;
  char last_title[MENU_TITLE];
  char last_failed_name[128];
  char last_error[128];
};

typedef void (*app_install_progress_fn)(const char *filename, void *userdata);

/* Convert an app title or zip stem into a lowercase [a-z0-9-] folder slug.
   Returns 0 on success, -1 if empty or out of bounds. */
int app_install_slug(const char *title, char *out, size_t n);

/* Ensure card/autorun/apps exists when card/autorun is a directory. */
int app_install_ensure_dir(const char *card);

/* Install an autorun zip package (plain, takeover, sibling-dir, or card
   package) onto card under slug/title/args. Does not unlink zip_path.
   Returns APP_INSTALL_OK (0), APP_INSTALL_ERR_BAD (-1), or
   APP_INSTALL_ERR_IO (-2). On error, err holds a short reason. */
int app_install_zip(const char *card, const char *zip_path, const char *slug,
                    const char *title, const char *args, char *out_id,
                    size_t out_id_n, char *err, size_t err_n);

/* Scan card/autorun/apps for dropped .zip files and install each one.
   Deletes each .zip on success; renames to .zip.failed and writes
   .zip.error.txt on failure. Returns 0 on completion, -1 on bad args. */
int app_install_pending_zips(const char *card,
                             app_install_progress_fn progress, void *userdata,
                             struct app_install_summary *summary);

