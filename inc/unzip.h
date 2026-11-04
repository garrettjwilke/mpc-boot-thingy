#pragma once
#include <stddef.h>

/* Copy the autorun package in zip_path into app_dir/autorun.
   On success script_rel is the boot.entry run path, relative to app_dir.
   A -1 may leave files in app_dir; the caller removes that directory.
   unzip_error() is a short reason after failure. */
int unzip_autorun(const char *zip_path, const char *app_dir, char *script_rel, size_t n);
const char *unzip_error(void);

/* A zip that also ships a sibling directory at the SD card root (WaveGrid-style).
   When the autorun script runs "../<name>/start.sh", extract every sibling
   directory under the wrapper to card_root (skipping the autorun folder and
   loose top-level files), and return launcher ("<name>/start.sh") plus dirs
   (space-separated top-level directory names written).
   Returns 1 when it was a card package and was extracted, 0 when it is a plain
   autorun package (use unzip_autorun), -1 on error (unzip_error()). */
int unzip_card_package(const char *zip_path, const char *card_root,
                       char *launcher, size_t launcher_n,
                       char *dirs, size_t dirs_n);

/* Extract every sibling directory under the wrapper to card_root (skipping the
   autorun folder and loose top-level files), and return dirs (space-separated
   top-level directory names written, or empty string if none).
   Returns 0 on success, -1 on error (unzip_error()). */
int unzip_sibling_dirs(const char *zip_path, const char *card_root,
                       char *dirs, size_t dirs_n);
