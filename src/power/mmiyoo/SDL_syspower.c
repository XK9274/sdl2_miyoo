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

#ifndef SDL_POWER_DISABLED
#if SDL_POWER_MMIYOO

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "SDL_power.h"
#include "../SDL_syspower.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define MMIYOO_MODEL_MINI 283
#define MMIYOO_MODEL_PLUS 354

#define MMIYOO_GPIO_EXPORT "/sys/class/gpio/export"
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

static SDL_bool
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

static SDL_bool
MMIYOO_WriteSysfs(const char *path, const char *value, size_t length)
{
    int fd;
    ssize_t written;

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        return SDL_FALSE;
    }

    written = write(fd, value, length);
    close(fd);

    return (written == (ssize_t)length) ? SDL_TRUE : SDL_FALSE;
}

static int
MMIYOO_GetDeviceModel(void)
{
    int model = 0;

    MMIYOO_ReadIntFile("/tmp/deviceModel", &model);
    return model;
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
        MMIYOO_WriteSysfs(MMIYOO_GPIO_EXPORT, "59", 2);
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

static SDL_bool
MMIYOO_GetChargeState(int model, SDL_bool *charging)
{
    int axp_charging = -1;

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

static SDL_bool
MMIYOO_GetBatteryPercent(int model, int *percent)
{
    int value = -1;

    if (MMIYOO_ReadIntFile("/tmp/percBat", &value)) {
        *percent = value;
        return SDL_TRUE;
    }

    if (model == MMIYOO_MODEL_PLUS || model == 0) {
        if (MMIYOO_ReadAxpTest(&value, NULL) && value >= 0) {
            *percent = value;
            return SDL_TRUE;
        }
    }

    if (model == MMIYOO_MODEL_MINI || model == 0) {
        return MMIYOO_ReadMiniBatteryADC(percent);
    }

    return SDL_FALSE;
}

SDL_bool
SDL_GetPowerInfo_MMIYOO(SDL_PowerState *state, int *seconds, int *percent)
{
    SDL_bool charging = SDL_FALSE;
    SDL_bool have_charging;
    SDL_bool have_percent;
    int model;
    int battery_percent = -1;

    model = MMIYOO_GetDeviceModel();
    have_charging = MMIYOO_GetChargeState(model, &charging);
    have_percent = MMIYOO_GetBatteryPercent(model, &battery_percent);

    *seconds = -1;
    *percent = -1;
    *state = SDL_POWERSTATE_UNKNOWN;

    if (!have_charging && !have_percent) {
        return SDL_FALSE;
    }

    if (have_percent) {
        if (battery_percent >= 0 && battery_percent <= 100) {
            *percent = battery_percent;
        } else if (battery_percent == 500) {
            charging = SDL_TRUE;
            have_charging = SDL_TRUE;
        }
    }

    if (have_charging) {
        if (charging) {
            *state = (*percent == 100) ? SDL_POWERSTATE_CHARGED : SDL_POWERSTATE_CHARGING;
        } else {
            *state = SDL_POWERSTATE_ON_BATTERY;
        }
    } else {
        *state = SDL_POWERSTATE_UNKNOWN;
    }

    return SDL_TRUE;
}

#endif /* SDL_POWER_MMIYOO */
#endif /* SDL_POWER_DISABLED */
