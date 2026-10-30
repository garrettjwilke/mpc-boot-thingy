#pragma once
#include <stddef.h>

/* Copy the autorun package in zip_path into app_dir/autorun.
   On success script_rel is the boot.entry run path, relative to app_dir.
   A -1 may leave files in app_dir; the caller removes that directory.
   unzip_error() is a short reason after failure. */
int unzip_autorun(const char *zip_path, const char *app_dir, char *script_rel, size_t n);
const char *unzip_error(void);
