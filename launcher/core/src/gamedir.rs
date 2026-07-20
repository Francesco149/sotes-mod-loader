//! The game install directory: locating/validating it, the on-disk paths the loader reads
//! (`mods\`, `oss_loader.cfg`, `oss_mods.cfg`, the log), and the loader **proxy** install.
//!
//! Proxy model (root README "Install"): the loader ships as a drop-in `version.dll`; the real
//! version API it forwards to is `realver.dll` (normally a copy of `C:\Windows\SysWOW64\version.dll`
//! — the game exe is never modified). So:
//!   - **install** — put our `version.dll` in the game dir with `realver.dll` beside it as the
//!     forward target. If a *foreign* `version.dll` is already there (the game shipped one), back
//!     it up to `realver.dll` first ("rename the real one"); otherwise copy `realver.dll` from the
//!     system version.dll.
//!   - **uninstall** — restore `realver.dll` back to `version.dll`, undoing the swap.
//!
//! It's all plain file moves/copies, so the whole thing unit-tests on Linux against temp dirs.
//! A game-dir `version.dll` is identified as "ours" by a byte (sha256) match against the loader
//! DLL we install — no registry marker or export probing needed.

use anyhow::{bail, Context, Result};
use std::path::{Path, PathBuf};

/// A game install directory + the loader-relevant paths within it.
#[derive(Debug, Clone)]
pub struct GameDir {
    pub root: PathBuf,
}

impl GameDir {
    pub fn new(root: impl Into<PathBuf>) -> Self {
        Self { root: root.into() }
    }

    pub fn exists(&self) -> bool {
        self.root.is_dir()
    }

    /// Whether the given game exe is present (the basic "is this the right folder?" check).
    pub fn has_exe(&self, exe_name: &str) -> bool {
        self.root.join(exe_name).is_file()
    }

    /// Search common install locations for a dir containing one of `exes` (tried in order per
    /// location). Returns the matching dir + the exe name found. Order: the dev scratch copy, then
    /// every Steam library's `steamapps/common/*`. Used to pre-fill the game dir on first launch.
    pub fn autodetect(exes: &[&str]) -> Option<(GameDir, String)> {
        for dir in Self::candidate_dirs() {
            for exe in exes {
                if dir.join(exe).is_file() {
                    return Some((GameDir::new(dir), (*exe).to_owned()));
                }
            }
        }
        None
    }

    /// Candidate game dirs to probe, in priority order.
    fn candidate_dirs() -> Vec<PathBuf> {
        let mut dirs = vec![PathBuf::from(r"C:\oss-ennse-voice-repro\stock")]; // dev scratch copy
        for lib in steam_libraries() {
            if let Ok(rd) = std::fs::read_dir(lib.join("steamapps").join("common")) {
                dirs.extend(rd.flatten().map(|e| e.path()).filter(|p| p.is_dir()));
            }
        }
        dirs
    }

    pub fn version_dll(&self) -> PathBuf {
        self.root.join("version.dll")
    }
    pub fn realver_dll(&self) -> PathBuf {
        self.root.join("realver.dll")
    }
    pub fn mods_dir(&self) -> PathBuf {
        self.root.join("mods")
    }
    pub fn oss_loader_cfg(&self) -> PathBuf {
        self.root.join("oss_loader.cfg")
    }
    pub fn oss_mods_cfg(&self) -> PathBuf {
        self.root.join("oss_mods.cfg")
    }
    pub fn log_path(&self) -> PathBuf {
        self.root.join("oss_modloader.log")
    }

    /// Resolve a registry `dest` (a path RELATIVE to the game dir, `/` or `\` separated) to an
    /// absolute path under the game dir. `None` if it isn't a safe relative path (empty, absolute,
    /// drive/UNC, or containing `..`) — an install must never write outside the game dir.
    pub fn resolve_dest(&self, rel: &str) -> Option<PathBuf> {
        if rel.is_empty() || rel.starts_with('/') || rel.starts_with('\\') || rel.contains(':') {
            return None;
        }
        let mut p = self.root.clone();
        for comp in rel.split(['/', '\\']) {
            match comp {
                "" | "." => continue,
                ".." => return None,
                c => p.push(c),
            }
        }
        Some(p)
    }
}

/// The state of the loader proxy in a game dir.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ProxyState {
    /// No `version.dll` — the loader is absent (the game loads the system version.dll).
    NotInstalled,
    /// Our loader `version.dll` + its `realver.dll` forward target — healthy.
    Installed,
    /// Our loader `version.dll` is present but `realver.dll` (the forward target) is missing —
    /// the proxy would fail to forward; [`ProxyInstaller::install`] repairs it.
    NeedsRepair,
    /// A `version.dll` that isn't our loader is present — install backs it up to `realver.dll`.
    Foreign,
}

/// Installs / repairs / removes the loader proxy in a game dir.
pub struct ProxyInstaller {
    /// Our built `version.dll` (the proxy to drop in). Also the identity oracle: a game-dir
    /// `version.dll` is "ours" iff it byte-matches this.
    pub loader_dll: PathBuf,
    /// Where to source `realver.dll` when the game dir has no `version.dll` to back up. Defaults
    /// to the 32-bit system version.dll.
    pub system_version_dll: PathBuf,
}

impl ProxyInstaller {
    /// The 32-bit system `version.dll` (WOW64 holds 32-bit DLLs on 64-bit Windows).
    pub const SYSTEM_VERSION_DLL: &'static str = r"C:\Windows\SysWOW64\version.dll";

    pub fn new(loader_dll: impl Into<PathBuf>, system_version_dll: impl Into<PathBuf>) -> Self {
        Self {
            loader_dll: loader_dll.into(),
            system_version_dll: system_version_dll.into(),
        }
    }

    /// The normal Windows setup: back up `realver.dll` from `C:\Windows\SysWOW64\version.dll`.
    pub fn windows(loader_dll: impl Into<PathBuf>) -> Self {
        Self::new(loader_dll, Self::SYSTEM_VERSION_DLL)
    }

    fn is_ours(&self, path: &Path) -> Result<bool> {
        if !path.exists() {
            return Ok(false);
        }
        let a = crate::verify::sha256_hex_file(path)
            .with_context(|| format!("hashing {}", path.display()))?;
        let b = crate::verify::sha256_hex_file(&self.loader_dll)
            .with_context(|| format!("hashing loader {}", self.loader_dll.display()))?;
        Ok(a == b)
    }

    pub fn detect(&self, g: &GameDir) -> Result<ProxyState> {
        if !g.version_dll().exists() {
            return Ok(ProxyState::NotInstalled);
        }
        if self.is_ours(&g.version_dll())? {
            Ok(if g.realver_dll().exists() {
                ProxyState::Installed
            } else {
                ProxyState::NeedsRepair
            })
        } else {
            Ok(ProxyState::Foreign)
        }
    }

    /// Idempotent install/repair. Returns the resulting state (`Installed` on success).
    pub fn install(&self, g: &GameDir) -> Result<ProxyState> {
        if !g.exists() {
            bail!("game dir does not exist: {}", g.root.display());
        }
        if !self.loader_dll.is_file() {
            bail!("loader version.dll not found: {}", self.loader_dll.display());
        }
        let v = g.version_dll();
        let r = g.realver_dll();

        match self.detect(g)? {
            // Already ours — just refresh version.dll from the loader (repair to the latest build).
            ProxyState::Installed => self.write_proxy(&v)?,
            // Ours but no forward target — restore realver from the system, refresh the proxy.
            ProxyState::NeedsRepair => {
                self.ensure_realver(&r)?;
                self.write_proxy(&v)?;
            }
            // Nothing there — realver from the system, then drop the proxy.
            ProxyState::NotInstalled => {
                self.ensure_realver(&r)?;
                self.write_proxy(&v)?;
            }
            // A real version.dll sits in the game dir — back it up to realver (the forward target),
            // then drop the proxy over it.
            ProxyState::Foreign => {
                if !r.exists() {
                    std::fs::rename(&v, &r)
                        .with_context(|| format!("backing up {} -> {}", v.display(), r.display()))?;
                }
                self.write_proxy(&v)?;
            }
        }

        let st = self.detect(g)?;
        if st != ProxyState::Installed {
            bail!("proxy install did not reach Installed (got {st:?})");
        }
        Ok(st)
    }

    /// Remove the proxy: if our `version.dll` is present, delete it and restore `realver.dll`
    /// back to `version.dll` (undoing the swap). Idempotent; a no-op if we're not installed. A
    /// *foreign* version.dll is left untouched.
    pub fn uninstall(&self, g: &GameDir) -> Result<()> {
        let v = g.version_dll();
        let r = g.realver_dll();
        if v.exists() && self.is_ours(&v)? {
            std::fs::remove_file(&v).with_context(|| format!("removing {}", v.display()))?;
        }
        if r.exists() && !v.exists() {
            std::fs::rename(&r, &v)
                .with_context(|| format!("restoring {} -> {}", r.display(), v.display()))?;
        }
        Ok(())
    }

    fn write_proxy(&self, version_dll: &Path) -> Result<()> {
        std::fs::copy(&self.loader_dll, version_dll)
            .with_context(|| format!("writing proxy {}", version_dll.display()))?;
        Ok(())
    }

    fn ensure_realver(&self, realver: &Path) -> Result<()> {
        if realver.exists() {
            return Ok(());
        }
        if !self.system_version_dll.is_file() {
            bail!(
                "no realver.dll and the system version.dll is missing: {}",
                self.system_version_dll.display()
            );
        }
        std::fs::copy(&self.system_version_dll, realver).with_context(|| {
            format!(
                "copying {} -> {}",
                self.system_version_dll.display(),
                realver.display()
            )
        })?;
        Ok(())
    }
}

/// Steam library roots (the default install + any in `libraryfolders.vdf`), de-duplicated.
fn steam_libraries() -> Vec<PathBuf> {
    let mut steam_roots: Vec<PathBuf> = ["ProgramFiles(x86)", "ProgramFiles"]
        .iter()
        .filter_map(|e| std::env::var_os(e))
        .map(|pf| PathBuf::from(pf).join("Steam"))
        .collect();
    steam_roots.push(PathBuf::from(r"C:\Program Files (x86)\Steam"));

    let mut libs = Vec::new();
    for steam in steam_roots {
        if steam.is_dir() {
            libs.push(steam.clone());
        }
        if let Ok(vdf) = std::fs::read_to_string(steam.join("steamapps").join("libraryfolders.vdf")) {
            libs.extend(parse_vdf_paths(&vdf).into_iter().map(PathBuf::from));
        }
    }
    libs.sort();
    libs.dedup();
    libs
}

/// Extract `"path"` values from a Steam `libraryfolders.vdf` (lines like `"path"  "D:\\SteamLibrary"`).
/// VDF escapes backslashes as `\\`, unescaped back to `\`.
fn parse_vdf_paths(vdf: &str) -> Vec<String> {
    vdf.lines()
        .filter_map(|line| {
            let rest = line.trim().strip_prefix("\"path\"")?;
            let after = &rest[rest.find('"')? + 1..];
            Some(after[..after.find('"')?].replace("\\\\", "\\"))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    // Fake "DLL" contents so identity + backup can be asserted by bytes.
    const LOADER: &[u8] = b"OUR-LOADER-version.dll";
    const SYSTEM: &[u8] = b"SYSTEM-version.dll";
    const GAME_SHIPPED: &[u8] = b"GAME-SHIPPED-version.dll";

    /// A temp game dir + a ProxyInstaller wired to fake loader/system DLLs.
    fn fixture() -> (tempfile::TempDir, GameDir, ProxyInstaller) {
        let td = tempfile::tempdir().unwrap();
        let root = td.path();
        let game = root.join("game");
        fs::create_dir(&game).unwrap();
        let loader = root.join("build/version.dll");
        fs::create_dir_all(loader.parent().unwrap()).unwrap();
        fs::write(&loader, LOADER).unwrap();
        let system = root.join("SysWOW64/version.dll");
        fs::create_dir_all(system.parent().unwrap()).unwrap();
        fs::write(&system, SYSTEM).unwrap();
        let inst = ProxyInstaller::new(loader, system);
        (td, GameDir::new(game), inst)
    }

    #[test]
    fn install_from_clean_dir() {
        let (_td, g, inst) = fixture();
        assert_eq!(inst.detect(&g).unwrap(), ProxyState::NotInstalled);

        assert_eq!(inst.install(&g).unwrap(), ProxyState::Installed);
        assert_eq!(fs::read(g.version_dll()).unwrap(), LOADER); // proxy = our loader
        assert_eq!(fs::read(g.realver_dll()).unwrap(), SYSTEM); // forward target = system dll
        assert_eq!(inst.detect(&g).unwrap(), ProxyState::Installed);
    }

    #[test]
    fn install_is_idempotent() {
        let (_td, g, inst) = fixture();
        inst.install(&g).unwrap();
        // Tamper realver to a distinct value, reinstall: realver must be PRESERVED (not re-copied).
        fs::write(g.realver_dll(), b"KEEP-ME").unwrap();
        assert_eq!(inst.install(&g).unwrap(), ProxyState::Installed);
        assert_eq!(fs::read(g.realver_dll()).unwrap(), b"KEEP-ME");
        assert_eq!(fs::read(g.version_dll()).unwrap(), LOADER);
    }

    #[test]
    fn foreign_version_dll_is_backed_up_then_restored() {
        let (_td, g, inst) = fixture();
        // The game shipped its own version.dll, no realver yet.
        fs::write(g.version_dll(), GAME_SHIPPED).unwrap();
        assert_eq!(inst.detect(&g).unwrap(), ProxyState::Foreign);

        inst.install(&g).unwrap();
        assert_eq!(fs::read(g.version_dll()).unwrap(), LOADER); // proxy dropped in
        assert_eq!(fs::read(g.realver_dll()).unwrap(), GAME_SHIPPED); // real one preserved

        inst.uninstall(&g).unwrap();
        assert_eq!(fs::read(g.version_dll()).unwrap(), GAME_SHIPPED); // real one restored
        assert!(!g.realver_dll().exists());
    }

    #[test]
    fn repairs_missing_realver() {
        let (_td, g, inst) = fixture();
        inst.install(&g).unwrap();
        fs::remove_file(g.realver_dll()).unwrap();
        assert_eq!(inst.detect(&g).unwrap(), ProxyState::NeedsRepair);
        assert_eq!(inst.install(&g).unwrap(), ProxyState::Installed);
        assert_eq!(fs::read(g.realver_dll()).unwrap(), SYSTEM);
    }

    #[test]
    fn uninstall_of_system_copy_restores_version_dll() {
        let (_td, g, inst) = fixture();
        inst.install(&g).unwrap(); // realver = system copy
        inst.uninstall(&g).unwrap();
        // realver renamed back to version.dll; now a system copy sits there (harmless, forwards fine).
        assert_eq!(fs::read(g.version_dll()).unwrap(), SYSTEM);
        assert!(!g.realver_dll().exists());
        // It's no longer "ours", so a re-detect sees a foreign dll (which a reinstall would back up).
        assert_eq!(inst.detect(&g).unwrap(), ProxyState::Foreign);
    }

    #[test]
    fn install_without_loader_dll_errors() {
        let (_td, g, _inst) = fixture();
        let bad = ProxyInstaller::new("/nonexistent/version.dll", "/nonexistent/system.dll");
        assert!(bad.install(&g).is_err());
    }

    #[test]
    fn parses_steam_library_paths_from_vdf() {
        let vdf = r#"
"libraryfolders"
{
    "0"
    {
        "path"		"C:\\Program Files (x86)\\Steam"
        "label"		""
    }
    "1"
    {
        "path"		"D:\\SteamLibrary"
    }
}
"#;
        assert_eq!(
            parse_vdf_paths(vdf),
            vec![
                r"C:\Program Files (x86)\Steam".to_string(),
                r"D:\SteamLibrary".to_string()
            ]
        );
        assert!(parse_vdf_paths("no paths here").is_empty());
    }

    #[test]
    fn resolve_dest_stays_inside_game_dir() {
        let g = GameDir::new("/games/sotes");
        assert_eq!(
            g.resolve_dest("mods/autoload/init.lua").unwrap(),
            PathBuf::from("/games/sotes/mods/autoload/init.lua")
        );
        assert_eq!(
            g.resolve_dest("mods\\x\\a.dll").unwrap(),
            PathBuf::from("/games/sotes/mods/x/a.dll")
        );
        assert!(g.resolve_dest("../escape").is_none());
        assert!(g.resolve_dest("/abs").is_none());
        assert!(g.resolve_dest("C:\\win").is_none());
        assert!(g.resolve_dest("").is_none());
    }
}
