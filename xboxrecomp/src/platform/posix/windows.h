/*
 * windows.h for POSIX hosts (Linux, Android).
 *
 * Only on the include path when the target is not Windows. It lets the PC
 * sources keep their #include <windows.h> and get the POSIX implementation
 * of the Win32 subset they use (win32_compat.h), plus the C headers the real
 * <windows.h> drags in implicitly and code has come to rely on.
 */
#ifndef OT_POSIX_WINDOWS_H
#define OT_POSIX_WINDOWS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <wchar.h>
#include <ctype.h>
#include <math.h>

#include "win32_compat.h"

#endif /* OT_POSIX_WINDOWS_H */
