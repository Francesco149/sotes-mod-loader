//! Dev tool + smoke: parse a real `registry.json` and (optionally) a `mod.toml`, print a
//! summary. Doubles as a hand-verification against `../sotes-mods` and `examples/config_demo`.
//!
//!   cargo run -p sml-core --example inspect -- ../../sotes-mods/registry.json \
//!                                              ../examples/config_demo/mod.toml
//!
//! (paths relative to the workspace root, `launcher/`).

use anyhow::Result;
use sml_core::modconfig::ModManifest;
use sml_core::registry::{FileSpec, Registry};

fn main() -> Result<()> {
    let mut args = std::env::args().skip(1);
    let reg_path = args.next().unwrap_or_else(|| "../../sotes-mods/registry.json".into());
    let toml_path = args.next();

    let reg = Registry::from_path(&reg_path)?;
    println!(
        "registry v{}  source={:?}  ({} mod(s))",
        reg.registry_version,
        reg.source.name,
        reg.mods.len()
    );
    for m in &reg.mods {
        match m.latest() {
            Some(v) => {
                println!("  - {:<16} {}  (latest {})", m.id, m.name, v.version);
                for f in &v.files {
                    let (tag, dest, ok) = match f {
                        FileSpec::Download { dest, .. } => ("download", dest, f.dest_is_safe()),
                        FileSpec::UserSupplied { dest, .. } => ("user", dest, f.dest_is_safe()),
                    };
                    println!("      [{tag}] {dest}{}", if ok { "" } else { "  !! UNSAFE DEST" });
                }
            }
            None => println!("  - {:<16} {}  (no release yet)", m.id, m.name),
        }
    }

    if let Some(p) = toml_path {
        let m = ModManifest::from_path(&p)?;
        println!("\nmod.toml: {} v{}  api={}  ({} setting(s))", m.id, m.version, m.api, m.config.len());
        for f in &m.config {
            let bounds = match (f.min, f.max) {
                (Some(a), Some(b)) => format!("  [{a}..{b}]"),
                _ => String::new(),
            };
            println!(
                "  - {:<12} {:?}  default={}{bounds}  {}",
                f.key,
                f.kind,
                f.default.to_cfg_string(),
                f.label.as_deref().unwrap_or("")
            );
        }
    }
    Ok(())
}
