-- modifyMoralityPurity.lua: set the hero's morality and purity to extreme
-- values via a sub-menu.
--
-- Run via the F5 selector menu (F5.lua). Opens a native menu box with five
-- presets. Selecting one applies it and re-opens the box so you can keep
-- testing; B returns to the F5 menu.
--
-- Values come from the game's own DebugMenu: morality and purity both range
-- -1000..1000, with 0 = neutral.
--   * Debug.SetHeroMorality(1000 / -1000 / 0)  -> fully good / evil / neutral
--   * Debug.SetHeroPurity(1000 / -1000 / 0)    -> fully pure / corrupt / neutral
-- Both take a single value (no hero-entity arg) and set the hero directly.
--
-- This script runs inside the F5 menu's per-frame coroutine (F5.lua's
-- run_script calls the loaded chunk directly, NOT inside pcall), so
-- coroutine.yield() resumes that coroutine next frame. A yield cannot cross a
-- C-call boundary, so the yield loops live directly in the coroutine; each
-- individual (non-yielding) API call is pcall'd instead.

local st = rawget(_G, "__f5_menu_state")

-- Morality/purity range (from the game's DebugMenu): -MAX..MAX, 0 = neutral.
local MAX = 1000

local function set_morality(value)
  pcall(Debug.SetHeroMorality, value)
end

local function set_purity(value)
  pcall(Debug.SetHeroPurity, value)
end

-- The menu items, in order. Each has a label and an apply function. "Neutral"
-- resets both axes to 0.
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

-- Let a just-closed menu box fully settle before re-opening it (the "white
-- highlight bug": the previously-hovered slot stays highlighted if a new box
-- opens before the close animation finishes). ~0.5 s at 60 fps clears it.
local function settle_box()
  for _ = 1, 30 do
    coroutine.yield()
  end
end

-- Open the sub-menu and wait for a selection. Returns the 1-based item index
-- (1..#ITEMS), or 0 if cancelled (B) or timed out.
local function open_menu()
  local args = { "Morality / Purity" }
  for i = 1, #ITEMS do
    args[i + 1] = ITEMS[i].label
  end
  local last_id = MessageEvents.GetMostRecentMessageID()
  GUI.DisplayMenuBox(unpack(args, 1, #args))
  local timeout = 600
  local choice
  while timeout > 0 do
    local posted, message = MessageEvents.IsMessagePosted(
        EMessageEventType.MESSAGE_EVENT_MENUBOX, last_id)
    if posted then
      choice = message:GetExtraDataAsNumber()
      break
    end
    timeout = timeout - 1
    coroutine.yield()
  end
  if choice == nil then
    return 0 -- timed out: cancel
  end
  coroutine.yield() -- let the menu box finish closing.
  return choice
end

-- Drive the sub-menu as a loop: apply the chosen preset and re-open; return to
-- the F5 menu on B or a cancel/timeout.
local function run()
  -- Let the F5 menu box fully tear down before opening the sub-menu box (the
  -- game drops a box opened on top of a live/tearing-down one).
  for _ = 1, 30 do
    coroutine.yield()
  end
  while true do
    local choice = open_menu()
    if choice >= 1 and choice <= #ITEMS then
      ITEMS[choice].apply()
      -- Let the box settle before re-opening it.
      settle_box()
      -- loop: re-open the sub-menu
    else
      -- B (0) or timeout: return to the F5 menu.
      if st then
        st.show = true
      end
      return
    end
  end
end

run()
