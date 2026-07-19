//! End-to-end pipeline smoke: fetch the LIVE sotes-mods registry over HTTPS, install a mod's
//! latest version into a temp game dir (download → sha256-verify → place), and report. A
//! successful run proves the registry's pinned hashes match the GitHub-served bytes.
//!
//!   cargo run -p sml-core --example install_demo --features http -- [mod_id] [registry_url]
//!
//! Needs the `http` feature. NOTE: build/run this on the **Windows target** — `ring`'s asm
//! doesn't link with this NixOS host's rust-lld, but the windows-gnu cross-build links fine
//! (mingw), which is the real target anyway:
//!   cargo build -p sml-core --example install_demo --features http --target x86_64-pc-windows-gnu

#[cfg(not(feature = "http"))]
fn main() {
    eprintln!("install_demo needs the `http` feature: cargo run -p sml-core --example install_demo --features http");
}

#[cfg(feature = "http")]
fn main() -> anyhow::Result<()> {
    use anyhow::anyhow;
    use sml_core::gamedir::GameDir;
    use sml_core::install::{self, HttpFetcher, InstalledManifest};
    use std::collections::BTreeMap;

    let mut args = std::env::args().skip(1);
    let mod_id = args.next().unwrap_or_else(|| "autoload".into());
    let url = args.next().unwrap_or_else(|| {
        "https://raw.githubusercontent.com/Francesco149/sotes-mods/master/registry.json".into()
    });

    let fetcher = HttpFetcher::new();
    println!("→ fetch registry: {url}");
    let reg = install::fetch_registry(&fetcher, &url)?;
    println!("  source \"{}\" (registry v{}), {} mod(s)", reg.source.name, reg.registry_version, reg.mods.len());

    let m = reg.find(&mod_id).ok_or_else(|| anyhow!("no mod '{mod_id}' in the registry"))?;
    let v = m.latest().ok_or_else(|| anyhow!("'{mod_id}' has no release yet"))?;
    println!("→ install {} v{} ({} file(s), over HTTPS, sha256-verified)", m.id, v.version, v.files.len());

    let dir = std::env::temp_dir().join(format!("sml-install-demo-{}", std::process::id()));
    std::fs::create_dir_all(&dir)?;
    let game = GameDir::new(&dir);
    let mut manifest = InstalledManifest::default();

    install::install_version(&game, &fetcher, Some(&reg.source.name), m, v, &BTreeMap::new(), &mut manifest)?;

    println!("✓ installed into {}", dir.display());
    for dest in &manifest.get(&mod_id).unwrap().files {
        let abs = game.resolve_dest(dest).unwrap();
        println!("    {dest}  ({} bytes) — hash matched", std::fs::metadata(&abs)?.len());
    }
    println!("  oss_installed.json:");
    for line in std::fs::read_to_string(InstalledManifest::path(&game))?.lines() {
        println!("    {line}");
    }

    let _ = std::fs::remove_dir_all(&dir);
    Ok(())
}
