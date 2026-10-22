-- character_editor.lua: the Character Editor mod (opt-in, off by default).
--
-- Listed in the F6 menu only when the mod is on (launcher: General -> Mods -> Character Editor,
-- env FABLE2_CHARACTER_EDITOR=1). The host then runs F6_character_editor.lua instead of F6.lua for the
-- F6 key; that file sets __recomp_character_editor and runs F6.lua, which adds the entry. With the mod
-- off nothing here is reachable. docs/character-editor.md.
--
-- Edits go through the game's own stat API, the same calls its quests use:
--   * morph (morality, purity, fat, strength, skill, will): Stats.ModifyX(hero, target - Stats.GetX(hero)),
--     the idiom the Music Box quest (qc235_musicboxaux) uses to restore the hero's morph exactly;
--   * gold: Money.Modify(hero, delta); renown: Stats.ModifyRenown(hero, delta).
-- Ranges are the game's own (its Startup/DebugMenu.txt sliders): morality and purity -1000..1000,
-- the body morphs 0..1. Gold and renown are kept at 0 or more (and below a sanity cap).
--
-- Safety:
--   * Backup first. Opening the editor asks the host to copy the save folder (RunScript of the no-op
--     character_editor_backup.lua, seen by the host's RunScript probe; src/core/fable2_character_editor.h).
--     The host answers in character_editor_status.lua. No answer, or a failed copy: the editor refuses
--     to change anything.
--   * Every target is clamped to its range, and only applied in normal gameplay (hero present).
--   * Changes live in memory until the game saves (autosave, F5 quick save or the pause menu), so
--     quitting without saving discards them.
--
-- Runs as a coroutine of the F6 menu (F6.lua's run_script), so the menu loops below wait across ticks
-- (st.box). A yield cannot cross pcall, so the loops stay outside pcall and each single game call is
-- pcall'd. F6 closes the editor at any point (st.box returns nil).

local st = rawget(_G, "__f6_menu_state")

local function call(f, ...)
  if type(f) ~= "function" then return false, nil end
  return pcall(f, ...)
end

local function hero()
  local h = nil
  if type(QuestManager) == "table" then h = QuestManager.HeroEntity end
  if h == nil and type(GetPlayerHero) == "function" then
    local ok, v = pcall(GetPlayerHero)
    if ok then h = v end
  end
  if type(h) ~= "userdata" then return nil end
  return h
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

local function round(v) return math.floor(v + 0.5) end

local function message(text)
  if st and st.message then st.message(text) else pcall(GUI.DisplayMessageBox, text) end
end

-- ---------------------------------------------------------------------------------------------------
-- Stats. Each has get(h) -> number and set(h, target) (target already clamped).

local function stat_api(getter, modifier)
  return {
    get = function(h)
      local ok, v = call(Stats and Stats[getter], h)
      if ok and type(v) == "number" then return v end
      return nil
    end,
    set = function(h, target, current)
      return call(Stats and Stats[modifier], h, target - current)
    end,
  }
end

local function gold_api()
  return {
    get = function(h)
      local ok, v = call(Money and Money.Get, h)
      if ok and type(v) == "number" then return v end
      return nil
    end,
    set = function(h, target, current)
      return call(Money and Money.Modify, h, target - current)
    end,
  }
end

local function percent(v) return tostring(round(v * 100)) .. "%" end
local function whole(v) return tostring(round(v)) end

-- kind "abs": value is the target; "rel": value is added to the current value.
local function opt(label, kind, value) return { label = label, kind = kind, value = value } end

local function morph_options()
  return {
    opt("None (0%)", "abs", 0.0), opt("Less (-25%)", "rel", -0.25), opt("Half (50%)", "abs", 0.5),
    opt("More (+25%)", "rel", 0.25), opt("Full (100%)", "abs", 1.0),
  }
end

local STATS = {
  {
    name = "Good / Evil", short = "Morality", lo = -1000, hi = 1000, show = whole,
    api = stat_api("GetMorality", "ModifyMorality"),
    options = {
      opt("Fully good (1000)", "abs", 1000), opt("More good (+250)", "rel", 250), opt("Neutral (0)", "abs", 0),
      opt("More evil (-250)", "rel", -250), opt("Fully evil (-1000)", "abs", -1000),
    },
  },
  {
    name = "Pure / Corrupt", short = "Purity", lo = -1000, hi = 1000, show = whole,
    api = stat_api("GetPurity", "ModifyPurity"),
    options = {
      opt("Fully pure (1000)", "abs", 1000), opt("More pure (+250)", "rel", 250), opt("Neutral (0)", "abs", 0),
      opt("More corrupt (-250)", "rel", -250), opt("Fully corrupt (-1000)", "abs", -1000),
    },
  },
  {
    name = "Gold", short = "Gold", lo = 0, hi = 10000000, show = whole, api = gold_api(),
    options = {
      opt("+100,000", "rel", 100000), opt("+10,000", "rel", 10000), opt("+1,000", "rel", 1000),
      opt("-1,000", "rel", -1000), opt("-10,000", "rel", -10000),
    },
  },
  {
    name = "Renown", short = "Renown", lo = 0, hi = 10000000, show = whole,
    api = stat_api("GetRenown", "ModifyRenown"),
    options = {
      opt("+10,000", "rel", 10000), opt("+1,000", "rel", 1000), opt("+100", "rel", 100),
      opt("-1,000", "rel", -1000), opt("-10,000", "rel", -10000),
    },
  },
  {
    name = "Fat / Thin", short = "Fat", lo = 0, hi = 1, show = percent,
    api = stat_api("GetFat", "ModifyFat"),
    options = {
      opt("Thinnest (0%)", "abs", 0.0), opt("Thinner (-25%)", "rel", -0.25), opt("Middle (50%)", "abs", 0.5),
      opt("Fatter (+25%)", "rel", 0.25), opt("Fattest (100%)", "abs", 1.0),
    },
  },
  {
    name = "Muscle (Strength)", short = "Strength", lo = 0, hi = 1, show = percent,
    api = stat_api("GetStrength", "ModifyStrength"), options = morph_options(),
  },
  {
    name = "Height (Skill)", short = "Skill", lo = 0, hi = 1, show = percent,
    api = stat_api("GetSkill", "ModifySkill"), options = morph_options(),
  },
  {
    name = "Will lines (Will)", short = "Will", lo = 0, hi = 1, show = percent,
    api = stat_api("GetWill", "ModifyWill"), options = morph_options(),
  },
}

-- Applies one option to a stat. Returns a short result string.
local function apply(stat, option)
  local h = hero()
  if not h then return "no hero (not in gameplay)" end
  local current = stat.api.get(h)
  if current == nil then return "could not read " .. stat.short end
  local target = option.value
  if option.kind == "rel" then target = current + option.value end
  target = clamp(target, stat.lo, stat.hi)
  if stat.show == whole then target = round(target) end
  if target == current then return stat.short .. " already " .. stat.show(current) end
  local ok = stat.api.set(h, target, current)
  if not ok then return "the game refused the " .. stat.short .. " change" end
  local now = stat.api.get(h)
  return stat.short .. ": " .. stat.show(current) .. " -> " .. stat.show(now or target)
end

-- ---------------------------------------------------------------------------------------------------
-- Backup handshake with the host (src/core/fable2_character_editor.h).

local function read_status()
  local chunk = loadfile("scripts/recomp/character_editor_status.lua")
  if type(chunk) ~= "function" then return nil, nil end
  local ok, n, result = pcall(chunk)
  if not ok then return nil, nil end
  return tostring(n), result
end

-- The count is compared as a string: the game's Lua numbers are single-precision floats, too coarse
-- for the host's clock-seeded counter.
local function backup_save()
  local before = read_status()
  local ok = pcall(RunScript, "scripts/recomp/character_editor_backup.lua")
  if not ok then return false, "RunScript failed" end
  local after, result = read_status()
  if after == nil or after == before then return false, "the host did not answer (is the mod on?)" end
  if result ~= "ok" then return false, tostring(result) end
  return true, nil
end

-- ---------------------------------------------------------------------------------------------------
-- Menu boxes: F6.lua's shared st.box (the game's GUI.DisplayMenuBox, a title plus at most 5 entries).
-- It returns the chosen entry, 0 for B, or nil when F6 closed the menu: then return at once.

local function stat_title(stat)
  local h = hero()
  local v = h and stat.api.get(h)
  if v == nil then return stat.name end
  return stat.name .. ": " .. stat.show(v)
end

-- Returns false when F6 closed the menu.
local function edit_stat(stat)
  local labels = {}
  for i, o in ipairs(stat.options) do labels[i] = o.label end
  while true do
    local choice = st.box(stat_title(stat), labels)
    if choice == nil then return false end
    if choice < 1 or choice > #stat.options then return true end  -- B: back to the stat list
    local result = apply(stat, stat.options[choice])
    pcall(cprint, "[character_editor] " .. result)
  end
end

-- Main menu: two pages of four stats. "More..." flips the page; it is slot 1, where the next box's
-- cursor starts, so the flip can reopen quickly (F6.lua). B leaves. Returns false when F6 closed it.
local PAGE = 4
local function main_menu()
  local page = 0
  local pages = math.ceil(#STATS / PAGE)
  while true do
    local entries, map = { "More..." }, {}
    for i = 1, PAGE do
      local stat = STATS[page * PAGE + i]
      if stat then
        entries[#entries + 1] = stat.name
        map[#entries] = stat
      end
    end
    local choice = st.box("Character Editor (" .. (page + 1) .. "/" .. pages .. ")", entries)
    if choice == nil then return false end
    if choice == 1 then
      page = (page + 1) % pages
    elseif map[choice] then
      if not edit_stat(map[choice]) then return false end
    else
      return true  -- B
    end
  end
end

local function run()
  if not (st and st.box) then
    message("Character Editor: open it from the script menu (6).")
    return
  end
  if not hero() then
    message("Character Editor: only available in normal gameplay.")
    return
  end
  local ok, why = backup_save()
  if not ok then
    message("Character Editor: could not back up the save, so nothing was changed.\n" .. tostring(why))
    return
  end
  if main_menu() then st.show = true end  -- B: back to the F6 menu
end

run()
