#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod installer;
mod pages;
mod theme;

use eframe::egui::{self, Color32};
use std::path::PathBuf;

#[derive(PartialEq, Clone, Copy)]
pub enum WizardPage {
    Welcome,
    Customize,
    Extracting,
    Finished,
}

pub struct PythonStyleSfx {
    pub page: WizardPage,
    pub open_folder_after: bool,
    pub overwrite_existing: bool,
    pub extract_dir: String,
    pub status_msg: String,
}

impl Default for PythonStyleSfx {
    fn default() -> Self {
        let mut default_path = dirs::download_dir().unwrap_or_else(|| PathBuf::from("C:\\"));
        default_path.push("MyApp");

        Self {
            page: WizardPage::Welcome,
            open_folder_after: true,
            overwrite_existing: true,
            extract_dir: default_path.to_string_lossy().to_string(),
            status_msg: String::new(),
        }
    }
}

impl PythonStyleSfx {
    pub fn start_extract(&mut self) {
        self.page = WizardPage::Extracting;
        match installer::extract_payload(&self.extract_dir) {
            Ok(msg) => self.status_msg = msg,
            Err(err) => self.status_msg = err,
        }
        if self.open_folder_after {
            let _ = std::process::Command::new("explorer")
                .arg(&self.extract_dir)
                .spawn();
        }
        self.page = WizardPage::Finished;
    }
}

impl eframe::App for PythonStyleSfx {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        ctx.set_visuals(egui::Visuals::light());

        egui::SidePanel::left("python_sidebar")
            .exact_width(165.0)
            .resizable(false)
            .frame(egui::Frame::none())
            .show(ctx, |ui| {
                theme::draw_sidebar(ui);
            });

        egui::CentralPanel::default()
            .frame(
                egui::Frame::none()
                    .fill(Color32::WHITE)
                    .inner_margin(egui::Margin::symmetric(24.0, 18.0)),
            )
            .show(ctx, |ui| match self.page {
                WizardPage::Welcome => pages::welcome::render(self, ui, ctx),
                WizardPage::Customize => pages::advanced::render(self, ui, ctx),
                WizardPage::Extracting => pages::status::render_installing(ui),
                WizardPage::Finished => pages::status::render_finished(self, ui, ctx),
            });
    }
}

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([610.0, 385.0])
            .with_resizable(false),
        ..Default::default()
    };

    eframe::run_native(
        "MyApp 1.0.0 Self-Extracting Archive",
        options,
        Box::new(|_cc| Ok(Box::new(PythonStyleSfx::default()))),
    )
}
