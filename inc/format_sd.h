#ifndef FORMAT_SD_H
#define FORMAT_SD_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*format_progress_fn)(const char *phase, void *userdata);

void format_sd_set_mock(int enable, int fail);
int format_sd_is_mock(void);

/* Formats the SD card volume as exFAT with the specified label,
   extracts clean autorun files, and prepares the card for reboot.
   Returns 0 on success, negative on error. */
int format_sd_card(const char *card, const char *label,
                   format_progress_fn progress, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* FORMAT_SD_H */

