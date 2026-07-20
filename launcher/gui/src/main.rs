//! sml-gui — the SotES mod-loader launcher (egui/eframe). A thin GUI over `sml-core`.
//!
//! Everything hangs off a selected **game dir** (a top bar sets it). Views:
//!   - **Installed** — the mods in the game dir; each shows a **Config** button *only if its
//!     `mod.toml` declares `[config]`*, expanding an inline generic editor bound to that mod's
//!     namespaced `oss_mods.cfg` values; plus Remove.
//!   - **Browse** — load a source's `registry.json` (an HTTPS URL or a local path) and list its
//!     mods; **Install** each latest version (download → sha256-verify → place), prompting for any
//!     `user_supplied` files via a native picker. Shows installed/update state per mod.
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
use sml_core::install::{self, HttpFetcher, InstalledManifest};
use sml_core::launcher_cfg::LauncherConfig;
use sml_core::modconfig::{ConfigField, ConfigValue, FieldKind, ModManifest};
use sml_core::registry::{FileSpec, Mod, Registry};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::mpsc;

/// The exe names to probe when auto-detecting a game dir. The unpacked dev exe is tried first (only
/// the scratch copy has it, and it's the one to launch there); a real Steam install falls through to
/// `sotes_en.exe`.
const GAME_EXES: &[&str] = &["sotes-trainer-oss.exe", "sotes_en.exe"];

/// One trusted source's fetched state: its url + the parsed registry (or an error string).
struct SourceState {
    url: String,
    result: Result<Registry, String>,
}

const OK_GREEN: egui::Color32 = egui::Color32::from_rgb(90, 180, 90);
const WARN_ORANGE: egui::Color32 = egui::Color32::from_rgb(210, 150, 60);
const DIM: egui::Color32 = egui::Color32::from_rgb(150, 150, 155);

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Installed,
    Browse,
    Loader,
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

/// A pending install waiting on the user to locate its `user_supplied` files. Opened when Install
/// is clicked on a mod whose latest version declares any `user_supplied` file; it drives a modal
/// that collects a local path per file (native picker or typed) before the download+verify runs.
struct InstallPrompt {
    mod_id: String,
    mod_name: String,
    version: String,
    files: Vec<UserFileReq>,
}

/// One `user_supplied` file the install needs located: its registry `dest` + the prompt to show,
/// whether it may be skipped, and the local path the user picked (empty until chosen).
struct UserFileReq {
    dest: String,
    prompt: String,
    optional: bool,
    path: String,
}

/// A snapshot of one Browse mod row — decouples rendering from the borrowed registry so an Install
/// click can mutate self (manifest/prompt) after the loop.
struct BrowseRow {
    id: String,
    name: String,
    description: String,
    /// Latest version string, or `None` for a not-yet-released mod (no Install button).
    latest: Option<String>,
    /// The latest version declares ≥1 `user_supplied` file (so Install opens the locate prompt).
    needs_user_files: bool,
    /// Installed version from the manifest, or `None` if not installed.
    installed: Option<String>,
}

/// `--smoke [--game DIR] [--registry URL|PATH] [--install MOD_ID] [--user-file DEST=PATH]...`:
/// load the game dir + registry, exercise a config write, optionally install a mod over HTTPS
/// (supplying any `user_supplied` files from `--user-file`, as the modal does), print a `[smoke]`
/// report, and self-close — so the Windows build is verifiable headlessly from WSL.
#[derive(Clone)]
struct Smoke {
    game: Option<String>,
    registry: Option<String>,
    install: Option<String>,
    user_files: BTreeMap<String, PathBuf>,
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

    // the loader's OWN settings ("mod zero"): loader.toml [config] schema -> flat oss_loader.cfg
    loader_schema: ModManifest,
    loader_cfg: KvFile,
    loader_cfg_dirty: bool,

    // proxy / launch
    loader_dll: String,
    game_exe: String,
    proxy: Option<ProxyState>,

    // sources / browse
    cfg: LauncherConfig,        // the persisted launcher settings (game dir, exe, trusted sources)
    new_source: String,         // the "add a source" input
    catalog: Vec<SourceState>,  // every trusted source's fetched registry
    fetch_rx: Option<mpsc::Receiver<Vec<SourceState>>>, // in-flight background fetch
    install_prompt: Option<InstallPrompt>,

    booted: bool,               // first-frame bootstrap ran (load config, autodetect, fetch)
    smoke: Option<Smoke>,
    smoke_done: bool,
}

impl Default for App {
    fn default() -> Self {
        Self {
            view: View::Installed,
            status: "starting…".to_owned(),
            game_path: String::new(),
            game: None,
            manifest: InstalledManifest::default(),
            installed: Vec::new(),
            mods_cfg: KvFile::new(),
            mods_cfg_dirty: false,
            open_config: None,
            loader_schema: ModManifest::from_toml_str(include_str!("../../../loader.toml"))
                .expect("bundled loader.toml is a valid schema (unit-tested in sml-core)"),
            loader_cfg: KvFile::new(),
            loader_cfg_dirty: false,
            loader_dll: "version.dll".to_owned(),
            game_exe: "sotes_en.exe".to_owned(),
            proxy: None,
            cfg: LauncherConfig::default(),
            new_source: String::new(),
            catalog: Vec::new(),
            fetch_rx: None,
            install_prompt: None,
            booted: false,
            smoke: None,
            smoke_done: false,
        }
    }
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        if !self.booted {
            self.booted = true;
            apply_style(ctx);
            if self.smoke.is_none() {
                self.bootstrap();
            }
        }
        if self.smoke.is_some() {
            if !self.smoke_done {
                self.smoke_done = true;
                self.run_smoke();
            }
            ctx.send_viewport_cmd(egui::ViewportCommand::Close);
            return;
        }

        // Drive the background source-fetch to completion (repaint while it's in flight).
        self.poll_fetch();
        if self.fetch_rx.is_some() {
            ctx.request_repaint();
        }

        egui::TopBottomPanel::top("game").show(ctx, |ui| {
            ui.add_space(6.0);
            ui.horizontal(|ui| {
                ui.label("Game dir:");
                // Stretch to fill — the field grows with the window instead of a fixed 360px stub.
                let resp = ui.add(egui::TextEdit::singleline(&mut self.game_path).desired_width(f32::INFINITY));
                if resp.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) {
                    self.use_game_dir();
                }
            });
            ui.horizontal(|ui| {
                if ui.button("Use").clicked() {
                    self.use_game_dir();
                }
                if ui.button("Auto-detect").clicked() {
                    self.autodetect_game_dir();
                }
                match &self.game {
                    Some(_) => ui.colored_label(OK_GREEN, "✓ loaded"),
                    None => ui.colored_label(DIM, "not set"),
                };
            });
            ui.add_space(6.0);
        });

        egui::TopBottomPanel::bottom("status").show(ctx, |ui| {
            ui.add_space(3.0);
            ui.label(egui::RichText::new(&self.status).small().color(DIM));
            ui.add_space(1.0);
        });

        egui::SidePanel::left("nav")
            .resizable(false)
            .exact_width(150.0)
            .show(ctx, |ui| {
                ui.add_space(8.0);
                ui.heading("SotES Loader");
                ui.add_space(2.0);
                ui.separator();
                ui.add_space(2.0);
                for (view, label) in [
                    (View::Installed, "🗁  Installed"),
                    (View::Browse, "🔎  Browse"),
                    (View::Loader, "⚙  Loader"),
                    (View::Launch, "▶  Launch"),
                ] {
                    ui.selectable_value(&mut self.view, view, label);
                }
            });

        egui::CentralPanel::default().show(ctx, |ui| match self.view {
            View::Installed => self.render_installed(ui),
            View::Browse => self.render_browse(ui),
            View::Loader => self.render_loader(ui),
            View::Launch => self.render_launch(ui),
        });

        // A pending user_supplied-file prompt floats above everything (modal-ish).
        self.render_install_prompt(ctx);
    }
}

/// A calm, consistent dark theme — uniform panel/window fills so a window resize shows no seam or
/// flash of a foreign color, and a touch more spacing so rows breathe.
fn apply_style(ctx: &egui::Context) {
    let mut v = egui::Visuals::dark();
    let bg = egui::Color32::from_rgb(24, 25, 28);
    v.panel_fill = bg;
    v.window_fill = bg; // clear_color defaults to window_fill → the resize gap matches the panels
    v.faint_bg_color = egui::Color32::from_rgb(33, 35, 39);
    v.extreme_bg_color = egui::Color32::from_rgb(18, 19, 22);
    let mut style = egui::Style {
        visuals: v,
        ..(*ctx.style()).clone()
    };
    style.spacing.item_spacing = egui::vec2(8.0, 7.0);
    style.spacing.button_padding = egui::vec2(9.0, 4.0);
    ctx.set_style(style);
}

/// Fetch every source's registry (HTTPS or local path), collecting per-source results.
fn fetch_all_sources(sources: Vec<String>) -> Vec<SourceState> {
    let fetcher = HttpFetcher::new();
    sources
        .into_iter()
        .map(|url| {
            let result = if url.starts_with("http://") || url.starts_with("https://") {
                install::fetch_registry(&fetcher, &url)
            } else {
                Registry::from_path(&url)
            }
            .map_err(|e| format!("{e:#}"));
            SourceState { url, result }
        })
        .collect()
}

impl App {
    // ── startup / sources ───────────────────────────────────────────────────
    /// First-frame setup: load the persisted config, auto-detect the game dir if none is saved and
    /// load it, then kick off the background fetch of every trusted source.
    fn bootstrap(&mut self) {
        self.cfg = LauncherConfig::load();
        self.game_exe = self.cfg.game_exe.clone();
        self.loader_dll = self.cfg.loader_dll.clone();
        if self.cfg.game_path.trim().is_empty() {
            self.autodetect_game_dir();
        } else {
            self.game_path = self.cfg.game_path.clone();
            self.use_game_dir();
        }
        self.begin_fetch();
    }

    fn autodetect_game_dir(&mut self) {
        match GameDir::autodetect(GAME_EXES) {
            Some((g, exe)) => {
                self.game_path = g.root.display().to_string();
                self.game_exe = exe;
                self.use_game_dir();
                self.status = format!("auto-detected game dir: {}", self.game_path);
            }
            None => {
                self.status = "no game dir found — set it above (Steam install, or the scratch copy)".into();
            }
        }
    }

    /// Fetch every trusted source in the background (the UI stays responsive; a repaint is requested
    /// each frame until the results land).
    fn begin_fetch(&mut self) {
        let sources = self.cfg.sources.clone();
        if sources.is_empty() {
            self.catalog.clear();
            self.fetch_rx = None;
            self.status = "no sources — add one under Browse".into();
            return;
        }
        let n = sources.len();
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let _ = tx.send(fetch_all_sources(sources));
        });
        self.fetch_rx = Some(rx);
        self.status = format!("fetching {n} source(s)…");
    }

    fn poll_fetch(&mut self) {
        let Some(rx) = &self.fetch_rx else { return };
        let Ok(catalog) = rx.try_recv() else { return };
        let ok = catalog.iter().filter(|s| s.result.is_ok()).count();
        let mods: usize =
            catalog.iter().filter_map(|s| s.result.as_ref().ok()).map(|r| r.mods.len()).sum();
        self.catalog = catalog;
        self.fetch_rx = None;
        self.status = format!("{ok}/{} source(s) loaded — {mods} mod(s) available", self.catalog.len());
    }

    fn add_source(&mut self, url: &str) {
        if self.cfg.add_source(url) {
            let _ = self.cfg.save();
            self.begin_fetch();
        }
    }
    fn remove_source(&mut self, url: &str) {
        self.cfg.remove_source(url);
        let _ = self.cfg.save();
        self.begin_fetch();
    }

    /// The latest version of `id` offered by any trusted source (for update detection).
    fn latest_available(&self, id: &str) -> Option<String> {
        self.catalog
            .iter()
            .filter_map(|s| s.result.as_ref().ok())
            .find_map(|reg| reg.find(id).and_then(|m| m.latest()).map(|v| v.version.clone()))
    }

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
        self.loader_cfg = KvFile::load(g.oss_loader_cfg()).unwrap_or_default();
        self.loader_cfg_dirty = false;
        self.open_config = None;
        self.game = Some(g);
        self.refresh_installed();
        self.proxy = self.detect_proxy();
        // Persist the chosen dir + exe so the next launch opens straight to it.
        self.cfg.game_path = self.game_path.clone();
        self.cfg.game_exe = self.game_exe.clone();
        let _ = self.cfg.save();
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

    // ── the loader's own settings ("mod zero") — render loader.toml [config] -> oss_loader.cfg ──
    fn render_loader(&mut self, ui: &mut egui::Ui) {
        ui.heading("Loader settings");
        ui.label(
            egui::RichText::new(
                "The loader's own oss_loader.cfg (the loader as \"mod zero\"). Voice + launcher settings apply at the next launch.",
            )
            .small()
            .color(DIM),
        );
        ui.add_space(6.0);
        if self.game.is_none() {
            ui.colored_label(WARN_ORANGE, "set a game dir above to edit the loader settings");
            return;
        }
        // Owned snapshot of the (static) schema so the render loop borrows only loader_cfg from self.
        let fields = self.loader_schema.config.clone();
        egui::ScrollArea::vertical().show(ui, |ui| {
            for f in &fields {
                render_field(ui, f, "", &mut self.loader_cfg, &mut self.loader_cfg_dirty);
                if let Some(d) = &f.description {
                    ui.label(egui::RichText::new(d).small().color(DIM));
                }
                ui.add_space(6.0);
            }
        });
        ui.separator();
        ui.horizontal(|ui| {
            ui.add_enabled_ui(self.loader_cfg_dirty, |ui| {
                if ui.button("Save").clicked() {
                    self.save_loader_cfg();
                }
            });
            if self.loader_cfg_dirty {
                ui.label(egui::RichText::new("unsaved").small().color(egui::Color32::from_rgb(200, 140, 40)));
            }
        });
    }

    fn save_loader_cfg(&mut self) {
        let Some(g) = &self.game else { return };
        match self.loader_cfg.write_oss_loader(g.oss_loader_cfg()) {
            Ok(()) => {
                self.loader_cfg_dirty = false;
                self.status = format!("saved {}", g.oss_loader_cfg().display());
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

        // Snapshot for rendering so we can freely mutate self.mods_cfg / open_config below. `update`
        // is the newer version a trusted source offers (None if up to date / unreleased / no source).
        let rows: Vec<(String, String, bool, Vec<ConfigField>, Option<String>)> = self
            .installed
            .iter()
            .map(|e| {
                let update = self.latest_available(&e.id).filter(|v| v != &e.version);
                (e.id.clone(), e.title(), e.has_config(), e.config().to_vec(), update)
            })
            .collect();

        let mut toggle: Option<String> = None;
        let mut remove: Option<String> = None;
        let mut update_click: Option<String> = None;

        egui::ScrollArea::vertical().show(ui, |ui| {
            for (id, title, has_config, fields, update) in &rows {
                ui.horizontal(|ui| {
                    ui.label(egui::RichText::new(title).strong());
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        // An Update button appears (rightmost) only when a source has a newer version.
                        if let Some(v) = update {
                            if ui
                                .button(egui::RichText::new(format!("⬆ Update → v{v}")).color(OK_GREEN))
                                .clicked()
                            {
                                update_click = Some(id.clone());
                            }
                        }
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
        if let Some(id) = update_click {
            self.begin_install(&id); // reuses the Browse install flow (download → verify → place over)
        }
    }

    fn render_browse(&mut self, ui: &mut egui::Ui) {
        ui.heading("Browse");
        ui.add_space(2.0);

        // ── trusted sources: add / remove / status (persisted; auto-fetched on start) ──
        let fetching = self.fetch_rx.is_some();
        egui::CollapsingHeader::new(format!("Sources ({})", self.cfg.sources.len()))
            .default_open(self.cfg.sources.len() != 1 || self.catalog.iter().any(|s| s.result.is_err()))
            .show(ui, |ui| {
                let mut remove: Option<String> = None;
                for s in &self.catalog {
                    ui.horizontal(|ui| {
                        match &s.result {
                            Ok(r) => { ui.colored_label(OK_GREEN, "●").on_hover_text(format!("{} mod(s)", r.mods.len())); }
                            Err(e) => { ui.colored_label(WARN_ORANGE, "▲").on_hover_text(e.clone()); }
                        }
                        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                            if ui.small_button("✕").on_hover_text("remove source").clicked() {
                                remove = Some(s.url.clone());
                            }
                            ui.with_layout(egui::Layout::left_to_right(egui::Align::Center), |ui| {
                                ui.label(egui::RichText::new(&s.url).small());
                            });
                        });
                    });
                }
                if fetching {
                    ui.label(egui::RichText::new("fetching…").small().color(DIM));
                }
                ui.add_space(4.0);
                ui.add(
                    egui::TextEdit::singleline(&mut self.new_source)
                        .desired_width(f32::INFINITY)
                        .hint_text("registry.json URL or local path"),
                );
                ui.horizontal(|ui| {
                    if ui.button("Add source").clicked() {
                        let u = self.new_source.trim().to_owned();
                        if !u.is_empty() {
                            self.add_source(&u);
                            self.new_source.clear();
                        }
                    }
                    if ui.add_enabled(!fetching, egui::Button::new("Refresh all")).clicked() {
                        self.begin_fetch();
                    }
                });
                if let Some(u) = remove {
                    self.remove_source(&u);
                }
            });
        ui.separator();

        if self.game.is_none() {
            ui.colored_label(WARN_ORANGE, "set a game dir above to install mods");
            ui.add_space(2.0);
        }
        if self.catalog.iter().all(|s| s.result.is_err()) {
            ui.label(egui::RichText::new(if fetching { "fetching sources…" } else { "no mods available" }).color(DIM));
            return;
        }

        // Snapshot every source's mod rows (owned) so an Install click can mutate self afterwards.
        let has_game = self.game.is_some();
        let sections: Vec<(String, Vec<BrowseRow>)> = self
            .catalog
            .iter()
            .filter_map(|s| {
                let reg = s.result.as_ref().ok()?;
                let rows = reg
                    .mods
                    .iter()
                    .map(|m| {
                        let latest = m.latest();
                        BrowseRow {
                            id: m.id.clone(),
                            name: m.name.clone(),
                            description: m.description.clone(),
                            latest: latest.map(|v| v.version.clone()),
                            needs_user_files: latest.is_some_and(|v| {
                                v.files.iter().any(|f| matches!(f, FileSpec::UserSupplied { .. }))
                            }),
                            installed: self.manifest.get(&m.id).map(|im| im.version.clone()),
                        }
                    })
                    .collect();
                Some((reg.source.name.clone(), rows))
            })
            .collect();

        let mut install_click: Option<String> = None;
        egui::ScrollArea::vertical().show(ui, |ui| {
            for (name, rows) in &sections {
                ui.add_space(4.0);
                ui.label(egui::RichText::new(name).strong().color(DIM));
                ui.add_space(2.0);
                for row in rows {
                    render_browse_row(ui, row, has_game, &mut install_click);
                }
            }
        });

        if let Some(id) = install_click {
            self.begin_install(&id);
        }
    }

    /// The mod `id` and its owning registry, searched across every loaded source (first match wins).
    fn catalog_find(&self, id: &str) -> Option<(&Registry, &Mod)> {
        self.catalog
            .iter()
            .filter_map(|s| s.result.as_ref().ok())
            .find_map(|reg| reg.find(id).map(|m| (reg, m)))
    }

    // ── install (Browse → download + verify + place) ─────────────────────────
    /// Clicked Install/Update on a Browse row. If the latest version declares `user_supplied`
    /// files, open the prompt to locate them; otherwise install straight away.
    fn begin_install(&mut self, id: &str) {
        // Pull owned data out so the `self.catalog` borrow ends before we touch self again.
        let extracted = {
            let Some((_, m)) = self.catalog_find(id) else {
                self.status = format!("{id} is not in any loaded source");
                return;
            };
            let Some(v) = m.latest() else {
                self.status = format!("{id} has no release to install");
                return;
            };
            let files: Vec<UserFileReq> = v
                .files
                .iter()
                .filter_map(|f| match f {
                    FileSpec::UserSupplied { dest, prompt, optional, .. } => Some(UserFileReq {
                        dest: dest.clone(),
                        prompt: prompt.clone(),
                        optional: *optional,
                        path: String::new(),
                    }),
                    _ => None,
                })
                .collect();
            (m.name.clone(), v.version.clone(), files)
        };
        let (mod_name, version, files) = extracted;
        if files.is_empty() {
            self.do_install(id, &version, BTreeMap::new());
        } else {
            self.install_prompt = Some(InstallPrompt { mod_id: id.to_string(), mod_name, version, files });
        }
    }

    /// Download + verify + place `id`'s `version` (falls back to latest) into the game dir, copying
    /// any located `user_supplied` files, then record it in the manifest and refresh Installed.
    fn do_install(&mut self, id: &str, version: &str, user_files: BTreeMap<String, PathBuf>) {
        let Some(g) = self.game.clone() else {
            self.status = "set a game dir before installing".into();
            return;
        };
        // Clone the mod + version out so the catalog borrow ends before we mutate self.manifest.
        let picked = {
            let Some((reg, m)) = self.catalog_find(id) else {
                self.status = format!("{id} is not in any loaded source");
                return;
            };
            let Some(v) = m.versions.iter().find(|v| v.version == version).or_else(|| m.latest()) else {
                self.status = format!("{id} has no release to install");
                return;
            };
            (m.clone(), v.clone(), reg.source.name.clone())
        };
        let (m, v, source) = picked;
        // ureq is blocking; mod files are small so a synchronous install keeps the UI simple
        // (async download + progress is a v0.2 nicety — see DESIGN).
        let fetcher = HttpFetcher::new();
        match install::install_version(&g, &fetcher, Some(&source), &m, &v, &user_files, &mut self.manifest) {
            Ok(()) => {
                let n = self.manifest.get(id).map_or(0, |e| e.files.len());
                self.refresh_installed();
                self.status = format!("installed {} v{} — {n} file(s) placed", m.id, v.version);
            }
            Err(e) => self.status = format!("install failed: {e:#}"),
        }
    }

    /// The modal collecting `user_supplied` file locations for a pending install. Taken out and put
    /// back each frame unless the user cancels or confirms (then the install runs).
    fn render_install_prompt(&mut self, ctx: &egui::Context) {
        let Some(mut prompt) = self.install_prompt.take() else {
            return;
        };
        enum Act {
            Keep,
            Cancel,
            Install,
        }
        let mut act = Act::Keep;
        egui::Window::new(format!("Install {}  ·  v{}", prompt.mod_name, prompt.version))
            .collapsible(false)
            .resizable(false)
            .anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0])
            .show(ctx, |ui| {
                ui.label("This mod needs files you supply from your own copy of the game:");
                ui.add_space(6.0);
                for f in &mut prompt.files {
                    let tag = if f.optional { "optional" } else { "required" };
                    ui.label(egui::RichText::new(format!("{}  ({tag} → {})", f.prompt, f.dest)).small());
                    ui.horizontal(|ui| {
                        ui.add(egui::TextEdit::singleline(&mut f.path).desired_width(360.0).hint_text("path to the file"));
                        if ui.button("Browse…").clicked() {
                            if let Some(p) = rfd::FileDialog::new().pick_file() {
                                f.path = p.display().to_string();
                            }
                        }
                    });
                    ui.add_space(4.0);
                }
                ui.separator();
                let ready = prompt.files.iter().all(|f| f.optional || !f.path.trim().is_empty());
                ui.horizontal(|ui| {
                    if ui.add_enabled(ready, egui::Button::new("Install")).clicked() {
                        act = Act::Install;
                    }
                    if ui.button("Cancel").clicked() {
                        act = Act::Cancel;
                    }
                    if !ready {
                        ui.label(egui::RichText::new("locate the required file(s) to continue").small().weak());
                    }
                });
            });
        match act {
            Act::Keep => self.install_prompt = Some(prompt),
            Act::Cancel => self.status = "install cancelled".into(),
            Act::Install => {
                let user_files: BTreeMap<String, PathBuf> = prompt
                    .files
                    .iter()
                    .filter(|f| !f.path.trim().is_empty())
                    .map(|f| (f.dest.clone(), PathBuf::from(f.path.trim())))
                    .collect();
                self.do_install(&prompt.mod_id, &prompt.version, user_files);
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
            ui.add(egui::TextEdit::singleline(&mut self.game_exe).desired_width(f32::INFINITY));
        });
        let exe_ok = g.root.join(&self.game_exe).is_file();
        if !exe_ok {
            ui.colored_label(WARN_ORANGE, format!("‘{}’ not found in the game dir", self.game_exe));
        }
        ui.add_space(6.0);
        let btn = egui::Button::new(egui::RichText::new("▶  Launch game").size(15.0).strong())
            .min_size(egui::vec2(150.0, 30.0))
            .fill(egui::Color32::from_rgb(38, 86, 48));
        if ui.add_enabled(exe_ok, btn).clicked() {
            self.launch_game(&g);
        }
    }

    fn launch_game(&mut self, g: &GameDir) {
        let exe = g.root.join(&self.game_exe);
        if !exe.is_file() {
            self.status = format!("game exe not found: {}", exe.display());
            return;
        }
        // Best-effort proxy install — NEVER block the launch on it. The dir usually already has our
        // version.dll (from a prior install / stage), and a user without our bundled loader should
        // still be able to launch. (This is why the button seemed dead before: the pre-launch install
        // failed because the launcher has no reference loader dll, and that aborted the whole launch.)
        let note = match self.proxy_installer().install(g) {
            Ok(s) => {
                self.proxy = Some(s);
                "loader installed".to_owned()
            }
            Err(_) => {
                self.proxy = self.detect_proxy();
                match self.proxy {
                    Some(ProxyState::Installed) => "loader present".to_owned(),
                    _ => "no loader — launching vanilla".to_owned(),
                }
            }
        };
        self.cfg.game_exe = self.game_exe.clone();
        let _ = self.cfg.save();
        match std::process::Command::new(&exe).current_dir(&g.root).spawn() {
            Ok(_child) => self.status = format!("launched {} — {note}", exe.display()),
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
            // Fetch synchronously into the catalog (headless — no background thread needed).
            self.cfg.sources = vec![reg.clone()];
            self.catalog = fetch_all_sources(self.cfg.sources.clone());
            match self.catalog.first().map(|s| &s.result) {
                Some(Ok(r)) => {
                    let ids: Vec<&str> = r.mods.iter().map(|m| m.id.as_str()).collect();
                    out += &format!("[smoke] registry OK: {} ({} mods: {})\n", r.source.name, r.mods.len(), ids.join(", "));
                }
                Some(Err(e)) => out += &format!("[smoke] registry FAIL: {e}\n"),
                None => {}
            }
        }

        // Exercise the real install pipeline (HttpFetcher → download → sha256-verify → place →
        // manifest) — the exact path the Browse Install button runs. Needs --registry + --game.
        if let Some(id) = &s.install {
            self.do_install(id, "", s.user_files.clone());
            out += &format!("[smoke] install {id}: {}\n", self.status);
            if let Some(g) = &self.game {
                if let Ok(txt) = std::fs::read_to_string(InstalledManifest::path(g)) {
                    for line in txt.lines() {
                        out += &format!("[smoke]   {line}\n");
                    }
                }
            }
        }

        out += "[smoke] window + GL + egui init OK (reached update); closing\n";
        print!("{out}");
        let _ = std::io::stdout().flush();
    }
}

/// Render one Browse mod row: name + version, the install/update control (right), then description.
fn render_browse_row(ui: &mut egui::Ui, row: &BrowseRow, has_game: bool, click: &mut Option<String>) {
    ui.horizontal(|ui| {
        ui.label(egui::RichText::new(&row.name).strong());
        match &row.latest {
            Some(v) => ui.label(egui::RichText::new(format!("v{v}")).color(DIM)),
            None => ui.label(egui::RichText::new("no release yet").italics().color(DIM)),
        };
        ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
            browse_action(ui, row, has_game, click);
        });
    });
    if !row.description.is_empty() {
        ui.label(egui::RichText::new(&row.description).small().color(DIM));
    }
    ui.add_space(6.0);
    ui.separator();
}

/// The right-aligned install control for one Browse row, keyed on release + installed state. Sets
/// `click` to the mod id when an Install/Update button is pressed (applied after the render loop).
fn browse_action(ui: &mut egui::Ui, row: &BrowseRow, has_game: bool, click: &mut Option<String>) {
    let Some(latest) = &row.latest else {
        return; // no release yet — the "no release yet" label at the left says it all
    };
    match &row.installed {
        // Installed at the latest — nothing to do.
        Some(cur) if cur == latest => {
            ui.label(egui::RichText::new("installed ✓").color(egui::Color32::from_rgb(60, 160, 60)));
        }
        // Installed at an older/other version — offer update-to-latest (installs over).
        Some(cur) => {
            if ui.add_enabled(has_game, egui::Button::new(format!("Update → v{latest}"))).clicked() {
                *click = Some(row.id.clone());
            }
            ui.label(egui::RichText::new(format!("v{cur} installed")).small().weak());
        }
        // Not installed — `Install…` (ellipsis) hints the user_supplied locate prompt.
        None => {
            let label = if row.needs_user_files { "Install…" } else { "Install" };
            if ui.add_enabled(has_game, egui::Button::new(label)).clicked() {
                *click = Some(row.id.clone());
            }
        }
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
    // All `--user-file DEST=PATH` pairs (repeatable), mapping a user_supplied dest to a local path.
    let user_files: BTreeMap<String, PathBuf> = args
        .windows(2)
        .filter(|w| w[0] == "--user-file")
        .filter_map(|w| w[1].split_once('=').map(|(d, p)| (d.to_string(), PathBuf::from(p))))
        .collect();
    let smoke = args.iter().any(|a| a == "--smoke").then(|| Smoke {
        game: arg_after("--game"),
        registry: arg_after("--registry"),
        install: arg_after("--install"),
        user_files,
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
