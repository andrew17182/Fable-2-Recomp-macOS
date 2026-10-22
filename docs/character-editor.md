# Character Editor (mod, phase 1, 2026-10-15)

An opt-in mod that edits the hero in a running game through the game's own Lua stat API. It is off by
default. When it is off, the script menu (6, the old F6 menu) and the game are unchanged.

- **Turn it on:** Launcher → Settings → Mods → "Character Editor (script menu)", or
  `FABLE2_CHARACTER_EDITOR=1` (`tools/fable2_dev_session.py launch ... --env FABLE2_CHARACTER_EDITOR=1`).
- **Use it:** in gameplay, press 6 (or F7) and pick "Character Editor". It is a game menu box, so it works
  with a controller: A selects, B goes back. It has two pages; "More..." flips between them.
- **Keep or discard:** edits change the live game. They reach the save only when the game saves (autosave,
  quick save (5) or the pause menu). Quit without saving and they are gone.

## What it edits (phase 1)

| Field | Menu | Range (clamped) | Game call |
|---|---|---|---|
| Good / evil | Fully good, +250, Neutral, −250, Fully evil | −1000 … 1000 | `Stats.ModifyMorality(hero, Δ)` |
| Pure / corrupt | Fully pure, +250, Neutral, −250, Fully corrupt | −1000 … 1000 | `Stats.ModifyPurity(hero, Δ)` |
| Gold | +100k, +10k, +1k, −1k, −10k | 0 … 10,000,000 | `Money.Modify(hero, Δ)` |
| Renown | +10k, +1k, +100, −1k, −10k | 0 … 10,000,000 | `Stats.ModifyRenown(hero, Δ)` |
| Fat / thin | 0%, −25%, 50%, +25%, 100% | 0 … 1 | `Stats.ModifyFat(hero, Δ)` |
| Muscle (strength morph) | 0%, −25%, 50%, +25%, 100% | 0 … 1 | `Stats.ModifyStrength(hero, Δ)` |
| Height (skill morph) | same | 0 … 1 | `Stats.ModifySkill(hero, Δ)` |
| Will lines (will morph) | same | 0 … 1 | `Stats.ModifyWill(hero, Δ)` |

Δ = clamped target − the current value (`Stats.GetX` / `Money.Get`). The game's Music Box quest
(`qc235_musicboxaux`) uses this same idiom to restore the hero's morph exactly. Each submenu title shows
the current value, so you can see what you are changing.

The ranges come from the game's own developer menu (`data/scripts/Startup/DebugMenu.txt`): morality and
purity −1000 … 1000, and the body morphs as 0 … 100% (`/ 100`). The gold and renown caps are ours. The
game's Lua numbers are single-precision floats, which are exact only up to 16.7 million, so both caps
stay below that.

## How it works

1. **The F6 entry.** With the mod on, the host runs `scripts/recomp/F6_character_editor.lua` for the F6 key
   instead of `F6.lua` (`src/core/fable2_character_editor.h`, `F6Path`). That file sets
   `__recomp_character_editor` and runs `F6.lua`, which then adds the "Character Editor" entry. A custom
   `f6_lua_path` is left alone. With the mod off, F6 runs `F6.lua` as before, and the entry is never added.
2. **Backup first.** Each time the editor opens, it calls `RunScript("scripts/recomp/character_editor_backup.lua")`,
   a no-op file. The host's RunScript probe (`LuaBind_RunScript_82806168` in `src/core/fable2_f5_lua.h`) sees
   the path and copies every save title folder (`saves/<profile>/4D5307F1`: hero slots and headers) to:
   - `$FABLE2_CHARACTER_EDITOR_BACKUP_DIR/<timestamp>/`, when that variable is set. Fable II Launcher play
     sessions set it to `out/save-backups/character-editor`, which outlives the pruned session folders.
   - Otherwise `<state dir>/character-editor-backups/<timestamp>/`. The player app's state dir is in
     Application Support. Test sessions keep their backups inside the session.

   It keeps the newest 20 backups. The copy happens on the guest main thread inside that RunScript call and
   takes about 2 ms. The host then writes its answer to `data/scripts/recomp/character_editor_status.lua`
   (a fixed 256-byte slot, like the in-game message slot) as `return "<count>", "ok"`. The editor goes on
   only if the count changed and the result is `"ok"`. Otherwise it shows "could not back up the save, so
   nothing was changed" and stops.
   - The count is a string because the game's Lua numbers are floats: two counts near the Unix time compare
     as equal. The first build compared numbers, and the third open in a session was refused.
3. **Edits.** Each pick calls the game API (above) through `pcall`, then reads the value back for the
   title. The game reacts the way it does to normal play:
   - Fully evil showed "You have performed an evil deed...".
   - Fattest awarded the "Salad Dodger" title (a game box that our menu correctly queued behind).
   - +100,000 gold showed the "You can now afford to buy a new home in Oakfield!" hint.
   - The morph texture is rebuilt by the game, and the hero's body changes at once.

Files:
- `src/lua/character_editor.lua` (the editor);
- `src/lua/F6_character_editor.lua`, `character_editor_backup.lua`, `character_editor_status.lua`;
- `src/lua/F6.lua` (the conditional entry);
- `src/core/fable2_character_editor.h`;
- `src/core/fable2_f5_lua.h` (two hook lines);
- `src/core/fable_2_app.h` (data root);
- launcher: `tools/macos/Shared/LaunchSettings.swift` (flag + env), `SettingsViews.swift` (Mods section);
- `tools/fable2_dev_session.py` (backup folder for play sessions).

## Why live Lua and not offline save editing

| | Live, game Lua API (chosen) | Offline save editing |
|---|---|---|
| Faithful | The game's own setters. Derived state stays consistent: morph texture, titles, hints, opinion icons. | Bytes edited behind the game's back. |
| Morph | The game re-bakes `texturemorphs.bin` (594 KB, the hero's morph textures) on the next save. | Morph values would disagree with the stored morph textures until the game rebuilt them. |
| Format work | None | `mainsave.bin` is LZ-compressed and chunked (a Bower Lake save decodes to 187 chunks / 1.52 MB, `docs/archive/area-load-profile-20261008.md`). Field layout, checksums and the re-encoder are unknown; no tracked decoder exists. |
| Risk | Values are clamped. Nothing is written until the game saves. A backup is made first. | A wrong byte can make a save unloadable. |
| Reach | Everything the game's Lua can call (below) | In principle everything, after a large reverse-engineering effort |

Offline editing is not needed for any field we want; it would only matter for editing a save without
running the game. It was not attempted.

## Reachable through the same API (not in phase 1)

All of these are in the game's own scripts (`FABLE2_LUA_API.md`; the bytecode of the shipped quest scripts
and `DebugMenu.txt`):

- **Body:**
  - `Stats.GetTan/ModifyTan` (0 … 1) and `Stats.GetHeight/ModifyHeight`;
  - `Debug.SetHeroAge(18 … 60)`, which has no getter in the scripts;
  - `Debug.SetMorphStrengthAtWhichFatCapped(0 … 1)`: the fat morph is capped by the strength morph, so a
    very muscular hero shows less fat.
- **Abilities:** `Stats.SetHeroAbilityLevel(hero, EHeroAbilityType.HERO_ABILITY_*, 0 … 5)` and
  `Stats.GetHeroAbilityLevel`. The 16 types are listed in DebugMenu.txt: physique, toughness, styles,
  accuracy, speed and the 8 spells.
  - `Experience.Modify(hero, EExperienceType.EXPERIENCE_STRENGTH|SKILL|WILL|GENERAL, n, false)`.
- **Appearance:**
  - Hair, beards, clothes and tattoos: `ScriptFunction.SetHeroWearingItemOfType(<ObjectX>, true, true)`
    and `AppearanceModifierManager.AddAndReplaceItemOfRecordID`;
  - Inventory: `Inventory.AddItemOfType(hero, <ObjectX>)`;
  - Dyes: `Debug.SetHeroWornItemDye(<clothing>, true, <ObjectInventoryDye…>)`;
  - Bulk unlocks: `Debug.AddAllHairstyles/AddAllTattoos/AddAllDyes/AddAllClothing`.
  The record names are in the quest scripts (e.g. `ObjectHairDreadlocks`, `ObjectTattooTorsoZ…`,
  `ObjectInventoryDyeClothingBasicBlue02`).
- **Titles:** `Stats.AddUnlockedTitle`, `Stats.SetTitleAsFreeToBuy`.

## Verified (2026-10-15, slot agent-3, save `hero1-dog-live_2026-10-07_18-49-47`, Thag's Cave)

Evidence: `out/validation/character-editor/` (KEEP). Before/after crops: `crop-01-before.png`,
`crop-21-after-fat-muscle.png` and `crop-49-thin-after.png`.

1. **Mod on, session `20261015-101535-7b62ca35`.**
   - F6 listed "Character Editor"; opening it made a backup. The backup is the full slot (6 files), as the
     log shows.
   - Edits: morality 15 → −1000, purity 0 → −1000, fat 0 → 100%, muscle 0 → 100%. The hero visibly grew
     heavier and more muscular and turned paler (`crop-01-before.png` vs `crop-21-after-fat-muscle.png`).
   - Clamp: "More evil (−250)" at −1000 stayed at −1000.
   - F5 saved the library entry `hero1-dog-live_2026-10-15_10-22-30`.
2. **Reload with the mod on, session `20261015-102331-2b34964b`.**
   - The hero loaded with the edited body; morality −1000 and fat 100% read back.
   - Gold 100 → −1,000 clamped to 0 → +100,000. Renown 0 → 1,000.
   - Fat back to 0% and muscle to 0%: thin again (`crop-49-thin-after.png`).
   - F5 saved `…10-26-22`.
3. **Reload with the mod off, session `20261015-102729-644dea42`.**
   - F6 showed the original menu ("NONE" in the 4th slot).
   - No `[character-editor]` log line and no backup folder.
   - The hero walked normally; F5 saved `…10-28-25`.
4. **Reload with the mod on, session `20261015-102826-6cc56366`.**
   - Gold 100,000 and renown 1,000 had survived the mod-off re-save.
   - The editor opened three times in a row, each with a backup (the string-count fix).

Every session was closed normally, and only other agents' games were left running. No live save or
Application Support file was touched: all saves were library copies with `--target-slot none`.

## Limits

- **In-game only.** The editor needs a running game in normal gameplay (it refuses without a hero). There is
  no launcher-side editor for a save on disk.
- **Menu box UI.** The game's box shows 5 entries, so values are presets and steps, not sliders.
- **Morph is not abilities.** Muscle/height/Will lines change the look only. Buying an ability later adds to
  the morph as usual. The fat morph is capped by the strength morph (see above).
- **Quest checks may fire.** Renown and morality feed quest checks such as renown gates and the
  good/evil comments, the same as earning them in play. A big jump can trigger those reactions at once.

## Next steps (phase 2)

1. **Appearance:**
   - hair and beard style;
   - tattoos and make-up;
   - clothing and dye, through `SetHeroWearingItemOfType` / `SetHeroWornItemDye`, with a curated list of
     record names from the scripts.
2. **Abilities and experience** (`SetHeroAbilityLevel`, `Experience.Modify`); tan, height and age.
3. **A nicer UI:**
   - an in-game overlay panel with sliders that sends targets to the same Lua through a host-written slot
     file; or
   - a launcher panel that applies targets once at load.
4. **A mod framework** shared with the randomizer: per-save opt-in, the backup handshake, and a "Mods"
   preset group.
