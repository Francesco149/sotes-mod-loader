# launcher/ — the package-manager launcher (later phase, P8)

An ImGui desktop app (styled like the trainer) that manages mods package-manager style
and launches the game with the loader present. **Built after the injected core proves
out** (P0–P5), so this folder is a placeholder for now.

What it will do (contract: [`../docs/REGISTRY.md`](../docs/REGISTRY.md)):

1. **Add git-repo mod sources** — paste a repo URL; the launcher fetches its
   `registry.json`. Adding a repo = explicitly trusting that one repo.
2. **Browse / search** — every mod across all sources, searchable by name/keywords, each
   versioned with release notes.
3. **Install / update** — download the version's files, verify each against its pinned
   `sha256`, place into the game's `mods\`; show release notes before an update.
4. **Launch** — write/refresh `version.dll` + `realver.dll`, then start the game.

Ships with **one default source**: `../../sotes-mods` (the author's mods).
