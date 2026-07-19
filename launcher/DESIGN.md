# launcher/ — P8 scope & design

The package-manager launcher: a **single-binary desktop app** that installs/updates SotES mods
package-manager style, edits mod + loader config, and launches the game with the loader present.
Ships **`sotes-mods`** as the default source. This is the P8 scope for a cold start — the *contract*
is [`../docs/REGISTRY.md`](../docs/REGISTRY.md); the *mod format / config / versioning* it consumes is
[`../docs/MOD-FORMAT.md`](../docs/MOD-FORMAT.md).

> **Status:** design only. Tech is an OPEN decision (see "Tech decision"). Nothing built yet.

## What already exists to build on

- **`docs/REGISTRY.md`** — the `registry.json` schema + the trust model (adding a git repo = trusting
  it; every downloaded file is verified against a pinned `sha256`, so the file host needn't be trusted).
- **The proxy install** — the loader is a proxy `version.dll`; the real one is renamed `realver.dll`.
  Today done by hand / a PowerShell one-liner; the launcher automates the backup+swap.
- **Config the launcher edits** (landed this session):
  - `mod.toml [config]` — each mod's settings **schema** (type/default/label/min/max). The launcher
    reads this to render a **generic settings table** — the SAME parse the loader does.
  - `oss_mods.cfg` — per-mod **values**, namespaced `<id>.<key>` (machine-managed, flat key=value).
  - `oss_loader.cfg` — the **loader's own** settings (ddraw, ui, keepactive, …), flat key=value.
- **Versioning to respect**: `registry_version` (registry schema), `[loader] min_version` +
  per-version `min_loader` (host version), mod `[loader] api` (the loader refuses incompatible mods at
  runtime — the launcher can pre-warn). See MOD-FORMAT "Versioning".
- **`../sotes-mods`** — the default source repo (scaffolded: `registry.json`, `mods/autoload`).

## Feature scope

### MVP (v0.1) — "install a mod, set it up, launch"
1. **Game install** — pick/validate the game dir (the exe + `realver.dll`); persist it.
2. **Loader proxy** — install/repair the proxy (`version.dll` ↔ `realver.dll` backup+swap); show state.
3. **Default source** — fetch `sotes-mods`' `registry.json`; list its mods + versions.
4. **Install / remove** a mod version — download each `download` file → **verify `sha256`** → place at
   `dest` (relative to the game dir, e.g. `mods/<id>/…`); prompt for `user_supplied` files; remove =
   delete the mod's files. Track installed state (version) in a launcher-managed manifest.
5. **Config editor** — for each installed mod, parse `mod.toml [config]` → a generic table
   (checkbox/int-slider/float-slider/text) → write `oss_mods.cfg`. Plus a **Loader** section editing
   `oss_loader.cfg` (see open decision #2 — ideally schema-driven, the loader as "mod zero").
6. **Launch** — ensure the proxy is installed, then start the exe detached.

### Later (v0.2+)
- Add/remove **custom sources**; merge all sources (`registry/*.json` split supported).
- **Search** (name/keywords), **release notes** shown before an update, **update-all**, **downgrade/pin**.
- **Dependency** resolution (`requires` across sources).
- **Compat surfacing** — grey out / warn on `min_loader` or mod-`api` mismatch before install.
- Multiple game installs / profiles.

## Architecture (modules — language-agnostic)

- **registry** — fetch a source (see open decision #4: raw-HTTPS vs shallow git clone), parse + merge
  `registry.json`, validate `registry_version`.
- **install** — resolve a version's `files[]`; download → sha256-verify → write to `dest`; handle
  `user_supplied` (prompt + optional verify); write/update the installed manifest.
- **gamedir** — locate/validate the install; proxy backup+swap; `mods\` add/remove; read/write
  `oss_mods.cfg` + `oss_loader.cfg`.
- **config** — parse `mod.toml [config]` (schema) for installed mods; bind values ↔ `oss_mods.cfg`.
- **launch** — start the exe detached (the `Start-Process` equivalent for the chosen lang).
- **ui** — views: Sources · Browse · Installed · Config · Launch.

Data flow: `registry.json` (trusted repo) → pick version → download+verify → `mods\<id>\` + game-dir
files → `mod.toml [config]` → config table → `oss_mods.cfg`. Launch swaps in `version.dll` and runs.

## Trust / verification (from REGISTRY.md — do not weaken)

Adding a source = trusting THAT repo's `registry.json`. Every `download` file is verified against its
pinned `sha256`; a mismatch aborts. So the file host (a GitHub release, any CDN) is untrusted — only
the registry is. `user_supplied` files (game assets we can't ship) are located by the user and
sha256-checked if a hash is given.

## Tech decision — OPEN (decide via a spike, not on paper)

Single static binary, no runtime dep, cross-buildable to a Windows exe **from WSL** (the dev env). The
package-manager plumbing (HTTPS, JSON, sha256, file I/O) dominates the effort; the GUI is second.

| | pkg-mgr plumbing | GUI options | X-compile → win from WSL | build speed | ecosystem | notes |
|---|---|---|---|---|---|---|
| **Rust** | reqwest/serde_json/sha2/git2 — excellent | **egui** (immediate-mode, ImGui-like), iced, tauri | good | slow-ish | mature | egui matches the loader's mental model |
| **Go** | net/http, encoding/json, crypto/sha256 — best stdlib | Fyne/gioui/Wails(web); GUI is the weak spot | **best** | fast | mature | fastest plumbing, weakest native GUI |
| **Zig** | mostly hand-rolled or C libs (libcurl, …) | Dear ImGui / raylib via C | excellent (`zig cc`) | **fast** (user's draw) | young | most plumbing work; tightest control |
| ImGui/C++ | painful (curl+a json lib+sha256 by hand) | Dear ImGui (shares the loader's renderer) | ok (mingw) | ok | — | consistency, but heaviest plumbing |

**Lean:** Rust+egui (one language does plumbing + an ImGui-style GUI + verified deps) or Go (fastest
plumbing; pair with Wails/web-UI or a TUI). Zig is viable if build speed + control outweigh the young
ecosystem. **Spike to decide:** build the *core slice* — fetch `sotes-mods`' `registry.json`, sha256-
verify one downloaded file, and render one installed mod's `[config]` as an editable table writing
`oss_mods.cfg` — in the top **two** candidates; compare ergonomics + build time; then commit.

## Build plan (phased)

1. **Spike** (above) → pick the language.
2. **MVP** in module order: gamedir+proxy → registry fetch/parse → install+verify → config editor →
   launch. Test against `../sotes-mods` (real source) + the scratch game copy.
3. **v0.2** — sources/search/updates/deps/compat-surfacing.

## Open decisions (resolve at the start of P8)

1. **GUI paradigm** — native immediate-mode (egui / Dear ImGui) vs web-UI (Wails/Tauri) vs TUI. Drives
   the language shortlist.
2. **Loader settings as schema ("mod zero")** — should the loader ship a `[config]`-style schema for
   its OWN settings so the launcher renders `oss_loader.cfg` generically (not hand-coded)? Clean + DRY;
   small loader-side addition.
3. **Installed-state manifest** — location (game dir vs launcher data dir) + format (json). Needed for
   update detection + clean removal.
4. **Fetch mechanism** — raw-HTTPS of `registry.json` + release assets (no git dependency, simplest) vs
   shallow `git clone` (needs git). Lean raw-HTTPS for the MVP.
5. **Launcher's own state** — where it stores the sources list + game path (per-user config dir).
