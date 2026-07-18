# Mod registry (the package-manager contract)

The launcher installs mods package-manager style. A **mod source** is a **git repo**
that contains a **registry** listing its mods, their versions, and — per version — the
files to download with pinned hashes.

## Trust model

**Adding a git repo as a source is an explicit act of trust in that one repo.** You
trust its maintainer to list honest files + hashes. The launcher then guarantees
**integrity**: every file it downloads is verified against the `sha256` pinned in the
registry you trusted, so the file host (a GitHub release, any CDN) does not need to be
trusted — only the registry does. This is the Homebrew-tap / APT-`Release` model.

The launcher ships with **one default source** (this project's mods, `../sotes-mods`);
the user adds more repos by URL. Removing a source hides its mods (installed mods stay).

## Registry file

`registry.json` at the git repo root (a source may also split it into `registry/*.json`;
the launcher merges them).

```json
{
  "registry_version": 1,
  "source": {
    "name": "sotes-mods",
    "description": "Francesco149's Fortune Summoners mods.",
    "homepage": "https://github.com/Francesco149/sotes-mods",
    "maintainer": "Francesco149"
  },
  "mods": [
    {
      "id": "ennse_voice",
      "name": "Japanese Voice Patch (EN-SE)",
      "description": "Restores the Japanese dialogue voices in the English Special Edition.",
      "keywords": ["voice", "japanese", "audio"],
      "target_games": ["sotes_en"],
      "versions": [
        {
          "version": "1.0.0",
          "released": "2026-07-18",
          "notes": "First mod-loader release. Fullscreen monster-SFX fix included.",
          "min_loader": "0.1.0",
          "requires": {},
          "files": [
            {
              "kind": "download",
              "dest": "mods/ennse_voice/init.lua",
              "url":  "https://github.com/Francesco149/sotes-mods/releases/download/ennse_voice-1.0.0/init.lua",
              "sha256": "…",
              "size": 1234
            },
            {
              "kind": "user_supplied",
              "dest": "sotesx_s.dll",
              "prompt": "Locate sotesx_s.dll from your Japanese copy (a game asset we can't distribute).",
              "sha256": "…",
              "optional": false
            }
          ]
        }
      ]
    }
  ]
}
```

### Fields

- **`source`** — display metadata for the whole repo (name/description/homepage/maintainer).
- **`mods[]`** — one entry per mod:
  - `id` — unique within the source; the on-disk `mods\<id>\` name and the update key.
  - `name` / `description` / `keywords` — what the launcher searches + displays.
  - `target_games` — profile ids the mod supports (the launcher warns on a mismatch).
  - `versions[]` — newest first; the launcher shows the latest and offers older ones.
- **`versions[]`** — per version:
  - `version` — semver; the update check compares against the installed version.
  - `released` — ISO date; `notes` — **release notes**, shown before an update is applied.
  - `min_loader` — refuse to install under an older loader host.
  - `requires` — `{ "<mod_id>": "<semver range>" }` dependencies (resolved across sources).
  - `files[]` — everything the version places into the game folder:
    - `kind: "download"` — fetched from `url`, verified against `sha256`, written to `dest`
      (a path **relative to the game folder**, e.g. `mods/<id>/init.lua` or a game-dir DLL).
    - `kind: "user_supplied"` — a file the user must provide (a game asset we can't ship,
      e.g. the voice bank). The launcher prompts (`prompt`), verifies `sha256` if given,
      and copies it to `dest`. `optional` allows skipping.

## Launcher operations (later phase)

1. **Add source** — user pastes a git repo URL; launcher fetches its `registry.json`
   (shallow clone or raw fetch), validates, records the source. = trusting that repo.
2. **Browse / search** — merge all sources' mods; search by `name`/`keywords`.
3. **Install** — pick a version; download each `download` file → verify `sha256` → place
   at `dest`; prompt for each `user_supplied` file; resolve `requires`.
4. **Update** — for each installed mod, if a newer `version` exists, show its `notes`,
   then apply on confirm. Downgrade/pin also supported.
5. **Launch** — start the game with the loader present (writes/updates the game-dir
   `version.dll` + `realver.dll` first, exactly like the current PowerShell one-liner).

The on-disk install layout is identical to a hand install (MOD-FORMAT.md): the launcher
is a convenience over the same `mods\` folder, never a separate runtime.
