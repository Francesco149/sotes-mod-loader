//! Parse a mod's `mod.toml` — the `[mod]` / `[loader]` metadata and the `[config]` settings
//! SCHEMA the launcher renders as a generic editor (checkbox / int-slider / float-slider / text).
//! Field VALUES (overrides of each field's `default`) live in `oss_mods.cfg`, namespaced
//! `<id>.<key>` — the SAME store the loader reads ([`crate::cfg`]).
//!
//! **Byte-compat contract.** Value (de)serialization mirrors the loader's `mod.config` exactly
//! (`core/lua_host.c` CONFIG_LUA) so files round-trip identically:
//!   - write ([`ConfigValue::to_cfg_string`], = Lua `cfg.set`): bool → `"true"`/`"false"`; int →
//!     decimal; float → shortest form (Lua `tostring`); string → raw, unquoted.
//!   - read ([`ConfigValue::from_cfg_string`], = Lua `coerce`): bool ← `"true"` **or** `"1"`; int ←
//!     `floor(tonumber)`; float ← `tonumber`.
//!   - clamp ([`ConfigField::clamp`], = Lua `cfg.set`): int/float clamped to `min`/`max`, int floored.

use anyhow::{bail, Context, Result};
use serde::Deserialize;

/// The `[loader] api` a mod is assumed to target when it omits one (MOD-FORMAT "Versioning").
pub const DEFAULT_API: &str = "1.0";

/// A parsed `mod.toml`: identity + the ordered `[config]` schema.
#[derive(Debug, Clone)]
pub struct ModManifest {
    pub id: String,
    pub name: String,
    pub version: String,
    pub description: String,
    pub authors: Vec<String>,
    pub target_games: Vec<String>,
    /// `[loader] api` (MAJOR.MINOR) — the mod API this mod targets; `"1.0"` if absent.
    pub api: String,
    /// `[loader] min_version` — the loader host floor, or empty if unset.
    pub min_version: String,
    /// `[config.*]` fields, in declaration order (the editor shows them as authored).
    pub config: Vec<ConfigField>,
}

/// One `[config.<key>]` setting: enough for a generic editor to render + bind a widget.
#[derive(Debug, Clone)]
pub struct ConfigField {
    pub key: String,
    pub kind: FieldKind,
    pub default: ConfigValue,
    /// Slider bounds (numeric fields). Stored as f64 for both int + float.
    pub min: Option<f64>,
    pub max: Option<f64>,
    pub label: Option<String>,
    pub description: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldKind {
    Bool,
    Int,
    Float,
    Text,
}

/// A typed setting value. `Text` for `type = "string"`.
#[derive(Debug, Clone, PartialEq)]
pub enum ConfigValue {
    Bool(bool),
    Int(i64),
    Float(f64),
    Text(String),
}

impl ConfigValue {
    /// Serialize for `oss_mods.cfg` — identical to the loader's `cfg.set` (CONFIG_LUA line 172):
    /// bool → `"true"`/`"false"`; else Lua `tostring` (int decimal, float shortest, string raw).
    pub fn to_cfg_string(&self) -> String {
        match self {
            ConfigValue::Bool(b) => if *b { "true" } else { "false" }.to_string(),
            ConfigValue::Int(i) => i.to_string(),
            // Rust's `{}` for f64 is the shortest round-tripping form (1.0 → "1", 1.5 → "1.5"),
            // matching Lua `tostring` for the small, bounded values a config slider produces.
            ConfigValue::Float(f) => format!("{f}"),
            ConfigValue::Text(s) => s.clone(),
        }
    }

    /// Coerce a stored string to `kind` — identical to the loader's `coerce` (CONFIG_LUA 141-143).
    pub fn from_cfg_string(kind: FieldKind, raw: &str) -> ConfigValue {
        match kind {
            // Lua: `raw=='true' or raw=='1'`.
            FieldKind::Bool => ConfigValue::Bool(raw == "true" || raw == "1"),
            // Lua: `math.floor(tonumber(raw) or 0)`.
            FieldKind::Int => ConfigValue::Int(parse_num(raw).unwrap_or(0.0).floor() as i64),
            // Lua: `tonumber(raw) or 0`.
            FieldKind::Float => ConfigValue::Float(parse_num(raw).unwrap_or(0.0)),
            FieldKind::Text => ConfigValue::Text(raw.to_string()),
        }
    }

    pub fn as_bool(&self) -> bool {
        match self {
            ConfigValue::Bool(b) => *b,
            ConfigValue::Int(i) => *i != 0,
            ConfigValue::Float(f) => *f != 0.0,
            ConfigValue::Text(s) => s == "true" || s == "1",
        }
    }

    pub fn as_f64(&self) -> f64 {
        match self {
            ConfigValue::Bool(b) => (*b) as i64 as f64,
            ConfigValue::Int(i) => *i as f64,
            ConfigValue::Float(f) => *f,
            ConfigValue::Text(s) => parse_num(s).unwrap_or(0.0),
        }
    }

    pub fn into_text(self) -> String {
        match self {
            ConfigValue::Text(s) => s,
            other => other.to_cfg_string(),
        }
    }
}

impl ConfigField {
    /// Clamp a value into this field's kind + `min`/`max` — identical to the loader's `cfg.set`
    /// (CONFIG_LUA 166-171): numeric fields clamp to bounds, int floors.
    pub fn clamp(&self, v: ConfigValue) -> ConfigValue {
        match self.kind {
            FieldKind::Bool => ConfigValue::Bool(v.as_bool()),
            FieldKind::Text => ConfigValue::Text(v.into_text()),
            FieldKind::Int | FieldKind::Float => {
                let mut f = v.as_f64();
                if let Some(mn) = self.min {
                    if f < mn {
                        f = mn;
                    }
                }
                if let Some(mx) = self.max {
                    if f > mx {
                        f = mx;
                    }
                }
                if self.kind == FieldKind::Int {
                    ConfigValue::Int(f.floor() as i64)
                } else {
                    ConfigValue::Float(f)
                }
            }
        }
    }

    /// This field's current value from `oss_mods.cfg` (`<id>.<key>`), or its `default` if unset.
    pub fn current(&self, kv: &crate::cfg::KvFile, mod_id: &str) -> ConfigValue {
        match kv.get(&nskey(mod_id, &self.key)) {
            Some(raw) => ConfigValue::from_cfg_string(self.kind, raw),
            None => self.default.clone(),
        }
    }

    /// Store `value` (clamped, loader-compatible string form) under `<id>.<key>`.
    pub fn store(&self, kv: &mut crate::cfg::KvFile, mod_id: &str, value: ConfigValue) {
        let v = self.clamp(value);
        kv.set(&nskey(mod_id, &self.key), &v.to_cfg_string());
    }
}

/// The `oss_mods.cfg` key for a mod's setting: `<modid>.<key>`.
pub fn nskey(mod_id: &str, key: &str) -> String {
    format!("{mod_id}.{key}")
}

// ── parse ────────────────────────────────────────────────────────────────────

impl ModManifest {
    pub fn from_toml_str(s: &str) -> Result<Self> {
        let raw: RawManifest = toml::from_str(s).context("parsing mod.toml")?;
        let mut config = Vec::with_capacity(raw.config.len());
        // `toml` is built with `preserve_order`, so this iterates in declaration order.
        for (key, val) in &raw.config {
            let tbl = val
                .as_table()
                .with_context(|| format!("[config.{key}] must be a table"))?;
            config.push(parse_field(key, tbl)?);
        }
        let loader = raw.loader.unwrap_or_default();
        Ok(ModManifest {
            id: raw.meta.id,
            name: raw.meta.name,
            version: raw.meta.version,
            description: raw.meta.description,
            authors: raw.meta.authors,
            target_games: raw.meta.target_games,
            api: loader.api.unwrap_or_else(|| DEFAULT_API.to_string()),
            min_version: loader.min_version.unwrap_or_default(),
            config,
        })
    }

    pub fn from_path(path: impl AsRef<std::path::Path>) -> Result<Self> {
        let path = path.as_ref();
        let s = std::fs::read_to_string(path)
            .with_context(|| format!("reading {}", path.display()))?;
        Self::from_toml_str(&s)
    }

    pub fn field(&self, key: &str) -> Option<&ConfigField> {
        self.config.iter().find(|f| f.key == key)
    }
}

#[derive(Deserialize)]
struct RawManifest {
    #[serde(rename = "mod")]
    meta: RawMeta,
    #[serde(default)]
    loader: Option<RawLoader>,
    #[serde(default)]
    config: toml::Table,
}

#[derive(Deserialize)]
struct RawMeta {
    id: String,
    name: String,
    #[serde(default)]
    version: String,
    #[serde(default)]
    description: String,
    #[serde(default)]
    authors: Vec<String>,
    #[serde(default)]
    target_games: Vec<String>,
}

#[derive(Deserialize, Default)]
struct RawLoader {
    #[serde(default)]
    api: Option<String>,
    #[serde(default)]
    min_version: Option<String>,
}

fn parse_field(key: &str, tbl: &toml::Table) -> Result<ConfigField> {
    let type_ = tbl
        .get("type")
        .and_then(|v| v.as_str())
        .with_context(|| format!("[config.{key}] missing string `type`"))?;
    let kind = match type_ {
        "bool" => FieldKind::Bool,
        "int" => FieldKind::Int,
        "float" => FieldKind::Float,
        "string" => FieldKind::Text,
        other => bail!("[config.{key}] unknown type '{other}' (want bool/int/float/string)"),
    };
    let dv = tbl
        .get("default")
        .with_context(|| format!("[config.{key}] missing `default`"))?;
    let default = match kind {
        FieldKind::Bool => ConfigValue::Bool(
            dv.as_bool()
                .with_context(|| format!("[config.{key}] default must be a bool"))?,
        ),
        FieldKind::Int => ConfigValue::Int(
            dv.as_integer()
                .with_context(|| format!("[config.{key}] default must be an int"))?,
        ),
        FieldKind::Float => ConfigValue::Float(
            num_f64(dv).with_context(|| format!("[config.{key}] default must be a number"))?,
        ),
        FieldKind::Text => ConfigValue::Text(
            dv.as_str()
                .with_context(|| format!("[config.{key}] default must be a string"))?
                .to_string(),
        ),
    };
    Ok(ConfigField {
        key: key.to_string(),
        kind,
        default,
        min: tbl.get("min").and_then(num_f64),
        max: tbl.get("max").and_then(num_f64),
        label: tbl.get("label").and_then(|v| v.as_str()).map(String::from),
        description: tbl
            .get("description")
            .and_then(|v| v.as_str())
            .map(String::from),
    })
}

/// A TOML number (int or float) as f64.
fn num_f64(v: &toml::Value) -> Option<f64> {
    match v {
        toml::Value::Integer(i) => Some(*i as f64),
        toml::Value::Float(f) => Some(*f),
        _ => None,
    }
}

/// Parse a stored string as a number the way Lua `tonumber` would for config values (a plain
/// decimal). Returns `None` for non-numbers (callers fall back to 0, matching `... or 0`).
fn parse_num(s: &str) -> Option<f64> {
    s.trim().parse::<f64>().ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cfg::KvFile;

    // The examples/config_demo/mod.toml schema (bool + int + float, with bounds + labels).
    const CONFIG_DEMO: &str = r#"
[mod]
id          = "config_demo"
name        = "Config Demo"
version     = "1.0.0"
description = "demo"
authors     = ["sotes-mod-loader"]
target_games = ["sotes_en"]

[loader]
api         = "1.0"
min_version = "0.1.0"

[config.enabled]
type    = "bool"
default = true
label   = "Enabled"
description = "Master toggle for the demo."

[config.speed]
type    = "int"
default = 5
min     = 0
max     = 20
label   = "Speed"

[config.ratio]
type    = "float"
default = 1.0
min     = 0.0
max     = 4.0
label   = "Ratio"
"#;

    #[test]
    fn parses_schema_in_declaration_order() {
        let m = ModManifest::from_toml_str(CONFIG_DEMO).unwrap();
        assert_eq!(m.id, "config_demo");
        assert_eq!(m.api, "1.0");
        let keys: Vec<&str> = m.config.iter().map(|f| f.key.as_str()).collect();
        assert_eq!(keys, ["enabled", "speed", "ratio"]); // declaration order preserved

        let enabled = m.field("enabled").unwrap();
        assert_eq!(enabled.kind, FieldKind::Bool);
        assert_eq!(enabled.default, ConfigValue::Bool(true));
        assert_eq!(enabled.label.as_deref(), Some("Enabled"));

        let speed = m.field("speed").unwrap();
        assert_eq!(speed.kind, FieldKind::Int);
        assert_eq!(speed.default, ConfigValue::Int(5));
        assert_eq!((speed.min, speed.max), (Some(0.0), Some(20.0)));

        let ratio = m.field("ratio").unwrap();
        assert_eq!(ratio.kind, FieldKind::Float);
        assert_eq!(ratio.default, ConfigValue::Float(1.0));
    }

    #[test]
    fn api_defaults_to_1_0_when_absent() {
        let m = ModManifest::from_toml_str(
            r#"[mod]
id = "x"
name = "X""#,
        )
        .unwrap();
        assert_eq!(m.api, DEFAULT_API);
        assert!(m.config.is_empty());
    }

    #[test]
    fn autoload_negative_default() {
        // The real autoload mod: slot default -1, range -1..20.
        let m = ModManifest::from_toml_str(
            r#"[mod]
id = "autoload"
name = "Auto-Load Save"
[config.slot]
type = "int"
default = -1
min = -1
max = 20"#,
        )
        .unwrap();
        let slot = m.field("slot").unwrap();
        assert_eq!(slot.default, ConfigValue::Int(-1));
        assert_eq!((slot.min, slot.max), (Some(-1.0), Some(20.0)));
    }

    #[test]
    fn serialization_matches_loader() {
        // write side (cfg.set): bool -> true/false, int decimal, float shortest, string raw.
        assert_eq!(ConfigValue::Bool(true).to_cfg_string(), "true");
        assert_eq!(ConfigValue::Bool(false).to_cfg_string(), "false");
        assert_eq!(ConfigValue::Int(5).to_cfg_string(), "5");
        assert_eq!(ConfigValue::Int(-1).to_cfg_string(), "-1");
        assert_eq!(ConfigValue::Float(1.0).to_cfg_string(), "1"); // Lua tostring(1.0) == "1"
        assert_eq!(ConfigValue::Float(1.5).to_cfg_string(), "1.5");
        assert_eq!(ConfigValue::Text("hi".into()).to_cfg_string(), "hi");
    }

    #[test]
    fn coercion_matches_loader() {
        use ConfigValue::*;
        // read side (coerce): bool accepts "true" OR "1"; int floors tonumber; float tonumber.
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Bool, "true"), Bool(true));
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Bool, "1"), Bool(true));
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Bool, "false"), Bool(false));
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Bool, "0"), Bool(false));
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Int, "5.9"), Int(5)); // floor
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Int, "-1"), Int(-1));
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Int, "junk"), Int(0)); // tonumber or 0
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Float, "1.5"), Float(1.5));
        assert_eq!(ConfigValue::from_cfg_string(FieldKind::Text, "x"), Text("x".into()));
    }

    #[test]
    fn clamp_matches_loader() {
        let m = ModManifest::from_toml_str(CONFIG_DEMO).unwrap();
        let speed = m.field("speed").unwrap(); // int 0..20
        assert_eq!(speed.clamp(ConfigValue::Int(25)), ConfigValue::Int(20));
        assert_eq!(speed.clamp(ConfigValue::Int(-3)), ConfigValue::Int(0));
        assert_eq!(speed.clamp(ConfigValue::Float(3.7)), ConfigValue::Int(3)); // clamp then floor
        let ratio = m.field("ratio").unwrap(); // float 0..4
        assert_eq!(ratio.clamp(ConfigValue::Float(9.0)), ConfigValue::Float(4.0));
        assert_eq!(ratio.clamp(ConfigValue::Float(-1.0)), ConfigValue::Float(0.0));
    }

    #[test]
    fn binding_round_trips_through_oss_mods_cfg() {
        let m = ModManifest::from_toml_str(CONFIG_DEMO).unwrap();
        let speed = m.field("speed").unwrap();
        let mut kv = KvFile::new();

        // unset -> default
        assert_eq!(speed.current(&kv, "config_demo"), ConfigValue::Int(5));

        // store (out of range) -> clamped + namespaced key written in loader form
        speed.store(&mut kv, "config_demo", ConfigValue::Int(99));
        assert_eq!(kv.get("config_demo.speed"), Some("20"));
        assert_eq!(speed.current(&kv, "config_demo"), ConfigValue::Int(20));
    }
}
