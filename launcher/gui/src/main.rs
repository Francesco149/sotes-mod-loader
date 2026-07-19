//! sml-gui — the SotES mod-loader launcher (egui/eframe). A thin GUI over `sml-core`: it turns
//! the plumbing (registry parse, `mod.toml [config]` schema, `oss_mods.cfg` I/O) into the views
//! DESIGN.md lists — Sources · Browse · Installed · Config · Launch.
//!
//! This is the FIRST GUI slice: Browse (load a source's `registry.json` and list its mods) and
//! Config (render an installed mod's `[config]` as a generic editable table, live-previewing the
//! `oss_mods.cfg` it writes) are wired to real `sml-core`. Install / proxy / launch land next.

// Release builds are windowed (no console); debug keeps the console for panics/logs.
#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

use eframe::egui;
use sml_core::cfg::KvFile;
use sml_core::modconfig::{ConfigValue, FieldKind, ModManifest};
use sml_core::registry::Registry;

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Sources,
    Browse,
    Installed,
    Config,
    Launch,
}

struct SmlApp {
    view: View,

    // Browse: a source's parsed registry (Err carries the message to show).
    registry_path: String,
    registry: Option<Result<Registry, String>>,

    // Config: a mod's parsed manifest + the in-memory oss_mods.cfg being edited.
    manifest_path: String,
    manifest: Option<Result<ModManifest, String>>,
    values: KvFile,
}

impl Default for SmlApp {
    fn default() -> Self {
        Self {
            view: View::Browse,
            // Dev defaults (relative to the workspace root, launcher/) so the slice is runnable
            // against the real repos; the real launcher will discover these via added sources.
            registry_path: "../sotes-mods/registry.json".to_owned(),
            registry: None,
            manifest_path: "../examples/config_demo/mod.toml".to_owned(),
            manifest: None,
            values: KvFile::new(),
        }
    }
}

impl eframe::App for SmlApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::SidePanel::left("nav").exact_width(160.0).show(ctx, |ui| {
            ui.add_space(6.0);
            ui.heading("SotES Loader");
            ui.label(egui::RichText::new("mod launcher").weak().small());
            ui.separator();
            for (v, label) in [
                (View::Sources, "Sources"),
                (View::Browse, "Browse"),
                (View::Installed, "Installed"),
                (View::Config, "Config"),
                (View::Launch, "Launch"),
            ] {
                ui.selectable_value(&mut self.view, v, label);
            }
        });

        egui::CentralPanel::default().show(ctx, |ui| match self.view {
            View::Browse => self.browse(ui),
            View::Config => self.config(ui),
            View::Sources => placeholder(ui, "Sources", "Add / remove git-repo mod sources (v0.2)."),
            View::Installed => {
                placeholder(ui, "Installed", "Installed mods, updates, and removal (needs the install module).")
            }
            View::Launch => placeholder(
                ui,
                "Launch",
                "Install/repair the version.dll proxy, then start the game detached.",
            ),
        });
    }
}

impl SmlApp {
    /// Browse a source: load its `registry.json` via `sml-core` and list the mods.
    fn browse(&mut self, ui: &mut egui::Ui) {
        ui.heading("Browse");
        ui.horizontal(|ui| {
            ui.label("registry.json:");
            ui.text_edit_singleline(&mut self.registry_path);
            if ui.button("Load").clicked() {
                self.registry =
                    Some(Registry::from_path(&self.registry_path).map_err(|e| format!("{e:#}")));
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
                ui.label(
                    egui::RichText::new(format!("{}  (registry v{})", reg.source.name, reg.registry_version))
                        .strong(),
                );
                if !reg.source.description.is_empty() {
                    ui.label(egui::RichText::new(&reg.source.description).weak());
                }
                ui.add_space(4.0);
                egui::ScrollArea::vertical().show(ui, |ui| {
                    for m in &reg.mods {
                        let ver = m.latest().map(|v| v.version.as_str());
                        ui.horizontal(|ui| {
                            ui.label(egui::RichText::new(&m.name).strong());
                            match ver {
                                Some(v) => ui.label(egui::RichText::new(format!("v{v}")).weak()),
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

    /// The generic config editor: parse a mod's `mod.toml [config]` and render each field as a
    /// widget bound to the in-memory `oss_mods.cfg`, previewing what gets written.
    fn config(&mut self, ui: &mut egui::Ui) {
        ui.heading("Config");
        ui.horizontal(|ui| {
            ui.label("mod.toml:");
            ui.text_edit_singleline(&mut self.manifest_path);
            if ui.button("Load").clicked() {
                self.manifest =
                    Some(ModManifest::from_path(&self.manifest_path).map_err(|e| format!("{e:#}")));
                self.values = KvFile::new(); // fresh edit buffer for the newly loaded mod
            }
        });
        ui.separator();

        // Snapshot the identity + schema so we can freely mutate `self.values` while rendering.
        let (mod_id, fields) = match &self.manifest {
            None => {
                ui.label("Load a mod's mod.toml to edit its settings.");
                return;
            }
            Some(Err(e)) => {
                ui.colored_label(egui::Color32::RED, e);
                return;
            }
            Some(Ok(m)) => (m.id.clone(), m.config.clone()),
        };

        if fields.is_empty() {
            ui.label("This mod declares no [config] settings.");
            return;
        }

        for f in &fields {
            let label = f.label.clone().unwrap_or_else(|| f.key.clone());
            let cur = f.current(&self.values, &mod_id);
            match f.kind {
                FieldKind::Bool => {
                    let mut b = cur.as_bool();
                    if ui.checkbox(&mut b, &label).changed() {
                        f.store(&mut self.values, &mod_id, ConfigValue::Bool(b));
                    }
                }
                FieldKind::Int => {
                    let lo = f.min.unwrap_or(0.0) as i64;
                    let hi = f.max.unwrap_or(100.0) as i64;
                    let mut n = cur.as_f64() as i64;
                    if ui.add(egui::Slider::new(&mut n, lo..=hi).text(&label)).changed() {
                        f.store(&mut self.values, &mod_id, ConfigValue::Int(n));
                    }
                }
                FieldKind::Float => {
                    let lo = f.min.unwrap_or(0.0);
                    let hi = f.max.unwrap_or(1.0);
                    let mut x = cur.as_f64();
                    if ui.add(egui::Slider::new(&mut x, lo..=hi).text(&label)).changed() {
                        f.store(&mut self.values, &mod_id, ConfigValue::Float(x));
                    }
                }
                FieldKind::Text => {
                    let mut s = cur.into_text();
                    ui.horizontal(|ui| {
                        ui.label(&label);
                        if ui.text_edit_singleline(&mut s).changed() {
                            f.store(&mut self.values, &mod_id, ConfigValue::Text(s));
                        }
                    });
                }
            }
            if let Some(desc) = &f.description {
                ui.label(egui::RichText::new(desc).small().weak());
            }
            ui.add_space(4.0);
        }

        ui.separator();
        ui.label(egui::RichText::new("oss_mods.cfg preview").strong());
        let preview = if self.values.is_empty() {
            "# (all defaults — nothing overridden yet)".to_owned()
        } else {
            self.values.to_oss_mods_string()
        };
        ui.add(
            egui::TextEdit::multiline(&mut preview.as_str())
                .code_editor()
                .desired_width(f32::INFINITY),
        );
    }
}

fn placeholder(ui: &mut egui::Ui, title: &str, note: &str) {
    ui.heading(title);
    ui.separator();
    ui.label(egui::RichText::new(note).weak());
}

fn main() -> eframe::Result<()> {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default().with_inner_size([820.0, 560.0]),
        ..Default::default()
    };
    eframe::run_native(
        "SotES Mod Loader",
        options,
        Box::new(|_cc| Ok(Box::new(SmlApp::default()))),
    )
}
