#include "../../SDL_internal.h"

#if SDL_VIDEO_DRIVER_MMIYOO

#include "SDL_video_mmiyoo.h"
#include "SDL_mmiyoo_messagebox.h"
#include "SDL_mmiyoo_font8x8.h"
#include "../../core/mmiyoo/SDL_mmiyoo.h"
#include "SDL_log.h"
#include "SDL_timer.h"

#define MB_PAD           6
#define MB_BTN_PAD_X     6
#define MB_BTN_PAD_Y     4
#define MB_BTN_GAP       6
#define MB_SHADOW_PX     3
#define MB_LINE_GAP      2
#define MB_MAX_BUTTONS   3
#define MB_MAX_MSG_LINES 4
#define MB_MAX_LINE_LEN  96
#define MB_FLUSH_ALIGN   4096u
#define MB_DARKEN_PCT    40 /* percent brightness kept, i.e. ~60% darkened */
#define MB_BLINK_FRAMES  18 /* ~300ms at the loop's 60Hz pace */

#define MB_COLOR_BG      0xFF171717u
#define MB_COLOR_OUTLINE 0xFF3A3A3Au
#define MB_COLOR_SHADOW  0xFF000000u
#define MB_COLOR_TEXT    0xFFE9E9E9u
#define MB_COLOR_BTN_BG  0xFF232323u
#define MB_COLOR_BTN_LN  0xFF3F3F3Fu
#define MB_COLOR_SEL_BG  0xFFE8837Fu
#define MB_COLOR_SEL_TX  0xFF1A1010u

typedef struct {
    Uint32 bg, text, box_outline, btn_bg, btn_border, btn_selected;
} MB_Theme;

typedef struct {
    SDL_Rect rect;
    int buttonid;
    const char *text;
    Uint32 flags;
} MB_Button;

typedef struct {
    SDL_Rect box;
    const char *title;
    char msg_lines[MB_MAX_MSG_LINES][MB_MAX_LINE_LEN];
    int msg_line_count;
    MB_Button buttons[MB_MAX_BUTTONS];
    int button_count;
    int visual_order[MB_MAX_BUTTONS]; /* screen left-to-right -> buttons[] index */
    int focus;                        /* index into visual_order/buttons */
} MB_Layout;

static Uint32
MB_PackRGB(Uint8 r, Uint8 g, Uint8 b)
{
    return 0xFF000000u | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
}

static void
MB_BuildTheme(const SDL_MessageBoxColorScheme *cs, MB_Theme *out)
{
    if (cs) {
        const SDL_MessageBoxColor *c;
        c = &cs->colors[SDL_MESSAGEBOX_COLOR_BACKGROUND];
        out->bg = MB_PackRGB(c->r, c->g, c->b);
        c = &cs->colors[SDL_MESSAGEBOX_COLOR_TEXT];
        out->text = MB_PackRGB(c->r, c->g, c->b);
        /* SDL_MessageBoxColorScheme has no dedicated dialog-outline entry;
         * BUTTON_BORDER is the closest "border" semantic it offers. */
        c = &cs->colors[SDL_MESSAGEBOX_COLOR_BUTTON_BORDER];
        out->box_outline = MB_PackRGB(c->r, c->g, c->b);
        out->btn_border = out->box_outline;
        c = &cs->colors[SDL_MESSAGEBOX_COLOR_BUTTON_BACKGROUND];
        out->btn_bg = MB_PackRGB(c->r, c->g, c->b);
        c = &cs->colors[SDL_MESSAGEBOX_COLOR_BUTTON_SELECTED];
        out->btn_selected = MB_PackRGB(c->r, c->g, c->b);
    } else {
        out->bg = MB_COLOR_BG;
        out->text = MB_COLOR_TEXT;
        out->box_outline = MB_COLOR_OUTLINE;
        out->btn_bg = MB_COLOR_BTN_BG;
        out->btn_border = MB_COLOR_BTN_LN;
        out->btn_selected = MB_COLOR_SEL_BG;
    }
}

/* Panel is mounted physically upside-down -- writes need a full point-
 * reflection through the screen center, not just a repositioned copy, or
 * text renders upside-down and mirrored. */
static inline void
MB_Plot(Uint8 *base, Uint32 stride, int screen_w, int screen_h, int lx, int ly, Uint32 argb)
{
    int fx, fy;
    if (lx < 0 || ly < 0 || lx >= screen_w || ly >= screen_h) {
        return;
    }
    fx = screen_w - 1 - lx;
    fy = screen_h - 1 - ly;
    *(Uint32 *)(base + (size_t)fy * stride + (size_t)fx * 4) = argb;
}

static void
MB_FillRect(Uint8 *base, Uint32 stride, int screen_w, int screen_h, const SDL_Rect *r, Uint32 argb)
{
    int row, col;
    for (row = 0; row < r->h; ++row) {
        for (col = 0; col < r->w; ++col) {
            MB_Plot(base, stride, screen_w, screen_h, r->x + col, r->y + row, argb);
        }
    }
}

static void
MB_DrawChar(Uint8 *base, Uint32 stride, int screen_w, int screen_h, int lx, int ly, char ch, Uint32 argb)
{
    const unsigned char *glyph = MMIYOO_Font8x8_Glyph((unsigned char)ch);
    int row, col;
    for (row = 0; row < MMIYOO_FONT8X8_H; ++row) {
        unsigned char bits = glyph[row];
        for (col = 0; col < MMIYOO_FONT8X8_W; ++col) {
            if (bits & (1u << col)) {
                MB_Plot(base, stride, screen_w, screen_h, lx + col, ly + row, argb);
            }
        }
    }
}

static void
MB_DrawText(Uint8 *base, Uint32 stride, int screen_w, int screen_h, int lx, int ly, const char *text, Uint32 argb)
{
    int cx = lx;
    for (; *text; ++text) {
        MB_DrawChar(base, stride, screen_w, screen_h, cx, ly, *text, argb);
        cx += MMIYOO_FONT8X8_W;
    }
}

static int
MB_TextWidth(const char *text)
{
    return (int)SDL_strlen(text) * MMIYOO_FONT8X8_W;
}

static int
MB_WrapMessage(const char *message, int max_width_px, char lines[][MB_MAX_LINE_LEN], int max_lines)
{
    int max_chars = max_width_px / MMIYOO_FONT8X8_W;
    int n = 0;
    const char *p = message;

    if (max_chars < 1) {
        max_chars = 1;
    }
    if (max_chars > MB_MAX_LINE_LEN - 1) {
        max_chars = MB_MAX_LINE_LEN - 1;
    }

    while (*p && n < max_lines) {
        int len = 0;
        int last_space = -1;
        const char *start = p;

        while (p[len] && p[len] != '\n' && len < max_chars) {
            if (p[len] == ' ') {
                last_space = len;
            }
            len++;
        }
        if (p[len] && p[len] != '\n' && p[len] != ' ' && last_space >= 0) {
            len = last_space;
        }

        SDL_memcpy(lines[n], start, (size_t)len);
        lines[n][len] = '\0';
        n++;

        p += len;
        while (*p == ' ') {
            p++;
        }
        if (*p == '\n') {
            p++;
        }
    }

    return n;
}

static void
MB_ComputeLayout(const SDL_MessageBoxData *data, int screen_w, int screen_h, MB_Layout *out)
{
    int box_w, box_h_floor, interior_w;
    int title_h, msg_h, btn_row_h, content_h;
    int i;
    SDL_bool right_to_left = (data->flags & SDL_MESSAGEBOX_BUTTONS_RIGHT_TO_LEFT) ? SDL_TRUE : SDL_FALSE;

    SDL_zerop(out);

    box_w = (screen_w * 60) / 100;
    box_h_floor = (screen_h * 10) / 100;
    interior_w = box_w - 2 * MB_PAD;
    if (interior_w < MMIYOO_FONT8X8_W) {
        interior_w = MMIYOO_FONT8X8_W;
    }

    out->title = (data->title && data->title[0]) ? data->title : NULL;
    title_h = out->title ? (MMIYOO_FONT8X8_H + MB_LINE_GAP) : 0;

    if (data->message && data->message[0]) {
        out->msg_line_count = MB_WrapMessage(data->message, interior_w, out->msg_lines, MB_MAX_MSG_LINES);
    }
    msg_h = out->msg_line_count * (MMIYOO_FONT8X8_H + MB_LINE_GAP);

    out->button_count = data->numbuttons;
    if (out->button_count > MB_MAX_BUTTONS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_VIDEO, "[MMIYOO] ShowMessageBox: clamping %d buttons to %d",
                    out->button_count, MB_MAX_BUTTONS);
        out->button_count = MB_MAX_BUTTONS;
    }
    btn_row_h = (out->button_count > 0) ? (MMIYOO_FONT8X8_H + 2 * MB_BTN_PAD_Y) : 0;

    content_h = MB_PAD * 2 + title_h + msg_h + (btn_row_h > 0 ? (MB_PAD + btn_row_h) : 0);

    out->box.w = box_w;
    out->box.h = (content_h > box_h_floor) ? content_h : box_h_floor;
    out->box.x = (screen_w - out->box.w) / 2;
    out->box.y = (screen_h - out->box.h) / 2;

    if (out->button_count > 0) {
        int widths[MB_MAX_BUTTONS];
        int total_w = 0;
        int row_y = out->box.y + out->box.h - MB_PAD - btn_row_h;
        int cursor_x;
        int k;

        for (i = 0; i < out->button_count; ++i) {
            const char *text = data->buttons[i].text ? data->buttons[i].text : "";
            widths[i] = MB_TextWidth(text) + 2 * MB_BTN_PAD_X;
            total_w += widths[i];
            if (i > 0) {
                total_w += MB_BTN_GAP;
            }
            out->buttons[i].buttonid = data->buttons[i].buttonid;
            out->buttons[i].text = text;
            out->buttons[i].flags = data->buttons[i].flags;
        }

        cursor_x = out->box.x + out->box.w - MB_PAD - total_w;
        for (k = 0; k < out->button_count; ++k) {
            int src = right_to_left ? (out->button_count - 1 - k) : k;
            out->visual_order[k] = src;
            out->buttons[src].rect.x = cursor_x;
            out->buttons[src].rect.y = row_y;
            out->buttons[src].rect.w = widths[src];
            out->buttons[src].rect.h = btn_row_h;
            cursor_x += widths[src] + MB_BTN_GAP;
        }

        out->focus = 0;
        for (k = 0; k < out->button_count; ++k) {
            if (out->buttons[out->visual_order[k]].flags & SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT) {
                out->focus = k;
                break;
            }
        }
    }
}

static void
MB_SnapshotDarken(Uint8 *dst, const Uint8 *src, Uint32 stride, int width, int height, int percent)
{
    int row, col;
    for (row = 0; row < height; ++row) {
        const Uint32 *srow = (const Uint32 *)(src + (size_t)row * stride);
        Uint32 *drow = (Uint32 *)(dst + (size_t)row * stride);
        for (col = 0; col < width; ++col) {
            Uint32 px = srow[col];
            Uint8 r = (Uint8)(((px >> 16) & 0xFF) * percent / 100);
            Uint8 g = (Uint8)(((px >> 8) & 0xFF) * percent / 100);
            Uint8 b = (Uint8)((px & 0xFF) * percent / 100);
            drow[col] = MB_PackRGB(r, g, b);
        }
    }
}

static void
MB_FlushWholeFrame(void *virAddr, Uint32 stride, int height)
{
#ifdef MMIYOO
    uintptr_t start = (uintptr_t)virAddr;
    uintptr_t end = start + (uintptr_t)stride * (uintptr_t)height;
    uintptr_t aligned_start = start & ~(uintptr_t)(MB_FLUSH_ALIGN - 1u);
    uintptr_t aligned_end = (end + MB_FLUSH_ALIGN - 1u) & ~(uintptr_t)(MB_FLUSH_ALIGN - 1u);
    MI_SYS_FlushInvCache((void *)aligned_start, (MI_U32)(aligned_end - aligned_start));
#else
    (void)virAddr;
    (void)stride;
    (void)height;
#endif
}

static void
MB_DrawButton(Uint8 *back, Uint32 stride, int screen_w, int screen_h,
              const MB_Theme *theme, const SDL_MessageBoxData *data,
              const MB_Button *b, SDL_bool focused, SDL_bool blink_on)
{
    Uint32 fill_c, border_c, text_c;
    SDL_Rect inner;

    if (data->colorScheme) {
        fill_c = theme->btn_bg;
        border_c = focused ? theme->btn_selected : theme->btn_border;
        text_c = theme->text;
    } else if (focused && blink_on) {
        fill_c = MB_COLOR_SEL_BG;
        border_c = MB_COLOR_SEL_BG;
        text_c = MB_COLOR_SEL_TX;
    } else {
        fill_c = theme->btn_bg;
        border_c = theme->btn_border;
        text_c = theme->text;
    }

    MB_FillRect(back, stride, screen_w, screen_h, &b->rect, border_c);
    inner.x = b->rect.x + 1;
    inner.y = b->rect.y + 1;
    inner.w = b->rect.w - 2;
    inner.h = b->rect.h - 2;
    MB_FillRect(back, stride, screen_w, screen_h, &inner, fill_c);
    MB_DrawText(back, stride, screen_w, screen_h, b->rect.x + MB_BTN_PAD_X, b->rect.y + MB_BTN_PAD_Y, b->text, text_c);
}

static void
MB_DrawFrame(Uint8 *back, Uint32 stride, int screen_w, int screen_h,
             const MB_Theme *theme, const SDL_MessageBoxData *data,
             const MB_Layout *layout, SDL_bool blink_on)
{
    SDL_Rect shadow, inner;
    int text_y;
    int i;

    shadow.x = layout->box.x + MB_SHADOW_PX;
    shadow.y = layout->box.y + MB_SHADOW_PX;
    shadow.w = layout->box.w;
    shadow.h = layout->box.h;
    MB_FillRect(back, stride, screen_w, screen_h, &shadow, MB_COLOR_SHADOW);

    MB_FillRect(back, stride, screen_w, screen_h, &layout->box, theme->box_outline);
    inner.x = layout->box.x + 1;
    inner.y = layout->box.y + 1;
    inner.w = layout->box.w - 2;
    inner.h = layout->box.h - 2;
    MB_FillRect(back, stride, screen_w, screen_h, &inner, theme->bg);

    text_y = layout->box.y + MB_PAD;
    if (layout->title) {
        MB_DrawText(back, stride, screen_w, screen_h, layout->box.x + MB_PAD, text_y, layout->title, theme->text);
        text_y += MMIYOO_FONT8X8_H + MB_LINE_GAP;
    }
    for (i = 0; i < layout->msg_line_count; ++i) {
        MB_DrawText(back, stride, screen_w, screen_h, layout->box.x + MB_PAD, text_y, layout->msg_lines[i], theme->text);
        text_y += MMIYOO_FONT8X8_H + MB_LINE_GAP;
    }

    for (i = 0; i < layout->button_count; ++i) {
        SDL_bool focused = (layout->visual_order[layout->focus] == i);
        MB_DrawButton(back, stride, screen_w, screen_h, theme, data, &layout->buttons[i], focused, blink_on);
    }
}

int
MMIYOO_ShowMessageBox(_THIS, const SDL_MessageBoxData *messageboxdata, int *buttonid)
{
    int screen_w, screen_h;
    Uint32 stride;
    Uint8 *overlay_vir;
    MB_Layout layout;
    MB_Theme theme;
    Uint32 prev_bitmap;
    int result_buttonid = -1;
    SDL_bool resolved = SDL_FALSE;
    Uint64 frame_counter = 0;

    (void)_this;

    if (!messageboxdata) {
        return SDL_SetError("Passed a NULL message box data pointer");
    }

    screen_w = (int)GFX_GetFrameWidth();
    screen_h = (int)GFX_GetFrameHeight();
    stride = GFX_GetFrameStride();

    overlay_vir = (Uint8 *)GFX_GetOverlayVirtual();
    if (!overlay_vir || screen_w <= 0 || screen_h <= 0) {
        return SDL_SetError("MMIYOO_ShowMessageBox: no framebuffer/overlay scratch available");
    }

    MB_BuildTheme(messageboxdata->colorScheme, &theme);
    MB_ComputeLayout(messageboxdata, screen_w, screen_h, &layout);

    GFX_FlushTextureFences();
    MB_SnapshotDarken(overlay_vir, (const Uint8 *)GFX_GetFrameBufferVirtual(), stride, screen_w, screen_h, MB_DARKEN_PCT);

    prev_bitmap = MMIYOO_GetKeypadBitmap();

    while (!resolved) {
        Uint8 *back = (Uint8 *)GFX_GetFrameBufferVirtual();
        Uint32 bitmap, pressed;
        SDL_bool blink_on = ((frame_counter / MB_BLINK_FRAMES) % 2) == 0;

        GFX_FlushTextureFences();
        SDL_memcpy(back, overlay_vir, (size_t)stride * (size_t)screen_h);

        MB_DrawFrame(back, stride, screen_w, screen_h, &theme, messageboxdata, &layout, blink_on);

        MB_FlushWholeFrame(back, stride, screen_h);
        GFX_SwapBuffers(SDL_TRUE);

        bitmap = MMIYOO_GetKeypadBitmap();
        pressed = bitmap & ~prev_bitmap;
        prev_bitmap = bitmap;

        if (layout.button_count > 0) {
            if (pressed & (1u << MMIYOO_BUTTON_LEFT)) {
                layout.focus = (layout.focus + layout.button_count - 1) % layout.button_count;
            }
            if (pressed & (1u << MMIYOO_BUTTON_RIGHT)) {
                layout.focus = (layout.focus + 1) % layout.button_count;
            }
            if (pressed & (1u << MMIYOO_BUTTON_A)) {
                int src = layout.visual_order[layout.focus];
                result_buttonid = layout.buttons[src].buttonid;
                resolved = SDL_TRUE;
            }
        } else if (pressed & (1u << MMIYOO_BUTTON_A)) {
            resolved = SDL_TRUE;
        }

        if (!resolved && (pressed & (1u << MMIYOO_BUTTON_B))) {
            int i;
            for (i = 0; i < layout.button_count; ++i) {
                if (layout.buttons[i].flags & SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT) {
                    result_buttonid = layout.buttons[i].buttonid;
                    resolved = SDL_TRUE;
                    break;
                }
            }
        }

        frame_counter++;
        SDL_Delay(1000 / 60);
    }

    if (buttonid && result_buttonid != -1) {
        *buttonid = result_buttonid;
    }

    return 0;
}

#endif /* SDL_VIDEO_DRIVER_MMIYOO */

/* vi: set ts=4 sw=4 expandtab: */
