use crate::theme::{primary_button, SUBTEXT_GRAY};
use crate::{Installer, WizardPage};
use eframe::egui::{self, Color32, RichText};

pub fn render(app: &mut Installer, ui: &mut egui::Ui, ctx: &egui::Context) {
    ui.label(RichText::new("Extraction Options").size(22.0).color(Color32::BLACK));
    ui.add_space(12.0);

    ui.checkbox(
        &mut app.overwrite_existing,
        RichText::new("Overwrite existing files in destination folder").size(12.5),
    );
    ui.checkbox(
        &mut app.open_folder_after,
        RichText::new("Open the folder in the file manager after extraction").size(12.5),
    );

    ui.add_space(18.0);
    ui.label(RichText::new("Destination folder").size(12.5).color(Color32::BLACK));
    ui.add_space(4.0);

    ui.horizontal(|ui| {
        ui.add_sized([ui.available_width() - 88.0, 24.0], egui::TextEdit::singleline(&mut app.extract_dir));
        if ui.add_sized([80.0, 24.0], egui::Button::new("Browse")).clicked() {
            if let Some(folder) = rfd::FileDialog::new().pick_folder() {
                app.extract_dir = folder.to_string_lossy().to_string();
            }
        }
    });
    ui.label(
        RichText::new("All files are portable and require no administrator privileges.")
            .size(11.0)
            .color(SUBTEXT_GRAY),
    );

    ui.with_layout(egui::Layout::bottom_up(egui::Align::LEFT), |ui| {
        ui.horizontal(|ui| {
            if ui.add_sized([82.0, 26.0], egui::Button::new("Back")).clicked() {
                app.page = WizardPage::Welcome;
            }
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.add_sized([82.0, 26.0], egui::Button::new("Cancel")).clicked() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
                ui.add_space(6.0);
                if ui.add_sized([82.0, 26.0], primary_button("Extract")).clicked() {
                    app.start_extract(ctx);
                }
            });
        });
    });
}
