/**
 * GL programs without the stutter.
 *
 * Every combiner state and vertex program of the title becomes a GL program
 * the first time it is drawn, and linking one takes the driver from a few to
 * tens of milliseconds on a phone -- a hitch each time something new comes on
 * screen. Two remedies:
 *
 *  - Disk cache. Each linked program's binary (glGetProgramBinary) is kept in
 *    <data>/ShaderCache/programs-<driver>.bin, keyed by a hash of its two
 *    sources; the next run loads it with glProgramBinary instead of building
 *    it. One file per driver (vendor, renderer, version): an updated driver
 *    starts a new one. A binary the driver refuses is built again.
 *
 *  - A worker thread with its own context, sharing objects with the render
 *    thread's, builds what the cache lacks for the vertex-program draws; until
 *    it is done the translator draws those on the CPU (the same image, more
 *    CPU time for a frame or two). The combiner-only programs, which the CPU
 *    path itself needs, are still built on the spot -- once ever, with the
 *    cache.
 *
 * OT_SHADER_CACHE=0 turns the disk cache off, OT_SHADER_ASYNC=0 the worker.
 */
#include "gles_progcache.h"
#include <EGL/egl.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GL_PROGRAM_BINARY_RETRIEVABLE_HINT
#define GL_PROGRAM_BINARY_RETRIEVABLE_HINT 0x8257
#endif

static char s_dir[1024];

void d3d8_SetShaderCacheDir(const char *dir)
{
    snprintf(s_dir, sizeof s_dir, "%s", dir ? dir : "");
}

static uint64_t fnv64(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--) { h ^= *b++; h *= 1099511628211ull; }
    return h;
}

static uint64_t src_key(const char *vs, const char *fs)
{
    uint64_t h = fnv64(1469598103934665603ull, vs, strlen(vs) + 1);
    h = fnv64(h, fs, strlen(fs) + 1);
    return h ? h : 1;
}

/* ---- compiling ------------------------------------------------------------ */

static volatile long s_compiled, s_compiled_async, s_loaded, s_rejected, s_fallbacks;

GLuint pc_compile(GLenum type, const char *src, const char *tag)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        static int told;
        char log[4096];
        glGetShaderInfoLog(sh, sizeof log, NULL, log);
        if (told++ < 6)
            fprintf(stderr, "[GLES] %s %s shader failed:\n%s\n--- source ---\n%s\n", tag,
                    type == GL_VERTEX_SHADER ? "vertex" : "fragment", log, src);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int s_binaries;              /* the driver hands out program binaries */

static GLuint build(const char *vs, const char *fs, const char *tag)
{
    GLuint v = pc_compile(GL_VERTEX_SHADER, vs, tag), f, p;
    GLint ok = 0;
    if (!v) return 0;
    f = pc_compile(GL_FRAGMENT_SHADER, fs, tag);
    if (!f) { glDeleteShader(v); return 0; }
    p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    if (s_binaries) glProgramParameteri(p, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
    glLinkProgram(p);
    glDetachShader(p, v);
    glDetachShader(p, f);
    glDeleteShader(v);
    glDeleteShader(f);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        static int told;
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        if (told++ < 6) fprintf(stderr, "[GLES] %s program link failed:\n%s\n", tag, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

/* ---- the disk cache --------------------------------------------------------- */

#define PC_MAGIC "OTPC1\n"

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *s_file;
static char s_path[1200];
static long s_end;                  /* where the next record goes */
static struct PcEnt { uint64_t key; long off; uint32_t len, fmt; } *s_tab;
static unsigned s_cap, s_n;

static struct PcEnt *tab_slot(uint64_t key)
{
    unsigned h = (unsigned)(key ^ (key >> 32)) & (s_cap - 1);
    while (s_tab[h].key && s_tab[h].key != key) h = (h + 1) & (s_cap - 1);
    return &s_tab[h];
}

static void tab_put(uint64_t key, long off, uint32_t len, uint32_t fmt)
{
    struct PcEnt *e;
    if ((s_n + 1) * 4 >= s_cap * 3) {
        struct PcEnt *old = s_tab;
        unsigned i, oc = s_cap;
        s_cap = s_cap ? s_cap * 2 : 1024;
        s_tab = (struct PcEnt *)calloc(s_cap, sizeof *s_tab);
        if (!s_tab) { s_tab = old; s_cap = oc; return; }
        s_n = 0;
        for (i = 0; i < oc; i++)
            if (old[i].key) { *tab_slot(old[i].key) = old[i]; s_n++; }
        free(old);
    }
    e = tab_slot(key);
    if (!e->key) s_n++;
    e->key = key; e->off = off; e->len = len; e->fmt = fmt;
}

static void mkdirs(const char *path)
{
    char b[1024];
    size_t i;
    snprintf(b, sizeof b, "%s", path);
    for (i = 1; b[i]; i++)
        if (b[i] == '/') { b[i] = 0; mkdir(b, 0755); b[i] = '/'; }
    mkdir(b, 0755);
}

static void cache_open(void)
{
    const char *e = getenv("OT_SHADER_CACHE");
    char id[768], path[1200], head[1024];
    GLint nfmt = 0;
    uint32_t idlen;
    if (e && e[0] == '0') return;
    glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &nfmt);
    if (nfmt <= 0) {
        fprintf(stderr, "[SHADERS] the driver keeps no program binaries: no disk cache\n");
        return;
    }
    s_binaries = 1;
    if (!s_dir[0]) return;
    snprintf(id, sizeof id, "%s|%s|%s", (const char *)glGetString(GL_VENDOR),
             (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    idlen = (uint32_t)strlen(id);
    mkdirs(s_dir);
    snprintf(path, sizeof path, "%s/programs-%016llx.bin", s_dir,
             (unsigned long long)fnv64(1469598103934665603ull, id, idlen));
    snprintf(s_path, sizeof s_path, "%s", path);
    {
        /* A driver that refused its own binaries before (cache_refused). */
        char mark[1210];
        FILE *f;
        snprintf(mark, sizeof mark, "%s.refused", path);
        if ((f = fopen(mark, "rb"))) {
            fclose(f);
            fprintf(stderr, "[SHADERS] this driver does not reload its program binaries: no disk cache\n");
            return;
        }
    }

    s_file = fopen(path, "r+b");
    if (s_file) {
        /* Header: magic, the driver's name; then records key, format, length, bytes. */
        uint32_t n = 0;
        if (fread(head, 1, 6, s_file) != 6 || memcmp(head, PC_MAGIC, 6) ||
            fread(&n, 4, 1, s_file) != 1 || n != idlen || n >= sizeof head ||
            fread(head, 1, n, s_file) != n || memcmp(head, id, n)) {
            fclose(s_file);
            s_file = NULL;
        }
    }
    if (!s_file) {
        s_file = fopen(path, "w+b");
        if (!s_file) { fprintf(stderr, "[SHADERS] cannot write %s\n", path); return; }
        fwrite(PC_MAGIC, 1, 6, s_file);
        fwrite(&idlen, 4, 1, s_file);
        fwrite(id, 1, idlen, s_file);
        fflush(s_file);
    }
    s_end = ftell(s_file);
    for (;;) {
        uint64_t key;
        uint32_t fmt, len;
        long off;
        if (fread(&key, 8, 1, s_file) != 1 || fread(&fmt, 4, 1, s_file) != 1 ||
            fread(&len, 4, 1, s_file) != 1)
            break;
        off = ftell(s_file);
        if (!len || fseek(s_file, (long)len, SEEK_CUR) || ftell(s_file) != off + (long)len)
            break;
        {   /* the file may end inside a record (the app was stopped writing it) */
            long here = ftell(s_file);
            fseek(s_file, 0, SEEK_END);
            if (ftell(s_file) < here) break;
            fseek(s_file, here, SEEK_SET);
        }
        tab_put(key, off, len, fmt);
        s_end = off + (long)len;
    }
    if (ftruncate(fileno(s_file), s_end) != 0) { /* a torn tail stays; it is skipped */ }
    fprintf(stderr, "[SHADERS] disk cache %s: %u programs\n", path, s_n);
}

/* Binaries the driver will not take back: after a few refusals and no
 * success, the cache is dropped for this driver, for good (the emulator's GL
 * translator does this; phone drivers keep theirs). */
static void cache_refused(void)
{
    char mark[1210];
    FILE *f;
    if (!s_file || s_loaded || s_rejected < 4) return;
    fclose(s_file);
    s_file = NULL;
    remove(s_path);
    snprintf(mark, sizeof mark, "%s.refused", s_path);
    if ((f = fopen(mark, "wb"))) fclose(f);
    fprintf(stderr, "[SHADERS] the driver refuses its program binaries: disk cache off\n");
}

static GLuint cache_load(uint64_t key)
{
    struct PcEnt e;
    void *buf;
    GLuint p;
    GLint ok = 0;
    pthread_mutex_lock(&s_lock);
    if (!s_file || !s_cap || !tab_slot(key)->key) { pthread_mutex_unlock(&s_lock); return 0; }
    e = *tab_slot(key);
    buf = malloc(e.len);
    if (!buf || fseek(s_file, e.off, SEEK_SET) || fread(buf, 1, e.len, s_file) != e.len) {
        pthread_mutex_unlock(&s_lock);
        free(buf);
        return 0;
    }
    pthread_mutex_unlock(&s_lock);
    p = glCreateProgram();
    glProgramBinary(p, (GLenum)e.fmt, buf, (GLsizei)e.len);
    free(buf);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(p);
        s_rejected++;
        pthread_mutex_lock(&s_lock);
        cache_refused();
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    s_loaded++;
    return p;
}

static void cache_store(uint64_t key, GLuint p)
{
    GLint len = 0;
    GLsizei got = 0;
    GLenum fmt = 0;
    void *buf;
    if (!s_file || !p) return;
    glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);
    if (len <= 0 || !(buf = malloc((size_t)len))) return;
    glGetProgramBinary(p, len, &got, &fmt, buf);
    if (got > 0) {
        uint32_t f = (uint32_t)fmt, l = (uint32_t)got;
        pthread_mutex_lock(&s_lock);
        if (s_file && !fseek(s_file, s_end, SEEK_SET) && fwrite(&key, 8, 1, s_file) == 1 &&
            fwrite(&f, 4, 1, s_file) == 1 && fwrite(&l, 4, 1, s_file) == 1 &&
            fwrite(buf, 1, l, s_file) == l && !fflush(s_file)) {
            tab_put(key, s_end + 16, l, f);
            s_end += 16 + (long)l;
        }
        pthread_mutex_unlock(&s_lock);
    }
    free(buf);
}

GLuint pc_load_cached(const char *vs, const char *fs)
{
    return cache_load(src_key(vs, fs));
}

GLuint pc_link_sync(const char *vs, const char *fs, const char *tag)
{
    uint64_t key = src_key(vs, fs);
    GLuint p = cache_load(key);
    if (p) return p;
    p = build(vs, fs, tag);
    if (p) { s_compiled++; cache_store(key, p); }
    return p;
}

/* ---- the worker ------------------------------------------------------------- */

struct PcJob {
    const char *vs, *fs, *tag;
    GLuint prog;
    int done;
    PcJob *next;
};

static pthread_mutex_t s_qlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_qcond = PTHREAD_COND_INITIALIZER;
static PcJob *s_head, *s_tail;
static int s_worker;                /* 1 running, -1 could not start */
static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLContext s_wctx = EGL_NO_CONTEXT;
static EGLSurface s_wsurf = EGL_NO_SURFACE;

static void *worker_main(void *arg)
{
    (void)arg;
    if (!eglMakeCurrent(s_dpy, s_wsurf, s_wsurf, s_wctx)) {
        fprintf(stderr, "[SHADERS] worker context: eglMakeCurrent failed (0x%04X)\n", eglGetError());
        pthread_mutex_lock(&s_qlock);
        s_worker = -1;
        pthread_cond_broadcast(&s_qcond);
        pthread_mutex_unlock(&s_qlock);
        return NULL;
    }
    pthread_mutex_lock(&s_qlock);
    s_worker = 1;
    pthread_cond_broadcast(&s_qcond);
    for (;;) {
        PcJob *j;
        uint64_t key;
        while (!s_head) pthread_cond_wait(&s_qcond, &s_qlock);
        j = s_head;
        s_head = j->next;
        if (!s_head) s_tail = NULL;
        pthread_mutex_unlock(&s_qlock);

        key = src_key(j->vs, j->fs);
        j->prog = build(j->vs, j->fs, j->tag);
        if (j->prog) { s_compiled_async++; cache_store(key, j->prog); }
        /* The render thread's context sees the program once this one is done. */
        glFinish();
        __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);

        pthread_mutex_lock(&s_qlock);
    }
    return NULL;
}

static void worker_start(void)
{
    const char *e = getenv("OT_SHADER_ASYNC");
    EGLContext cur;
    EGLConfig cfg;
    EGLint id = 0, n = 0, stype = 0;
    const char *ext;
    static const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    pthread_t t;
    if (e && e[0] == '0') { s_worker = -1; return; }
    s_dpy = eglGetCurrentDisplay();
    cur = eglGetCurrentContext();
    if (s_dpy == EGL_NO_DISPLAY || cur == EGL_NO_CONTEXT) {
        fprintf(stderr, "[SHADERS] no EGL context: new programs are built on the render thread\n");
        s_worker = -1;
        return;
    }
    eglQueryContext(s_dpy, cur, EGL_CONFIG_ID, &id);
    {
        EGLint attr[] = { EGL_CONFIG_ID, id, EGL_NONE };
        if (!eglChooseConfig(s_dpy, attr, &cfg, 1, &n) || n < 1) { s_worker = -1; return; }
    }
    ext = eglQueryString(s_dpy, EGL_EXTENSIONS);
    if (!(ext && strstr(ext, "EGL_KHR_surfaceless_context"))) {
        /* A 1x1 pbuffer, from a config that can make one. */
        eglGetConfigAttrib(s_dpy, cfg, EGL_SURFACE_TYPE, &stype);
        if (!(stype & EGL_PBUFFER_BIT)) {
            EGLint attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, 0x40 /* ES3 */,
                              EGL_NONE };
            if (!eglChooseConfig(s_dpy, attr, &cfg, 1, &n) || n < 1) { s_worker = -1; return; }
        }
        {
            static const EGLint pb[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
            s_wsurf = eglCreatePbufferSurface(s_dpy, cfg, pb);
            if (s_wsurf == EGL_NO_SURFACE) { s_worker = -1; return; }
        }
    }
    s_wctx = eglCreateContext(s_dpy, cfg, cur, ctx_attr);
    if (s_wctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[SHADERS] no shared context (0x%04X): new programs are built on the render thread\n",
                eglGetError());
        s_worker = -1;
        return;
    }
    if (pthread_create(&t, NULL, worker_main, NULL)) { s_worker = -1; return; }
    pthread_detach(t);
    pthread_mutex_lock(&s_qlock);
    while (!s_worker) pthread_cond_wait(&s_qcond, &s_qlock);
    pthread_mutex_unlock(&s_qlock);
    if (s_worker > 0) fprintf(stderr, "[SHADERS] new vertex-program shaders are built on a worker thread\n");
}

PcJob *pc_link_async(const char *vs, const char *fs, const char *tag)
{
    PcJob *j;
    if (s_worker <= 0 || !(j = (PcJob *)calloc(1, sizeof *j))) return NULL;
    j->vs = vs; j->fs = fs; j->tag = tag;
    pthread_mutex_lock(&s_qlock);
    if (s_tail) s_tail->next = j; else s_head = j;
    s_tail = j;
    pthread_cond_signal(&s_qcond);
    pthread_mutex_unlock(&s_qlock);
    return j;
}

int pc_job_poll(PcJob *j, GLuint *prog)
{
    if (!__atomic_load_n(&j->done, __ATOMIC_ACQUIRE)) return 0;
    *prog = j->prog;
    free(j);
    return 1;
}

void pc_init(void)
{
    static int done;
    if (done++) return;
    cache_open();
    worker_start();
}

void pc_note_fallback(void) { s_fallbacks++; }

void pc_report(void)
{
    static long last;
    long total = s_compiled + s_compiled_async + s_loaded;
    if (total == last) return;
    last = total;
    fprintf(stderr, "[SHADERS] programs: %ld from the disk cache, %ld built on the render thread, "
            "%ld on the worker (%ld draws on the CPU meanwhile), %ld cached binaries refused\n",
            s_loaded, s_compiled, s_compiled_async, s_fallbacks, s_rejected);
}
