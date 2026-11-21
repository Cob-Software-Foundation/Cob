use crate::platform::ARCH_LABEL;
use crate::theme::{action_link_block, title_with_logo};
use crate::{Installer, WizardPage, APP_VERSION};
use eframe::egui::{self, Color32, RichText};

pub fn render(app: &mut Installer, ui: &mut egui::Ui, ctx: &egui::Context) {
    title_with_logo(ui, &app.logo, &format!("Extract Cob {} ({})", APP_VERSION, ARCH_LABEL));
    ui.add_space(6.0);
    ui.label(
        RichText::new(
            "Select Extract Now to unpack Cob to the default folder, or choose Customize to select a different destination.",
        )
        .size(12.5)
        .color(Color32::from_rgb(40, 40, 40)),
    );
    ui.add_space(22.0);

    let path_preview = app.extract_dir.clone();
    if action_link_block(
        ui,
        "Extract Now",
        &[&path_preview, "Portable archive — no installer, registry, PATH or administrator changes"],
    ) {
        app.start_extract(ctx);
    }

    ui.add_space(16.0);

    if action_link_block(ui, "Customize extraction", &["Choose destination folder and extraction options"]) {
        app.page = WizardPage::Customize;
    }

    ui.with_layout(egui::Layout::bottom_up(egui::Align::LEFT), |ui| {
        ui.horizontal(|ui| {
            ui.checkbox(
                &mut app.open_folder_after,
                RichText::new("Open destination folder when finished").size(12.5),
            );
            ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                if ui.add_sized([82.0, 26.0], egui::Button::new("Cancel")).clicked() {
                    ctx.send_viewport_cmd(egui::ViewportCommand::Close);
                }
            });
        });
    });
}
