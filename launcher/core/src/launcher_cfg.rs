//! The launcher's OWN persisted settings (distinct from the game's `oss_*.cfg`): the chosen game
//! dir + exe, the loader dll name, and the list of **trusted mod sources** (registry URLs/paths).
//!
//! Persisted as JSON in the per-user config dir (`%APPDATA%\sotes-mod-loader\launcher.json` on
//! Windows, `$XDG_CONFIG_HOME`/`~/.config` otherwise) so it survives across launches. Adding a
//! source here == permanently trusting it; the launcher auto-fetches every source on startup.

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// The default mod source shipped with the launcher (the sotes-mods registry over HTTPS).
pub const DEFAULT_SOURCE: &str =
    "https://raw.githubusercontent.com/Francesco149/sotes-mods/master/registry.json";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(default)]
pub struct LauncherConfig {
    /// The game install directory (empty until detected/chosen).
    pub game_path: String,
    /// The game exe to launch (auto-set by detection; Steam EN = `sotes_en.exe`).
    pub game_exe: String,
    /// The loader proxy dll name (always `version.dll` in practice).
    pub loader_dll: String,
    /// Trusted mod sources — registry.json URLs (`http(s)://`) or local paths. Auto-fetched on start.
    pub sources: Vec<String>,
}

impl Default for LauncherConfig {
    fn default() -> Self {
        Self {
            game_path: String::new(),
            game_exe: "sotes_en.exe".to_owned(),
            loader_dll: "version.dll".to_owned(),
            sources: vec![DEFAULT_SOURCE.to_owned()],
        }
    }
}

impl LauncherConfig {
    /// The per-user config file path (`%APPDATA%\sotes-mod-loader\launcher.json`, else XDG, else CWD).
    pub fn default_path() -> PathBuf {
        let base = std::env::var_os("APPDATA")
            .map(PathBuf::from)
            .or_else(|| std::env::var_os("XDG_CONFIG_HOME").map(PathBuf::from))
            .or_else(|| std::env::var_os("HOME").map(|h| PathBuf::from(h).join(".config")));
        match base {
            Some(dir) => dir.join("sotes-mod-loader").join("launcher.json"),
            None => PathBuf::from("sotes-launcher.json"),
        }
    }

    /// Load from `path`, or return defaults if it is missing/unreadable/corrupt (never fails — a
    /// broken config must not stop the launcher from opening).
    pub fn load_from(path: &Path) -> Self {
        std::fs::read_to_string(path)
            .ok()
            .and_then(|s| serde_json::from_str(&s).ok())
            .unwrap_or_default()
    }

    /// Load from [`default_path`](Self::default_path).
    pub fn load() -> Self {
        Self::load_from(&Self::default_path())
    }

    pub fn save_to(&self, path: &Path) -> anyhow::Result<()> {
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir)?;
        }
        std::fs::write(path, serde_json::to_string_pretty(self)?)?;
        Ok(())
    }

    pub fn save(&self) -> anyhow::Result<()> {
        self.save_to(&Self::default_path())
    }

    /// Trust a source (trimmed, de-duplicated). Returns true if it was newly added.
    pub fn add_source(&mut self, url: &str) -> bool {
        let u = url.trim().to_owned();
        if u.is_empty() || self.sources.iter().any(|s| s == &u) {
            return false;
        }
        self.sources.push(u);
        true
    }

    /// Untrust a source. The default source can be removed too (re-added via Reset if desired).
    pub fn remove_source(&mut self, url: &str) {
        self.sources.retain(|s| s != url);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn defaults_have_the_default_source() {
        let c = LauncherConfig::default();
        assert_eq!(c.sources, vec![DEFAULT_SOURCE.to_owned()]);
        assert_eq!(c.loader_dll, "version.dll");
    }

    #[test]
    fn add_source_dedups_and_trims() {
        let mut c = LauncherConfig::default();
        assert!(c.add_source("  https://example.com/r.json  "));
        assert!(!c.add_source("https://example.com/r.json")); // already trusted
        assert!(!c.add_source("   ")); // empty
        assert_eq!(c.sources.len(), 2);
        c.remove_source("https://example.com/r.json");
        assert_eq!(c.sources, vec![DEFAULT_SOURCE.to_owned()]);
    }

    #[test]
    fn round_trips_through_disk_and_tolerates_garbage() {
        let td = tempfile::tempdir().unwrap();
        let p = td.path().join("sub").join("launcher.json");
        let mut c = LauncherConfig::default();
        c.game_path = r"C:\games\sotes".to_owned();
        c.add_source("https://example.com/r.json");
        c.save_to(&p).unwrap();

        let back = LauncherConfig::load_from(&p);
        assert_eq!(back, c);

        // missing file → defaults; corrupt file → defaults (never panics)
        assert_eq!(LauncherConfig::load_from(&td.path().join("nope.json")), LauncherConfig::default());
        std::fs::write(&p, "not json {{{").unwrap();
        assert_eq!(LauncherConfig::load_from(&p), LauncherConfig::default());
    }
}
