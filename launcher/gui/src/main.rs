//! sml-gui — the SotES mod-loader launcher (egui/eframe). A thin GUI over `sml-core`.
//!
//! Everything hangs off a selected **game dir** (a top bar sets it). Views:
//!   - **Installed** — the mods in the game dir; each shows a **Config** button *only if its
//!     `mod.toml` declares `[config]`*, expanding an inline generic editor bound to that mod's
//!     namespaced `oss_mods.cfg` values; plus Remove.
//!   - **Browse** — load a source's `registry.json` and list its mods (install wiring is next).
//!   - **Launch** — the `version.dll`↔`realver.dll` proxy state + install/repair/uninstall, and
//!     launch the game.
//!
//! The config editor is deliberately **per-mod** (a button on the mod itself), not a standalone
//! path-picker — that matches how you actually configure an installed mod.

// Release builds are windowed (no console); debug keeps the console for panics/logs + `--smoke`.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use sml_core::cfg::KvFile;
use sml_core::gamedir::{GameDir, ProxyInstaller, ProxyState};
use sml_core::install::{self, InstalledManifest};
use sml_core::modconfig::{ConfigField, ConfigValue, FieldKind, ModManifest};
use sml_core::registry::Registry;
use std::path::PathBuf;

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Installed,
    Browse,
    Launch,
}

/// One installed mod: version (from the installed manifest) + its parsed `mod.toml` (for the
/// per-mod config schema; `None` if the mod ships no manifest).
struct Installed {
    id: String,
    version: String,
    manifest: Option<ModManifest>,
}

impl Installed {
    fn config(&self) -> &[ConfigField] {
        self.manifest.as_ref().map_or(&[], |m| &m.config)
    }
    fn has_config(&self) -> bool {
        !self.config().is_empty()
    }
    fn title(&self) -> String {
        let name = self.manifest.as_ref().map_or(self.id.as_str(), |m| m.name.as_str());
        format!("{name}  ·  v{}", self.version)
    }
}

/// `--smoke [--game DIR] [--registry PATH]`: load the game dir + registry, exercise a config
/// write, print a `[smoke]` report, and self-close — so the Windows build is verifiable from WSL.
#[derive(Clone)]
struct Smoke {
    game: Option<String>,
    registry: Option<String>,
}

struct App {
    view: View,
    status: String,

    // game dir + its derived state
    game_path: String,
    game: Option<GameDir>,
    manifest: InstalledManifest,
    installed: Vec<Installed>,
    mods_cfg: KvFile,
    mods_cfg_dirty: bool,
    open_config: Option<String>,

    // proxy / launch
    loader_dll: String,
    game_exe: String,
    proxy: Option<ProxyState>,

    // browse
    registry_path: String,
    registry: Option<Result<Registry, String>>,

    smoke: Option<Smoke>,
    smoke_done: bool,
}

impl Default for App {
    fn default() -> Self {
        Self {
            view: View::Installed,
            status: "select a game dir".to_owned(),
            // Dev defaults matching the staged test layout / the unpacked EN-SE dev dir.
            game_path: r"C:\oss-ennse-voice-repro\stock".to_owned(),
            game: None,
            manifest: InstalledManifest::default(),
            installed: Vec::new(),
            mods_cfg: KvFile::new(),
            mods_cfg_dirty: false,
            open_config: None,
            loader_dll: "version.dll".to_owned(),
            game_exe: "sotes-trainer-oss.exe".to_owned(),
            proxy: None,
            registry_path: "../sotes-mods/registry.json".to_owned(),
            registry: None,
            smoke: None,
            smoke_done: false,
        }
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if self.smoke.is_some() {
            if !self.smoke_done {
                self.smoke_done = true;
                self.run_smoke();
            }
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
        }

        egui::TopBottomPanel::top("game").show(ctx, |ui| {
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.label("Game dir:");
                ui.add(egui::TextEdit::singleline(&mut self.game_path).desired_width(360.0));
                if ui.button("Use").clicked() {
                    self.use_game_dir();
                }
                match &self.game {
                    Some(_) => ui.label(egui::RichText::new("✓ loaded").color(egui::Color32::from_rgb(60, 160, 60))),
                    None => ui.label(egui::RichText::new("not set").weak()),
                };
            });
            ui.add_space(4.0);
        });

        egui::TopBottomPanel::bottom("status").show(ctx, |ui| {
            ui.add_space(2.0);
            ui.label(egui::RichText::new(&self.status).small().weak());
        });

        egui::SidePanel::left("nav").exact_width(140.0).show(ctx, |ui| {
            ui.add_space(6.0);
            ui.heading("SotES Loader");
            ui.separator();
            ui.selectable_value(&mut self.view, View::Installed, "Installed");
            ui.selectable_value(&mut self.view, View::Browse, "Browse");
            ui.selectable_value(&mut self.view, View::Launch, "Launch");
        });

        egui::CentralPanel::default().show(ctx, |ui| match self.view {
            View::Installed => self.render_installed(ui),
            View::Browse => self.render_browse(ui),
            View::Launch => self.render_launch(ui),
        });
    }
}

impl App {
    // ── game dir ────────────────────────────────────────────────────────────
    fn use_game_dir(&mut self) {
        let g = GameDir::new(PathBuf::from(&self.game_path));
        if !g.exists() {
            self.game = None;
            self.installed.clear();
            self.status = format!("game dir not found: {}", self.game_path);
            return;
        }
        self.manifest = InstalledManifest::load(&g).unwrap_or_default();
        self.mods_cfg = KvFile::load(g.oss_mods_cfg()).unwrap_or_default();
        self.mods_cfg_dirty = false;
        self.open_config = None;
        self.game = Some(g);
        self.refresh_installed();
        self.proxy = self.detect_proxy();
        self.status = format!("using {} — {} mod(s) installed", self.game_path, self.installed.len());
    }

    fn refresh_installed(&mut self) {
        self.installed.clear();
        let Some(g) = &self.game else { return };
        for (id, im) in &self.manifest.mods {
            let mp = g.mods_dir().join(id).join("mod.toml");
            self.installed.push(Installed {
                id: id.clone(),
                version: im.version.clone(),
                manifest: ModManifest::from_path(&mp).ok(),
            });
        }
    }

    fn save_mods_cfg(&mut self) {
        let Some(g) = &self.game else { return };
        match self.mods_cfg.write_oss_mods(g.oss_mods_cfg()) {
            Ok(()) => {
                self.mods_cfg_dirty = false;
                self.status = format!("saved {}", g.oss_mods_cfg().display());
            }
            Err(e) => self.status = format!("save failed: {e:#}"),
        }
    }

    fn remove_mod(&mut self, id: &str) {
        let Some(g) = self.game.clone() else { return };
        match install::remove_mod(&g, id, &mut self.manifest) {
            Ok(()) => {
                self.mods_cfg.remove_prefix(&format!("{id}."));
                let _ = self.mods_cfg.write_oss_mods(g.oss_mods_cfg());
                if self.open_config.as_deref() == Some(id) {
                    self.open_config = None;
                }
                self.refresh_installed();
                self.status = format!("removed {id}");
            }
            Err(e) => self.status = format!("remove failed: {e:#}"),
        }
    }

    // ── proxy ───────────────────────────────────────────────────────────────
    fn proxy_installer(&self) -> ProxyInstaller {
        ProxyInstaller::windows(PathBuf::from(&self.loader_dll))
    }
    fn detect_proxy(&self) -> Option<ProxyState> {
        self.game.as_ref().and_then(|g| self.proxy_installer().detect(g).ok())
    }

    // ── views ───────────────────────────────────────────────────────────────
    fn render_installed(&mut self, ui: &mut egui::Ui) {
        ui.heading("Installed");
        ui.separator();
        if self.game.is_none() {
            ui.label("Set a game dir above to see its installed mods.");
            return;
        }
        if self.installed.is_empty() {
            ui.label("No mods installed (no oss_installed.json entries). Install some from Browse.");
            return;
        }

        // Snapshot for rendering so we can freely mutate self.mods_cfg / open_config below.
        let rows: Vec<(String, String, bool, Vec<ConfigField>)> = self
            .installed
            .iter()
            .map(|e| (e.id.clone(), e.title(), e.has_config(), e.config().to_vec()))
            .collect();

        let mut toggle: Option<String> = None;
        let mut remove: Option<String> = None;

        egui::ScrollArea::vertical().show(ui, |ui| {
            for (id, title, has_config, fields) in &rows {
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new(title).strong());
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if ui.button("Remove").clicked() {
                            remove = Some(id.clone());
                        }
                        // The Config button exists ONLY for mods that declare [config].
                        if *has_config {
                            let open = self.open_config.as_deref() == Some(id);
                            if ui.selectable_label(open, "⚙ Config").clicked() {
                                toggle = Some(id.clone());
                            }
                        }
                    });
                });

                if self.open_config.as_deref() == Some(id) {
                    ui.indent(id, |ui| {
                        for f in fields {
                            render_field(ui, f, id, &mut self.mods_cfg, &mut self.mods_cfg_dirty);
                        }
                        ui.add_space(2.0);
                        ui.horizontal(|ui| {
                            ui.add_enabled_ui(self.mods_cfg_dirty, |ui| {
                                if ui.button("Save").clicked() {
                                    self.save_mods_cfg();
                                }
                            });
                            if self.mods_cfg_dirty {
                                ui.label(egui::RichText::new("unsaved").small().color(egui::Color32::from_rgb(200, 140, 40)));
                            }
                        });
                    });
                }
                ui.separator();
            }
        });

        if let Some(id) = toggle {
            self.open_config = if self.open_config.as_deref() == Some(id.as_str()) { None } else { Some(id) };
        }
        if let Some(id) = remove {
            self.remove_mod(&id);
        }
    }

    fn render_browse(&mut self, ui: &mut egui::Ui) {
        ui.heading("Browse");
        ui.horizontal(|ui| {
            ui.label("registry.json:");
            ui.text_edit_singleline(&mut self.registry_path);
            if ui.button("Load").clicked() {
                self.registry = Some(Registry::from_path(&self.registry_path).map_err(|e| format!("{e:#}")));
            }
        });
        ui.separator();
        match &self.registry {
            None => {
                ui.label("Load a source's registry.json to browse its mods.");
            }
            Some(Err(e)) => {
                ui.colored_label(egui::Color32::RED, e);
            }
            Some(Ok(reg)) => {
                ui.label(egui::RichText::new(format!("{}  (registry v{})", reg.source.name, reg.registry_version)).strong());
                ui.add_space(4.0);
                egui::ScrollArea::vertical().show(ui, |ui| {
                    for m in &reg.mods {
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(&m.name).strong());
                            match m.latest() {
                                Some(v) => ui.label(egui::RichText::new(format!("v{}", v.version)).weak()),
                                None => ui.label(egui::RichText::new("no release yet").italics().weak()),
                            };
                        });
                        if !m.description.is_empty() {
                            ui.label(egui::RichText::new(&m.description).small());
                        }
                        ui.add_space(6.0);
                    }
                });
            }
        }
    }

    fn render_launch(&mut self, ui: &mut egui::Ui) {
        ui.heading("Launch");
        ui.separator();
        let Some(g) = self.game.clone() else {
            ui.label("Set a game dir above first.");
            return;
        };

        ui.horizontal(|ui| {
            ui.label("Loader version.dll:");
            ui.text_edit_singleline(&mut self.loader_dll);
        });
        let state = self.proxy.unwrap_or(ProxyState::NotInstalled);
        let (txt, col) = match state {
            ProxyState::Installed => ("proxy installed ✓", egui::Color32::from_rgb(60, 160, 60)),
            ProxyState::NotInstalled => ("proxy not installed", egui::Color32::GRAY),
            ProxyState::NeedsRepair => ("needs repair (realver.dll missing)", egui::Color32::from_rgb(200, 140, 40)),
            ProxyState::Foreign => ("a foreign version.dll is present", egui::Color32::from_rgb(200, 140, 40)),
        };
        ui.label(egui::RichText::new(txt).color(col));

        ui.horizontal(|ui| {
            if ui.button("Install / repair proxy").clicked() {
                match self.proxy_installer().install(&g) {
                    Ok(s) => { self.proxy = Some(s); self.status = "proxy installed".into(); }
                    Err(e) => self.status = format!("proxy install failed: {e:#}"),
                }
            }
            if ui.button("Uninstall proxy").clicked() {
                match self.proxy_installer().uninstall(&g) {
                    Ok(()) => { self.proxy = self.detect_proxy(); self.status = "proxy removed".into(); }
                    Err(e) => self.status = format!("proxy uninstall failed: {e:#}"),
                }
            }
            if ui.button("Refresh").clicked() {
                self.proxy = self.detect_proxy();
            }
        });

        ui.separator();
        ui.horizontal(|ui| {
            ui.label("Game exe:");
            ui.text_edit_singleline(&mut self.game_exe);
        });
        if ui.button("▶ Launch game").clicked() {
            self.launch_game(&g);
        }
    }

    fn launch_game(&mut self, g: &GameDir) {
        // Ensure the loader is present, then start the exe detached (spawn + drop = no wait).
        if let Err(e) = self.proxy_installer().install(g) {
            self.status = format!("could not install proxy before launch: {e:#}");
            return;
        }
        self.proxy = self.detect_proxy();
        let exe = g.root.join(&self.game_exe);
        match std::process::Command::new(&exe).current_dir(&g.root).spawn() {
            Ok(_child) => self.status = format!("launched {}", exe.display()),
            Err(e) => self.status = format!("launch failed ({}): {e}", exe.display()),
        }
    }

    // ── smoke ────────────────────────────────────────────────────────────────
    fn run_smoke(&mut self) {
        use std::io::Write;
        let s = self.smoke.clone().expect("smoke");
        let mut out = String::from("[smoke] sml-gui up on Windows — exercising sml-core in-process\n");

        if let Some(game) = &s.game {
            self.game_path = game.clone();
            self.use_game_dir();
            out += &format!(
                "[smoke] game dir: {} (proxy: {:?}); {} installed mod(s)\n",
                self.game_path,
                self.proxy,
                self.installed.len()
            );
            for e in &self.installed {
                out += &format!(
                    "[smoke]   {} v{} (config: {})\n",
                    e.id,
                    e.version,
                    if e.has_config() { format!("{} setting(s)", e.config().len()) } else { "none".into() }
                );
            }
            // Exercise a config write on the first config-bearing mod, then persist.
            let edit = self.installed.iter().find(|e| e.has_config()).map(|e| {
                let f = e.config().iter().find(|f| matches!(f.kind, FieldKind::Int | FieldKind::Float)).or_else(|| e.config().first()).unwrap();
                (e.id.clone(), f.clone())
            });
            if let Some((id, f)) = edit {
                let v = match f.kind {
                    FieldKind::Bool => ConfigValue::Bool(true),
                    FieldKind::Int => ConfigValue::Int((f.min.unwrap_or(0.0) + f.max.unwrap_or(4.0)) as i64 / 2),
                    FieldKind::Float => ConfigValue::Float((f.min.unwrap_or(0.0) + f.max.unwrap_or(4.0)) / 2.0),
                    FieldKind::Text => ConfigValue::Text("smoke".into()),
                };
                f.store(&mut self.mods_cfg, &id, v);
                self.save_mods_cfg();
                out += &format!("[smoke] wrote {}.{} -> {}\n", id, f.key, self.status);
                for line in self.mods_cfg.to_oss_mods_string().lines() {
                    out += &format!("[smoke]   {line}\n");
                }
            }
        }

        if let Some(reg) = &s.registry {
            match Registry::from_path(reg) {
                Ok(r) => {
                    let ids: Vec<&str> = r.mods.iter().map(|m| m.id.as_str()).collect();
                    out += &format!("[smoke] registry OK: {} ({} mods: {})\n", r.source.name, r.mods.len(), ids.join(", "));
                }
                Err(e) => out += &format!("[smoke] registry FAIL: {e:#}\n"),
            }
        }

        out += "[smoke] window + GL + egui init OK (reached update); closing\n";
        print!("{out}");
        let _ = std::io::stdout().flush();
    }
}

/// Render one `[config]` field as a widget bound to `kv` (namespaced `<mod_id>.<key>`); flips
/// `dirty` when the value changes. A free fn so it borrows only the two fields it needs.
fn render_field(ui: &mut egui::Ui, f: &ConfigField, mod_id: &str, kv: &mut KvFile, dirty: &mut bool) {
    let label = f.label.clone().unwrap_or_else(|| f.key.clone());
    let cur = f.current(kv, mod_id);
    match f.kind {
        FieldKind::Bool => {
            let mut b = cur.as_bool();
            if ui.checkbox(&mut b, &label).changed() {
                f.store(kv, mod_id, ConfigValue::Bool(b));
                *dirty = true;
            }
        }
        FieldKind::Int => {
            let lo = f.min.unwrap_or(0.0) as i64;
            let hi = f.max.unwrap_or(100.0) as i64;
            let mut n = cur.as_f64() as i64;
            if ui.add(egui::Slider::new(&mut n, lo..=hi).text(&label)).changed() {
                f.store(kv, mod_id, ConfigValue::Int(n));
                *dirty = true;
            }
        }
        FieldKind::Float => {
            let lo = f.min.unwrap_or(0.0);
            let hi = f.max.unwrap_or(1.0);
            let mut x = cur.as_f64();
            if ui.add(egui::Slider::new(&mut x, lo..=hi).text(&label)).changed() {
                f.store(kv, mod_id, ConfigValue::Float(x));
                *dirty = true;
            }
        }
        FieldKind::Text => {
            let mut sv = cur.into_text();
            ui.horizontal(|ui| {
                ui.label(&label);
                if ui.text_edit_singleline(&mut sv).changed() {
                    f.store(kv, mod_id, ConfigValue::Text(sv));
                    *dirty = true;
                }
            });
        }
    }
    if let Some(desc) = &f.description {
        ui.label(egui::RichText::new(desc).small().weak());
    }
}

fn main() -> eframe::Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let arg_after =
        |flag: &str| args.iter().position(|a| a == flag).and_then(|i| args.get(i + 1)).cloned();
    let smoke = args.iter().any(|a| a == "--smoke").then(|| Smoke {
        game: arg_after("--game"),
        registry: arg_after("--registry"),
    });

    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([820.0, 600.0]),
        ..Default::default()
    };
    eframe::run_native(
        "SotES Mod Loader",
        options,
        Box::new(move |_cc| {
            let mut app = App::default();
            app.smoke = smoke;
            Ok(Box::new(app))
        }),
    )
}
