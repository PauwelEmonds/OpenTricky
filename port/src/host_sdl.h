/*
 * host_sdl.h -- the game's host on Linux and Android (SDL2): the window, the
 * event loop, keyboard / controller / touch input, where files live, and the
 * menu before the game. The Windows build uses the Win32 window of the
 * renderer and the launcher window instead. See host_sdl.c.
 */
#ifndef SSX_HOST_SDL_H
#define SSX_HOST_SDL_H

#include <windows.h>

struct LauncherConfig;

/* Folder for the settings file, the log, the saves and the screenshots:
 * beside the executable on Linux, the app's own files on Android. */
const char *host_data_dir(void);

/* Size of the display the window is on. */
void host_display_size(int *w, int *h);

/* Show a file or folder to the player, if the platform can. */
void host_open_path(const char *path, BOOL folder);

/* The menu before the game. TRUE = play with `cfg`. */
BOOL host_launcher_run(struct LauncherConfig *cfg);

/* Run the game: SDL, the window, then `game_main` on a thread of its own
 * with a large stack while this (the main) thread handles the window's
 * events. Returns the process exit code. */
int host_run(int (*game_main)(void));

#endif /* SSX_HOST_SDL_H */
