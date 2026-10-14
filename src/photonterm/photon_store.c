/* photon_store.c - Dialing directory persistence
 *
 * Copyright (C) 2026 fewtarius and PhotonTERM contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Simple INI file format:
 *
 *   [0]
 *   name=My BBS
 *   addr=bbs.example.com
 *   port=23
 *   conn_type=0
 *   user=myuser
 *   pass=
 *   comment=
 *   added=0
 *   last_connected=0
 *   calls=0
 *   has_fingerprint=0
 *   ssh_fingerprint=
 *
 * Entries are numbered sections [0], [1], etc.
 * Unrecognised keys are silently ignored.
 * Missing keys take zero/empty defaults.
 */

#include "photon_store.h"
#include "photon_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#ifdef _WIN32
#include <direct.h>
#endif

/* ── Config directory ───────────────────────────────────────────────── */

const char *photon_store_config_dir(void)
{
    static char dir[PATH_MAX] = "";
    if (dir[0]) return dir;

    const char *home = getenv("HOME");
#ifdef _WIN32
    if (!home) home = getenv("USERPROFILE");
    if (!home) home = getenv("APPDATA");
#endif
    if (!home) { strlcpy(dir, ".", sizeof(dir)); return dir; }

    /* Prefer XDG: ~/.config/photonterm */
    char xdg[PATH_MAX];
    snprintf(xdg, sizeof(xdg), "%s/.config/photonterm", home);
    struct stat st;
    if (stat(xdg, &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(dir, xdg, sizeof(dir));
        return dir;
    }

    /* Legacy: ~/.photonterm */
    char legacy[PATH_MAX];
    snprintf(legacy, sizeof(legacy), "%s/.photonterm", home);
    if (stat(legacy, &st) == 0 && S_ISDIR(st.st_mode)) {
        strlcpy(dir, legacy, sizeof(dir));
        return dir;
    }

    /* Create XDG dir */
#ifdef _WIN32
    _mkdir(xdg);
#else
    mkdir(xdg, 0700);
#endif
    strlcpy(dir, xdg, sizeof(dir));
    return dir;
}

/* ── Hex helpers ────────────────────────────────────────────────────── */

static void hex_encode(char *out, const uint8_t *in, size_t len)
{
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", in[i]);
}

static void hex_decode(uint8_t *out, size_t outlen, const char *in)
{
    size_t n = strlen(in) / 2;
    if (n > outlen) n = outlen;
    for (size_t i = 0; i < n; i++) {
        unsigned int b = 0;
        sscanf(in + i * 2, "%02x", &b);
        out[i] = (uint8_t)b;
    }
}

/* ── INI parser ─────────────────────────────────────────────────────── */

static void strip_nl(char *s)
{
    char *p = s + strlen(s);
    while (p > s && (p[-1] == '\n' || p[-1] == '\r')) *--p = 0;
}

int photon_store_load(photon_bbs_t *entries, int max, char *path_out, size_t path_max)
{
    char path[PATH_MAX];
    strlcpy(path, photon_store_config_dir(), sizeof(path));
    strlcat(path, "/bbslist.ini", sizeof(path));
    if (path_out) strlcpy(path_out, path, path_max);

    FILE *f = fopen(path, "r");
    if (!f) {
        /* No file = empty list, not an error */
        return 0;
    }

    int  count   = 0;
    photon_bbs_t *cur = NULL;
    char line[4096];

    while (fgets(line, sizeof(line), f)) {
        strip_nl(line);
        /* Skip blank and comment lines */
        if (!line[0] || line[0] == ';' || line[0] == '#') continue;

        /* Section header [N] */
        if (line[0] == '[') {
            int idx = atoi(line + 1);
            if (idx < 0 || idx >= max) { cur = NULL; continue; }
            if (idx >= count) count = idx + 1;
            cur = &entries[idx];
            memset(cur, 0, sizeof(*cur));
            cur->id = idx;
            continue;
        }

        if (!cur) continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;

#define STRFIELD(k, f) \
        if (strcmp(key, k) == 0) { strlcpy(cur->f, val, sizeof(cur->f)); continue; }
#define INTFIELD(k, f) \
        if (strcmp(key, k) == 0) { cur->f = atol(val); continue; }

        STRFIELD("name",    name)
        STRFIELD("addr",    addr)
        STRFIELD("user",    user)
        STRFIELD("pass",    pass)
        STRFIELD("comment", comment)
        INTFIELD("port",    port)
        INTFIELD("conn_type", conn_type)
        INTFIELD("term_mode", term_mode)
        INTFIELD("palette_mode", palette_mode)
        INTFIELD("added",   added)
        INTFIELD("last_connected", last_connected)
        INTFIELD("calls",   calls)
        INTFIELD("has_fingerprint", has_fingerprint)
        if (strcmp(key, "ssh_fingerprint") == 0) {
            hex_decode(cur->ssh_fingerprint, PHOTON_FINGERPRINT_LEN, val);
            continue;
        }
    }

    fclose(f);
    return count;
}

/* ── INI writer ─────────────────────────────────────────────────────── */

/* Write a string field to the INI file, stripping any embedded newlines
 * that would corrupt the file format. */
static void ini_write_str(FILE *f, const char *key, const char *val)
{
    fputs(key, f);
    fputs("=", f);
    for (const char *p = val; *p; p++) {
        if (*p == '\n' || *p == '\r') continue;
        fputc(*p, f);
    }
    fputc('\n', f);
}

bool photon_store_save(const photon_bbs_t *entries, int count)
{
    char path[PATH_MAX];
    strlcpy(path, photon_store_config_dir(), sizeof(path));
    strlcat(path, "/bbslist.ini", sizeof(path));

    /* Write to a temp file first, then rename */
    char tmp[PATH_MAX];
    strlcpy(tmp, path, sizeof(tmp));
    strlcat(tmp, ".tmp", sizeof(tmp));

    FILE *f = fopen_secure(tmp);
    if (!f) return false;

    fprintf(f, "; PhotonTERM dialing directory\n");
    fprintf(f, "; Do not edit while PhotonTERM is running.\n\n");

    char fphex[PHOTON_FINGERPRINT_LEN * 2 + 1];

    for (int i = 0; i < count; i++) {
        const photon_bbs_t *e = &entries[i];
        hex_encode(fphex, e->ssh_fingerprint, PHOTON_FINGERPRINT_LEN);
        fprintf(f, "[%d]\n",               i);
        ini_write_str(f, "name",          e->name);
        ini_write_str(f, "addr",          e->addr);
        fprintf(f, "port=%u\n",            e->port);
        fprintf(f, "conn_type=%d\n",       (int)e->conn_type);
        fprintf(f, "term_mode=%d\n",       (int)e->term_mode);
        fprintf(f, "palette_mode=%d\n",    (int)e->palette_mode);
        ini_write_str(f, "user",          e->user);
        ini_write_str(f, "pass",          e->pass);
        ini_write_str(f, "comment",       e->comment);
        fprintf(f, "added=%lld\n",         (long long)e->added);
        fprintf(f, "last_connected=%lld\n", (long long)e->last_connected);
        fprintf(f, "calls=%u\n",           e->calls);
        fprintf(f, "has_fingerprint=%d\n", e->has_fingerprint ? 1 : 0);
        fprintf(f, "ssh_fingerprint=%s\n", fphex);
        fprintf(f, "\n");
    }

    fclose(f);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return false;
    }
    return true;
}

/* Update a single entry in the store, matching by name+addr+port+conn_type.
 * Loads the full list, replaces the matching entry, and saves. */
bool photon_store_update_entry(const photon_bbs_t *entry)
{
    if (!entry) return false;

    photon_bbs_t list[MAX_BBS_ENTRIES];
    int count = photon_store_load(list, MAX_BBS_ENTRIES, NULL, 0);
    if (count < 0) count = 0;

    int found = -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(list[i].name, entry->name) == 0 &&
            strcmp(list[i].addr, entry->addr) == 0 &&
            list[i].port == entry->port &&
            list[i].conn_type == entry->conn_type) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        /* Entry doesn't exist yet — append if there's room */
        if (count >= MAX_BBS_ENTRIES) return false;
        found = count++;
    }

    list[found] = *entry;
    list[found].id = found;
    return photon_store_save(list, count);
}
