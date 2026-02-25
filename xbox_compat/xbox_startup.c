/* Xbox hardware pre-initialization and debug logging.
 * Called before main() via constructor attribute, so hardware resets run
 * before SDL_Init.
 *
 * - AC97 cold reset: clears stale audio DMA/interrupts from dashboard or
 *   previous app launch.  Without this, audio may not work on re-launch.
 * - Video mode: re-applies dashboard-configured resolution so nxdk HAL
 *   framebuffer matches (480i/480p/720p/1080i autodetect). */

#include <hal/video.h>
#include <hal/audio.h>
#include <hal/debug.h>
#include <hal/xbox.h>
#include <stdarg.h>
#include <stdio.h>    /* vsnprintf */
#include <stdlib.h>   /* atexit */
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

/* atexit handler: stop audio DMA and pause hardware before the quick-reboot
 * that nxdk's exit() triggers.  This gives the next app launch a cleaner
 * starting state. */
static void xbox_cleanup(void)
{
    xbox_log("XBOX: cleanup — pausing audio hardware\n");
    XAudioPause();
}

__attribute__((constructor))
static void xbox_hw_preinit(void)
{
    /* --- AC97 audio cold reset ---
     * Quick-reboots may leave the AC97 controller with stale DMA descriptors,
     * running engines, or pending interrupts.  XAudioInit performs a full cold
     * reset (toggle bit 1 of MMIO 0x12C, wait for 0x130 bit 8, reset
     * busmasters, clear IRQs, enable S/PDIF PCI bit, re-register ISR).
     * Passing NULL callback leaves audio paused. */
    XAudioInit(16, 2, NULL, NULL);

    /* --- Video mode --- */
    VIDEO_MODE vm = XVideoGetMode();
    if (vm.width > 0 && vm.height > 0) {
        XVideoSetMode(vm.width, vm.height, 32, REFRESH_DEFAULT);
    } else {
        XVideoSetMode(640, 480, 32, REFRESH_60HZ);
    }

    /* Register cleanup handler (runs before exit()'s HalReturnToFirmware). */
    atexit(xbox_cleanup);

    /* Open log file */
    static const char * const log_paths[] = {
        "D:\\dn3d_debug.log",
        "E:\\test_xemu.log",
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
