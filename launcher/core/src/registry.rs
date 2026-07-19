//! Parse + validate a mod source's `registry.json` (the package-manager contract).
//!
//! Schema + trust model live in `docs/REGISTRY.md`. Adding a source = trusting THAT repo's
//! registry; every `download` file is then verified against its pinned `sha256` (see
//! [`crate::verify`]) so the *file host* needn't be trusted — only the registry does. This
//! module only parses/validates/merges the manifest; fetching (local path now, HTTPS next)
//! and install live in later modules.

use anyhow::{bail, Context, Result};
use serde::Deserialize;
use std::collections::{BTreeMap, HashSet};

/// The newest `registry_version` this launcher understands. A source declaring a higher
/// number is a future format — refuse it rather than silently misread it.
pub const SUPPORTED_REGISTRY_VERSION: u32 = 1;

/// A whole source manifest (`registry.json` at a source repo's root). Unknown fields (e.g.
/// the sample repo's `_note`) are ignored, so metadata can grow without breaking old launchers.
#[derive(Debug, Clone, Deserialize)]
pub struct Registry {
    pub registry_version: u32,
    pub source: Source,
    #[serde(default)]
    pub mods: Vec<Mod>,
}

/// Display metadata for the whole repo.
#[derive(Debug, Clone, Deserialize)]
pub struct Source {
    pub name: String,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub homepage: String,
    #[serde(default)]
    pub maintainer: String,
}

/// One mod in a source. `id` is unique within the source: it is the on-disk `mods\<id>\`
/// folder name AND the update key.
#[derive(Debug, Clone, Deserialize)]
pub struct Mod {
    pub id: String,
    pub name: String,
    #[serde(default)]
    pub description: String,
    #[serde(default)]
    pub keywords: Vec<String>,
    #[serde(default)]
    pub target_games: Vec<String>,
    /// Newest first (the author's responsibility, per REGISTRY.md). May be empty — a mod
    /// listed for discoverability before its first loader release (all three sotes-mods
    /// entries are like this today).
    #[serde(default)]
    pub versions: Vec<Version>,
}

/// One release of a mod: what to place into the game folder, plus the metadata the update
/// check and compat surfacing use.
#[derive(Debug, Clone, Deserialize)]
pub struct Version {
    pub version: String,
    #[serde(default)]
    pub released: String,
    /// Release notes — shown before an update is applied.
    #[serde(default)]
    pub notes: String,
    /// Refuse to install under an older loader host (semver). Absent = no floor.
    #[serde(default)]
    pub min_loader: Option<String>,
    /// `{ "<mod_id>": "<semver range>" }` dependencies, resolved across sources.
    #[serde(default)]
    pub requires: BTreeMap<String, String>,
    #[serde(default)]
    pub files: Vec<FileSpec>,
}

/// One file a version places into the game folder. `dest` is always **relative to the game
/// dir** (e.g. `mods/<id>/init.lua` or a game-dir DLL) — never absolute, never escaping.
#[derive(Debug, Clone, Deserialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum FileSpec {
    /// Fetched from `url`, verified against `sha256`, written to `dest`. The host is untrusted.
    Download {
        dest: String,
        url: String,
        sha256: String,
        #[serde(default)]
        size: Option<u64>,
    },
    /// A game asset we can't ship (e.g. a voice bank): the user locates it, we sha256-check it
    /// if a hash is given, then copy it to `dest`. `optional` allows skipping.
    UserSupplied {
        dest: String,
        #[serde(default)]
        prompt: String,
        #[serde(default)]
        sha256: Option<String>,
        #[serde(default)]
        optional: bool,
    },
}

impl FileSpec {
    /// The install destination, relative to the game dir (both variants carry one).
    pub fn dest(&self) -> &str {
        match self {
            FileSpec::Download { dest, .. } | FileSpec::UserSupplied { dest, .. } => dest,
        }
    }

    /// Reject a `dest` that isn't a safe relative path (absolute, or climbing out with `..`,
    /// or a drive/UNC prefix). A registry is trusted, but a hostile/typo'd `dest` must never
    /// write outside the game dir — belt-and-suspenders before we ever touch the filesystem.
    pub fn dest_is_safe(&self) -> bool {
        let d = self.dest();
        if d.is_empty() {
            return false;
        }
        // No absolute / drive / UNC roots.
        if d.starts_with('/') || d.starts_with('\\') || d.contains(':') {
            return false;
        }
        // No `..` component (split on both separators — dests are written with `/`, but be safe).
        !d.split(['/', '\\']).any(|c| c == "..")
    }
}

impl Registry {
    /// Parse + validate a `registry.json` from text.
    pub fn from_json_str(s: &str) -> Result<Self> {
        let reg: Registry = serde_json::from_str(s).context("parsing registry.json")?;
        reg.validate()?;
        Ok(reg)
    }

    /// Read + parse a local `registry.json` (a source added as a local path, and the dev
    /// path against `../sotes-mods`). The HTTPS fetch lands next and reuses [`from_json_str`].
    pub fn from_path(path: impl AsRef<std::path::Path>) -> Result<Self> {
        let path = path.as_ref();
        let s = std::fs::read_to_string(path)
            .with_context(|| format!("reading {}", path.display()))?;
        Self::from_json_str(&s)
    }

    /// Structural checks beyond "it's valid JSON": a format we understand, and unique ids.
    pub fn validate(&self) -> Result<()> {
        if self.registry_version > SUPPORTED_REGISTRY_VERSION {
            bail!(
                "registry_version {} is newer than this launcher supports ({}); update the launcher",
                self.registry_version,
                SUPPORTED_REGISTRY_VERSION
            );
        }
        let mut seen = HashSet::new();
        for m in &self.mods {
            if !seen.insert(m.id.as_str()) {
                bail!("duplicate mod id '{}' in source '{}'", m.id, self.source.name);
            }
        }
        Ok(())
    }

    /// A source may split its registry into `registry/*.json`; the launcher merges the shards.
    /// Source metadata is kept from `self`; mods are appended (a dup id across shards is an error).
    pub fn merge(&mut self, other: Registry) -> Result<()> {
        for m in other.mods {
            if self.find(&m.id).is_some() {
                bail!("duplicate mod id '{}' across registry shards", m.id);
            }
            self.mods.push(m);
        }
        Ok(())
    }

    pub fn find(&self, id: &str) -> Option<&Mod> {
        self.mods.iter().find(|m| m.id == id)
    }
}

impl Mod {
    /// The version the launcher shows by default (newest = first, per the contract). `None`
    /// for a not-yet-released mod (empty `versions`).
    pub fn latest(&self) -> Option<&Version> {
        self.versions.first()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // A full-schema fixture (from REGISTRY.md's example): a populated version with BOTH file
    // kinds, plus the `_note` unknown field the real sotes-mods registry carries.
    const FULL: &str = r#"{
      "registry_version": 1,
      "_note": "ignored unknown field",
      "source": { "name": "sotes-mods", "maintainer": "Francesco149" },
      "mods": [
        {
          "id": "ennse_voice",
          "name": "Japanese Voice Patch (EN-SE)",
          "keywords": ["voice", "japanese"],
          "target_games": ["sotes_en"],
          "versions": [
            {
              "version": "1.0.0",
              "released": "2026-07-18",
              "notes": "First release.",
              "min_loader": "0.1.0",
              "files": [
                { "kind": "download", "dest": "mods/ennse_voice/init.lua",
                  "url": "https://example/init.lua", "sha256": "abc123", "size": 1234 },
                { "kind": "user_supplied", "dest": "sotesx_s.dll",
                  "prompt": "Locate sotesx_s.dll.", "optional": false }
              ]
            }
          ]
        }
      ]
    }"#;

    #[test]
    fn parses_full_schema() {
        let reg = Registry::from_json_str(FULL).unwrap();
        assert_eq!(reg.registry_version, 1);
        assert_eq!(reg.source.name, "sotes-mods");
        let m = reg.find("ennse_voice").unwrap();
        assert_eq!(m.keywords, ["voice", "japanese"]);
        let v = m.latest().unwrap();
        assert_eq!(v.version, "1.0.0");
        assert_eq!(v.min_loader.as_deref(), Some("0.1.0"));
        assert_eq!(v.files.len(), 2);
        match &v.files[0] {
            FileSpec::Download { url, sha256, size, dest } => {
                assert_eq!(url, "https://example/init.lua");
                assert_eq!(sha256, "abc123");
                assert_eq!(*size, Some(1234));
                assert!(v.files[0].dest_is_safe(), "{dest} should be a safe relative dest");
            }
            _ => panic!("file[0] should be a download"),
        }
        assert!(matches!(&v.files[1], FileSpec::UserSupplied { optional: false, .. }));
    }

    #[test]
    fn empty_versions_ok() {
        // The real sotes-mods entries today: listed, not yet released.
        let reg = Registry::from_json_str(
            r#"{ "registry_version": 1, "source": { "name": "s" },
                 "mods": [ { "id": "autoload", "name": "Auto-Load Save" } ] }"#,
        )
        .unwrap();
        assert!(reg.find("autoload").unwrap().latest().is_none());
    }

    #[test]
    fn rejects_future_registry_version() {
        let err = Registry::from_json_str(
            r#"{ "registry_version": 999, "source": { "name": "s" }, "mods": [] }"#,
        )
        .unwrap_err();
        assert!(err.to_string().contains("newer than this launcher supports"));
    }

    #[test]
    fn rejects_duplicate_ids() {
        let err = Registry::from_json_str(
            r#"{ "registry_version": 1, "source": { "name": "s" },
                 "mods": [ { "id": "x", "name": "X" }, { "id": "x", "name": "X2" } ] }"#,
        )
        .unwrap_err();
        assert!(err.to_string().contains("duplicate mod id 'x'"));
    }

    #[test]
    fn rejects_unsafe_dests() {
        let mk = |dest: &str| FileSpec::Download {
            dest: dest.into(),
            url: "u".into(),
            sha256: "h".into(),
            size: None,
        };
        assert!(mk("mods/x/init.lua").dest_is_safe());
        assert!(mk("version.dll").dest_is_safe());
        assert!(!mk("/etc/passwd").dest_is_safe());
        assert!(!mk("..\\..\\windows\\system32\\x.dll").dest_is_safe());
        assert!(!mk("C:\\evil.dll").dest_is_safe());
        assert!(!mk("").dest_is_safe());
    }
}
