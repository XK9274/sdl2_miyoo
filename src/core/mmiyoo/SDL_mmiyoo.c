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

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/fb.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "SDL_atomic.h"
#include "SDL_error.h"
#include "SDL_hints.h"
#include "SDL_log.h"
#include "SDL_mutex.h"
#include "SDL_pixels.h"
#include "SDL_thread.h"
#include "SDL_mmiyoo.h"
#include "../../thread/SDL_systhread.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define MMIYOO_GPIO_EXPORT "/sys/class/gpio/export"

#define MMIYOO_RUMBLE_GPIO "48"
#define MMIYOO_RUMBLE_GPIO_DIR "/sys/class/gpio/gpio48"
#define MMIYOO_RUMBLE_GPIO_DIRECTION MMIYOO_RUMBLE_GPIO_DIR "/direction"
#define MMIYOO_RUMBLE_GPIO_VALUE MMIYOO_RUMBLE_GPIO_DIR "/value"

#define MMIYOO_CHARGE_GPIO "59"
#define MMIYOO_CHARGE_GPIO_DIR "/sys/devices/gpiochip0/gpio/gpio59"
#define MMIYOO_CHARGE_GPIO_DIRECTION MMIYOO_CHARGE_GPIO_DIR "/direction"
#define MMIYOO_CHARGE_GPIO_VALUE MMIYOO_CHARGE_GPIO_DIR "/value"

#define SARADC_IOC_MAGIC 'a'
#define IOCTL_SAR_INIT _IO(SARADC_IOC_MAGIC, 0)
#define IOCTL_SAR_SET_CHANNEL_READ_VALUE _IO(SARADC_IOC_MAGIC, 1)

typedef struct {
    int channel_value;
    int adc_value;
} SAR_ADC_CONFIG_READ;

static SDL_bool rumble_gpio_ready = SDL_FALSE;

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
    int model = 0;

    if (!MMIYOO_ReadIntFile("/tmp/deviceModel", &model)) {
        return MMIYOO_MODEL_UNKNOWN;
    }

    if (model == MMIYOO_MODEL_MINI || model == MMIYOO_MODEL_PLUS) {
        return (MMIYOO_DeviceModel)model;
    }

    return MMIYOO_MODEL_UNKNOWN;
}

Uint32
MMIYOO_KeycodeToButtonMask(int code)
{
    switch (code) {
    case KEY_UP:
        return (1u << MMIYOO_BUTTON_UP);
    case KEY_DOWN:
        return (1u << MMIYOO_BUTTON_DOWN);
    case KEY_LEFT:
        return (1u << MMIYOO_BUTTON_LEFT);
    case KEY_RIGHT:
        return (1u << MMIYOO_BUTTON_RIGHT);
    case KEY_SPACE:
        return (1u << MMIYOO_BUTTON_A);
    case KEY_LEFTCTRL:
        return (1u << MMIYOO_BUTTON_B);
    case KEY_LEFTSHIFT:
        return (1u << MMIYOO_BUTTON_X);
    case KEY_LEFTALT:
        return (1u << MMIYOO_BUTTON_Y);
    case KEY_ENTER:
        return (1u << MMIYOO_BUTTON_START);
    case KEY_RIGHTCTRL:
        return (1u << MMIYOO_BUTTON_SELECT);
    case KEY_E:
        return (1u << MMIYOO_BUTTON_L1);
    case KEY_TAB:
        return (1u << MMIYOO_BUTTON_L2);
    case KEY_T:
        return (1u << MMIYOO_BUTTON_R1);
    case KEY_BACKSPACE:
        return (1u << MMIYOO_BUTTON_R2);
    case KEY_ESC:
        return (1u << MMIYOO_BUTTON_MENU);
    case KEY_POWER:
        return (1u << MMIYOO_BUTTON_POWER);
    case KEY_VOLUMEUP:
        return (1u << MMIYOO_BUTTON_VOLUP);
    case KEY_VOLUMEDOWN:
        return (1u << MMIYOO_BUTTON_VOLDOWN);
    default:
        return 0;
    }
}

/* Shared raw-input layer: a single reader of /dev/input/event0, consumed by
 * both the video backend (keyboard/mouse emulation) and the joystick
 * backend, instead of each independently opening and parsing the device.
 * Reference-counted so either (or both) can init/deinit in any order. */
static SDL_atomic_t s_input_ref_count;
static SDL_atomic_t s_input_running;
static int s_input_fd = -1;
static SDL_mutex *s_input_mutex = NULL;
static SDL_Thread *s_input_thread = NULL;
static Uint32 s_keypad_bitmap = 0;

static int
MMIYOO_InputThread(void *data)
{
    struct input_event ev = {0};

    (void)data;

    while (SDL_AtomicGet(&s_input_running)) {
        if (s_input_fd >= 0) {
            ssize_t bytes;

            while ((bytes = read(s_input_fd, &ev, sizeof(ev))) == sizeof(ev)) {
                if ((ev.type == EV_KEY) && (ev.value != 2)) {
                    const Uint32 bit = MMIYOO_KeycodeToButtonMask(ev.code);

                    if (bit) {
                        SDL_LockMutex(s_input_mutex);
                        if (ev.value) {
                            s_keypad_bitmap |= bit;
                        } else {
                            s_keypad_bitmap &= ~bit;
                        }
                        SDL_UnlockMutex(s_input_mutex);
                    }
                }
            }

            if ((bytes < 0) && (errno != EAGAIN) && (errno != EWOULDBLOCK) && (errno != EINTR)) {
                usleep(1000000 / 60);
            }
        }
        usleep(1000000 / 60);
    }

    return 0;
}

void
MMIYOO_InputInit(void)
{
    if (SDL_AtomicIncRef(&s_input_ref_count) > 0) {
        return;
    }

    s_input_mutex = SDL_CreateMutex();
    if (!s_input_mutex) {
        return;
    }

#if defined(MMIYOO)
    s_input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (s_input_fd < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_INPUT,
                    "MMIYOO_InputInit: failed to open /dev/input/event0: %s",
                    strerror(errno));
    }
#endif

    SDL_AtomicSet(&s_input_running, 1);
    s_input_thread = SDL_CreateThreadInternal(MMIYOO_InputThread, "MMIYOOInputThread", 4096, NULL);
    if (!s_input_thread) {
        SDL_AtomicSet(&s_input_running, 0);
        if (s_input_fd >= 0) {
            close(s_input_fd);
            s_input_fd = -1;
        }
    }
}

void
MMIYOO_InputDeinit(void)
{
    /* SDL_AtomicDecRef only returns true once the count truly reaches zero --
     * skip teardown while another owner (joystick or event backend) still
     * holds a reference. */
    if (!SDL_AtomicDecRef(&s_input_ref_count)) {
        return;
    }

    SDL_AtomicSet(&s_input_running, 0);
    if (s_input_thread) {
        SDL_WaitThread(s_input_thread, NULL);
        s_input_thread = NULL;
    }
    if (s_input_mutex) {
        SDL_DestroyMutex(s_input_mutex);
        s_input_mutex = NULL;
    }
    if (s_input_fd >= 0) {
        close(s_input_fd);
        s_input_fd = -1;
    }
}

Uint32
MMIYOO_GetKeypadBitmap(void)
{
    Uint32 bitmap = 0;

    if (s_input_mutex) {
        SDL_LockMutex(s_input_mutex);
        bitmap = s_keypad_bitmap;
        SDL_UnlockMutex(s_input_mutex);
    }

    return bitmap;
}

SDL_bool
MMIYOO_IsKeyboardModeActive(void)
{
    const char *mode = SDL_GetHint(SDL_HINT_MMIYOO_INPUT_MODE);
    return (mode && SDL_strcmp(mode, MMIYOO_INPUT_MODE_KEYBOARD) == 0) ? SDL_TRUE : SDL_FALSE;
}

SDL_bool
MMIYOO_IsJoystickModeActive(void)
{
    return MMIYOO_IsKeyboardModeActive() ? SDL_FALSE : SDL_TRUE;
}

MMIYOO_VSyncMode_e
MMIYOO_GetVSyncMode(void)
{
    const char *mode = SDL_GetHint(SDL_HINT_MMIYOO_VSYNC_MODE);
    if (mode && SDL_strcmp(mode, "adaptive") == 0) {
        return MMIYOO_VSYNC_MODE_ADAPTIVE;
    }
    if (mode && SDL_strcmp(mode, "strict") == 0) {
        return MMIYOO_VSYNC_MODE_STRICT;
    }
    /* Default is "off" - see SDL_HINT_MMIYOO_VSYNC_MODE comment in SDL_mmiyoo.h. */
    return MMIYOO_VSYNC_MODE_OFF;
}

MMIYOO_VSyncMode_e
MMIYOO_ResolvePresentVSyncMode(SDL_bool renderer_vsync_requested)
{
    const char *mode = SDL_GetHint(SDL_HINT_MMIYOO_VSYNC_MODE);
    if (mode && *mode) {
        return MMIYOO_GetVSyncMode();
    }
    return renderer_vsync_requested ? MMIYOO_VSYNC_MODE_ADAPTIVE : MMIYOO_VSYNC_MODE_OFF;
}

void
MMIYOO_GetDefaultFramebufferInfo(MMIYOO_FramebufferInfo *info)
{
    if (!info) {
        return;
    }

    info->width = MMIYOO_DEFAULT_FRAMEBUFFER_WIDTH;
    info->height = MMIYOO_DEFAULT_FRAMEBUFFER_HEIGHT;
    info->bytes_per_pixel = MMIYOO_DEFAULT_FRAMEBUFFER_BYTES_PER_PIXEL;
    info->stride = info->width * info->bytes_per_pixel;
    info->active_size = info->stride * info->height;
    info->sdl_format = SDL_PIXELFORMAT_ARGB8888;
}

SDL_bool
MMIYOO_GetFramebufferInfoFromFD(int fb_fd, MMIYOO_FramebufferInfo *info)
{
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    SDL_bool have_vinfo;
    SDL_bool have_finfo;

    if (!info) {
        return SDL_FALSE;
    }

    MMIYOO_GetDefaultFramebufferInfo(info);

    if (fb_fd < 0) {
        return SDL_FALSE;
    }

    SDL_zero(vinfo);
    SDL_zero(finfo);
    have_vinfo = (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) ? SDL_TRUE : SDL_FALSE;
    have_finfo = (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) ? SDL_TRUE : SDL_FALSE;

    if (!have_vinfo && !have_finfo) {
        return SDL_FALSE;
    }

    if (have_vinfo) {
        if (vinfo.xres > 0) {
            info->width = (int)vinfo.xres;
        }
        if (vinfo.yres > 0) {
            info->height = (int)vinfo.yres;
        }
        if (vinfo.bits_per_pixel > 0) {
            info->bytes_per_pixel = (int)((vinfo.bits_per_pixel + 7) / 8);
            if (info->bytes_per_pixel <= 0) {
                info->bytes_per_pixel = MMIYOO_DEFAULT_FRAMEBUFFER_BYTES_PER_PIXEL;
            }
            info->sdl_format = (vinfo.bits_per_pixel == 16) ? SDL_PIXELFORMAT_RGB565 : SDL_PIXELFORMAT_ARGB8888;
        }
    }

    if (have_finfo && finfo.line_length > 0) {
        info->stride = (int)finfo.line_length;
    } else {
        info->stride = info->width * info->bytes_per_pixel;
    }

    info->active_size = info->stride * info->height;
    if (info->active_size <= 0) {
        info->active_size = info->width * info->height * info->bytes_per_pixel;
    }

    return SDL_TRUE;
}

static int
MMIYOO_InitRumbleGPIO(void)
{
    int result;

    if (rumble_gpio_ready) {
        return 0;
    }

    if (access(MMIYOO_RUMBLE_GPIO_DIR, F_OK) < 0) {
        MMIYOO_WriteSysfs(MMIYOO_GPIO_EXPORT, MMIYOO_RUMBLE_GPIO, SDL_strlen(MMIYOO_RUMBLE_GPIO));
    }

    result = MMIYOO_WriteSysfs(MMIYOO_RUMBLE_GPIO_DIRECTION, "out", 3);
    if (result < 0) {
        return result;
    }

    result = MMIYOO_WriteSysfs(MMIYOO_RUMBLE_GPIO_VALUE, "1", 1);
    if (result < 0) {
        return result;
    }

    rumble_gpio_ready = SDL_TRUE;
    return 0;
}

SDL_bool
MMIYOO_HasRumble(void)
{
    return (MMIYOO_InitRumbleGPIO() == 0) ? SDL_TRUE : SDL_FALSE;
}

int
MMIYOO_SetRumble(SDL_bool enabled)
{
    int result;

    result = MMIYOO_InitRumbleGPIO();
    if (result < 0) {
        return result;
    }

    return MMIYOO_WriteSysfs(MMIYOO_RUMBLE_GPIO_VALUE, enabled ? "0" : "1", 1);
}

static SDL_bool
MMIYOO_ReadAxpTest(int *battery, int *charging)
{
    char buffer[128];
    FILE *fp;
    int parsed_battery = -1;
    int parsed_charging = -1;

    fp = popen("cd /customer/app/ ; ./axp_test", "r");
    if (!fp) {
        return SDL_FALSE;
    }

    if (!fgets(buffer, sizeof(buffer), fp)) {
        pclose(fp);
        return SDL_FALSE;
    }
    pclose(fp);

    if (SDL_sscanf(buffer, "{\"battery\":%d, \"voltage\":%*d, \"charging\":%d}", &parsed_battery, &parsed_charging) != 2) {
        return SDL_FALSE;
    }

    if (battery) {
        *battery = parsed_battery;
    }
    if (charging) {
        *charging = parsed_charging;
    }

    return SDL_TRUE;
}

static SDL_bool
MMIYOO_IsChargingGPIO(SDL_bool *charging)
{
    char value = '\0';
    int fd;

    fd = open(MMIYOO_CHARGE_GPIO_VALUE, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        MMIYOO_WriteSysfs(MMIYOO_GPIO_EXPORT, MMIYOO_CHARGE_GPIO, SDL_strlen(MMIYOO_CHARGE_GPIO));
        MMIYOO_WriteSysfs(MMIYOO_CHARGE_GPIO_DIRECTION, "in", 2);
        fd = open(MMIYOO_CHARGE_GPIO_VALUE, O_RDONLY | O_CLOEXEC);
    }

    if (fd < 0) {
        return SDL_FALSE;
    }

    if (read(fd, &value, 1) != 1) {
        close(fd);
        return SDL_FALSE;
    }
    close(fd);

    *charging = (value == '1') ? SDL_TRUE : SDL_FALSE;
    return SDL_TRUE;
}

static int
MMIYOO_BatteryPercentFromADC(int value)
{
    if (value == 100) {
        return 500;
    }
    if (value >= 578) {
        return 100;
    }
    if (value >= 528) {
        return value - 478;
    }
    if (value >= 512) {
        return (int)(value * 2.125f - 1068.0f);
    }
    if (value >= 480) {
        return (int)(value * 0.51613f - 243.742f);
    }
    return 0;
}

static SDL_bool
MMIYOO_ReadMiniBatteryADC(int *percent)
{
    SAR_ADC_CONFIG_READ adcConfig;
    int fd;

    fd = open("/dev/sar", O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return SDL_FALSE;
    }

    SDL_zero(adcConfig);
    if (ioctl(fd, IOCTL_SAR_INIT, NULL) < 0 ||
            ioctl(fd, IOCTL_SAR_SET_CHANNEL_READ_VALUE, &adcConfig) < 0) {
        close(fd);
        return SDL_FALSE;
    }

    close(fd);
    *percent = MMIYOO_BatteryPercentFromADC(adcConfig.adc_value);
    return SDL_TRUE;
}

SDL_bool
MMIYOO_IsCharging(SDL_bool *charging)
{
    int axp_charging = -1;
    MMIYOO_DeviceModel model = MMIYOO_GetDeviceModel();

    if (model == MMIYOO_MODEL_PLUS) {
        if (MMIYOO_ReadAxpTest(NULL, &axp_charging)) {
            *charging = (axp_charging == 3) ? SDL_TRUE : SDL_FALSE;
            return SDL_TRUE;
        }
        return SDL_FALSE;
    }

    if (model == MMIYOO_MODEL_MINI) {
        return MMIYOO_IsChargingGPIO(charging);
    }

    if (MMIYOO_ReadAxpTest(NULL, &axp_charging)) {
        *charging = (axp_charging == 3) ? SDL_TRUE : SDL_FALSE;
        return SDL_TRUE;
    }

    return MMIYOO_IsChargingGPIO(charging);
}

SDL_bool
MMIYOO_GetBatteryPercent(int *percent)
{
    int value = -1;
    MMIYOO_DeviceModel model = MMIYOO_GetDeviceModel();

    if (MMIYOO_ReadIntFile("/tmp/percBat", &value)) {
        *percent = value;
        return SDL_TRUE;
    }

    if (model == MMIYOO_MODEL_PLUS || model == MMIYOO_MODEL_UNKNOWN) {
        if (MMIYOO_ReadAxpTest(&value, NULL) && value >= 0) {
            *percent = value;
            return SDL_TRUE;
        }
    }

    if (model == MMIYOO_MODEL_MINI || model == MMIYOO_MODEL_UNKNOWN) {
        return MMIYOO_ReadMiniBatteryADC(percent);
    }

    return SDL_FALSE;
}

/* vi: set ts=4 sw=4 expandtab: */
