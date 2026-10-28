/* =====================================================================
 * barn -- a window to put things in             (shuck barn)
 * ---------------------------------------------------------------------
 *   barn_open(title)              open a window; returns its handle (1), 0 on failure
 *   barn_label(h, text)           set the text at the top of the window; 0
 *   barn_wait(h, seconds)         keep the window alive and responsive for a
 *                                 while (the close button is ignored: only
 *                                 barn_close() closes it); 0
 *   barn_close(h)                 close the window; 0
 *   barn_button(h, label)         a button; 1 if it was clicked since you last
 *                                 asked, else 0
 *   barn_slider(h, label, max)    a 0..max slider; returns its value
 *   barn_textbox(h, label)        an editable text box; returns its contents
 *
 * Built on raylib + raygui (vendor/), compiled into libbarn. raylib allows
 * ONE window per process, so the only handle is 1; opening a second window
 * before closing the first warns and returns 0. Each widget is identified
 * by its label: the first call with a label creates it (stacked below the
 * previous ones), later calls read it.
 *
 * This is the old built-in _cobwindow code (window_open and friends),
 * moved out of the interpreter into a standard-library module.
 * ===================================================================== */
#include "stdhelp.h"
#include "raylib.h"
#define RAYGUI_IMPLEMENTATION
#include "raygui.h"

#define MOD "barn"
#define BARN_LABEL_MAX        512
#define BARN_MAX_WIDGETS      16
#define BARN_WIDGET_LABEL_MAX 128
#define BARN_WIDGET_TEXT_MAX  256

static int  barn_is_open = 0;
static char barn_label_text[BARN_LABEL_MAX];
static char *barn_textbox_buf;

typedef enum { BARN_BUTTON, BARN_SLIDER, BARN_TEXTBOX } BarnKind;
typedef struct {
    BarnKind kind;
    char  label[BARN_WIDGET_LABEL_MAX];
    float y;
    int   clicked_since_read;
    int   slider_ready;
    float value, min_value, max_value;
    char  text[BARN_WIDGET_TEXT_MAX];
} BarnWidget;

static BarnWidget barn_widgets[BARN_MAX_WIDGETS];
static int   barn_widget_count = 0;
static float barn_widget_next_y = 60.0f;

static BarnWidget *barn_find_or_create(BarnKind kind, const char *label) {
    int i;
    for (i = 0; i < barn_widget_count; i++)
        if (barn_widgets[i].kind == kind && strcmp(barn_widgets[i].label, label) == 0) return &barn_widgets[i];
    if (barn_widget_count >= BARN_MAX_WIDGETS) {
        fprintf(stderr, "[" MOD "] warning: too many widgets (max %d); \"%s\" ignored\n", BARN_MAX_WIDGETS, label);
        return NULL;
    }
    {
        BarnWidget *w = &barn_widgets[barn_widget_count++];
        memset(w, 0, sizeof(*w));
        w->kind = kind;
        snprintf(w->label, sizeof(w->label), "%s", label);
        w->y = barn_widget_next_y;
        barn_widget_next_y += 40.0f;
        return w;
    }
}
static void barn_redraw(void) {
    int i;
    BeginDrawing();
    ClearBackground(RAYWHITE);
    DrawText(barn_label_text, 20, 20, 20, BLACK);
    for (i = 0; i < barn_widget_count; i++) {
        BarnWidget *w = &barn_widgets[i];
        Rectangle bounds = { 20, w->y, 240, 30 };
        switch (w->kind) {
            case BARN_BUTTON:
                if (GuiButton(bounds, w->label)) w->clicked_since_read = 1;
                break;
            case BARN_SLIDER: {
                char caption[BARN_WIDGET_LABEL_MAX + 32];
                snprintf(caption, sizeof(caption), "%s: %d", w->label, (int)(w->value + 0.5f));
                GuiSliderBar(bounds, NULL, caption, &w->value, w->min_value, w->max_value);
                break;
            }
            case BARN_TEXTBOX:
                GuiTextBox(bounds, w->text, (int)sizeof(w->text), true);
                break;
        }
    }
    EndDrawing();
}
static int barn_check(const char *fn, long h) {
    if (h == 1 && barn_is_open) return 1;
    fprintf(stderr, "[" MOD "] warning: %s() on invalid/closed handle %ld\n", fn, h);
    return 0;
}

COB_EXPORT CobValue barn_open(CobValue *argv, int argc) {
    const char *title;
    if (!cs_arity(MOD, "barn_open", argc, 1)) return cob_int(0);
    title = cs_str(MOD, "barn_open", argv, argc, 0);
    if (barn_is_open) {
        fprintf(stderr, "[" MOD "] warning: barn_open(\"%s\") failed: a window is already open "
                        "(one window per process); call barn_close() first\n", title);
        return cob_int(0);
    }
    InitWindow(600, 400, title);
    if (!IsWindowReady()) {
        fprintf(stderr, "[" MOD "] warning: barn_open(\"%s\") failed: no window could be created (no display?)\n", title);
        return cob_int(0);
    }
    SetTargetFPS(30);
    barn_label_text[0] = '\0';
    barn_widget_count = 0; barn_widget_next_y = 60.0f;
    barn_is_open = 1;
    barn_redraw();
    return cob_int(1);
}
COB_EXPORT CobValue barn_label(CobValue *argv, int argc) {
    long h; const char *text;
    if (!cs_arity(MOD, "barn_label", argc, 2)) return cob_int(0);
    h = cs_int(MOD, "barn_label", argv, argc, 0); text = cs_str(MOD, "barn_label", argv, argc, 1);
    if (!barn_check("barn_label", h)) return cob_int(0);
    snprintf(barn_label_text, BARN_LABEL_MAX, "%s", text);
    barn_redraw();
    return cob_int(0);
}
COB_EXPORT CobValue barn_wait(CobValue *argv, int argc) {
    long h, seconds; double start;
    if (!cs_arity(MOD, "barn_wait", argc, 2)) return cob_int(0);
    h = cs_int(MOD, "barn_wait", argv, argc, 0); seconds = cs_int(MOD, "barn_wait", argv, argc, 1);
    if (!barn_check("barn_wait", h)) return cob_int(0);
    start = GetTime();
    do {
        (void)WindowShouldClose(); /* the close button is deliberately ignored */
        barn_redraw();
    } while (GetTime() - start < (double)seconds);
    return cob_int(0);
}
COB_EXPORT CobValue barn_close(CobValue *argv, int argc) {
    long h;
    if (!cs_arity(MOD, "barn_close", argc, 1)) return cob_int(0);
    h = cs_int(MOD, "barn_close", argv, argc, 0);
    if (h == 1 && barn_is_open) { CloseWindow(); barn_is_open = 0; }
    return cob_int(0);
}
COB_EXPORT CobValue barn_button(CobValue *argv, int argc) {
    long h, clicked; const char *label; BarnWidget *w;
    if (!cs_arity(MOD, "barn_button", argc, 2)) return cob_int(0);
    h = cs_int(MOD, "barn_button", argv, argc, 0); label = cs_str(MOD, "barn_button", argv, argc, 1);
    if (!barn_check("barn_button", h)) return cob_int(0);
    w = barn_find_or_create(BARN_BUTTON, label);
    if (!w) return cob_int(0);
    barn_redraw();
    clicked = w->clicked_since_read;
    w->clicked_since_read = 0;
    return cob_int(clicked);
}
COB_EXPORT CobValue barn_slider(CobValue *argv, int argc) {
    long h, max_value; const char *label; BarnWidget *w;
    if (!cs_arity(MOD, "barn_slider", argc, 3)) return cob_int(0);
    h = cs_int(MOD, "barn_slider", argv, argc, 0); label = cs_str(MOD, "barn_slider", argv, argc, 1);
    max_value = cs_int(MOD, "barn_slider", argv, argc, 2);
    if (!barn_check("barn_slider", h)) return cob_int(0);
    if (max_value <= 0) {
        fprintf(stderr, "[" MOD "] warning: barn_slider() needs a positive max, got %ld; treated as 1\n", max_value);
        max_value = 1;
    }
    w = barn_find_or_create(BARN_SLIDER, label);
    if (!w) return cob_int(0);
    if (!w->slider_ready || w->max_value != (float)max_value) {
        w->min_value = 0.0f;
        w->max_value = (float)max_value;
        if (!w->slider_ready) w->value = 0.0f;
        else if (w->value > w->max_value) w->value = w->max_value;
        w->slider_ready = 1;
    }
    barn_redraw();
    return cob_int((long)(w->value + 0.5f));
}
COB_EXPORT CobValue barn_textbox(CobValue *argv, int argc) {
    long h; const char *label; BarnWidget *w;
    if (!cs_arity(MOD, "barn_textbox", argc, 2)) return cob_str("");
    h = cs_int(MOD, "barn_textbox", argv, argc, 0); label = cs_str(MOD, "barn_textbox", argv, argc, 1);
    if (!barn_check("barn_textbox", h)) return cob_str("");
    w = barn_find_or_create(BARN_TEXTBOX, label);
    if (!w) return cob_str("");
    barn_redraw();
    return cs_ret_str(&barn_textbox_buf, w->text, strlen(w->text));
}
#if defined(__GNUC__) || defined(__clang__)
__attribute__((destructor)) static void barn_shutdown(void) {
    if (barn_is_open) { CloseWindow(); barn_is_open = 0; }
    free(barn_textbox_buf);
    barn_textbox_buf = NULL;
}
#endif
