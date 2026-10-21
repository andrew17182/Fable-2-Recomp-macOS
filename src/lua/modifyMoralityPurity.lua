-- modifyMoralityPurity.lua: set the hero's morality and purity to extreme values via a sub-menu.
--
-- Run from the F6 menu (F6.lua). Opens a game menu box with five presets. Selecting one applies it and
-- re-opens the box so you can keep testing; B returns to the F6 menu; F6 closes it (st.box returns nil).
--
-- Values come from the game's own DebugMenu: morality and purity both range -1000..1000, 0 = neutral.
--   * Debug.SetHeroMorality(1000 / -1000 / 0)  -> fully good / evil / neutral
--   * Debug.SetHeroPurity(1000 / -1000 / 0)    -> fully pure / corrupt / neutral
-- Both take a single value (no hero-entity arg) and set the hero directly.
--
-- Runs as a coroutine of the F6 menu (F6.lua's run_script); st.box waits across ticks. A yield cannot
-- cross pcall, so the loop stays outside pcall and each single game call is pcall'd.

local st = rawget(_G, "__f6_menu_state")

-- Morality/purity range (from the game's DebugMenu): -MAX..MAX, 0 = neutral.
local MAX = 1000

local function set_morality(value)
  pcall(Debug.SetHeroMorality, value)
end

local function set_purity(value)
  pcall(Debug.SetHeroPurity, value)
end

-- The menu items, in order. Each has a label and an apply function. "Neutral" resets both axes to 0.
local ITEMS = {
  { label = "Max Evil Morality", apply = function() set_morality(-MAX) end },
  { label = "Max Good Morality", apply = function() set_morality(MAX) end },
  {
    label = "Neutral",
    apply = function()
      set_morality(0)
      set_purity(0)
    end,
  },
  { label = "Max Evil Purity", apply = function() set_purity(-MAX) end },
  { label = "Max Good Purity", apply = function() set_purity(MAX) end },
}

local function run()
  if not (st and st.box) then
    pcall(GUI.DisplayMessageBox, "Morality / Purity: open it from the F6 menu.")
    return
  end
  local labels = {}
  for i = 1, #ITEMS do labels[i] = ITEMS[i].label end
  while true do
    local choice = st.box("Morality / Purity", labels)
    if choice == nil then return end -- F6 closed the menu
    if choice >= 1 and choice <= #ITEMS then
      ITEMS[choice].apply()
    else
      st.show = true -- B: back to the F6 menu
      return
    end
  end
end

run()
