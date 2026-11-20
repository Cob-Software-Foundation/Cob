use std::fs;
use std::io::Cursor;
use std::path::PathBuf;

const PAYLOAD_ZIP: &[u8] = include_bytes!("../payload.zip");

pub fn extract_payload(target_dir: &str) -> Result<String, String> {
    let target_path = PathBuf::from(target_dir);
    let reader = Cursor::new(PAYLOAD_ZIP);

    let mut archive = zip::ZipArchive::new(reader)
        .map_err(|e| format!("Failed to read embedded archive: {}", e))?;

    fs::create_dir_all(&target_path)
        .map_err(|e| format!("Failed to create destination folder: {}", e))?;

    archive
        .extract(&target_path)
        .map_err(|e| format!("Extraction error: {}", e))?;

    Ok(format!("Extracted {} files to {}", archive.len(), target_path.display()))
}
