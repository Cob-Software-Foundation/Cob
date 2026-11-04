//! barn -- a window to put things in                    (shuck barn)
//!
//!   barn_open(title)              open a window; returns its handle (1), 0 on failure
//!   barn_label(h, text)           set the text at the top of the window; 0
//!   barn_wait(h, seconds)         keep the window alive and responsive for a
//!                                 while (the close button is ignored: only
//!                                 barn_close() closes it); 0
//!   barn_close(h)                 close the window; 0
//!   barn_button(h, label)         a button; 1 if it was clicked since you last
//!                                 asked, else 0
//!   barn_slider(h, label, max)    a 0..max slider; returns its value
//!   barn_textbox(h, label)        an editable text box; returns its contents
//!
//! Built on raylib + raygui (vendor/). raylib allows ONE window per process, so
//! the only handle is 1; opening a second window before closing the first warns
//! and returns 0. Each widget is identified by its label: the first call with a
//! label creates it (stacked below the previous ones), later calls read it.

use std::ffi::{c_char, c_int};
use std::sync::Mutex;

use cob_abi::*;

const MOD: &str = "barn";
const BARN_LABEL_MAX: usize = 512;
const BARN_MAX_WIDGETS: usize = 16;
const BARN_WIDGET_LABEL_MAX: usize = 128;
const BARN_WIDGET_TEXT_MAX: usize = 256;

// ---- the C libraries -------------------------------------------------------------

#[repr(C)]
#[derive(Clone, Copy)]
struct Color {
    r: u8,
    g: u8,
    b: u8,
    a: u8,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Rectangle {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}

const RAYWHITE: Color = Color { r: 245, g: 245, b: 245, a: 255 };
const BLACK: Color = Color { r: 0, g: 0, b: 0, a: 255 };

extern "C" {
    fn InitWindow(width: c_int, height: c_int, title: *const c_char);
    fn IsWindowReady() -> bool;
    fn CloseWindow();
    fn SetTargetFPS(fps: c_int);
    fn WindowShouldClose() -> bool;
    fn GetTime() -> f64;
    fn BeginDrawing();
    fn EndDrawing();
    fn ClearBackground(color: Color);
    fn DrawText(text: *const c_char, x: c_int, y: c_int, font_size: c_int, color: Color);

    fn GuiButton(bounds: Rectangle, text: *const c_char) -> c_int;
    fn GuiSliderBar(
        bounds: Rectangle,
        text_left: *const c_char,
        text_right: *const c_char,
        value: *mut f32,
        min: f32,
        max: f32,
    ) -> c_int;
    fn GuiTextBox(bounds: Rectangle, text: *mut c_char, text_size: c_int, edit_mode: bool) -> c_int;

    fn atexit(cb: extern "C" fn()) -> c_int;
}

// ---- state ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum Kind {
    Button,
    Slider,
    Textbox,
}

struct Widget {
    kind: Kind,
    /// as stored: at most BARN_WIDGET_LABEL_MAX-1 bytes
    label: Vec<u8>,
    y: f32,
    clicked_since_read: bool,
    slider_ready: bool,
    value: f32,
    min_value: f32,
    max_value: f32,
    /// NUL-terminated edit buffer for the text box
    text: [u8; BARN_WIDGET_TEXT_MAX],
}

struct State {
    is_open: bool,
    label_text: Vec<u8>,
    widgets: Vec<Widget>,
    next_y: f32,
    atexit_registered: bool,
}

static STATE: Mutex<State> = Mutex::new(State {
    is_open: false,
    label_text: Vec::new(),
    widgets: Vec::new(),
    next_y: 60.0,
    atexit_registered: false,
});
static TEXTBOX_RET: Slot = Slot::new();

fn state() -> std::sync::MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
}

/// `snprintf(dst, max, "%s", s)`: at most max-1 bytes survive.
fn truncated(s: &[u8], max: usize) -> Vec<u8> {
    s[..s.len().min(max - 1)].to_vec()
}

fn cstr(bytes: &[u8]) -> Vec<u8> {
    let mut v = bytes.to_vec();
    v.push(0);
    v
}

impl State {
    fn find_or_create(&mut self, kind: Kind, label: &[u8]) -> Option<usize> {
        // the stored label is truncated, so an over-long label never matches
        // again (it creates a new widget each call) -- as it always has
        if let Some(i) = self.widgets.iter().position(|w| w.kind == kind && w.label == label) {
            return Some(i);
        }
        if self.widgets.len() >= BARN_MAX_WIDGETS {
            ewrite(&format!(
                "[{}] warning: too many widgets (max {}); \"{}\" ignored\n",
                MOD,
                BARN_MAX_WIDGETS,
                lossy(label)
            ));
            return None;
        }
        self.widgets.push(Widget {
            kind,
            label: truncated(label, BARN_WIDGET_LABEL_MAX),
            y: self.next_y,
            clicked_since_read: false,
            slider_ready: false,
            value: 0.0,
            min_value: 0.0,
            max_value: 0.0,
            text: [0; BARN_WIDGET_TEXT_MAX],
        });
        self.next_y += 40.0;
        Some(self.widgets.len() - 1)
    }

    fn redraw(&mut self) {
        unsafe {
            BeginDrawing();
            ClearBackground(RAYWHITE);
            DrawText(cstr(&self.label_text).as_ptr() as *const c_char, 20, 20, 20, BLACK);
            for w in self.widgets.iter_mut() {
                let bounds = Rectangle { x: 20.0, y: w.y, width: 240.0, height: 30.0 };
                let label = cstr(&w.label);
                match w.kind {
                    Kind::Button => {
                        if GuiButton(bounds, label.as_ptr() as *const c_char) != 0 {
                            w.clicked_since_read = true;
                        }
                    }
                    Kind::Slider => {
                        let caption = cstr(format!("{}: {}", lossy(&w.label), (w.value + 0.5) as i32).as_bytes());
                        GuiSliderBar(
                            bounds,
                            std::ptr::null(),
                            caption.as_ptr() as *const c_char,
                            &mut w.value,
                            w.min_value,
                            w.max_value,
                        );
                    }
                    Kind::Textbox => {
                        GuiTextBox(bounds, w.text.as_mut_ptr() as *mut c_char, BARN_WIDGET_TEXT_MAX as c_int, true);
                    }
                }
            }
            EndDrawing();
        }
    }

    fn check(&self, func: &str, h: i64) -> bool {
        if h == 1 && self.is_open {
            return true;
        }
        ewrite(&format!("[{}] warning: {}() on invalid/closed handle {}\n", MOD, func, h));
        false
    }
}

/// Runs at process exit (or when the library is unloaded): never leave a
/// window behind.
extern "C" fn barn_shutdown() {
    let mut s = state();
    if s.is_open {
        unsafe { CloseWindow() };
        s.is_open = false;
    }
}

// ---- the module's functions --------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn barn_open(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_open", argc, 1) {
        return cob_int(0);
    }
    let title = arg_str(MOD, "barn_open", argv, argc, 0);
    let mut s = state();
    if s.is_open {
        ewrite(&format!(
            "[{}] warning: barn_open(\"{}\") failed: a window is already open (one window per process); call barn_close() first\n",
            MOD,
            lossy(title)
        ));
        return cob_int(0);
    }
    InitWindow(600, 400, cstr(title).as_ptr() as *const c_char);
    if !IsWindowReady() {
        ewrite(&format!(
            "[{}] warning: barn_open(\"{}\") failed: no window could be created (no display?)\n",
            MOD,
            lossy(title)
        ));
        return cob_int(0);
    }
    if !s.atexit_registered {
        atexit(barn_shutdown);
        s.atexit_registered = true;
    }
    SetTargetFPS(30);
    s.label_text.clear();
    s.widgets.clear();
    s.next_y = 60.0;
    s.is_open = true;
    s.redraw();
    cob_int(1)
}

#[no_mangle]
pub unsafe extern "C" fn barn_label(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_label", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "barn_label", argv, argc, 0);
    let text = arg_str(MOD, "barn_label", argv, argc, 1);
    let mut s = state();
    if !s.check("barn_label", h) {
        return cob_int(0);
    }
    s.label_text = truncated(text, BARN_LABEL_MAX);
    s.redraw();
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn barn_wait(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_wait", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "barn_wait", argv, argc, 0);
    let seconds = arg_int(MOD, "barn_wait", argv, argc, 1);
    let mut s = state();
    if !s.check("barn_wait", h) {
        return cob_int(0);
    }
    let start = GetTime();
    loop {
        let _ = WindowShouldClose(); // the close button is deliberately ignored
        s.redraw();
        if GetTime() - start >= seconds as f64 {
            break;
        }
    }
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn barn_close(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_close", argc, 1) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "barn_close", argv, argc, 0);
    let mut s = state();
    if h == 1 && s.is_open {
        CloseWindow();
        s.is_open = false;
    }
    cob_int(0)
}

#[no_mangle]
pub unsafe extern "C" fn barn_button(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_button", argc, 2) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "barn_button", argv, argc, 0);
    let label = arg_str(MOD, "barn_button", argv, argc, 1);
    let mut s = state();
    if !s.check("barn_button", h) {
        return cob_int(0);
    }
    let Some(i) = s.find_or_create(Kind::Button, label) else { return cob_int(0) };
    s.redraw();
    let clicked = s.widgets[i].clicked_since_read;
    s.widgets[i].clicked_since_read = false;
    cob_int(clicked as i64)
}

#[no_mangle]
pub unsafe extern "C" fn barn_slider(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_slider", argc, 3) {
        return cob_int(0);
    }
    let h = arg_int(MOD, "barn_slider", argv, argc, 0);
    let label = arg_str(MOD, "barn_slider", argv, argc, 1);
    let mut max_value = arg_int(MOD, "barn_slider", argv, argc, 2);
    let mut s = state();
    if !s.check("barn_slider", h) {
        return cob_int(0);
    }
    if max_value <= 0 {
        ewrite(&format!("[{}] warning: barn_slider() needs a positive max, got {}; treated as 1\n", MOD, max_value));
        max_value = 1;
    }
    let Some(i) = s.find_or_create(Kind::Slider, label) else { return cob_int(0) };
    {
        let w = &mut s.widgets[i];
        if !w.slider_ready || w.max_value != max_value as f32 {
            w.min_value = 0.0;
            w.max_value = max_value as f32;
            if !w.slider_ready {
                w.value = 0.0;
            } else if w.value > w.max_value {
                w.value = w.max_value;
            }
            w.slider_ready = true;
        }
    }
    s.redraw();
    cob_int((s.widgets[i].value + 0.5) as i64)
}

#[no_mangle]
pub unsafe extern "C" fn barn_textbox(argv: *mut CobValue, argc: c_int) -> CobValue {
    if !arity(MOD, "barn_textbox", argc, 2) {
        return cob_empty_str();
    }
    let h = arg_int(MOD, "barn_textbox", argv, argc, 0);
    let label = arg_str(MOD, "barn_textbox", argv, argc, 1);
    let mut s = state();
    if !s.check("barn_textbox", h) {
        return cob_empty_str();
    }
    let Some(i) = s.find_or_create(Kind::Textbox, label) else { return cob_empty_str() };
    s.redraw();
    let buf = &s.widgets[i].text;
    let len = buf.iter().position(|&b| b == 0).unwrap_or(buf.len());
    TEXTBOX_RET.ret(&buf[..len])
}
