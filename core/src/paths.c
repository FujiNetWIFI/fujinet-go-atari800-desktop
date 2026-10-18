/*
 * a800session path layout: XDG config/data directories, media, imported
 * ROMs, save states and the FujiNet runtime tree.
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "session_internal.h"

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Compiled-in locations (set by CMake), so a git checkout runs uninstalled
 * and an installed copy finds its runtime beside the executable / in libdir. */
#ifndef A800_DEV_FUJINET_OUT
#define A800_DEV_FUJINET_OUT ""
#endif
#ifndef A800_INSTALL_DATADIR
#define A800_INSTALL_DATADIR ""
#endif
#ifndef A800_INSTALL_LIBDIR
#define A800_INSTALL_LIBDIR ""
#endif

#if defined(__APPLE__)
#define A800_FUJINET_LIB_NAME "libfujinet.dylib"
#elif defined(_WIN32)
#define A800_FUJINET_LIB_NAME "fujinet.dll"
#else
#define A800_FUJINET_LIB_NAME "libfujinet.so"
#endif

static int is_sep(char c)
{
#if defined(_WIN32)
    return c == '/' || c == '\\';
#else
    return c == '/';
#endif
}

static int make_dir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

int paths_mkdir_p(const char *path)
{
    char buf[A800_PATH_MAX];
    char *p;
    if (!path || !*path) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    for (p = buf + 1; *p; p++) {
        if (is_sep(*p)) {
            char save = *p;
            *p = '\0';
            if (make_dir(buf) != 0 && errno != EEXIST) return -1;
            *p = save;
        }
    }
    if (make_dir(buf) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* Per-user config/data directory. On Windows this is %APPDATA% (config) or
 * %LOCALAPPDATA% (data); elsewhere the XDG variable, then $HOME/suffix. */
static void default_dir(char *dst, size_t dstsz, const char *xdg_env,
                        const char *win_env, const char *home_suffix)
{
#if defined(_WIN32)
    const char *v = getenv(win_env);
    (void)xdg_env;
    (void)home_suffix;
    snprintf(dst, dstsz, "%s\\fujinet-go-atari800", (v && *v) ? v : ".");
#else
    const char *v = getenv(xdg_env);
    (void)win_env;
    if (v && *v) {
        snprintf(dst, dstsz, "%s/fujinet-go-atari800", v);
    } else {
        const char *home = getenv("HOME");
        snprintf(dst, dstsz, "%s/%s/fujinet-go-atari800", home ? home : ".",
                 home_suffix);
    }
#endif
}

/* dir/leaf into dst; 0, or -1 and an empty dst (which then fails loudly
 * where it is used, rather than naming some other file) when it is too long */
static int join(char *dst, size_t dstsz, const char *dir, const char *leaf)
{
    size_t const a = strlen(dir), b = strlen(leaf);
    if (a + 1 + b + 1 > dstsz) {
        if (dstsz) dst[0] = '\0';
        return -1;
    }
    memmove(dst, dir, a);
    dst[a] = '/';
    memcpy(dst + a + 1, leaf, b + 1);
    return 0;
}

int paths_init(struct a800session *s, const char *config_dir,
               const char *data_dir)
{
    if (config_dir && *config_dir)
        snprintf(s->config_dir, sizeof s->config_dir, "%s", config_dir);
    else
        default_dir(s->config_dir, sizeof s->config_dir,
                    "XDG_CONFIG_HOME", "APPDATA", ".config");

    if (data_dir && *data_dir)
        snprintf(s->data_dir, sizeof s->data_dir, "%s", data_dir);
    else
        default_dir(s->data_dir, sizeof s->data_dir,
                    "XDG_DATA_HOME", "LOCALAPPDATA", ".local/share");

    join(s->settings_file, sizeof s->settings_file, s->config_dir, "settings.ini");

    /* FujiNet runtime tree lives under the data dir; the web UI is fixed. */
    join(s->fujinet_root, sizeof s->fujinet_root, s->data_dir, "fujinet");
    join(s->fujinet_config, sizeof s->fujinet_config, s->fujinet_root, "fnconfig.ini");
    join(s->fujinet_sd, sizeof s->fujinet_sd, s->fujinet_root, "SD");
    join(s->fujinet_data, sizeof s->fujinet_data, s->fujinet_root, "data");
    snprintf(s->webui_url, sizeof s->webui_url, "http://127.0.0.1:%d/",
             A800SESSION_WEBUI_PORT);

    if (paths_mkdir_p(s->config_dir) != 0) {
        session_set_error(s, "cannot create config dir %s", s->config_dir);
        return -1;
    }
    if (paths_mkdir_p(s->data_dir) != 0) {
        session_set_error(s, "cannot create data dir %s", s->data_dir);
        return -1;
    }
    /* media the user opens (and blank disks they make) and imported ROMs */
    snprintf(s->media_dir, sizeof s->media_dir, "%s/media", s->data_dir);
    if (paths_mkdir_p(s->media_dir) != 0) {
        session_set_error(s, "cannot create media dir %s", s->media_dir);
        return -1;
    }
    snprintf(s->roms_dir, sizeof s->roms_dir, "%s/roms", s->data_dir);
    if (paths_mkdir_p(s->roms_dir) != 0) {
        session_set_error(s, "cannot create ROM dir %s", s->roms_dir);
        return -1;
    }
    snprintf(s->states_dir, sizeof s->states_dir, "%s/states", s->data_dir);
    if (paths_mkdir_p(s->states_dir) != 0) {
        session_set_error(s, "cannot create states dir %s", s->states_dir);
        return -1;
    }
    return 0;
}

/* ---- FujiNet runtime provisioning ---- */

static int is_file(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p)
{
    struct stat st;
    return p && *p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    return rc;
}


/* Directory holding the running executable, or "" -- where a Windows install
 * (a folder you copy: fujinet.dll beside the exe) and a macOS bundle (the
 * runtime in Contents/MacOS beside the executable) keep the runtime. */
static const char *exe_dir(void)
{
    static char dir[A800_PATH_MAX];
    static int done;
    if (done)
        return dir;
    done = 1;
    dir[0] = '\0';
#if defined(_WIN32)
    if (GetModuleFileNameA(NULL, dir, (DWORD)sizeof dir) == 0) { dir[0] = '\0'; }
    else { char *p = strrchr(dir, '\\'); if (!p) p = strrchr(dir, '/'); if (p) *p = '\0'; else dir[0] = '\0'; }
#elif defined(__APPLE__)
    uint32_t sz = sizeof dir;
    extern int _NSGetExecutablePath(char *, uint32_t *);
    if (_NSGetExecutablePath(dir, &sz) == 0) { char *p = strrchr(dir, '/'); if (p) *p = '\0'; }
    else dir[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if (n > 0) { dir[n] = '\0'; char *p = strrchr(dir, '/'); if (p) *p = '\0'; }
    else dir[0] = '\0';
#endif
    return dir;
}

/* Try each candidate directory for libfujinet; on success set s->fujinet_lib
 * and return the directory (which also holds the runtime asset tree). */
static const char *resolve_lib_dir(struct a800session *s)
{
    static char dir[A800_PATH_MAX];
    const char *candidates[] = {
        getenv("FUJINET_LIB_DIR"),   /* may be NULL -- skipped, not terminal */
        exe_dir(),                   /* Windows folder / macOS bundle */
        A800_DEV_FUJINET_OUT,
        A800_INSTALL_LIBDIR,
    };
    const int n = (int)(sizeof candidates / sizeof candidates[0]);
    for (int i = 0; i < n; i++) {
        if (!candidates[i] || !*candidates[i]) continue;
        char lib[A800_PATH_MAX];
        snprintf(lib, sizeof lib, "%s/%s", candidates[i], A800_FUJINET_LIB_NAME);
        if (is_file(lib)) {
            snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", lib);
            snprintf(dir, sizeof dir, "%s", candidates[i]);
            return dir;
        }
    }
    /* an explicit full path to the library */
    const char *libenv = getenv("FUJINET_LIB");
    if (libenv && is_file(libenv)) {
        snprintf(s->fujinet_lib, sizeof s->fujinet_lib, "%s", libenv);
        return "";   /* assets come from the install datadir below */
    }
    return NULL;
}

/* ---- keeping the user's runtime tree up to date ----------------------------
 * <root>/manifest.txt lists every bundled file copied in, with the CRC it
 * had. On a new bundle (version.txt differs), a file is replaced only if the
 * user's copy still has that CRC: whatever they edited stays theirs.
 * fnconfig.ini is copied once and never again; SD/ only gains files. */

static uint32_t file_crc(const char *path, int *ok)
{
    uint8_t buf[65536];
    uint32_t crc = 0xffffffffu;
    size_t n;
    FILE *fp = fopen(path, "rb");
    static uint32_t table[256];
    static int init;
    if (!init) {
        uint32_t i, j, c;
        for (i = 0; i < 256; i++) {
            for (c = i, j = 0; j < 8; j++)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = 1;
    }
    *ok = fp != NULL;
    if (!fp)
        return 0;
    while ((n = fread(buf, 1, sizeof buf, fp)) > 0) {
        size_t i;
        for (i = 0; i < n; i++)
            crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
    }
    fclose(fp);
    return ~crc;
}

typedef struct {
    char (*rel)[256];
    uint32_t *crc;
    int n, cap;
} manifest;

static void manifest_add(manifest *m, const char *rel, uint32_t crc)
{
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 64;
        void *r = realloc(m->rel, (size_t)cap * sizeof *m->rel);
        void *c = realloc(m->crc, (size_t)cap * sizeof *m->crc);
        if (!r || !c)
            return;
        m->rel = r;
        m->crc = c;
        m->cap = cap;
    }
    snprintf(m->rel[m->n], sizeof m->rel[m->n], "%s", rel);
    m->crc[m->n++] = crc;
}

static int manifest_find(const manifest *m, const char *rel, uint32_t *crc)
{
    int i;
    for (i = 0; i < m->n; i++)
        if (!strcmp(m->rel[i], rel)) {
            *crc = m->crc[i];
            return 1;
        }
    return 0;
}

static void manifest_load(manifest *m, const char *path)
{
    char line[400];
    FILE *fp = fopen(path, "r");
    if (!fp)
        return;
    while (fgets(line, sizeof line, fp)) {
        unsigned long crc;
        char rel[300];
        if (sscanf(line, "%lx %299[^\n]", &crc, rel) == 2)
            manifest_add(m, rel, (uint32_t)crc);
    }
    fclose(fp);
}

/* Copies src_dir/rel_dir into dst_root, honouring the old manifest. */
static void provision_dir(const char *src_root, const char *dst_root, const char *rel_dir,
                          const manifest *old, manifest *now, int add_only)
{
    char src[A800_PATH_MAX], dst[A800_PATH_MAX];
    DIR *d;
    struct dirent *e;
    snprintf(src, sizeof src, "%s/%s", src_root, rel_dir);
    snprintf(dst, sizeof dst, "%s/%s", dst_root, rel_dir);
    paths_mkdir_p(dst);
    d = opendir(src);
    if (!d)
        return;
    while ((e = readdir(d)) != NULL) {
        char rel[512], from[A800_PATH_MAX], to[A800_PATH_MAX];
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(rel, sizeof rel, "%s/%s", rel_dir, e->d_name);
        snprintf(from, sizeof from, "%s/%s", src_root, rel);
        snprintf(to, sizeof to, "%s/%s", dst_root, rel);
        if (stat(from, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            provision_dir(src_root, dst_root, rel, old, now, add_only);
        }
        else if (S_ISREG(st.st_mode)) {
            int ok_src, ok_dst;
            uint32_t want = file_crc(from, &ok_src), have, was;
            if (!ok_src)
                continue;
            have = file_crc(to, &ok_dst);
            if (!ok_dst || (!add_only && manifest_find(old, rel, &was) && was == have))
                copy_file(from, to);
            manifest_add(now, rel, want);
        }
    }
    closedir(d);
}

static void provision_tree(struct a800session *s, const char *asset_src)
{
    char path[A800_PATH_MAX], src[A800_PATH_MAX], want[256] = "", have[256] = "";
    manifest old = {0}, now = {0};
    FILE *fp;
    int i;

    paths_mkdir_p(s->fujinet_root);
    if (!is_file(s->fujinet_config)) {
        snprintf(src, sizeof src, "%s/fnconfig.ini", asset_src);
        copy_file(src, s->fujinet_config);
    }
    snprintf(src, sizeof src, "%s/upstream-commit.txt", asset_src);
    if ((fp = fopen(src, "r")) != NULL) {
        if (!fgets(want, sizeof want, fp))
            want[0] = 0;
        fclose(fp);
    }
    snprintf(path, sizeof path, "%s/version.txt", s->fujinet_root);
    if ((fp = fopen(path, "r")) != NULL) {
        if (!fgets(have, sizeof have, fp))
            have[0] = 0;
        fclose(fp);
    }
    if (*want && !strcmp(want, have) && is_dir(s->fujinet_data))
        return;

    snprintf(path, sizeof path, "%s/manifest.txt", s->fujinet_root);
    manifest_load(&old, path);
    provision_dir(asset_src, s->fujinet_root, "data", &old, &now, 0);
    provision_dir(asset_src, s->fujinet_root, "SD", &old, &now, 1);
    if ((fp = fopen(path, "w")) != NULL) {
        for (i = 0; i < now.n; i++)
            fprintf(fp, "%08lx %s\n", (unsigned long)now.crc[i], now.rel[i]);
        fclose(fp);
    }
    snprintf(path, sizeof path, "%s/version.txt", s->fujinet_root);
    if ((fp = fopen(path, "w")) != NULL) {
        fputs(want, fp);
        fclose(fp);
    }
    free(old.rel);
    free(old.crc);
    free(now.rel);
    free(now.crc);
}

int paths_provision_fujinet(struct a800session *s)
{
    const char *libdir = resolve_lib_dir(s);
    if (!libdir && !*s->fujinet_lib) {
        session_set_error(s, "libfujinet not found (build with WITH_FUJINET=ON "
                          "or set FUJINET_LIB_DIR)");
        return -1;
    }

    /* Where the pristine runtime assets live. The dev out dir holds them
     * directly (fnconfig.ini/data/SD next to the .so); an install keeps them
     * in a "fujinet/" subdir beside the exe (Windows) / in the datadir. Try
     * the library dir, then <libdir>/fujinet, then the install datadir. */
    char asset_src[A800_PATH_MAX];
    char probe[A800_PATH_MAX];
    asset_src[0] = '\0';
    const char *cands[4];
    int nc = 0;
    static char c0[A800_PATH_MAX], c1[A800_PATH_MAX];
    if (s->fujinet_runtime_src[0])
        cands[nc++] = s->fujinet_runtime_src;
    if (libdir && *libdir) {
        snprintf(c0, sizeof c0, "%s", libdir);          cands[nc++] = c0;
        join(c1, sizeof c1, libdir, "fujinet");          cands[nc++] = c1;
    }
    cands[nc++] = A800_INSTALL_DATADIR "/fujinet";
    for (int i = 0; i < nc; i++) {
        snprintf(probe, sizeof probe, "%s/fnconfig.ini", cands[i]);
        if (is_file(probe)) { snprintf(asset_src, sizeof asset_src, "%s", cands[i]); break; }
    }

    if (*asset_src)
        provision_tree(s, asset_src);
    return 0;
}
