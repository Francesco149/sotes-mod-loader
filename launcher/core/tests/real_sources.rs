//! Integration test against the REAL sibling files when they're present (they are in the dev
//! tree: `../sotes-mods` and `examples/config_demo`). Skips gracefully in a bare checkout so it
//! never fails a CI that only has this repo — the unit tests carry the hermetic coverage.

use std::path::PathBuf;

/// Resolve a path relative to this crate (`launcher/core`).
fn rel(p: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(p)
}

#[test]
fn parses_the_real_sotes_mods_registry() {
    let path = rel("../../../sotes-mods/registry.json");
    if !path.exists() {
        eprintln!("skip: {} not present", path.display());
        return;
    }
    let reg = sml_core::registry::Registry::from_path(&path).expect("real registry should parse");
    assert_eq!(reg.source.name, "sotes-mods");
    // The three mods are listed for discoverability before their first loader release.
    for id in ["autoload", "ennse_voice", "sotes_trainer"] {
        assert!(reg.find(id).is_some(), "missing mod {id}");
    }
}

#[test]
fn parses_the_real_config_demo_manifest() {
    let path = rel("../../examples/config_demo/mod.toml");
    if !path.exists() {
        eprintln!("skip: {} not present", path.display());
        return;
    }
    let m = sml_core::modconfig::ModManifest::from_path(&path)
        .expect("real config_demo mod.toml should parse");
    assert_eq!(m.id, "config_demo");
    let keys: Vec<&str> = m.config.iter().map(|f| f.key.as_str()).collect();
    assert_eq!(keys, ["enabled", "speed", "ratio"]);
}

#[test]
fn parses_the_real_autoload_manifest() {
    let path = rel("../../../sotes-mods/mods/autoload/mod.toml");
    if !path.exists() {
        eprintln!("skip: {} not present", path.display());
        return;
    }
    let m = sml_core::modconfig::ModManifest::from_path(&path).expect("real autoload mod.toml");
    assert_eq!(m.id, "autoload");
    let slot = m.field("slot").expect("autoload has a `slot` setting");
    assert_eq!(slot.default, sml_core::modconfig::ConfigValue::Int(-1));
}
