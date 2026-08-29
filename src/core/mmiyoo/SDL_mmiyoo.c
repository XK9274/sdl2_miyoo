/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.
*/
#include "../../SDL_internal.h"
#include "SDL.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "SDL_error.h"
#include "SDL_mmiyoo.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

/* Sysfs/file I/O utility, device/hardware identity. Shared input, power,
 * and display helpers live in _input.c, _power.c, and _display.c. */

int
MMIYOO_WriteSysfs(const char *path, const char *value, size_t length)
{
    int fd;
    int saved_errno;
    ssize_t written;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return SDL_SetError("Unable to open %s: %s", path, strerror(errno));
    }

    written = write(fd, value, length);
    saved_errno = errno;
    close(fd);

    if (written != (ssize_t)length) {
        if (written >= 0) {
            return SDL_SetError("Unable to write complete value to %s", path);
        }
        return SDL_SetError("Unable to write %s: %s", path, strerror(saved_errno));
    }

    return 0;
}

SDL_bool
MMIYOO_ReadIntFile(const char *path, int *value)
{
    char buffer[32];
    int fd;
    ssize_t bytes;

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return SDL_FALSE;
    }

    bytes = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);

    if (bytes <= 0) {
        return SDL_FALSE;
    }

    buffer[bytes] = '\0';
    *value = SDL_atoi(buffer);
    return SDL_TRUE;
}

MMIYOO_DeviceModel
MMIYOO_GetDeviceModel(void)
{
    char buffer[64];
    FILE *fp;
    int model = 0;

    /* SdUpgradeImage lives in the bootloader env partition, so it reads the
     * same regardless of which CFW overlay is installed on top. */
    fp = popen("/etc/fw_printenv SdUpgradeImage", "r");
    if (!fp) {
        return MMIYOO_MODEL_UNKNOWN;
    }

    if (!fgets(buffer, sizeof(buffer), fp)) {
        pclose(fp);
        return MMIYOO_MODEL_UNKNOWN;
    }
    pclose(fp);

    if (SDL_sscanf(buffer, "SdUpgradeImage=miyoo%d_fw.img", &model) != 1) {
        return MMIYOO_MODEL_UNKNOWN;
    }

    if (model == MMIYOO_MODEL_MINI || model == MMIYOO_MODEL_PLUS) {
        return (MMIYOO_DeviceModel)model;
    }

    return MMIYOO_MODEL_UNKNOWN;
}

MMIYOO_CFW
MMIYOO_GetCFW(void)
{
    /* TODO: detect CFW identity once marker paths/strings are confirmed. */
    return MMIYOO_CFW_UNKNOWN;
}

SDL_bool
MMIYOO_ProbeHardware(void)
{
    /* /dev/mi_sys confirms a Sigmastar board; /customer/app/MainUI confirms
     * it's specifically a Miyoo firmware image (any CFW). Existence checks
     * only -- no open/ioctl, so this can't take over hardware or crash on a
     * non-Miyoo box. */
    if (access("/dev/mi_sys", F_OK) != 0) {
        return SDL_FALSE;
    }

    if (access("/customer/app/MainUI", F_OK) != 0) {
        return SDL_FALSE;
    }

    return SDL_TRUE;
}

/* vi: set ts=4 sw=4 expandtab: */
