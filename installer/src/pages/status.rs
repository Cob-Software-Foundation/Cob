use crate::platform;
use crate::theme::{title_with_logo, ERROR_RED, SUBTEXT_GRAY};
use crate::Installer;
use eframe::egui::{self, Color32, RichText};

pub fn render_installing(app: &Installer, ui: &mut egui::Ui) {
    title_with_logo(ui, &app.logo, "Extracting Archive");
    ui.add_space(20.0);
    ui.label("Unpacking files from archive...");
    ui.add_space(8.0);

    let (done, total) = app.progress;
    let fraction = if total == 0 { 0.0 } else { done as f32 / total as f32 };
    ui.add(
        egui::ProgressBar::new(fraction)
            .text(format!("{} of {} files", done, total))
            .animate(true),
    );
}

pub fn render_finished(app: &Installer, ui: &mut egui::Ui, ctx: &egui::Context) {
    if app.succeeded {
        title_with_logo(ui, &app.logo, "Extraction was successful");
        ui.add_space(10.0);
        ui.label(RichText::new("Your files have been unpacked and are ready to use.").size(12.5));
        ui.add_space(12.0);
        ui.label(RichText::new(&app.status_msg).size(12.0).color(SUBTEXT_GRAY));
        ui.add_space(8.0);
        ui.label(
            RichText::new("Run cob from that folder, or add the folder to your PATH.")
                .size(12.0)
                .color(SUBTEXT_GRAY),
        );
    } else {
        ui.label(RichText::new("Extraction failed").size(22.0).color(ERROR_RED));
        ui.add_space(10.0);
        ui.label(RichText::new(&app.status_msg).size(12.5).color(Color32::BLACK));
    }

    ui.with_layout(egui::Layout::bottom_up(egui::Align::RIGHT), |ui| {
        if ui.add_sized([82.0, 26.0], egui::Button::new("Close")).clicked() {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
        ui.add_space(6.0);
        if app.succeeded && ui.add_sized([96.0, 26.0], egui::Button::new("Open Folder")).clicked() {
            platform::open_folder(&app.extract_dir);
        }
    });
}
