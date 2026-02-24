/* Xbox video pre-initialization and debug logging
 * Called before main() via constructor attribute, so XVideoSetMode runs
 * before SDL_Init (SDL2's Xbox backend reads XVideoGetMode() for window size).
 *
 * We read the current mode from the BIOS/dashboard setting (via XVideoGetMode)
 * and re-apply it so nxdk's HAL framebuffer is initialized at the correct size.
 * This preserves whatever the user configured: 480i, 480p, 720p, or 1080i. */

#include <hal/video.h>
#include <hal/debug.h>
#include <stdarg.h>
#include <stdio.h>    /* vsnprintf */
#include <string.h>
#include "fcntl.h"    /* _O_WRONLY / _O_CREAT / _O_TRUNC */

/* Use _open/_write directly (posix_io.c / NtCreateFile) rather than fopen.
 * fopen goes through pdclib stdio which silently failed; _open uses the same
 * NtCreateFile path that successfully reads DUKE3D.GRP. */
static int xbox_log_fd = -1;

/* _write is declared in io.h but we can also just declare it here */
extern int _write(int fd, const void *buf, unsigned int count);
extern int _open(const char *path, int flags, ...);

void xbox_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    int len;

    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (len < 0) len = 0;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;

    /* Kernel debug output — captured by xemu console and debug tools */
    OutputDebugStringA(buf);

    /* File log — written with _write so it survives a crash */
    if (xbox_log_fd >= 0)
        _write(xbox_log_fd, buf, (unsigned)len);
}

__attribute__((constructor))
static void xbox_video_preinit(void)
{
    VIDEO_MODE vm = XVideoGetMode();
    if (vm.width > 0 && vm.height > 0) {
        /* Re-apply the dashboard-configured mode at 32bpp. */
        XVideoSetMode(vm.width, vm.height, 32, REFRESH_DEFAULT);
    } else {
        /* Fallback if no mode was configured: 480p */
        XVideoSetMode(640, 480, 32, REFRESH_60HZ);
    }

    /* Open log file using _open → NtCreateFile (same path that reads GRP).
     * Try several locations; whichever succeeds first is used. */
    static const char * const log_paths[] = {
        "D:\\dn3d_debug.log",
        NULL
    };
    for (int i = 0; log_paths[i] && xbox_log_fd < 0; i++)
        xbox_log_fd = _open(log_paths[i], _O_WRONLY | _O_CREAT | _O_TRUNC, 0);

    xbox_log("=== jfduke3d Xbox log ===\n");
    xbox_log("Video: %dx%d 32bpp  log_fd=%d\n",
        vm.width  > 0 ? vm.width  : 640,
        vm.height > 0 ? vm.height : 480,
        xbox_log_fd);
}
