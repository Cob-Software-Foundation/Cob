use eframe::egui::{self, Color32, FontId, Pos2, Rect, RichText, Rounding, Sense, TextureHandle, Vec2};

use crate::platform::PLATFORM_NAME;

/// Cob green, from the corn logo's husk: buttons and selection.
pub const ACCENT: Color32 = Color32::from_rgb(34, 139, 52);
/// Darker green for link text, so it stays readable on white.
pub const LINK: Color32 = Color32::from_rgb(22, 110, 40);
pub const HOVER_BG: Color32 = Color32::from_rgb(232, 246, 234);
pub const SUBTEXT_GRAY: Color32 = Color32::from_rgb(100, 100, 100);
pub const ERROR_RED: Color32 = Color32::from_rgb(180, 30, 30);

const WHITE_SOFT: Color32 = Color32::from_rgb(228, 246, 226);

/// Green selection/hyperlink colours on top of egui's light theme.
pub fn apply(ctx: &egui::Context) {
    let mut v = egui::Visuals::light();
    v.selection.bg_fill = ACCENT;
    v.hyperlink_color = LINK;
    ctx.set_visuals(v);
}

/// The left banner: the pre-rendered gradient + logo image, with live text on
/// top so the version and platform can never drift out of date.
pub fn draw_sidebar(ui: &mut egui::Ui, banner: &TextureHandle) {
    let rect = ui.available_rect_before_wrap();
    let painter = ui.painter();

    painter.image(
        banner.id(),
        rect,
        Rect::from_min_max(Pos2::new(0.0, 0.0), Pos2::new(1.0, 1.0)),
        Color32::WHITE,
    );

    let x = rect.left() + 18.0;
    painter.text(
        Pos2::new(x, rect.bottom() - 92.0),
        egui::Align2::LEFT_BOTTOM,
        "Cob",
        FontId::proportional(34.0),
        Color32::WHITE,
    );
    painter.text(
        Pos2::new(x, rect.bottom() - 66.0),
        egui::Align2::LEFT_BOTTOM,
        format!("{}  for  {}", crate::APP_VERSION, PLATFORM_NAME),
        FontId::proportional(13.0),
        WHITE_SOFT,
    );
    painter.text(
        Pos2::new(x, rect.bottom() - 16.0),
        egui::Align2::LEFT_BOTTOM,
        "Handcrafted by\nPixel-Pulse",
        FontId::proportional(11.0),
        WHITE_SOFT,
    );
}

/// A page title with the small logo to its left.
pub fn title_with_logo(ui: &mut egui::Ui, logo: &TextureHandle, text: &str) {
    ui.horizontal(|ui| {
        ui.image((logo.id(), Vec2::splat(34.0)));
        ui.label(RichText::new(text).size(22.0).color(Color32::BLACK));
    });
}

/// A clickable "▸ Title" row with grey-ish detail lines under it.
pub fn action_link_block(ui: &mut egui::Ui, title: &str, lines: &[&str]) -> bool {
    let desired_size = Vec2::new(ui.available_width(), 26.0 + (lines.len() as f32 * 16.0));
    let (rect, response) = ui.allocate_exact_size(desired_size, Sense::click());

    if response.hovered() {
        ui.painter().rect_filled(rect, Rounding::same(4.0), HOVER_BG);
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }

    let painter = ui.painter();
    // A drawn triangle, not a "→" glyph: egui's bundled font has no arrow, so the
    // character shows up as an empty box.
    let (ax, ay) = (rect.left() + 9.0, rect.top() + 13.0);
    painter.add(egui::Shape::convex_polygon(
        vec![Pos2::new(ax - 4.0, ay - 5.0), Pos2::new(ax - 4.0, ay + 5.0), Pos2::new(ax + 4.5, ay)],
        LINK,
        egui::Stroke::NONE,
    ));
    painter.text(
        Pos2::new(rect.left() + 22.0, rect.top() + 4.0),
        egui::Align2::LEFT_TOP,
        title,
        FontId::proportional(16.5),
        LINK,
    );
    for (i, line) in lines.iter().enumerate() {
        painter.text(
            Pos2::new(rect.left() + 22.0, rect.top() + 26.0 + (i as f32 * 15.0)),
            egui::Align2::LEFT_TOP,
            *line,
            FontId::proportional(12.0),
            LINK,
        );
    }

    response.clicked()
}

/// The filled green default button.
pub fn primary_button(text: &str) -> egui::Button<'static> {
    egui::Button::new(RichText::new(text).color(Color32::WHITE)).fill(ACCENT)
}
