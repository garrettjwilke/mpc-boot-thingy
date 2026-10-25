#pragma once
#include "menu.h"

/* Drop-in files in card/autorun/patch-catalog. Stock does not load these. */
#define BOOT_PATCH_MAX 16
#define BOOT_PATCH_ID 48
#define BOOT_PATCH_TITLE 48

enum {
    BOOT_EDIT_OK = 0,
    BOOT_EDIT_ERR = -1,
    BOOT_EDIT_BAD_TITLE = -2,
    BOOT_EDIT_BAD_PATCH = -3,
    BOOT_EDIT_BAD_ID = -4
};

struct boot_patch {
    char id[BOOT_PATCH_ID];
    char title[BOOT_PATCH_TITLE];
};

/* Sorted by id. A missing catalog directory yields 0. */
int boot_edit_list(const char *card, struct boot_patch *out, int max);

/* id is "entry:<file>.entry" to update, or NULL/empty to create boot-N.
   An empty title on create becomes "Boot N". out_id receives the menu id.
   The entry runs autorun/boot-edits/<slug>/launch.sh. */
int boot_edit_save(const char *card, const char *id, const char *title,
                   const char *const *patch_ids, int npatches,
                   char *out_id, size_t out_n);

/* Removes autorun/boot-edits/<slug> when id names a boot=ac50 entry.
   0 when there is nothing to remove. Does not delete the .entry file. */
int boot_edit_remove(const char *card, const char *id);
