//! Install / remove a mod version into a game dir — the package-manager core.
//!
//! A version's `files[]` (registry.rs) are resolved to game-dir paths, each `download` fetched
//! and **sha256-verified against the pinned hash before it's written** (the file host is
//! untrusted — see `docs/REGISTRY.md`), each `user_supplied` copied from a path the user picked
//! (verified if a hash is given). What got placed is recorded in an installed manifest so removal
//! is exact.
//!
//! The network is abstracted behind [`Fetcher`] so this whole flow unit-tests on Linux with a
//! stub fetcher (no network). The real HTTPS fetch ([`HttpFetcher`]) is behind the `http` feature
//! (reqwest + rustls), so `cargo test` on the plumbing stays reqwest-free and fast.

use crate::gamedir::GameDir;
use crate::registry::{FileSpec, Mod, Registry, Version};
use crate::verify;
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// Fetches the bytes at a URL. Abstracted so install is testable without network.
pub trait Fetcher {
    /// The whole file at `url` into memory (mod files are small).
    fn fetch(&self, url: &str) -> Result<Vec<u8>>;
}

/// Parse a source's `registry.json` fetched over the `Fetcher` (the "add a source by URL" path).
pub fn fetch_registry<F: Fetcher>(fetcher: &F, url: &str) -> Result<Registry> {
    let bytes = fetcher
        .fetch(url)
        .with_context(|| format!("fetching registry {url}"))?;
    let text = String::from_utf8(bytes).context("registry.json is not UTF-8")?;
    Registry::from_json_str(&text)
}

/// The launcher's record of what's installed in a game dir (its own state, not the loader's).
#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct InstalledManifest {
    #[serde(default)]
    pub mods: BTreeMap<String, InstalledMod>,
}

/// One installed mod: the version present and the game-dir-relative files it placed.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct InstalledMod {
    pub version: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub source: Option<String>,
    /// Dests (relative to the game dir) this mod placed — the exact removal set.
    pub files: Vec<String>,
}

impl InstalledManifest {
    /// The launcher-managed manifest file, in the game dir (the loader ignores it).
    pub const FILE: &'static str = "oss_installed.json";

    pub fn path(game: &GameDir) -> PathBuf {
        game.root.join(Self::FILE)
    }

    /// Load the manifest; a missing file is an empty manifest (nothing installed yet).
    pub fn load(game: &GameDir) -> Result<Self> {
        let p = Self::path(game);
        match std::fs::read_to_string(&p) {
            Ok(s) => serde_json::from_str(&s).with_context(|| format!("parsing {}", p.display())),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Self::default()),
            Err(e) => Err(e).with_context(|| format!("reading {}", p.display())),
        }
    }

    pub fn save(&self, game: &GameDir) -> Result<()> {
        let p = Self::path(game);
        let s = serde_json::to_string_pretty(self).context("serializing installed manifest")?;
        std::fs::write(&p, s).with_context(|| format!("writing {}", p.display()))
    }

    pub fn get(&self, id: &str) -> Option<&InstalledMod> {
        self.mods.get(id)
    }
    pub fn is_installed(&self, id: &str) -> bool {
        self.mods.contains_key(id)
    }
}

/// Install `m`'s version `v` into `game`, verifying every download and recording it in `manifest`
/// (which is saved on success). `user_files` maps a `user_supplied` file's `dest` to the local
/// path the user picked. On any failure, files written *this call* are rolled back.
pub fn install_version<F: Fetcher>(
    game: &GameDir,
    fetcher: &F,
    source_name: Option<&str>,
    m: &Mod,
    v: &Version,
    user_files: &BTreeMap<String, PathBuf>,
    manifest: &mut InstalledManifest,
) -> Result<()> {
    // Pre-flight: every dest must stay inside the game dir before we write anything.
    for f in &v.files {
        if game.resolve_dest(f.dest()).is_none() {
            bail!("unsafe dest '{}' in {} {}", f.dest(), m.id, v.version);
        }
    }

    let mut written: Vec<(String, PathBuf)> = Vec::new();
    let placed = place_files(game, fetcher, v, user_files, &mut written);

    if let Err(e) = placed {
        for (_, abs) in &written {
            let _ = std::fs::remove_file(abs); // best-effort rollback of this call's writes
        }
        return Err(e);
    }

    manifest.mods.insert(
        m.id.clone(),
        InstalledMod {
            version: v.version.clone(),
            source: source_name.map(String::from),
            files: written.into_iter().map(|(d, _)| d).collect(),
        },
    );
    manifest.save(game)
}

fn place_files<F: Fetcher>(
    game: &GameDir,
    fetcher: &F,
    v: &Version,
    user_files: &BTreeMap<String, PathBuf>,
    written: &mut Vec<(String, PathBuf)>,
) -> Result<()> {
    for f in &v.files {
        let dest = f.dest().to_string();
        let abs = game.resolve_dest(&dest).expect("checked in pre-flight");
        match f {
            FileSpec::Download { url, sha256, .. } => {
                let bytes = fetcher
                    .fetch(url)
                    .with_context(|| format!("downloading {url}"))?;
                verify::ensure(&bytes, sha256)
                    .with_context(|| format!("verifying {dest}"))?;
                write_file(&abs, &bytes)?;
                written.push((dest, abs));
            }
            FileSpec::UserSupplied {
                sha256,
                optional,
                prompt,
                ..
            } => match user_files.get(&dest) {
                Some(src) => {
                    let bytes = std::fs::read(src)
                        .with_context(|| format!("reading user file {}", src.display()))?;
                    if let Some(h) = sha256 {
                        verify::ensure(&bytes, h)
                            .with_context(|| format!("verifying user file {dest}"))?;
                    }
                    write_file(&abs, &bytes)?;
                    written.push((dest, abs));
                }
                None if *optional => {}
                None => bail!("required user-supplied file not provided: {dest} ({prompt})"),
            },
        }
    }
    Ok(())
}

/// Remove an installed mod: delete the files it placed, prune now-empty dirs under the game dir,
/// and drop it from `manifest` (saved). A no-op if `id` isn't installed.
pub fn remove_mod(game: &GameDir, id: &str, manifest: &mut InstalledManifest) -> Result<()> {
    let Some(entry) = manifest.mods.remove(id) else {
        return Ok(());
    };
    for dest in &entry.files {
        if let Some(abs) = game.resolve_dest(dest) {
            let _ = std::fs::remove_file(&abs);
            prune_empty_dirs(&game.root, abs.parent());
        }
    }
    manifest.save(game)
}

fn write_file(abs: &Path, bytes: &[u8]) -> Result<()> {
    if let Some(parent) = abs.parent() {
        std::fs::create_dir_all(parent)
            .with_context(|| format!("creating {}", parent.display()))?;
    }
    std::fs::write(abs, bytes).with_context(|| format!("writing {}", abs.display()))
}

/// Remove empty directories from `start` upward, stopping at (and never removing) `game_root`.
fn prune_empty_dirs(game_root: &Path, start: Option<&Path>) {
    let mut dir = start;
    while let Some(d) = dir {
        if d == game_root || !d.starts_with(game_root) {
            break;
        }
        let empty = std::fs::read_dir(d)
            .map(|mut it| it.next().is_none())
            .unwrap_or(false);
        if !empty {
            break; // unreadable or non-empty
        }
        if std::fs::remove_dir(d).is_err() {
            break;
        }
        dir = d.parent();
    }
}

/// HTTPS fetcher (ureq + rustls/ring). Behind the `http` feature so the plumbing tests stay light.
#[cfg(feature = "http")]
pub struct HttpFetcher {
    agent: ureq::Agent,
}

#[cfg(feature = "http")]
impl Default for HttpFetcher {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(feature = "http")]
impl HttpFetcher {
    /// Cap a single download at 64 MiB — mod files are small; refuse a runaway body.
    const MAX_BYTES: u64 = 64 * 1024 * 1024;

    pub fn new() -> Self {
        let agent = ureq::AgentBuilder::new()
            .user_agent(concat!("sotes-launcher/", env!("CARGO_PKG_VERSION")))
            .build();
        Self { agent }
    }
}

#[cfg(feature = "http")]
impl Fetcher for HttpFetcher {
    fn fetch(&self, url: &str) -> Result<Vec<u8>> {
        use std::io::Read;
        // ureq returns Err for >= 400, so a bad status is already an error here.
        let resp = self.agent.get(url).call().with_context(|| format!("GET {url}"))?;
        let mut buf = Vec::new();
        resp.into_reader()
            .take(Self::MAX_BYTES)
            .read_to_end(&mut buf)
            .with_context(|| format!("reading {url}"))?;
        Ok(buf)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::registry::Mod;
    use std::fs;

    /// An in-memory fetcher: url -> bytes.
    struct StubFetcher(BTreeMap<String, Vec<u8>>);
    impl Fetcher for StubFetcher {
        fn fetch(&self, url: &str) -> Result<Vec<u8>> {
            self.0
                .get(url)
                .cloned()
                .ok_or_else(|| anyhow::anyhow!("stub: no url {url}"))
        }
    }

    fn game() -> (tempfile::TempDir, GameDir) {
        let td = tempfile::tempdir().unwrap();
        let g = GameDir::new(td.path().join("game"));
        fs::create_dir_all(&g.root).unwrap();
        (td, g)
    }

    fn dl(dest: &str, url: &str, bytes: &[u8]) -> FileSpec {
        FileSpec::Download {
            dest: dest.into(),
            url: url.into(),
            sha256: verify::sha256_hex(bytes),
            size: Some(bytes.len() as u64),
        }
    }

    fn a_mod(id: &str) -> Mod {
        Mod {
            id: id.into(),
            name: id.into(),
            description: String::new(),
            keywords: vec![],
            target_games: vec![],
            versions: vec![],
        }
    }

    #[test]
    fn installs_downloads_and_records_manifest() {
        let (_td, g) = game();
        let body = b"-- init.lua\nmod.log('hi')\n";
        let url = "https://host/init.lua";
        let v = Version {
            version: "1.0.0".into(),
            released: String::new(),
            notes: String::new(),
            min_loader: None,
            requires: Default::default(),
            files: vec![dl("mods/hello/init.lua", url, body)],
        };
        let fetcher = StubFetcher(BTreeMap::from([(url.to_string(), body.to_vec())]));
        let mut manifest = InstalledManifest::default();

        install_version(&g, &fetcher, Some("sotes-mods"), &a_mod("hello"), &v, &BTreeMap::new(), &mut manifest)
            .unwrap();

        assert_eq!(fs::read(g.root.join("mods/hello/init.lua")).unwrap(), body);
        let e = manifest.get("hello").unwrap();
        assert_eq!(e.version, "1.0.0");
        assert_eq!(e.source.as_deref(), Some("sotes-mods"));
        assert_eq!(e.files, ["mods/hello/init.lua"]);
        // manifest persisted + reloads
        assert!(InstalledManifest::load(&g).unwrap().is_installed("hello"));
    }

    #[test]
    fn hash_mismatch_aborts_and_rolls_back() {
        let (_td, g) = game();
        let url = "https://host/init.lua";
        // registry pins the hash of "GOOD" but the host serves "EVIL"
        let mut v = Version {
            version: "1.0.0".into(),
            released: String::new(),
            notes: String::new(),
            min_loader: None,
            requires: Default::default(),
            files: vec![dl("mods/x/init.lua", url, b"GOOD")],
        };
        // a second good file placed BEFORE the bad one, to prove rollback removes it too
        v.files.insert(0, dl("mods/x/a.lua", "https://host/a", b"AAA"));
        let fetcher = StubFetcher(BTreeMap::from([
            ("https://host/a".to_string(), b"AAA".to_vec()),
            (url.to_string(), b"EVIL".to_vec()),
        ]));
        let mut manifest = InstalledManifest::default();

        let err = install_version(&g, &fetcher, None, &a_mod("x"), &v, &BTreeMap::new(), &mut manifest)
            .unwrap_err();
        assert!(err.to_string().contains("verifying mods/x/init.lua"), "{err}");
        assert!(!g.root.join("mods/x/init.lua").exists());
        assert!(!g.root.join("mods/x/a.lua").exists(), "the good file must be rolled back too");
        assert!(!manifest.is_installed("x"));
    }

    #[test]
    fn user_supplied_required_and_optional() {
        let (_td, g) = game();
        // provide a real source file for the required user_supplied dll
        let src = g.root.join("../user_sotesx_s.dll");
        fs::write(&src, b"VOICEBANK").unwrap();
        let v = Version {
            version: "1.0.0".into(),
            released: String::new(),
            notes: String::new(),
            min_loader: None,
            requires: Default::default(),
            files: vec![
                FileSpec::UserSupplied {
                    dest: "sotesx_s.dll".into(),
                    prompt: "Locate sotesx_s.dll".into(),
                    sha256: Some(verify::sha256_hex(b"VOICEBANK")),
                    optional: false,
                },
                FileSpec::UserSupplied {
                    dest: "optional.dat".into(),
                    prompt: "optional".into(),
                    sha256: None,
                    optional: true,
                },
            ],
        };
        let fetcher = StubFetcher(BTreeMap::new());
        let user = BTreeMap::from([("sotesx_s.dll".to_string(), src.clone())]);
        let mut manifest = InstalledManifest::default();

        install_version(&g, &fetcher, None, &a_mod("voice"), &v, &user, &mut manifest).unwrap();
        assert_eq!(fs::read(g.root.join("sotesx_s.dll")).unwrap(), b"VOICEBANK");
        assert!(!g.root.join("optional.dat").exists()); // optional + not provided -> skipped
        assert_eq!(manifest.get("voice").unwrap().files, ["sotesx_s.dll"]);

        // required-but-missing -> error
        let mut m2 = InstalledManifest::default();
        let err = install_version(&g, &fetcher, None, &a_mod("v2"), &v, &BTreeMap::new(), &mut m2)
            .unwrap_err();
        assert!(err.to_string().contains("required user-supplied file"), "{err}");
    }

    #[test]
    fn remove_deletes_files_and_prunes_dirs() {
        let (_td, g) = game();
        let body = b"x";
        let url = "https://host/f";
        let v = Version {
            version: "1.0.0".into(),
            released: String::new(),
            notes: String::new(),
            min_loader: None,
            requires: Default::default(),
            files: vec![dl("mods/hello/init.lua", url, body)],
        };
        let fetcher = StubFetcher(BTreeMap::from([(url.to_string(), body.to_vec())]));
        let mut manifest = InstalledManifest::default();
        install_version(&g, &fetcher, None, &a_mod("hello"), &v, &BTreeMap::new(), &mut manifest).unwrap();
        assert!(g.root.join("mods/hello").is_dir());

        remove_mod(&g, "hello", &mut manifest).unwrap();
        assert!(!g.root.join("mods/hello/init.lua").exists());
        assert!(!g.root.join("mods/hello").exists(), "empty mod dir pruned");
        assert!(!g.root.join("mods").exists(), "empty mods dir pruned");
        assert!(!manifest.is_installed("hello"));
        // removing again is a no-op
        remove_mod(&g, "hello", &mut manifest).unwrap();
    }

    #[test]
    fn rejects_unsafe_dest_before_writing() {
        let (_td, g) = game();
        let v = Version {
            version: "1.0.0".into(),
            released: String::new(),
            notes: String::new(),
            min_loader: None,
            requires: Default::default(),
            files: vec![dl("../escape.dll", "https://host/e", b"E")],
        };
        let fetcher = StubFetcher(BTreeMap::from([("https://host/e".to_string(), b"E".to_vec())]));
        let mut manifest = InstalledManifest::default();
        let err = install_version(&g, &fetcher, None, &a_mod("bad"), &v, &BTreeMap::new(), &mut manifest)
            .unwrap_err();
        assert!(err.to_string().contains("unsafe dest"), "{err}");
    }
}
