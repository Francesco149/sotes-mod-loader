# launcher/ — the package-manager launcher (later phase, P8)

A **single-binary desktop app** that manages mods package-manager style, edits mod + loader
config, and launches the game with the loader present. **Built after the injected core proves
out** (P0–P5 ✅) — this folder is design-only for now.

- **Full scope + design → [`DESIGN.md`](DESIGN.md)** (feature scope, architecture, the tech
  decision, build plan). Tech **decided: Rust + egui**; the `sml-core` plumbing crate (registry /
  verify / mod-config / cfg) is built + tested on Linux — the GUI + install/proxy/launch are next.
- Contract → [`../docs/REGISTRY.md`](../docs/REGISTRY.md); mod format/config/versioning it consumes →
  [`../docs/MOD-FORMAT.md`](../docs/MOD-FORMAT.md).

What it will do (contract: [`../docs/REGISTRY.md`](../docs/REGISTRY.md)):

1. **Add git-repo mod sources** — paste a repo URL; the launcher fetches its
   `registry.json`. Adding a repo = explicitly trusting that one repo.
2. **Browse / search** — every mod across all sources, searchable by name/keywords, each
   versioned with release notes.
3. **Install / update** — download the version's files, verify each against its pinned
   `sha256`, place into the game's `mods\`; show release notes before an update.
4. **Config** — a generic settings editor per installed mod, rendered from its `mod.toml [config]`
   schema, written to `oss_mods.cfg` (+ the loader's own `oss_loader.cfg`).
5. **Launch** — write/refresh `version.dll` + `realver.dll`, then start the game.

Ships with **one default source**: `../../sotes-mods` (the author's mods).
