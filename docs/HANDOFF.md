# HANDOFF — current checkpoint

Rolling "where we are / what's next" for a fresh session. Orient: this → `DESIGN.md`
(architecture + phase table) → `MOD-FORMAT.md` (the `mod.*` API) → `TESTING.md`.

> **Cross-reference `../OpenSummoners`** — the parity port + the **canonical RE home** for the
> SAME `sotes.exe`. Look there before reversing (decompile `docs/decompiled/`, findings, Frida
> harness, trainer `tools/sotes_trainer/SE_CODE_MAP.md`); and **record any NEW engine RE finding
> THERE** (`SE_CODE_MAP.md` / `engine-quirks.md`), not only in this repo — loader docs cover the
> loader, game facts belong to the port so both projects benefit. (README "Related".)

## v2 REVAMP IN PROGRESS (2026-11-24) — read `docs/plans/v2-revamp.md` first

**v1 was broken on the current Steam build** (SE update 2026-08-09: `0x437c70` is no longer a function
start, so the safepoint hooked garbage and mods never initialized; v1's `present_va`, save/attract/input
patch sites, the roster `render_root` pointer and the voice patch signatures are all from the old build).
v2 so far (commits `10a1efe`, `b5b3b2b`, `6cb69d2`):

- **Edition layer** (`core/syms.{c,h}`, generated `core/gen/sotes_syms.c` by `tools/symgen.py` from
  OpenSummoners' local RE data + the user's exes): enold / ense-en / ense-jp detected from the PE header,
  confirmed by anchor code hashes; every function symbol hash-verified (mismatch → that symbol off).
  `mod.sym.{va,info,find,name_at}`, `mod.edition`.  Regenerate: `python3 tools/symgen.py`.
- **Frame hook = `flip`** (all states, all editions) — replaces the v1 safepoint + present hooks; ddraw
  capture rides it (screen-ctx layout verified on all 3).  `mod.frame()`, `mod.screen.{shot,ctx}`.
- **`mod.input`** — scripted keyboard via dinput's own GetDeviceState/GetDeviceData (no game VAs).
- **Dev server** (`devserver=PORT` / `OSS_DEVSERVER`) + **`tools/sml`** — real exe under wine, own prefix
  per run under `/mnt/ephemeral/sotes-mod-loader/`:
  ```
  tools/sml up R --edition ense-en|ense-jp|enold [--mods a,b] [--saves steam|DIR|none] [--cfg k=v]
  tools/sml lua R 'return mod.sym.info("flip")'     # engine thread, JSON
  tools/sml play R "ent*2 .*30 R*2 X*2" --wait     # rd movie syntax
  tools/sml shot R out.png                          # exact back buffer — LOOK at it
  tools/sml wait R 'EXPR' ; tools/sml log R ; tools/sml down R|--all
  ```
  Verified: all three editions boot side by side (enold 1303 / ense-en 1212 / ense-jp 1201 symbols, 0
  disabled); keys-only title → save picker → gameplay → walking on ense-en.
- **Bindings v2, first wave (2026-11-24):** `core/sotes_layout.{h,c}` = per-edition struct offsets (enold from
  OpenSummoners `src/world_layout.h`; SE via **`tools/fieldxw.py`** — aligns crosswalked enold↔SE function pairs with
  capstone and votes on the displacement each SE site uses; ense-jp == ense-en everywhere checked).  `g_world` symbol
  (symgen now reads rhost's `world_ptr`).  **`mod.game.roster` / `coordinates` / `mouse` rebuilt** on world → room
  (`+0x2784` / SE `+0x3788`) → `slots[8]` (`+0x4030`) → pslot (active `+0x9c4`/`+0x9e4`, char `+0x9f4`/`+0xa14`) →
  char (stats `+0x750`/`+0x760`, body `+0x40` → x/y `+4/+8`); controlled = `room->leader` (`+0x200c`).  No heap scan,
  no executor dependency.  Stat block: SE = enold +4 from `0xd4` on; **level fix**: HUD "Lv" = combat level (SE `0xdc`)
  + adventurer (`0xe4`); `0xe0` = combat max (stars) — v1 summed max + adventurer (wrong whenever M < N).  The port's
  enold `party.h` names those three fields differently (`level_bonus` / `star_count` / `combat_level_max`) — same
  sum; worth a rename there (not done: port session owns that repo).
  **Gate:** `tools/sml continue R` + `tools/sml test R tests/roster.lua` → PASS on enold / ense-en / ense-jp,
  numbers checked against the HUD (TESTING.md).  `sml up` copies `*.sdt` only (config.dat moves the title / picker
  cursors) and no longer races parallel `up`s for a prefix slot.
- **Bindings v2, wave 2 (2026-11-24, api 1.1):** `mod.game.camera` / `map` / `flags` (story = the fill table
  room `+0x40ac`, area = room `+0x8`) / `items` (inventory room `+0x2010` ×16 B, count `+0x4010`; gold `+0x4014`, cap
  `+0x4020`) — all room offsets equal on SE (fieldxw).  Only SE difference found: the **area table** record is 344 B
  (enold 336) with its count at `+0x56004` (enold `+0x54004`) — read from SE `area_find` 0x565a20 (layout
  `area_size` / `area_count`).  Writes refuse off the engine thread.  **Gate:** `tests/world.lua` PASS ×3; HUD-checked
  (SE gold ×630; `set_gold(4321)` → HUD ×4321).
- **save.load / attract v2 (2026-11-24):** `core/lua/title.lua` (embedded): Tier-2 typed hooks on the title driver's
  own functions, symbols in **`tools/symgen_manual.tsv`** (title_menu [+ SE's second variant title_menu_se], save_list,
  menu_runner — found by aligning `build_title_drive`'s call sequence + jump table per edition: action 28 = save list,
  12 = load).  Pending load → title menu returns 28 unseen; the list's first frame moves the cursor to the row whose
  slot matches (`*(ctrl+0x17c)` ×16 {kind 10040, slot}; cursor `*(*(ctrl+0x174)+0x14)`, same on SE via accessors
  0x5e8ef0/0x5e8f10) and confirms; Quest Selection (post-game) → Main Quest.  SE's list has 80 rows (empty slots
  too) — the game refuses an empty one → `failed`.  Attract off = title menu's 0 (idle) → 31 (loop to the title).
  `sml continue --slot N` uses it.  v1 C drive + `mod.game.input` (ring injection) deleted.  **Crosswalk caution:**
  confidence-1 SE symbols can be wrong — `tm_41c2f0` (SE = a free loop) and `item_kind` (SE = the list row-slot
  accessor) were; check the disasm before hooking a conf-1 symbol.
- **Sandbox ops (2026-11-24, api 1.2)** — `mod.game.map.rooms/room/warp/warp_status/event`, `items.give/catalog`,
  `roster.set`, `coordinates.set` (MOD-FORMAT).  **Warp = the warp item's own request** on the room loop (world
  `+0x1044`; `+0x10` req / `+0x900` {key, gate, face, kind} / u16 ui_lock `+0x28`; SE `+0x14` / `+0x944` / `+0x2c`,
  from fieldxw on warp_request 0x54ecf0 / 0x5b3850 / room_warp_tick): retail rules + dissolve, or `force` = ui_lock 1
  (room_warp_tick's own `--ui_lock == 0 → return 10`).  Learned the hard way: (1) **the area table holds rooms with
  NO map data** (999999 + the 110/120/130 series: 20 on SE, 20 on enold) — loading one = retail's fatal "The map data
  is not found." + exit, so `loadable` = retail's own lookup (SE record `+0x148` ≠ 0 → `wmap%d.sdt`; else
  `FindResourceA(map_module, record +0xc, "DATA")`, map_module enold `*0x8a6e7c` / SE `*0x92b230` / JP `*0x9263a0`,
  override = area table `+count+4`); (2) **an event script's own frame loop calls room_warp_tick and DROPS its
  result** — a warp armed during a cutscene is consumed and lost, so warp refuses while world `+0x2760` (SE `+0x3760`,
  script_begin/_end) is set.  Towns (record `+0x114` == 2 → world `+0x1030`) refuse the unforced warp, as retail.
  Items: SE has its own `room_item_add` (0x404db0 / JP 0x404e00, called by item_give) with per-category caps — called,
  never re-implemented; catalog = `item_table` (enold 736 B / SE 940 B records) + SE's `item_table_ext`.
  **Port-repo findings owed** (not written there — a port session runs in parallel; add to OpenSummoners
  `engine-quirks.md` / `SE_CODE_MAP.md` when it's free): the map-less rooms, the dropped warp return in events, the
  SE W-map path (`+0x148`), SE room_item_add / item tables / map_module VAs, SE warp-request offsets.
- **Voice v2 (2026-11-24, api 1.5)** — the 2026-08 SE builds load `sotesx_s.dll` THEMSELVES on EN too (boot loader ense-en
  0x5d7a47 → bank global 0x92b220; JP 0x926390), so v1's EN "seed" is gone; the registrar bug is still shipped on both:
  `sound_registrar` (new symbol: ense-en 0x59b3b0 / ense-jp 0x599ab0 / enold 0x57b280 = port al_57b280) skips every
  `deluxe_id == 0` row (`0f 84` at +0x17ac) once the bank is loaded and `settings+0x254` (a new deluxe-off option?) is 0.
  `core/voice.c` waits for the registrar's hash (`sym_va_early`), matches the whole bank/settings/deluxe-id block by bytes
  inside it (operands wild, both short jumps must agree) and points the je at the non-deluxe path; derives the table /
  slot array / bank from the operands.  `mod.game.voice.status()` + **`tests/voice.lua`**: shipped 0/58 no-variant rows
  registered → patched 58/58, deluxe 200/200 from the bank, both SE editions; enold off.  **Real Steam exes on Windows
  (packed, SteamStub decrypt race) PASS:** ense-en waits 40–50 ms after attach and still beats the registrar (58/58), ense-jp
  43 ms (58/58), EN voice=0 → 0/58; plus roster / world / sandbox / actors PASS on both with a Steam save (harness
  `tools/win/`, TESTING.md "Windows").  Not listened
  to (headless); same redirect v1 had USER-confirmed by ear.  The port's `tools/ennse_voice` still has the OLD build's
  VAs (fails safe → no-op) — owed there with the findings below.  `../sotes-mods` registry `ennse_voice` (unreleased)
  says "supply sotesx_s.dll": the user's copy in the SE dir is 2009-dated (likely user-supplied, not a Steam file); the new EN
  exe loads it IF present (absent ⇒ bank 0 ⇒ no deluxe branch ⇒ no bug).  So that entry = "drop the file in" + this built-in.
- **Trainer v2 = `../sotes-mods/mods/trainer`** (0.1.0, unreleased; registry id `sotes_trainer` → `trainer`): god /
  heal / gold / F7 teleport-to-cursor / warp browser (exits + every loadable room by map; force on by default) / item
  giver, panels in the loader UI, `_G.trainer` for agents (dev-server `lua`).  Verified on ense-en (`sml up --mods
  trainer`, `--cfg ui=1` + `sml shot --x` shows the companion window).  Not yet ported from the old trainer: autoskip /
  dlgskip / fastskip (dialogue), mousefly (continuous), door hijack (superseded by warp).
- **`tools/sml_mcp.py`** — the agent MCP over sml + the dev server (TESTING.md "For agents"); replaces the old
  trainer MCP for v2 (that one spoke the injected trainer's socket).
- **Dialogue skip (api 1.3, 2026-11-24)** — `mod.game.dialogue.skip(on)` / `.get()`: retail's OWN skip.  The message
  window (msg_update 0x43b980) sends its last control key 8 (reveal rest / end) every frame input axis 6 (input_mgr
  `+0x12c`) is held; the keyboard producer (enold 0x46a880, port `input_dik.c:80-81`) sets axis 6 on DIK_TAB without Alt
  (or the configurable skip bind).  So skip = hold TAB through the DirectInput layer (`di_auto`, a loader-owned hold
  layer separate from scripts / `input.hold`; `exec_set_pre_input` runs the gate each frame before the input latches),
  ONLY while `map.event()` — no VA, no code patch, every edition.  Choice lists: enold `grid_nav` has no key-8 case →
  untouched (SE unverified; the old trainer's je-NOP at 0x437740 did advance choices, a different path).  **Gate:**
  `tests/dialogue.sh R` (a `--saves none` run: new game by keys → the intro event (210110) → control: still open 600
  idle frames → skip → event ends, free roam not holding) PASS ense-en (2833 f) / ense-jp (2532 f) / enold (2793 f; its
  new-game config opens on Difficulty — the test switches to Up+Enter after 80 tries); `SKIP_ON='trainer.skip_dialogue(true)'` drives the trainer's option.  Trainer: `skip_dialogue` config +
  panel checkbox + `trainer.skip_dialogue(on)`; MCP: `skip_dialogue`.
- **Actors (api 1.4, 2026-11-24)** — `mod.game.actors.list/codes/valid/spawn/remove`.  spawn = retail's
  `summon_spawn` (enold 0x55ce70 / SE 0x57b910 / JP 0x579c40; stdcall code, anchor body, dx, dy, mode, 0, facing,
  char_init anchor, a13): its own round-robin free slot + `char_init` with origin 4.  Absolute placement = a zeroed
  fake body + mode 0 (summon_spawn adds nothing); by the leader = mode 4 on the leader's body (centre-x, bottom; dx
  mirrored when it faces left); char_init anchor 5 = centre-x / feet.  **Open question closed: sprite banks need no
  preparation** — 51110 (Org Archer) + 50360, never placed in the test room, draw and act (ense-en + enold, looked).
  **The real hazard = unknown codes**: char_init's default arm = "Unknown Object Type Character." + exit(0).  So
  **`tools/charcodes.py`** walks char_init's code dispatch statically (capstone, interval arithmetic through cmp/jcc,
  sub/dec flags, lea, byte index + dword jump tables; the default = the body pushing that string) →
  `core/gen/sotes_charcodes.c` (numbers only).  enold 122 codes == the port's hand-ported `char_init.c` case list
  exactly; SE 134 (JP == EN): drops 100164–100169 + 100187, adds 100181 + 100190–100207.  remove = `char_unload` (0x41e360, the story
  scripts' removal).  Spawned with no dramatist handle → the code's default look (50300 = a green witch, not the
  room's named one).  **Gate:** `tests/actors.lua` PASS ×3.  MCP: `actors` / `spawn` / `despawn`.
- **Names:** `actors.catalog()` from the **`stat_table`** data symbol (= the port's base-stat table DAT_0067ac58;
  SE 0x65e3a8 / JP 0x65c3a8, 0xdc rows: +0 code, +0x3a display name, +0x8a English) — 73 / 85 of the codes named.
- **Trainer spawn panel done** (`../sotes-mods` 5672b0b): catalog slider + "ahead of the leader" + per-spawn remove;
  `trainer.spawn(code, dx_px)` / `trainer.despawn(slot)`.  Party codes (50010–50013) spawn as idle non-party clones
  (no pslot) and remove cleanly (ense-jp, 120 frames).  **ImGui shows Shift-JIS as `?`** (UTF-8, no CJK font): the
  trainer uses `en` on ense-jp; the Items panel still prints JP item names raw — a `mod.text` CP932→UTF-8 helper + a
  CJK font range would fix both.
- **Hand-play copy (2026-11-24):** `C:\sotes-modded` + desktop shortcut "SotES with mods", auto-refreshed on commit (TESTING.md).
- **Custom content (api 1.7, 2026-11-25)** — the `testroom` PoC (`../sotes-mods/mods/testroom`): a new door in Arche's house
  (210140, ground floor left of the front door) → a NEW room 210142 (map DATA 4001 built from the back room's DATA 1026)
  with a custom NPC (code 50240, handle 990001) whose talk script is Lua.  Loader pieces:
  - **`mod.res`** (`core/resov.c`) — the resource overlay: the EXE's four kernel32 resource imports patched (lazily, first
    `set`); `FindResourceA` answers overlay entries (any module, or one) with a fake HRSRC, `LoadResource`/`SizeofResource`
    serve the bytes.  Maps AND sprite sheets go through it (both `FindResourceA(.., "DATA")`).  `read` = the real one.
    A mod builds bytes from the user's own data — nothing shipped.
  - **`map.add_room` / `set_exit`** (`sotes_bindings.c`) — the heap area table is rebuilt every session, so additions are
    re-applied from a Tier-1 observer on `area_find` (thiscall, ECX = the table, both editions).  `loadable` / warp now
    consult the overlay (`resov_find`).
  - **Talk scripts** = a typed pre-hook on `script_engine` claiming (room, ev +0x14 mode 2, ev +0x4 actor +0x1d8 handle)
    and calling the retail primitives over FFI (`script_begin` … `scene_msg` … `script_wait` … `script_end`); their nested
    frames re-enter Lua (on_frame / dev server) — fine with the JIT compiled out (FFI calls are plain C calls).
  - Map format facts (from the port's `tools/extract/map_data.py` + looking): cells = {co, piece, bank, frame, shape, -, -}
    (`co` = per-piece base + 10 × frame); the house walls are wide panel pieces (113015 2-col / 113035 4-col; door =
    113035 bank 2230 frame 11); plane 1 draws over plane 0 (the staircase hides a door there).  Doors = invisible 70102
    devices (A[0] zone) in front of the panel (x + 32 px).  Map NPC record: A[1] handle, A[4] facing (≠ 0 = LEFT), A[6]
    presence (9 = always).  The SE talk list is world **+0x3794** (enold +0x2790); its probe = a 40 px strip from 8 px in
    front of the leader (`body_probe_rect`), so the leader must FACE the NPC.
  - **Zone order matters:** `zone_pass` takes the FIRST live zone actor (actor-slot order = map object order) overlapping
    the leader and returns when its zone has no exit record — script zones (70105, ids 9xx) overlapping a door shadow it.
    Put an added door device FIRST in the object list (testroom: Arche's house zone 905 at x 96).
  - **Bad SE symbol fixed:** `script_end` ense-en/jp were the crosswalk's 0x4981c0 / 0x498100 (a story-flag lookup it
    calls) — calling it faulted at the end of the first talk.  Real: 0x497bd0 / 0x497b10 (enold prologue, unique;
    `symgen_manual.tsv`).  Port-repo finding owed (crosswalk TSV).
  - **Testing note:** under wine the flip rate (~200 fps) is far above the game's input poll — hold movie keys ≥ 10 flips
    (`L*30` to turn, `V*10` to talk); 1–6-flip taps silently drop.  V (ring 38) = talk; Enter advances messages.
  - **Gate:** `tests/testroom.sh R` (a `--mods testroom` run, continued) — house overlay hit, door → 210142, 2 talks run +
    end, back through the door.
- **Event scripts + sound (api 1.8, 2026-11-25)** — `mod.game.script` / `mod.game.sound` (`core/lua/script.lua`, embedded,
  hooks armed on first use) + the testroom 0.2 **Mod Hall** (`../sotes-mods` 2f32d41).  Talk handlers claim script_engine
  events by the actor's handle and run retail's primitives; full list in MOD-FORMAT.  RE facts it rests on (all SE-checked by
  disasm / the SE IDA export; **port-repo findings owed**, not written there — a port session runs in parallel):
  - **Choice list** = the runner's generic list (scene_run): +0x584 u16 count, +0x588 pick / default id, +0x58c id on cancel,
    +0x590 no-cancel, window +0x594 w / +0x598 x / +0x59c y (height 28 n + 64), 0x58-B rows from +0x5a0 {text[0x50], kind,
    id}, wait mode +0x20 = 5.  scene_yes_no just fills 2 rows; **SE yes_no takes an arg** (→ +0x590), SE shop_run a 4th
    (sell mode, retail passes 1), **SE room_party_heal (room, all, mp)** (enold 2 args).  Line break token in text = `%n`.
  - Narration box = scene_msg kind 3, no actor.  Books: title[0x28] + 2052-B pages {text[2048], u32 end}; the first page
    with end ≠ 0 terminates.  Shop stock = 0-terminated int32 ids.  Fades: ms (+0x3c), effect wait +0x20 2 / +0x24 1 /
    +0x28 1 plain, 2 fade / +0x2c ms.
  - **Retail's inn = el_41c390** (enold 0x41c390 / SE 0x417390 / JP 0x417380): (ev, price CP, 1, mode 0, speaker, offer
    printf, decline, not-enough, morning, bye) — its own "Inn - N to stay" window.  si_41a160 = the rest alone (jingle,
    fade, world tick off +0x1c — clear it after, as the inn does).
  - Furniture talk beat = scene_talk_face(ev, 0, actor, **0, 0**) (people 1, 1); a device asked to face back hangs the cast wait.
  - **touch_pass only runs on the talk / examine keys**; gimmick_pass (enold 0x59afd0 / SE 0x5c3860) runs every free-roam
    frame → run() / after() hang off it.  event_run's room arg = the room POINTER (world +0x2784 / SE +0x3788).
  - entity_set_handle (0x555f00 / SE 0x574610, thiscall) = +0x1d8 + the world handle list (+0x2790 / SE +0x3794) — a
    summon_spawn'd NPC becomes talkable.
  - **Town rooms** (record +0x114 = 2): weapons drawn with Z still swing and barrels break, but hits do no damage either way
    — spawned monsters are inert there; outside towns they're real enemies (fight, die, drop coins).  add_room{town=false}.
  - Sound pools: sys (g_sys: enold 0x8a6b60 / en 0x92af08 / jp 0x926078) + 0x364 ×465 / en 0x378 ×483 / jp 0x374 ×483, all
    in use; pool {u16 n, next, voices*, dsound*, u16 res, -, hmodule, u16 group}, WAVE via FindResourceA(mod, res,
    "WAVE") (a plain RIFF).  Custom sounds = our pool swapped into slot 0 only during sfx_play.
  - Under wine the house floor line for coordinates.set is **y 52000** (54400 = inside the floor: the leader can jump but
    not walk or swing — cost an hour).  Keys: Z draw/sheathe, X attack / examine, V talk, C jump, esc cancel.
  - symgen's rh_hooks regex skipped the table rows (`"name", 0xVA,`) — +178 symbols now.
  - **Gate:** `tests/hall.sh R` (a `--mods testroom` run, continued) — PASS on ense-en.  **Not yet run on enold / ense-jp.**
  - `sml` runs now play into a null sink (`sml_silent`; USER: the attract was audible) — PipeWire also remembers
    "  sotes" → sml_silent.
- **Next (modding PoC):** custom sprites (sheets are DATA resources too: a recolour / new bank through the overlay — format
  RE first), hall.sh on enold + ense-jp, a portrait / voice in s:say, camera shake, Windows hand-play of the hall.
- **Next:** more `tests/*.lua` → a fuller Windows runner (`tools/win/` is launch + Lua only: no `--saves` isolation — it uses the real install + Steam saves, backs up / restores `version.dll` by hand) (own scheduled task, never `oss-rhost-runner`).  Host tests (`make -C core tests`) still
  assume WSL interop — port them to run under wine.

## State: P0–P5 DONE + **DDraw present (borderless + windowed takeover)** + **in-game overlay** + **`mod.config`** + **semantic `mod.game` ops** + **mod API versioning** + **early-boot phase & the `OssModEarlyInit` ABI** + **built-in JP voice restore** (2026-07-20). Next = **P8 launcher** + P7 trainer as a mod.

### Session 2026-07-20 (early-boot ABI + a live RE pass)
- **`OssModEarlyInit`** — the general early-boot native ABI (`core/oss_mod_api.h` "EARLY-BOOT ABI"):
  a mod patches the game DURING its own boot (before the engine inits) with a small vtable —
  `log`/guarded `mem`/`patch`(RWX code)/`config`/**`when_text_ready`** (the shared decrypt-wait that
  generalizes the voice restore's SteamStub poll).  Loaded in a new early native pass in `loader.c`;
  `examples/early_probe` + a host `EARLY_ABI_OK` test; validated in-game.
- **Live RE (vs save 15)** — recorded in **`../OpenSummoners`**: `docs/findings/save15-live-stats.md`
  (the character **stat block** — name/HP/MP/the 4 combat stats/levels/exp; the **adventure-stats**
  struct; the per-char **save record**; and the negative result that SE has **no fixed-index party
  array**) and `docs/findings/title-menu-state.md` (the title runner + input-ring + injection).
- **`mod.game.roster`** now exposes the full stats (attack/defense/spirit/resist, combat & adventurer
  level, `char_level` = their sum, exp/exp_max); `mod.game.input.press(id)` injects UI buttons for
  **arbitrary menu navigation**.
- **Auto-load "defaults to Start" bug — FIXED:** the title-menu selection **cursor** is the standard
  `menu_ctrl` cursor (`*(ctrl+0x174)+0x14`), and `ctrl` = `menu_list_latch`'s ecx (`0x43ce50`).
  `save.load` now hooks that latch to capture the live title ctrl, injects nav `3` to provoke the
  first latch (it only runs on a consumed button), forces the cursor onto the **Continue** row
  (`action 0x1c`), then commits with **`0x24`** (the RE-verified title commit; the old code injected
  `0x25`, which the title never polls).  No Continue row (no save) → refuses to start a new game.
  Disasm-verified; **end-to-end in-game run still to confirm**.  See `../OpenSummoners`
  `docs/findings/title-menu-state.md`.

The loader now also OWNS THE DISPLAY: it hooks the DirectDraw present, and either mirrors the game in
the companion window (`ddraw=1`, default) or TAKES OVER the game window with a vsync'd, sharp-bilinear
D3D11 swapchain — **`ddraw_takeover=1`** borderless-fullscreen OR **`=2`** a resizable normal window
(both user-confirmed smooth).  In takeover the `mod.ui` host draws as an **in-game overlay** on top of
the game frame (F10 toggles) — retiring the companion window there.  See "DDraw present backend",
"Phase C — the in-game overlay", and "Mod config + semantic ops + versioning".

The loader is a usable modding API — in Lua AND native C: guarded memory (`mod.mem`, incl. `patch` for
code), live RE'd game state + **semantic ops** (`mod.game.*` — roster/coords/mouse, plus `attract`/
`save` that expose RE'd menu-drives so a mod ORCHESTRATES instead of reimplementing), per-frame/one-shot
engine work (`mod.on_frame`/`mod.main`), chained hooks (`mod.hook`), ImGui panels (`mod.ui`), and
**per-mod settings** (`mod.config`, schema in `mod.toml [config]`).  Mods declare the API they need
(`[loader] api`); the loader **refuses an incompatible one**.

The loader is a usable modding API — in Lua AND native C: a mod reads guarded memory
(`mod.mem` / `api->mem_*`), reads live RE'd game state (`mod.game.*`), runs per-frame/one-shot
on the engine thread (`mod.on_frame`/`mod.main` / `api->on_frame`/`main_enqueue`), **hooks
any function alongside other mods** (`mod.hook` — Tier-1 observe + Tier-2 typed; native
`api->hook_entry` shares the same chain), and adds **ImGui panels/windows** to a loader-owned
companion window (`mod.ui`).

| phase | what | status |
|---|---|---|
| P0 | proxy `version.dll` + LuaJIT host + `mods\` scan (native + Lua) | ✅ |
| P1 | `mod.mem` (guarded) + togglable `mod.game.*` bindings (roster, coordinates) | ✅ |
| P2 | main-thread executor: WndProc bootstrap → safepoint hook `0x437c70` → `mod.main`/`on_frame` | ✅ |
| P3 | chained hook registry — **Tier-1 observers** (`mod.hook.entry`) **+ Tier-2 typed** (`mod.hook.typed`) | ✅ |
| P4 | native-mod C ABI: `OssModInit(const OssApi*)`, shared hook + executor registries | ✅ |
| P5 | ImGui UI: loader-owned companion window — `mod.ui` + a lock-free snapshot pipeline | ✅ (host + in-game) |
| P6 | JP voice restore — landed as a **built-in** (early-boot phase, `core/voice.c`), + the general **`OssModEarlyInit`** ABI it generalizes into | ✅ |
| P7 | trainer → a mod; coexistence verified | ⬜ |
| P8 | launcher (package manager: git-repo sources, install/update, Launch) | ⬜ (NEXT) |

## Mod config + semantic ops + versioning (NEW — 2026-07-19, groundwork for P8)

Direction set this session: **known RE work becomes a semantic loader op; a mod collapses to telling
the loader what to do + its config.** Arbitrary `mod.mem.patch`/`mod.hook` stay for the un-upstreamed
frontier.  Landed:

- **`mod.config`** — a mod declares its settings in **`mod.toml [config]`** (`type` bool/int/float/
  string, `default`, `label`, `min`/`max`); `mod.config.get/set/schema`.  Schema parsed by a small
  pure-Lua TOML subset reader in the loader (`CONFIG_LUA` in `lua_host.c`) — the SAME file a Go/Rust/Zig
  launcher will read to render the SAME table.  Values in **`oss_mods.cfg`** (namespaced `<id>.<key>`,
  machine-managed, separate from `oss_loader.cfg`), **debounced** write (a slider drag → one write, via
  `config_mod_flush` on the safepoint).  `examples/config_demo` renders a generic editor from the schema.
- **Semantic `mod.game` ops (SotES)** — `mod.game.attract.set(on)` (attract/demo toggle) and
  `mod.game.save.load(slot)` (drive title→picker→save).  The old `core/sotes_autoload.c` + `autoload=`
  flag are **retired** — the mechanism moved into `sotes_bindings.c`, and auto-loading a save is now the
  **`autoload` mod** (in `../sotes-mods`, the first core→mod extraction): a few lines calling `save.load`.
- **`mod.mem.patch(va, bytes)`** — general code patching (RWX + memcpy + icache flush).
- **Mod API versioning** — `OSS_API_VERSION` (MAJOR.MINOR); a mod's `[loader] api` is checked before it
  runs and an incompatible one is **REFUSED** (major mismatch = breaking; minor-too-new = missing
  features; absent ⇒ "1.0").  `mod.api_version` for runtime checks.  See MOD-FORMAT "Versioning".

All validated in-game (EN-SE): config round-trips + persists, the autoload mod reaches gameplay via
`mod.game.save`, an api-"2.0" mod is refused.

**Next (P8 launcher):** a single-binary package manager (Go/Rust/Zig — decide when starting; ImGui/C++
also on the table) per `REGISTRY.md`, reading `mod.toml [config]` to render the generic settings editor
and writing `oss_mods.cfg`.  Ships `../sotes-mods` as the default source.  The registry lives in
`../sotes-mods` (already scaffolded: `registry.json` + `mods/autoload`).

## DDraw present backend + borderless-fullscreen (NEW — `core/ddraw_present.{h,cpp}`, 2026-07-19)

SotES renders through a DirectDraw7 software blitter and PRESENTS once per frame in **zdd_present
(EN-SE `0x5e1470`, `profile.present_va`)** — thiscall, ECX = the ZDD screen ctx (mode field `+0x164`),
finished back-buffer surface at `*(*(ECX+0x16c)+0x2c)` (640×480, **24bpp** in EN-SE windowed).  The
executor hooks it (asm thunk like the safepoint), and `ddraw_present.cpp` re-presents the frame through
our own **vsync'd D3D11 swapchain**.  Two modes (config):

- **`ddraw=1`** (default): capture the back-buffer into a lock-free triple buffer; the UI thread draws
  it as a game mirror behind the companion UI.  *Proven.*
- **`ddraw_takeover=1`** (opt-in): OWN the game window — the present thunk **`ret 4`'s to SKIP the
  game's own (unsynced ~130fps GDI BitBlt) present** and we drive the window: **borderless-fullscreen**
  (`WS_POPUP` over the monitor), **sharp-bilinear** upscale, aspect-correct pillarbox, `Present(1,0)`
  (smooths + paces).  Makes windowed play look/feel like a smooth exclusive-fullscreen.  Companion
  window auto-hides (F8 to show).  *User-confirmed in-game "feels good".*

Also **`mod.game.mouse.get()`** → `{screen_x, screen_y, over, world_x, world_y}`: the cursor in the
game's 640×480 space (the ddraw layer undoes the pillarbox / client scale) + world centi-px via the
camera (`render_root+0x104c` → view obj; origin `+0x60/+0x5c`, span `+0x64/+0x68`).  *Validated to the px.*

## Phase C — the in-game overlay (DONE, 2026-07-19)

The `mod.ui` panels now draw ON TOP of the game frame, inside the game window, in borderless takeover —
retiring the companion window there.  The P5-anticipated "renderer-hook backend consuming the SAME
snapshot": **`mod.ui` is unchanged.**  Implemented (`ddraw: Phase C` commit):

- **ONE ImGui context, owned by the ENGINE thread** — `ui_overlay_present()` (`ui.cpp`), called from
  `ddp_engine_present()` after the game-frame quad, before `Present(1,0)`.  In takeover `ui_start`
  does NOT spawn the companion UI thread (the ⚠ from the plan — ImGui's global current-context would
  be raced across two threads), so there's exactly one context on one thread.  The engine thread is
  BOTH producer (`ui_build`) and consumer, so the lock-free triple buffer just degenerates to
  same-thread latest-wins — the existing `draw_from_snapshot`/`replay_cmds`/`snap_fetch` are reused
  as-is (all already in `ui.cpp`; no factoring needed).
- **Input** — the executor's `boot_wndproc` forwards to `ui_overlay_wndproc()`; the overlay is
  **MOUSE-ONLY** (no ImGui keyboard-nav), so gameplay keys belong entirely to the game (you keep full
  control with the overlay up).  The game ignores the mouse — no contention.  Toggle is **F10** (F8
  collides with SotES' in-game menu; F10 is a system key so we consume it to avoid menu-mode).
- **Validated in-game** (unpacked EN-SE, `ddraw_takeover=1`): overlay drew on the engine-thread ImGui
  over the game frame, gameplay reached via autoload, F10 toggles, mouse clicks reached the `mod.ui`
  callbacks (button presses logged), keyboard still drove the character — no crash.

**Next:** flip `ddraw_takeover` default-on after edge-testing — multi-monitor (`MonitorFromWindow`
picks the game's monitor — verify placement), alt-tab in/out of the borderless window, and restoring
the `WS_POPUP` style + overlay ImGui teardown on clean unload (`DllMain` `DLL_PROCESS_DETACH`,
reserved==NULL).  Then **P6 voice**.

**In-game validation** (unpacked EN-SE `sotes-trainer-oss.exe`, 2026-07-19): executor armed
on the real `0x437c70` (game ran 24 min, no crash); `mod.game.roster` read all 3 live party
members (correct coords/HP + `active` detect); the hook registry's codegen thunk installed
on `kb_poll`, register capture correct (`ctx.ecx == the real this`), 2 cbs on 1 VA both
fired, `remove` worked. Evidence: `TESTING.md`.

**Tier-2 validation** — host (`make -C core tests` → `>> TYPED_HOOK_OK`): an FFI-closure
detour **modifies args, blocks, and modifies the return** across **cdecl / __stdcall /
__thiscall** (callee-stack-cleanup + ecx-capture — the crashy bits — all clean); the
**off-thread gate** skips the Lua chain; cross-tier exclusion holds both ways. **AND in-game**
(2026-07-19, `examples/hook_typed`): the typed hook installed on the live `kb_poll` thiscall,
the FFI closure marshaled the real `ecx` (keyboard device) as `ctx.args[1]` **every frame**
(`this match=true`, hits 119→599), and `mod.hook.remove` froze it cleanly (game stayed up).

## Build / run

Self-contained flake (no dep on OpenSummoners). Everything in `nix develop`:
```
nix develop --command make -C core            # -> build/version.dll  (LuaJIT cross-built once, cached)
```
Host-side tests: `core/test/{lj_smoke,host_stub,exec_test}.c` + `examples/*` (run from NTFS,
not `\\wsl$`). In-game: see `TESTING.md` (stage into the game dir, launch with
`tools/dev-launch.sh`, read `oss_modloader.log`, restore).

## Core files (`core/`)

- `loader.c` — DllMain, proxy attach, `config_load`, `mods\` scan (defers Lua mods to the
  executor); after arming, starts the UI host (`ui_start`, unless `ui=0`).
- `lua_host.c` — LuaJIT bring-up (JIT compiled OUT, FFI on), per-mod env + `pcall` isolation,
  builds each mod's `mod` table (mem/game/main/on_frame/hook/**ui**).  Lua is single-threaded on
  the engine thread (the UI thread never touches it — no lock).
- `ui.cpp` / `ui.h` — the ImGui/DX11 UI host (P5): the UI thread + companion window, the lock-free
  snapshot **triple buffer** + **atomic input slots**, the `mod.ui` binding + widgets.  `ui_build()`
  (engine thread, from the safepoint) records the snapshot; the UI thread replays it.  Only C++ TU.
- `mem.c` — guarded r/w (VirtualQuery), scan, module/reloc. `mem.h` guards used by bindings.
- `game_bindings.c` — the togglable `mod.game.*` registry (dynamic-resolve metatable + enable/disable).
- `sotes_bindings.c` — SotES roster+coordinates, profile-gated. **PORT-DEBT: heap-scan (task #7).**
- `executor.c` — WndProc bootstrap (positive `profile.window_class` match — see gotchas),
  safepoint hook (register-capture asm thunk), job queue + on_frame, launcher dismiss
  (`skip_launcher`/`windowed`), `exec_main_tid`/`exec_main_tid_ptr`/`exec_ti_mgr`/`exec_game_hwnd`.
  The safepoint also calls `ui_build()` (records the UI snapshot on the engine thread).
- `hooks.c` — the chained hook registry (both tiers): Tier-1 per-VA codegen thunk →
  `hook_dispatch` → observer chain; Tier-2 per-VA main-thread gate → FFI-closure dispatcher
  (the embedded `TIER2_LUA`) → typed chain. `g_hk[]` carries a `tier` (VA is T1 xor T2).
- `oss_mod_api.h` — the STABLE native-mod ABI (the only header a native mod needs): `OssApi`
  vtable, `OssHookCtx`, `OssModInit`.  `native_bridge.c` — fills that vtable from mem/executor/
  hooks and hands it to each native mod (`oss_api()`).
- `profile.c` — the C game profile (safepoint VA, **window class**, launcher class + control ids).
- `config.c` — `oss_loader.cfg` (key=value) reader. `minhook/` — vendored trampoline backend.
- `prof.c` — opt-in per-frame profiler (`profile=1`): times the safepoint / ui_build / roster on the
  engine thread, logs avg/max µs every 5 s.  `sotes_autoload.c` — dev save auto-load (menu-drive) +
  keepactive lives in `executor.c` (`exec_keepactive`, `exec_defer_fn`, `exec_sp_now`).

## P3 Tier-2 — typed hooks (LANDED)

`mod.hook.typed(va, "<ret> [__conv](args)", {pre=,post=})`. How it works (all in `hooks.c`
+ an embedded Lua chunk `TIER2_LUA`):

- **An FFI closure IS the detour** (the locked design). `ffi.cast(ctype, dispatch)` makes the
  Lua chain-dispatcher C-callable; MinHook points the VA at it (via the gate, below). The
  dispatcher marshals typed args into `ctx.args`, runs pre (may mutate args / set
  `ctx.blocked`+`ctx.ret`), calls the original via `ffi.cast(ctype, trampoline)` unless
  blocked, runs post (may rewrite `ctx.ret`). Each cb in `pcall` (fault → disabled + logged).
- **A tiny C main-thread GATE fronts the closure** (`gen_gate_thunk`): `mov eax,fs:[0x24]; cmp
  [&g_main_tid]; jne -> jmp tramp; jmp closure`. This is the keystone fix — a pure FFI-closure
  detour would enter the shared `lua_State` on *whatever* thread calls the target; an
  off-engine-thread call would corrupt every mod. The gate runs Lua only on the engine thread
  and otherwise runs the original untouched. (EAX/EFLAGS are dead at a call boundary; ECX/EDX/
  stack pass through, so thiscall `this` reaches the closure.)
- **Verified fact:** LuaJIT x86 callbacks handle `__cdecl/__stdcall/__thiscall/__fastcall`
  incl. the non-cdecl callee stack cleanup (`lj_ccallback.c:610-663`) — so a typed detour with
  the target's real convention is stack-correct. The sig string **must match the real
  convention + arg count** (wrong count → stack corruption; that's why Tier-1 is the default).
- **One tier per VA** (one MinHook per target): a unified `g_hk[]` with a `tier` field; Tier-1
  and Tier-2 refuse each other on the same VA (+log). Within a tier, mods chain.
- **Handles:** Tier-2 handles are `>= 0x40000000`; `mod.hook.remove` routes by handle. Removal
  marks the chain entry dead (MinHook + the closure stay installed — a later reclaim).

Host-proven exhaustively by `core/test/hook_typed_test.c` (`make -C core tests`). Not yet run
in-game — `examples/hook_typed/init.lua` is the smoke test (typed `kb_poll`, observe-only).

## P4 native-mod C ABI (LANDED)

`mods\<name>.dll` exports `int OssModInit(const OssApi *api)`; the loader resolves it
(`GetProcAddress`) and **defers it to the first safepoint** so it runs on the engine thread
like a Lua mod's init.  `OssApi` (`core/oss_mod_api.h` — the whole contract, versioned by
`abi_version` + `struct_size`, append-only) wires to the internal services via
`core/native_bridge.c`:

- `log`; guarded `mem_read/write/readable/writable/base/reloc/scan` (the AOB scan core was
  lifted out of `l_scan` into `mem_scan_aob`).
- `main_enqueue`/`on_frame` — the C-side of `mod.main`/`mod.on_frame`.  The executor's job
  queue + on_frame list now hold a tagged entry (Lua ref OR C fn+user).
- `hook_entry`/`hook_remove` — a native `OssHookEntryFn` joins the SAME per-VA Tier-1 chain as
  Lua `mod.hook.entry` (the `hcb` slot is tagged; the dispatcher builds the Lua ctx table only
  when a Lua cb is present, so a native-only hook needs no Lua).  Entry handles are now
  `0x10000 | (rec<<8) | slot` (never 0, since the ABI uses 0 = fail).
- A DLL with NO `OssModInit` still loads as a legacy standalone native mod (the trainer/voice
  today) — its DllMain runs, no ABI handoff.

Host-proven by `core/test/native_mod_test.c` (`>> NATIVE_ABI_OK`).  Example: `examples/
native_hello/` (a real `.dll` — `make -C examples/native_hello`).  **Not yet run in-game**
(host-verified; the trainer/voice migrations P6/P7 will exercise it live).

Native gaps (deferred, ABI-compatible to add): **no SEH isolation** for native cbs (a faulting
native mod can fault the game; Lua gets pcall); no native **Tier-2 typed** hooks; no native
`mod.game.*` accessor.

## P5 UI host (LANDED — `core/ui.cpp` + `ui.h`)

A loader-owned **companion window** (ImGui/DX11, its own thread + DX11 device — the trainer's
stock-example helper, HW→WARP, `R8G8B8A8`, DISCARD, `Present(1,0)`), fed by a **lock-free,
decoupled pipeline** to/from the engine thread.  (The first cut ran the mod.ui callbacks on the UI
thread under a shared "Lua Big Lock" that every engine-thread Lua entry point also took; in-game
that coupled the game's main thread to the UI thread + locked the hook hot-path → **visible lag**.
Replaced — the LBL is **gone**; Lua is single-threaded on the engine thread again.)

- **engine thread — `ui_build()`** (called from the executor safepoint, self-throttled ~30 Hz via
  `GetTickCount`): runs the mod.ui callbacks, which **record a plain-data snapshot** (per-surface
  command lists), and publishes it through a **lock-free triple buffer** (`g_ctl` CAS; latest-wins —
  an unconsumed frame is overwritten, never queued).  `SetEvent(g_dirty)` only when the snapshot
  actually changed.  This is the *only* place Lua runs for the UI.
- **UI thread** — **event-driven** (`MsgWaitForMultipleObjects` on input / the dirty event / a 250 ms
  heartbeat; renders only on input, a changed snapshot, or an active drag → **~zero idle cost**).
  Fetches the latest snapshot lock-free and **replays** it into ImGui; **never touches lua_State**.
  Interactions go back through **per-widget atomic slots** (`g_input[]`, id = `surf*MAX_W+ix`; value
  latest-wins, button clicks sticky-OR until the engine drains+clears them at the next build).
- **`mod.ui` unchanged** (immediate-mode on the surface): `panel`/`window` + `text[_disabled/
  _wrapped/_colored]`/`bullet`/`separator`/`spacing`/`same_line`/`button`/`small_button`/`checkbox`/
  `slider_int`/`slider_float`/`progress`.  Widgets are inert unless `g_building` (engine thread,
  inside `ui_build`), so a stray call elsewhere is a no-op not a crash.  A **UI-thread live-value
  cache** (`g_live[]`) keeps sliders/checkboxes smooth between the throttled builds — it re-seeds
  from the snapshot only when the mod changed the value AND the widget isn't `IsItemActive()`, so
  the engine's lagged sample can't fight a live drag.  ~1 build-cycle input latency, imperceptible.
  A faulting callback disables just that surface (`pcall`).  Toggle **F8** (`ui_key`); build rate
  `ui_hz` (default 30); `ui=0` disables.

**Nothing locks between the two threads** (the requirement): the snapshot is a lock-free triple
buffer and input is a fixed atomic-slot array — both latest-wins / drop-old, so a slow UI never
stalls the game and a busy game never stalls the UI, and stale data is dropped, not accumulated.

**In-game overlay: DONE** (Phase C — see that section).  It landed exactly as the P5 note predicted:
an **internal renderer-hook backend** (the engine-thread D3D11 present) consuming the SAME snapshot,
so `mod.ui` didn't change.  In takeover it replaces the companion window (which isn't started there);
the transparent-DWM-window approach stayed retired.

**Build:** `ui.cpp` + the ImGui TUs compile with **g++**; the whole DLL links with **g++ +
`-static -static-libgcc -static-libstdc++`** (the game's DLL path has no `libstdc++-6`/`libgcc_s`),
libs `-lgdi32 -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -limm32`.  DLL ~1.4 MB, imports only system
DLLs.  Host tests link the UI objects (via g++) and stay green.

**Validated:** `make -C core tests` green (`EXEC_OK` / `TYPED_HOOK_OK` / `NATIVE_ABI_OK` — the LBL
removal + the Tier-2 `make_dispatch` revert didn't regress).  `core/test/ui_smoke.c` drives the full
build→triple-buffer→replay path host-side (`make -C core ../build/ui_smoke.exe && (cd build &&
./ui_smoke.exe 3)` → `>> UI_SMOKE_OK`).  **In-game** (real EN-SE): the companion window came up on
the armed safepoint (`[ui] companion window up`, `ui_build` running every frame), no crash, no draw
faults.

UI gaps (deferred): no `mod.ui` in the native ABI (Lua-
only); no input-text / combo / tree widgets (curated set — extend as needed); no per-surface remove
(`ui.panel`/`window` return a handle for a future `ui.remove`); positional input keys can
misattribute one frame if a mod reorders its widgets (self-correcting).

## Open debts / follow-ups

- **#7 — roster heap-scan** (`PORT-DEBT(sotes-roster-heapscan)`): **mostly retired** (commit
  `perf: roster …`).  `sotes_bindings.c` now (a) does NOTHING at the title/menu (`render_root
  *(0x92dd38)==0` → no party → no scan: killed the ~65 ms menu walk → ~3 µs) and (b) in-scene scans
  the two **SE-verified render bands** (EFFECT `render_root+0x1160`×32, CHARACTER `render_root+0x11e0`
  ×128; ~160 reads) for the party codes; the full-heap walk stays only as an ultimate fallback (never
  hit in testing).  Validated in-game: identical roster (Arche/Sana/Stella), scene-transition cost
  200-600 ms → ~2.4 ms.  **Remaining (deferred):** the base-game "canonical" party array
  `map_obj+0x4030` doesn't reach the SE party via any `render_root+off` single/double indirection I
  could pin live (needs deeper SE memory tracing) — the bands suffice for now.
- **#12 — Tier-2 follow-ups:** (a) run the in-game smoke test (`examples/hook_typed`); (b)
  quiescent teardown — `remove` leaves the MinHook + FFI closure installed (dead-marked in the
  chain); reclaim on empty. (c) unify tiers on one VA (a typed hook that also runs Tier-1
  observers) — deferred; the xor rule is safe + simple for now.
- Executor's safepoint hook could become the first CLIENT of the `hooks.c` registry (unify);
  kept separate for now to not disturb the proven executor.

## Gotchas (hard-won)

- **RE addresses: the OpenSummoners `docs/decompiled/by-address/*.c` files are the BASE-GAME unpack
  (`sotes.unpacked.exe`, md5 `278bad…`) — a DIFFERENT edition than our target, the EN Special Edition
  (`sotes-ense-en.exe` == `sotes_en.exe` == `sotes-trainer-oss.exe`, md5 `3fe1bc9f…`).** The ddraw
  code is SHIFTED between editions: the RE labels `zdd_present` `0x5b8fc0`, but in EN-SE that address
  is mid-instruction (junk); the real present is **`0x5e1470`** (found by signature: the `+0x164`
  mode jump-table + the windowed `GetDC`+`BitBlt(0xcc0020)` case).  The mod loader's OTHER addresses
  (safepoint `0x437c70`, `render_root *0x92dd38`) happen to match because they were validated on EN-SE.
  **Lesson: never take a NEW hook VA straight from the by-address RE files — verify it's a real entry
  in EN-SE first** (`objdump -d -b pei-i386 --start-address=…`, or grep the `.text` disasm for the
  function's signature).  Editions also differ in surface format: EN-SE's back-buffer is **24bpp RGB888**
  (windowed, mode 2), not the base game's 16bpp.
- App-dir `version.dll` side-load works only from **NTFS**, not `\\wsl$` (Windows blocks remote-dir DLL load).
- **The game IDLES its input poll when its window is UNFOCUSED** — the safepoint (`0x437c70`) stops firing and the log freezes at `[exec] safepoint hook armed` (looks exactly like a hang/crash, but the game is fine at the title). **`keepactive` is now a DEFAULT-ON built-in** (re-posts `WM_ACTIVATEAPP(TRUE)` *only when the game isn't foreground*, so normal play is untouched; needed so the companion UI window doesn't freeze the game when clicked). Disable with `keepactive=0`. *This cost hours of misdiagnosis once — a known-good build "hung" the moment the window lost foreground.* If a run stops at "safepoint hook armed" with no `[mod]`/`[prof]` lines, suspect focus first, not the code.
- **Built-in defaults changed** (2026-07-19): `keepactive` and `skip_launcher` are now **default-ON**; the loader also arms the executor + UI **even with no `mods\` folder** (recognized game / `oss_loader.cfg` also trigger it) — the built-in QoL is independent of any mods being present. And it **only loads `mods\*.dll` that export `OssModInit`** (non-API DLLs are skipped, not run).
- **Reach gameplay + profile:** drop the **`autoload` mod** (`../sotes-mods/mods/autoload`) into `mods\` — it calls `mod.game.save.load` to drive the title/picker into a save (slot via its `[config]`, default newest).  (Was the `autoload=1` core flag — now a mod; the RE'd drive is `mod.game.save` in `sotes_bindings.c`.)  `profile=1` logs per-frame loader overhead every 5 s (`prof.c`). Real-gameplay numbers: `0x437c70` fires **~1000×/sec**, the loader is ~1% of the engine thread steady-state (safepoint + ui_build ≈ 9 µs), the UI machinery is negligible.
- The stock dir's `mods\sotes_trainer.dll` **also hooks `0x437c70`** — but the loader now **auto-skips any DLL that doesn't export `OssModInit`** (probed via `DONT_RESOLVE_DLL_REFERENCES`, so its DllMain never runs), so a stray legacy trainer/voice DLL in `mods\` can't fight the executor. One MinHook per target VA. (Port them to the API to load them.)
- WSL launch: `cmd /c start` blocks on the process-tree job → use `tools/dev-launch.sh` (PowerShell `Start-Process`).
- **Bootstrap must POSITIVELY match the game window** (`profile.window_class`, SotES `CLASS_LIZSOFT_SOTES`), never "first non-launcher top-level window": the game spawns an early TRANSIENT top-level window (+ a DirectShow `ActiveMovie` + IME windows) that the old heuristic latched and subclassed — then it got destroyed, silently dropping the subclass + the posted `WM_OSS_BOOT`, so the executor never armed (intermittent; looked like a Tier-2 failure but wasn't). Enumerate a live instance to fingerprint the real window: `tools/win-fingerprint.ps1 -TargetPid <pid>` (class/title/style/owner/size).
- Kills: exact pid via `tasklist.exe`/`taskkill.exe`, never `pkill` (siblings share the frida-server). After a kill, the exe file lock releases async — retry the `version.dll` restore.
- i686 mingw prefixes C symbols with `_`; the asm observer thunk uses `asm("name")` labels to match (see `executor.c`/`hooks.c`).
- LuaJIT is Lua **5.1** (`setfenv`, no `goto`/integer division semantics of 5.3).
- **UI: the two threads must NEVER share the lua_State — that was the LBL, and it lagged the game.**
  Lua runs only on the engine thread (`ui_build` at the safepoint records the snapshot). The UI
  thread consumes a lock-free **triple buffer** (snapshot) + writes **atomic input slots** — no lock,
  latest-wins, drops old data. If you extend the UI, keep this discipline: the UI thread reads the
  snapshot + writes atomics, and nothing more; anything Lua goes through `ui_build` on the engine
  thread. The mod.ui widgets are inert unless `g_building`, so a stray widget call off-build no-ops.
- **UI: `mod.ui` callbacks run on the ENGINE thread now** (inside `ui_build`, throttled ~30 Hz), not
  the UI thread — so they may read game state directly, but heavy work there costs the game frame
  (like `on_frame`); push it through `mod.main`. Input has ~1 build-cycle latency (imperceptible).
- **UI: linking is g++ now.** The final DLL + the host tests link with `i686-w64-mingw32-g++` and
  `-static-libstdc++` (the ImGui TUs pull in libstdc++). A plain-gcc link will fail to resolve C++
  symbols; a non-static link produces a DLL the game can't `LoadLibrary` (no libstdc++-6 on its path).
- **UI: the in-game overlay (takeover) and the companion window are MUTUALLY EXCLUSIVE — never run
  both.** They'd be two ImGui contexts on two threads racing ImGui's global current-context.  `ui_start`
  starts the companion ONLY when not in takeover; the overlay (engine thread, `ui_overlay_present`) runs
  ONLY in takeover.  Keep that invariant if you touch either.  Both consume the SAME snapshot — do NOT
  resurrect the transparent-DWM-window overlay; the snapshot abstraction is what makes the swap
  mod-invisible.  The overlay is MOUSE-ONLY (keyboard stays with the game); toggle is F10, not F8
  (F8 is SotES' in-game menu key).
