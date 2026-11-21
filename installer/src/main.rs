#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod installer;
mod pages;
mod platform;
mod theme;

use eframe::egui::{self, Color32, ColorImage, TextureHandle, TextureOptions};
use std::path::PathBuf;
use std::sync::{mpsc, Arc};
use std::thread;

/// Shown in the title bar, sidebar and headings -- always the crate version,
/// so bumping Cargo.toml is the only edit a new release needs.
pub const APP_VERSION: &str = env!("CARGO_PKG_VERSION");

const ICON_PNG: &[u8] = include_bytes!("../assets/icon.png");
const LOGO_PNG: &[u8] = include_bytes!("../assets/logo.png");
const SIDEBAR_PNG: &[u8] = include_bytes!("../assets/sidebar.png");

#[derive(PartialEq, Clone, Copy)]
pub enum WizardPage {
    Welcome,
    Customize,
    Extracting,
    Finished,
}

/// What the extraction thread tells the UI.
enum Msg {
    Progress(usize, usize),
    Done(Result<installer::Summary, String>),
}

pub struct Installer {
    pub page: WizardPage,
    pub open_folder_after: bool,
    pub overwrite_existing: bool,
    pub extract_dir: String,
    pub status_msg: String,
    pub succeeded: bool,
    /// (files done, files total) while extracting
    pub progress: (usize, usize),
    pub logo: TextureHandle,
    banner: TextureHandle,
    rx: Option<mpsc::Receiver<Msg>>,
}

fn load_png(ctx: &egui::Context, name: &str, bytes: &[u8]) -> TextureHandle {
    let img = image::load_from_memory(bytes).expect("embedded PNG is valid").to_rgba8();
    let size = [img.width() as usize, img.height() as usize];
    ctx.load_texture(name, ColorImage::from_rgba_unmultiplied(size, img.as_raw()), TextureOptions::LINEAR)
}

impl Installer {
    fn new(cc: &eframe::CreationContext<'_>) -> Self {
        theme::apply(&cc.egui_ctx);
        Self {
            page: WizardPage::Welcome,
            open_folder_after: true,
            overwrite_existing: true,
            extract_dir: platform::default_install_dir().to_string_lossy().to_string(),
            status_msg: String::new(),
            succeeded: false,
            progress: (0, 0),
            logo: load_png(&cc.egui_ctx, "logo", LOGO_PNG),
            banner: load_png(&cc.egui_ctx, "sidebar", SIDEBAR_PNG),
            rx: None,
        }
    }

    /// Starts unpacking on a worker thread so the progress page keeps drawing.
    pub fn start_extract(&mut self, ctx: &egui::Context) {
        let (tx, rx) = mpsc::channel();
        self.rx = Some(rx);
        self.page = WizardPage::Extracting;
        self.progress = (0, 0);

        let dest = PathBuf::from(&self.extract_dir);
        let overwrite = self.overwrite_existing;
        let ctx = ctx.clone();
        thread::spawn(move || {
            let result = installer::extract(installer::PAYLOAD_ZIP, &dest, overwrite, |done, total| {
                let _ = tx.send(Msg::Progress(done, total));
                ctx.request_repaint();
            });
            let _ = tx.send(Msg::Done(result));
            ctx.request_repaint();
        });
    }

    fn poll_extraction(&mut self) {
        let Some(rx) = &self.rx else { return };
        let mut finished = None;
        while let Ok(msg) = rx.try_recv() {
            match msg {
                Msg::Progress(done, total) => self.progress = (done, total),
                Msg::Done(result) => finished = Some(result),
            }
        }
        if let Some(result) = finished {
            self.rx = None;
            match result {
                Ok(summary) => {
                    self.succeeded = true;
                    self.status_msg = summary.message();
                    if self.open_folder_after {
                        platform::open_folder(&self.extract_dir);
                    }
                }
                Err(err) => {
                    self.succeeded = false;
                    self.status_msg = err;
                }
            }
            self.page = WizardPage::Finished;
        }
    }
}

impl eframe::App for Installer {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.poll_extraction();

        egui::SidePanel::left("cob_sidebar")
            .exact_width(165.0)
            .resizable(false)
            .frame(egui::Frame::none())
            .show(ctx, |ui| {
                theme::draw_sidebar(ui, &self.banner);
            });

        egui::CentralPanel::default()
            .frame(egui::Frame::none().fill(Color32::WHITE).inner_margin(egui::Margin::symmetric(24.0, 18.0)))
            .show(ctx, |ui| match self.page {
                WizardPage::Welcome => pages::welcome::render(self, ui, ctx),
                WizardPage::Customize => pages::advanced::render(self, ui, ctx),
                WizardPage::Extracting => pages::status::render_installing(self, ui),
                WizardPage::Finished => pages::status::render_finished(self, ui, ctx),
            });
    }
}

fn window_icon() -> egui::IconData {
    let img = image::load_from_memory(ICON_PNG).expect("embedded icon is valid").to_rgba8();
    egui::IconData { width: img.width(), height: img.height(), rgba: img.into_raw() }
}

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([610.0, 385.0])
            .with_resizable(false)
            .with_icon(Arc::new(window_icon())),
        ..Default::default()
    };

    eframe::run_native(
        &format!("Cob {} Setup", APP_VERSION),
        options,
        Box::new(|cc| Ok(Box::new(Installer::new(cc)))),
    )
}
