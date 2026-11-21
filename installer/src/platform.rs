//! The few places the wizard has to know which OS it is on.

use std::path::PathBuf;
use std::process::Command;

pub const PLATFORM_NAME: &str = if cfg!(target_os = "windows") {
    "Windows"
} else if cfg!(target_os = "macos") {
    "macOS"
} else {
    "Linux"
};

pub const ARCH_LABEL: &str = if cfg!(target_pointer_width = "64") { "64-bit" } else { "32-bit" };

/// Where "Extract Now" puts Cob: `<Downloads>/Cob`, falling back to the home folder.
pub fn default_install_dir() -> PathBuf {
    let mut p = dirs::download_dir().or_else(dirs::home_dir).unwrap_or_else(|| PathBuf::from("."));
    p.push("Cob");
    p
}

/// Shows `dir` in the system file manager. Failure is ignored: it is a courtesy.
pub fn open_folder(dir: &str) {
    let opener = if cfg!(target_os = "windows") {
        "explorer"
    } else if cfg!(target_os = "macos") {
        "open"
    } else {
        "xdg-open"
    };
    let _ = Command::new(opener).arg(dir).spawn();
}
