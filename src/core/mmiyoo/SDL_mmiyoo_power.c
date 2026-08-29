/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2022 Sam Lantinga <slouken@libsdl.org>
  Copyright (C) 2026-2026 XK9274 <xk.github@pm.me>

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
#include <sys/ioctl.h>

#include "SDL_error.h"
#include "SDL_mmiyoo.h"

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

/* Rumble (GPIO actuator control) and battery/charging status (GPIO + ADC
 * sensing), used by the joystick and haptic backends (rumble) and the
 * power backend (battery). */

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
