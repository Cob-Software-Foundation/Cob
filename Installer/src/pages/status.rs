use crate::theme::SUBTEXT_GRAY;
use crate::PythonStyleSfx;
use eframe::egui::{self, Color32, RichText};

pub fn render_installing(ui: &mut egui::Ui) {
    ui.label(
        RichText::new("Extracting Archive")
            .size(22.0)
            .color(Color32::BLACK),
    );
    ui.add_space(20.0);
    ui.label("Unpacking files from archive...");
    ui.add_space(8.0);
    ui.add(egui::ProgressBar::new(0.75).animate(true));
}

pub fn render_finished(app: &PythonStyleSfx, ui: &mut egui::Ui, ctx: &egui::Context) {
    ui.label(
        RichText::new("Extraction was successful")
            .size(22.0)
            .color(Color32::BLACK),
    );
    ui.add_space(10.0);
    ui.label(
        RichText::new("Your files have been unpacked and are ready to use.")
            .size(12.5),
    );
    ui.add_space(12.0);
    ui.label(RichText::new(&app.status_msg).size(12.0).color(SUBTEXT_GRAY));

    ui.with_layout(egui::Layout::bottom_up(egui::Align::RIGHT), |ui| {
        if ui
            .add_sized([82.0, 26.0], egui::Button::new("Close"))
            .clicked()
        {
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }
        ui.add_space(6.0);
        if ui
            .add_sized([96.0, 26.0], egui::Button::new("Open Folder"))
            .clicked()
        {
            let _ = std::process::Command::new("explorer")
                .arg(&app.extract_dir)
                .spawn();
        }
    });
}
