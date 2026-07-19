//! sml-core — the SotES mod-loader launcher's engine (plumbing; GUI-free).
//!
//! Modules mirror `launcher/DESIGN.md` "Architecture (modules)". Everything here is pure
//! logic — unit-tested and runnable on Linux — so the eframe GUI (`sml-gui`) can stay a
//! thin layer on top, and the Windows cross-toolchain is only needed for that GUI.
//!
//! | module | DESIGN module | does |
//! |---|---|---|
//! | [`registry`] | registry | parse / validate / merge a source's `registry.json` (the pkg-mgr contract) |
//! | [`verify`]   | install  | sha256 integrity — every download checked against the trusted registry |
//! | [`modconfig`]| config   | parse a mod's `mod.toml [config]` SCHEMA (the generic settings editor) |
//! | [`cfg`]      | gamedir  | the loader's flat `oss_mods.cfg` / `oss_loader.cfg`, byte-compatible |
//! | [`gamedir`]  | gamedir  | locate/validate the install + the `version.dll`↔`realver.dll` proxy swap |
//!
//! Still to land (next commits): HTTPS fetch + download (into the [`install`](self) flow), the
//! installed-state manifest, and launching the game detached.

pub mod cfg;
pub mod gamedir;
pub mod modconfig;
pub mod registry;
pub mod verify;
