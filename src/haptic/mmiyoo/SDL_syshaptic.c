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

#if defined(SDL_HAPTIC_MMIYOO)

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "SDL_haptic.h"
#include "SDL_timer.h"
#include "../SDL_syshaptic.h"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define MMIYOO_RUMBLE_GPIO "48"
#define MMIYOO_RUMBLE_GPIO_DIR "/sys/class/gpio/gpio48"
#define MMIYOO_RUMBLE_GPIO_DIRECTION MMIYOO_RUMBLE_GPIO_DIR "/direction"
#define MMIYOO_RUMBLE_GPIO_VALUE MMIYOO_RUMBLE_GPIO_DIR "/value"

struct haptic_hwdata {
    SDL_TimerID timer;
    SDL_bool playing;
};

struct haptic_hweffect {
    int unused;
};

static SDL_bool rumble_gpio_ready = SDL_FALSE;
static SDL_bool haptic_available = SDL_FALSE;
static SDL_Haptic *open_haptic = NULL;

static int
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

static int
MMIYOO_InitRumbleGPIO(void)
{
    int result;

    if (rumble_gpio_ready) {
        return 0;
    }

    if (access(MMIYOO_RUMBLE_GPIO_DIR, F_OK) < 0) {
        result = MMIYOO_WriteSysfs("/sys/class/gpio/export", MMIYOO_RUMBLE_GPIO, SDL_strlen(MMIYOO_RUMBLE_GPIO));
        if (result < 0 && errno != EBUSY) {
            return result;
        }
    }

    result = MMIYOO_WriteSysfs(MMIYOO_RUMBLE_GPIO_DIRECTION, "out", 3);
    if (result < 0) {
        return result;
    }

    rumble_gpio_ready = SDL_TRUE;
    return MMIYOO_WriteSysfs(MMIYOO_RUMBLE_GPIO_VALUE, "1", 1);
}

static int
MMIYOO_SetRumble(SDL_bool enabled)
{
    int result;

    result = MMIYOO_InitRumbleGPIO();
    if (result < 0) {
        return result;
    }

    return MMIYOO_WriteSysfs(MMIYOO_RUMBLE_GPIO_VALUE, enabled ? "0" : "1", 1);
}

static Uint32 SDLCALL
MMIYOO_HapticTimer(Uint32 interval, void *param)
{
    SDL_Haptic *haptic = (SDL_Haptic *)param;

    (void)interval;

    if (haptic && haptic->hwdata) {
        haptic->hwdata->timer = 0;
        haptic->hwdata->playing = SDL_FALSE;
    }
    MMIYOO_SetRumble(SDL_FALSE);

    return 0;
}

int
SDL_SYS_HapticInit(void)
{
    haptic_available = (MMIYOO_InitRumbleGPIO() == 0) ? SDL_TRUE : SDL_FALSE;
    return 0;
}

int
SDL_SYS_NumHaptics(void)
{
    return haptic_available ? 1 : 0;
}

const char *
SDL_SYS_HapticName(int index)
{
    if (!haptic_available || index != 0) {
        SDL_SetError("No such haptic device");
        return NULL;
    }

    return "MMiyoo Rumble";
}

int
SDL_SYS_HapticOpen(SDL_Haptic *haptic)
{
    if (!haptic_available || haptic->index != 0) {
        return SDL_SetError("No such haptic device");
    }

    haptic->hwdata = (struct haptic_hwdata *)SDL_calloc(1, sizeof(*haptic->hwdata));
    if (!haptic->hwdata) {
        return SDL_OutOfMemory();
    }

    haptic->supported = SDL_HAPTIC_LEFTRIGHT;
    haptic->neffects = 1;
    haptic->nplaying = 1;
    haptic->naxes = 0;
    haptic->effects = (struct haptic_effect *)SDL_calloc(1, sizeof(struct haptic_effect));
    if (!haptic->effects) {
        SDL_free(haptic->hwdata);
        haptic->hwdata = NULL;
        return SDL_OutOfMemory();
    }

    open_haptic = haptic;
    return 0;
}

int
SDL_SYS_HapticMouse(void)
{
    return -1;
}

int
SDL_SYS_JoystickIsHaptic(SDL_Joystick *joystick)
{
    (void)joystick;

    return haptic_available ? 1 : 0;
}

int
SDL_SYS_HapticOpenFromJoystick(SDL_Haptic *haptic, SDL_Joystick *joystick)
{
    (void)joystick;

    haptic->index = 0;
    return SDL_SYS_HapticOpen(haptic);
}

int
SDL_SYS_JoystickSameHaptic(SDL_Haptic *haptic, SDL_Joystick *joystick)
{
    (void)joystick;

    return (haptic && haptic->index == 0) ? 1 : 0;
}

void
SDL_SYS_HapticClose(SDL_Haptic *haptic)
{
    if (haptic && haptic->hwdata) {
        if (haptic->hwdata->timer) {
            SDL_RemoveTimer(haptic->hwdata->timer);
            haptic->hwdata->timer = 0;
        }
        MMIYOO_SetRumble(SDL_FALSE);
        SDL_free(haptic->effects);
        haptic->effects = NULL;
        SDL_free(haptic->hwdata);
        haptic->hwdata = NULL;
    }

    if (open_haptic == haptic) {
        open_haptic = NULL;
    }
}

void
SDL_SYS_HapticQuit(void)
{
    MMIYOO_SetRumble(SDL_FALSE);
    open_haptic = NULL;
}

int
SDL_SYS_HapticNewEffect(SDL_Haptic *haptic, struct haptic_effect *effect, SDL_HapticEffect *base)
{
    (void)haptic;

    if (base->type != SDL_HAPTIC_LEFTRIGHT) {
        return SDL_SetError("Haptic: Effect not supported by haptic device.");
    }

    effect->hweffect = (struct haptic_hweffect *)SDL_calloc(1, sizeof(*effect->hweffect));
    if (!effect->hweffect) {
        return SDL_OutOfMemory();
    }

    return 0;
}

int
SDL_SYS_HapticUpdateEffect(SDL_Haptic *haptic, struct haptic_effect *effect, SDL_HapticEffect *data)
{
    (void)haptic;
    (void)effect;

    if (data->type != SDL_HAPTIC_LEFTRIGHT) {
        return SDL_SetError("Haptic: Effect not supported by haptic device.");
    }

    return 0;
}

int
SDL_SYS_HapticRunEffect(SDL_Haptic *haptic, struct haptic_effect *effect, Uint32 iterations)
{
    SDL_bool enabled;
    Uint32 length;

    (void)iterations;

    if (!haptic || !haptic->hwdata || effect->effect.type != SDL_HAPTIC_LEFTRIGHT) {
        return SDL_SetError("Haptic: Invalid Miyoo rumble effect");
    }

    if (haptic->hwdata->timer) {
        SDL_RemoveTimer(haptic->hwdata->timer);
        haptic->hwdata->timer = 0;
    }

    enabled = (effect->effect.leftright.large_magnitude || effect->effect.leftright.small_magnitude) ? SDL_TRUE : SDL_FALSE;
    length = effect->effect.leftright.length;

    if (MMIYOO_SetRumble(enabled) < 0) {
        return -1;
    }

    haptic->hwdata->playing = enabled;
    if (enabled && length > 0 && length != SDL_HAPTIC_INFINITY) {
        haptic->hwdata->timer = SDL_AddTimer(length, MMIYOO_HapticTimer, haptic);
    }

    return 0;
}

int
SDL_SYS_HapticStopEffect(SDL_Haptic *haptic, struct haptic_effect *effect)
{
    (void)effect;

    if (haptic && haptic->hwdata && haptic->hwdata->timer) {
        SDL_RemoveTimer(haptic->hwdata->timer);
        haptic->hwdata->timer = 0;
    }
    if (haptic && haptic->hwdata) {
        haptic->hwdata->playing = SDL_FALSE;
    }

    return MMIYOO_SetRumble(SDL_FALSE);
}

void
SDL_SYS_HapticDestroyEffect(SDL_Haptic *haptic, struct haptic_effect *effect)
{
    (void)haptic;

    if (effect && effect->hweffect) {
        SDL_free(effect->hweffect);
        effect->hweffect = NULL;
    }
}

int
SDL_SYS_HapticGetEffectStatus(SDL_Haptic *haptic, struct haptic_effect *effect)
{
    (void)effect;

    return (haptic && haptic->hwdata && haptic->hwdata->playing) ? 1 : 0;
}

int
SDL_SYS_HapticSetGain(SDL_Haptic *haptic, int gain)
{
    (void)haptic;
    (void)gain;

    return 0;
}

int
SDL_SYS_HapticSetAutocenter(SDL_Haptic *haptic, int autocenter)
{
    (void)haptic;
    (void)autocenter;

    return SDL_Unsupported();
}

int
SDL_SYS_HapticPause(SDL_Haptic *haptic)
{
    (void)haptic;

    return MMIYOO_SetRumble(SDL_FALSE);
}

int
SDL_SYS_HapticUnpause(SDL_Haptic *haptic)
{
    (void)haptic;

    return 0;
}

int
SDL_SYS_HapticStopAll(SDL_Haptic *haptic)
{
    return SDL_SYS_HapticStopEffect(haptic, NULL);
}

#endif
