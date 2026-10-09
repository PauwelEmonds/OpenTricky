/* GL programs: built once, kept on disk, compiled off the render thread when
 * they are new (gles_progcache.c). */
#ifndef GLES_PROGCACHE_H
#define GLES_PROGCACHE_H

#include "gles_internal.h"

/* On the render thread, its context current, after d3d8_SetShaderCacheDir. */
void pc_init(void);

GLuint pc_compile(GLenum type, const char *src, const char *tag);

/* The program of these sources: from the disk cache, or compiled and linked
 * here and then stored. 0 if it does not build. */
GLuint pc_link_sync(const char *vs, const char *fs, const char *tag);

/* Only from the disk cache; 0 if it is not there (nothing is compiled). */
GLuint pc_load_cached(const char *vs, const char *fs);

/* Compile and link on the worker thread (its own context, sharing objects
 * with the render thread's). NULL if there is no worker: link it here. The
 * sources must stay valid until the job is done. */
typedef struct PcJob PcJob;
PcJob *pc_link_async(const char *vs, const char *fs, const char *tag);

/* 0 while it builds; then 1, with *prog the program (0 if it failed). The
 * job is released when it reports 1. */
int pc_job_poll(PcJob *j, GLuint *prog);

/* Counters for the log. */
void pc_note_fallback(void);
void pc_report(void);

#endif
