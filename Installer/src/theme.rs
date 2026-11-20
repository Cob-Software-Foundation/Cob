use eframe::egui::{self, Color32, FontId, Pos2, Rect, RichText, Rounding, Sense, Stroke, Vec2};

pub const LINK_BLUE: Color32 = Color32::from_rgb(0, 102, 204);
pub const HOVER_BG: Color32 = Color32::from_rgb(235, 244, 254);
pub const SUBTEXT_GRAY: Color32 = Color32::from_rgb(100, 100, 100);
pub const SIDEBAR_BG: Color32 = Color32::from_rgb(245, 246, 248);
pub const ACCENT_BLUE: Color32 = Color32::from_rgb(55, 118, 171);
pub const ACCENT_YELLOW: Color32 = Color32::from_rgb(255, 212, 59);

pub fn draw_sidebar(ui: &mut egui::Ui) {
    let rect = ui.available_rect_before_wrap();
    let painter = ui.painter();

    painter.rect_filled(rect, Rounding::ZERO, SIDEBAR_BG);

    // Top-left blue rounded accent
    let top_accent = Rect::from_min_size(
        Pos2::new(rect.left() - 15.0, rect.top() - 15.0),
        Vec2::new(135.0, 95.0),
    );
    painter.rect_filled(top_accent, Rounding::same(18.0), ACCENT_BLUE);

    // Center geometric logo badge
    let center_x = rect.center().x;
    let center_y = rect.top() + 185.0;
    painter.rect_filled(
        Rect::from_center_size(Pos2::new(center_x - 10.0, center_y - 10.0), Vec2::new(42.0, 42.0)),
        Rounding::same(10.0),
        ACCENT_BLUE,
    );
    painter.rect_filled(
        Rect::from_center_size(Pos2::new(center_x + 10.0, center_y + 10.0), Vec2::new(42.0, 42.0)),
        Rounding::same(10.0),
        ACCENT_YELLOW,
    );

    // Bottom-right gold rounded accent
    let bot_accent = Rect::from_min_size(
        Pos2::new(rect.left() + 75.0, rect.bottom() - 105.0),
        Vec2::new(110.0, 125.0),
    );
    painter.rect_filled(bot_accent, Rounding::same(18.0), ACCENT_YELLOW);

    // Bottom branding text
    painter.text(
        Pos2::new(rect.left() + 16.0, rect.bottom() - 68.0),
        egui::Align2::LEFT_BOTTOM,
        "MyApp",
        FontId::proportional(20.0),
        Color32::from_rgb(75, 75, 75),
    );
    painter.text(
        Pos2::new(rect.left() + 58.0, rect.bottom() - 48.0),
        egui::Align2::LEFT_BOTTOM,
        "for",
        FontId::proportional(15.0),
        Color32::from_rgb(95, 95, 95),
    );
    painter.text(
        Pos2::new(rect.left() + 16.0, rect.bottom() - 22.0),
        egui::Align2::LEFT_BOTTOM,
        "Windows",
        FontId::proportional(21.0),
        Color32::from_rgb(75, 75, 75),
    );

    painter.line_segment(
        [rect.right_top(), rect.right_bottom()],
        Stroke::new(1.0_f32, Color32::from_rgb(225, 225, 225)),
    );
}

pub fn action_link_block(ui: &mut egui::Ui, title: &str, lines: &[&str]) -> bool {
    let desired_size = Vec2::new(ui.available_width(), 26.0 + (lines.len() as f32 * 16.0));
    let (rect, response) = ui.allocate_exact_size(desired_size, Sense::click());

    if response.hovered() {
        ui.painter().rect_filled(rect, Rounding::same(4.0), HOVER_BG);
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }

    let painter = ui.painter();
    painter.text(
        Pos2::new(rect.left() + 6.0, rect.top() + 4.0),
        egui::Align2::LEFT_TOP,
        format!("→  {}", title),
        FontId::proportional(16.5),
        LINK_BLUE,
    );

    for (i, line) in lines.iter().enumerate() {
        painter.text(
            Pos2::new(rect.left() + 30.0, rect.top() + 26.0 + (i as f32 * 15.0)),
            egui::Align2::LEFT_TOP,
            *line,
            FontId::proportional(12.0),
            LINK_BLUE,
        );
    }

    response.clicked()
}

pub fn feature_checkbox(ui: &mut egui::Ui, checked: &mut bool, title: &str, desc: &str) {
    ui.checkbox(checked, RichText::new(title).size(13.5).color(Color32::BLACK));
    ui.horizontal(|ui| {
        ui.add_space(22.0);
        ui.label(RichText::new(desc).size(11.5).color(SUBTEXT_GRAY));
    });
    ui.add_space(6.0);
}
