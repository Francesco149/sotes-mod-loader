//! The two flat `key=value` files beside the game exe, exactly as the loader reads them
//! (`core/config.c`):
//!
//!   - `oss_loader.cfg` — the LOADER's own settings (ddraw, ui, keepactive, …). Hand-commented;
//!     the loader treats it read-only. We parse it (to show current state) and preserve comments
//!     on write. (Schema-driven "mod zero" editing is DESIGN open-decision #2.)
//!   - `oss_mods.cfg` — per-mod VALUES, keys namespaced `<modid>.<setting>`. Machine-managed:
//!     the loader (`mod.config.set`) and the launcher rewrite it wholesale, so it carries only a
//!     fixed 2-line header. The launcher's writer is **byte-identical** to `config.c`'s so a file
//!     the launcher writes and one the loader writes don't spuriously differ.
//!
//! Parsing matches `config.c`'s `read_kv`: trim each line, skip blank / `#` / `;` comment lines,
//! split on the FIRST `=`, trim key and value.

use std::path::Path;

/// The exact header `config.c` (`write_mods_file`) emits — replicated so launcher- and
/// loader-written `oss_mods.cfg` files are byte-identical.
pub const OSS_MODS_HEADER: &str = "\
# oss_mods.cfg — per-mod config values (machine-managed; edit via the launcher or in-game).\n\
# key = <modid>.<setting>; the settings + defaults are declared in each mod's mod.toml [config].\n";

/// An ordered flat key=value store. Order is preserved (append-on-new) so rewrites stay stable.
#[derive(Debug, Default, Clone)]
pub struct KvFile {
    entries: Vec<(String, String)>,
}

impl KvFile {
    pub fn new() -> Self {
        Self::default()
    }

    /// Parse flat key=value text the way `config.c read_kv` does: trim; skip blank and `#`/`;`
    /// comment lines; split on the first `=`; trim key and value.
    pub fn parse(text: &str) -> Self {
        let mut entries = Vec::new();
        for line in text.lines() {
            let s = line.trim();
            if s.is_empty() || s.starts_with('#') || s.starts_with(';') {
                continue;
            }
            let Some((k, v)) = s.split_once('=') else {
                continue;
            };
            entries.push((k.trim().to_string(), v.trim().to_string()));
        }
        Self { entries }
    }

    /// Load a file; a missing file is an empty store (the loader treats absent = defaults).
    pub fn load(path: impl AsRef<Path>) -> std::io::Result<Self> {
        match std::fs::read_to_string(path) {
            Ok(text) => Ok(Self::parse(&text)),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Self::new()),
            Err(e) => Err(e),
        }
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.entries
            .iter()
            .find(|(k, _)| k == key)
            .map(|(_, v)| v.as_str())
    }

    /// `oss_loader.cfg` int lookup, matching `config_get_int` (`atoi`: leading integer, else def).
    pub fn get_int(&self, key: &str, def: i64) -> i64 {
        self.get(key).map(atoi).unwrap_or(def)
    }

    /// Set (replace in place) or append a key, preserving order.
    pub fn set(&mut self, key: &str, val: &str) {
        if let Some(e) = self.entries.iter_mut().find(|(k, _)| k == key) {
            e.1 = val.to_string();
        } else {
            self.entries.push((key.to_string(), val.to_string()));
        }
    }

    /// Remove a key; returns whether it was present.
    pub fn remove(&mut self, key: &str) -> bool {
        let before = self.entries.len();
        self.entries.retain(|(k, _)| k != key);
        self.entries.len() != before
    }

    /// Drop every key starting with `prefix` — used to clear a mod's values on uninstall
    /// (`remove_prefix("<id>.")`).
    pub fn remove_prefix(&mut self, prefix: &str) {
        self.entries.retain(|(k, _)| !k.starts_with(prefix));
    }

    pub fn iter(&self) -> impl Iterator<Item = (&str, &str)> {
        self.entries.iter().map(|(k, v)| (k.as_str(), v.as_str()))
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Serialize as `oss_mods.cfg` — the fixed header then `key = val` lines, exactly like
    /// `config.c write_mods_file` (`fprintf(f, "%s = %s\n", ...)`).
    pub fn to_oss_mods_string(&self) -> String {
        let mut s = String::from(OSS_MODS_HEADER);
        for (k, v) in &self.entries {
            s.push_str(k);
            s.push_str(" = ");
            s.push_str(v);
            s.push('\n');
        }
        s
    }

    /// Write `oss_mods.cfg` (byte-identical to the loader's writer).
    pub fn write_oss_mods(&self, path: impl AsRef<Path>) -> std::io::Result<()> {
        std::fs::write(path, self.to_oss_mods_string())
    }
}

/// `atoi`-style: the leading (optionally signed) integer, or 0 — matches `config_get_int`'s C `atoi`.
fn atoi(s: &str) -> i64 {
    let s = s.trim_start();
    let (sign, rest) = match s.strip_prefix('-') {
        Some(r) => (-1, r),
        None => (1, s.strip_prefix('+').unwrap_or(s)),
    };
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    sign * digits.parse::<i64>().unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_matches_loader_read_kv() {
        let text = "\
# a comment
; another comment

ui = 1
  windowed=0
autoload.slot = 3
weird = a = b = c
";
        let kv = KvFile::parse(text);
        assert_eq!(kv.get("ui"), Some("1"));
        assert_eq!(kv.get("windowed"), Some("0")); // leading ws + no spaces around '='
        assert_eq!(kv.get("autoload.slot"), Some("3"));
        assert_eq!(kv.get("weird"), Some("a = b = c")); // split on FIRST '=' only
        assert_eq!(kv.get("missing"), None);
        assert_eq!(kv.len(), 4);
    }

    #[test]
    fn get_int_like_atoi() {
        let kv = KvFile::parse("a = 5\nb = -1\nc = 12abc\nd = xyz\n");
        assert_eq!(kv.get_int("a", 9), 5);
        assert_eq!(kv.get_int("b", 9), -1);
        assert_eq!(kv.get_int("c", 9), 12); // trailing junk ignored, like atoi
        assert_eq!(kv.get_int("d", 9), 0); // unparseable -> 0 (atoi), NOT the default
        assert_eq!(kv.get_int("missing", 9), 9); // absent -> default
    }

    #[test]
    fn set_replaces_in_place_and_appends() {
        let mut kv = KvFile::parse("a.x = 1\nb.y = 2\n");
        kv.set("a.x", "9"); // replace, keep position
        kv.set("c.z", "3"); // append
        let got: Vec<_> = kv.iter().collect();
        assert_eq!(got, vec![("a.x", "9"), ("b.y", "2"), ("c.z", "3")]);
    }

    #[test]
    fn remove_prefix_clears_a_mods_values() {
        let mut kv = KvFile::parse("autoload.slot = 3\nautoload.foo = 1\nother.k = 2\n");
        kv.remove_prefix("autoload.");
        let got: Vec<_> = kv.iter().collect();
        assert_eq!(got, vec![("other.k", "2")]);
    }

    #[test]
    fn write_oss_mods_is_byte_identical_to_loader() {
        // Exactly what config.c's write_mods_file would produce for one value.
        let mut kv = KvFile::new();
        kv.set("autoload.slot", "3");
        let expected = "\
# oss_mods.cfg — per-mod config values (machine-managed; edit via the launcher or in-game).
# key = <modid>.<setting>; the settings + defaults are declared in each mod's mod.toml [config].
autoload.slot = 3
";
        assert_eq!(kv.to_oss_mods_string(), expected);
    }

    #[test]
    fn round_trips_through_write_and_parse() {
        let mut kv = KvFile::new();
        kv.set("autoload.slot", "3");
        kv.set("config_demo.enabled", "true");
        kv.set("config_demo.ratio", "1.5");
        let reparsed = KvFile::parse(&kv.to_oss_mods_string());
        let a: Vec<_> = kv.iter().collect();
        let b: Vec<_> = reparsed.iter().collect();
        assert_eq!(a, b);
    }
}
