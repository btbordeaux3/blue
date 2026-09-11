/*
 * state.c - Tiny persistence layer. Kept glib-free for testability.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "esp_wifi_agent/state.h"

char *esp_state_load(const char *path)
{
    FILE *f;
    char  buf[ESP_STATE_MAX_NAME + 2];
    size_t n;
    char *s;

    if (!path) return NULL;

    f = fopen(path, "r");
    if (!f) return NULL;

    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return NULL;

    buf[n] = '\0';
    s = strdup(buf);
    if (!s) return NULL;

    /* strip trailing newline / CR */
    s[strcspn(s, "\r\n")] = '\0';
    if (*s == '\0') {
        free(s);
        return NULL;
    }
    return s;
}

bool esp_state_save(const char *path, const char *name)
{
    char *tmp = NULL;
    FILE *f;
    bool  ok = false;

    if (!path || !name) return false;

    if (asprintf(&tmp, "%s.tmp", path) < 0) return false;

    f = fopen(tmp, "w");
    if (!f) goto out;
    if (fprintf(f, "%s\n", name) < 0) {
        fclose(f);
        unlink(tmp);
        goto out;
    }
    if (fclose(f) != 0) {
        unlink(tmp);
        goto out;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        goto out;
    }
    ok = true;

out:
    free(tmp);
    return ok;
}