# v2 revamp — multi-edition core, autonomous real-exe testing, trainer-as-mod + sandbox

Started 2026-11-24. Why: v1 hard-codes EN-SE VAs (one edition), its frame safepoint (`0x437c70`, an
input-poll fn) does not fire in every state (observed: wine + Xvfb, title screen — mods never init), and
nothing is testable without a human at the game. Since v1, OpenSummoners has ported ~1440 retail functions
with names, built per-edition crosswalks, and rhost knows the frame boundary (the Flip) per edition.

Hard rules: never ship game bytes (VAs / names / hashes only); never touch OpenSummoners' working files or
its wine prefixes / rhost runners (a port session runs in parallel) — read its data, write here.

## Editions (all three in hand, none relocatable, no ASLR)

| tag | exe | PE TimeDateStamp | .text vsize | entry (unpacked) |
|---|---|---|---|---|
| `enold` | 2012 EN Steam `sotes.exe` (no DRM) | 2012-01-27 02:04:32 | 0x1ca795 | 0x5c0a8f |
| `ense-en` | SE EN `sotes_en.exe` (SteamStub; same header when packed) | 2026-08-09 06:37:37 | 0x1fbcf5 | 0x5f0d7b |
| `ense-jp` | SE JP `sotes.exe` (SteamStub) | 2026-08-09 05:37:13 | 0x1f9875 | 0x5ee91b |

Detect = (TimeDateStamp, .text size) → candidate; confirm = code hashes of anchor symbols after `.text`
decrypt (`when_text_ready`). Unknown build → (phase 2) hash-signature scan: per symbol a masked-bytes hash
(absolute operands masked via hde32), matched over 16-aligned `.text` starts. No bytes shipped.

## Pieces

1. **Symbols** — `tools/symgen.py` (reads OpenSummoners' local data: rhost hook-table names + enold VAs,
   `retail_fields.json`, crosswalk TSVs, rhost `EDITIONS`, IDA function sizes, the user's three unpacked exes)
   → `core/gen/sotes_syms.c` (committed: name, kind, per-edition VA + 16-byte code hash). Runtime
   `sym_va("flip")`; Lua `mod.sym.va(name)`, `mod.edition`. Every binding resolves by name; a symbol that
   fails its hash is disabled (+ logged), bindings needing it go inert.
2. **Frame safepoint = the Flip** (rhost's frame anchor; enold 0x5b8fc0 / ense-en 0x5e14e0). Fires once per
   presented frame in every state (title, menus, cutscenes, gameplay).
3. **Input injection on the DirectInput COM vtable** (`GetDeviceState` / `GetDeviceData` post-hook ORs
   injected key levels) — no engine VAs, works on every edition; plus the RE'd UI-button ring for menus.
4. **Dev server** (opt-in `devserver=PORT`): localhost line-JSON — `lua` (eval on the engine thread at the
   safepoint, JSON result), `read`/`write`, `keys` (hold/tap with frame counts), `shot` (back buffer → BMP),
   `wait` (Lua predicate + frame timeout), `log`, `quit`. The trainer MCP + tests talk to it.
5. **`tools/sml`** — agent CLI over real exes under wine: own prefix + per-run dirs at
   `/mnt/ephemeral/sotes-mod-loader/` (assets symlinked, exe hard-linked, saves copied), own Xvfb per run.
   `up --edition E [--mods ..] [--save SRC]`, `lua`, `keys`, `shot`, `wait`, `down`, `test` (suites under
   `tests/*.lua`, each edition → PASS/FAIL). Windows real-device runs: later, own scheduled task (never the
   `oss-rhost-runner`).
6. **Bindings v2** — roster / coords / camera / map / flags / items / save / input rebuilt on symbols + port
   struct layouts (cdefs from OpenSummoners `src/*.h`, flagged per edition where layouts are verified).
7. **Trainer → `../sotes-mods/mods/trainer`** — the old injected trainer's features as a Lua mod over
   loader ops, + a **sandbox**: spawn actors, set story flags, give items, trigger scripts / events, warp —
   all via engine functions the port identified, run at the safepoint. Doubles as a behaviour probe for the
   port (same calls, compare). MCP server over the dev server.

## Order

| # | step | gate |
|---|---|---|
| 1 | flip safepoint + dev server + `sml up/lua/down` on ense-en | `sml lua 'return mod.edition'` from a wine run |
| 2 | symgen + edition detect + hash verify | 3 editions boot, all anchors verified |
| 3 | DInput injection + `shot` | agent reaches gameplay via keys only, looks at it |
| 4 | bindings v2 + `sml test` suites | suites PASS on 3 editions — **roster / coordinates / mouse done** (2026-11-24, `tests/roster.lua` PASS ×3); **camera / map / flags / items done** (`tests/world.lua` PASS ×3); **save.load / attract on symbols done** (`core/lua/title.lua`; `mod.game.input` retired for `mod.input`) |
| 5 | trainer mod + sandbox + MCP | spawn / flag / warp tests PASS — **sandbox ops done** (api 1.2: map.rooms / warp / event, items.give / catalog, roster.set, coordinates.set; `tests/sandbox.lua` + `tests/warp.sh` PASS ×3); trainer mod (`../sotes-mods/mods/trainer`) + MCP (`tools/sml_mcp.py`) done; **dialogue skip done** (api 1.3 `dialogue.skip`, `tests/dialogue.sh`); **actor spawn done** (api 1.4 `actors`, `tests/actors.lua` PASS ×3); **voice on symbols done** (api 1.5, `tests/voice.lua` PASS ×3 + voice=0 ×2) |
| 6 | Windows real-device runner | same suites on Windows |
