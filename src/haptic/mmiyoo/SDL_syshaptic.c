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

#include "SDL_haptic.h"
#include "SDL_timer.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"
#include "../SDL_syshaptic.h"

struct haptic_hwdata {
    SDL_TimerID timer;
    SDL_bool playing;
    SDL_bool paused;
    SDL_bool enabled;      /* on/off state to restore on resume */
    Uint32 start_ticks;    /* SDL_GetTicks() when timer was armed, 0 = no timer */
    Uint32 length_ms;      /* remaining/full duration; 0 = infinite or no timer */
};

struct haptic_hweffect {
    int unused;
};

static SDL_Haptic *open_haptic = NULL;

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
    return 0;
}

int
SDL_SYS_NumHaptics(void)
{
    return MMIYOO_HasRumble() ? 1 : 0;
}

const char *
SDL_SYS_HapticName(int index)
{
    if (!MMIYOO_HasRumble() || index != 0) {
        SDL_SetError("No such haptic device");
        return NULL;
    }

    return "MMiyoo Rumble";
}

int
SDL_SYS_HapticOpen(SDL_Haptic *haptic)
{
    if (!MMIYOO_HasRumble() || haptic->index != 0) {
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

    return MMIYOO_HasRumble() ? 1 : 0;
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
    haptic->hwdata->paused = SDL_FALSE;
    haptic->hwdata->enabled = enabled;
    haptic->hwdata->length_ms = (length != SDL_HAPTIC_INFINITY) ? length : 0;
    haptic->hwdata->start_ticks = 0;
    if (enabled && length > 0 && length != SDL_HAPTIC_INFINITY) {
        haptic->hwdata->start_ticks = SDL_GetTicks();
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
        haptic->hwdata->paused = SDL_FALSE;
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

    return (haptic && haptic->hwdata && haptic->hwdata->playing && !haptic->hwdata->paused) ? 1 : 0;
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
    if (!haptic || !haptic->hwdata || !haptic->hwdata->playing || haptic->hwdata->paused) {
        return 0;
    }

    if (haptic->hwdata->timer) {
        Uint32 elapsed;

        SDL_RemoveTimer(haptic->hwdata->timer);
        haptic->hwdata->timer = 0;

        elapsed = SDL_GetTicks() - haptic->hwdata->start_ticks;
        haptic->hwdata->length_ms = (elapsed < haptic->hwdata->length_ms)
            ? (haptic->hwdata->length_ms - elapsed) : 0;
    }

    haptic->hwdata->paused = SDL_TRUE;
    return MMIYOO_SetRumble(SDL_FALSE);
}

int
SDL_SYS_HapticUnpause(SDL_Haptic *haptic)
{
    if (!haptic || !haptic->hwdata || !haptic->hwdata->paused) {
        return 0;
    }

    haptic->hwdata->paused = SDL_FALSE;

    if (haptic->hwdata->enabled && haptic->hwdata->length_ms > 0) {
        haptic->hwdata->start_ticks = SDL_GetTicks();
        haptic->hwdata->timer = SDL_AddTimer(haptic->hwdata->length_ms, MMIYOO_HapticTimer, haptic);
    }

    return MMIYOO_SetRumble(haptic->hwdata->enabled);
}

int
SDL_SYS_HapticStopAll(SDL_Haptic *haptic)
{
    return SDL_SYS_HapticStopEffect(haptic, NULL);
}

#endif
