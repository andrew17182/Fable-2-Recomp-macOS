-- F6 selector menu (custom Lua). Moved from F5 when F5 became quick save.
--
-- F6 (or number-row 6) re-runs this file. The first run registers ONE persistent per-frame script with
-- the GeneralScriptManager (the in-game Debug Menu's model); every press after that only flips flags
-- that the script reads on its next tick:
--   * no menu up       -> open the F6 menu;
--   * any menu, page, submenu or message of ours up -> close all of it (F6 always closes).
--
-- Menu boxes (GUI.DisplayMenuBox: a title plus at most 5 entries, B = 0) go through st.box below, which
-- every F6 script shares (character_editor.lua, modifyMoney.lua, modifyMoralityPurity.lua). Timing, as
-- measured 2026-10-21 (the script ticks at 15 Hz, 67 ms):
--   * A box opened less than 2 ticks after the previous one closed is dropped by the game (the script
--     then waits for a selection that never comes).
--   * A box opened less than about 7 ticks after the previous one closed keeps the old box's hovered
--     slot highlighted, and that stale highlight sticks (the "white highlight bug" the game's own
--     MultipageMenu works around with a 0.45 s wait).
--   * The new box's cursor starts on slot 1, so when slot 1 was the one chosen the stale highlight is
--     the cursor itself: a page flip on slot 1 is safe after 3 ticks.
-- So st.box waits SETTLE_FULL ticks after a close, or SETTLE_FAST when slot 1 was chosen. Paged menus
-- put their "More..." / "Next" entry in slot 1. (The old fixed 30-tick waits made every page switch
-- take about 2 s and returning to this menu about 5 s.)
--
-- Closing: the game's menu box has no close call (DisplayMenuBox returns no handle), so F6 dismisses
-- it the way a player does. This file RunScript()s the no-op F6_close_box.lua; the host's RunScript
-- probe sees it and taps B on the pad for two reads (src/core/fable2_f5_lua.h). The box posts
-- "cancelled", st.box returns nil, and every caller unwinds without opening anything else. Messages
-- (st.message) are menu boxes too, for the same reason.
--
-- Scripts run in a coroutine of their own, resumed once per tick, so a script error ends that script
-- (with a message) instead of the whole menu, and F6 keeps working.
--
-- To add a script, add { name = "...", path = "..." } to F6_MENU_ITEMS. `path` is relative to the game
-- VFS root (data/), forward slashes, e.g. "scripts/recomp/myScript.lua", and must be a line in
-- data/dir.manifest (the build adds src/lua/*.lua).

local F6_MENU_ITEMS = {
  { name = "Get Player Position", path = "scripts/recomp/getPlayerPos.lua" },
  { name = "Modify Money", path = "scripts/recomp/modifyMoney.lua" },
  { name = "Morality / Purity", path = "scripts/recomp/modifyMoralityPurity.lua" },
}
-- Character Editor mod (opt-in): listed only when the host ran F6_character_editor.lua, i.e. with
-- FABLE2_CHARACTER_EDITOR=1 (docs/character-editor.md). The page has room for 4 entries.
if rawget(_G, "__recomp_character_editor") then
  F6_MENU_ITEMS[#F6_MENU_ITEMS + 1] = { name = "Character Editor", path = "scripts/recomp/character_editor.lua" }
end

local CLOSE_BOX = "scripts/recomp/F6_close_box.lua"

-- Shared state that persists across F6 presses (the game uses one Lua state, so a global table
-- survives across separate runs of this file).
local st = rawget(_G, "__f6_menu_state")
if not st or st.version ~= 2 then
  st = { version = 2, show = false, active = false, closing = false, registered = false,
         tick = 0, ready_tick = 0, box_open = false, close_sent = -1000 }
  rawset(_G, "__f6_menu_state", st)
end

-- This press.
if st.active then
  -- A menu, page, submenu or message of ours is up: close everything.
  st.closing = true
  st.show = false
  if st.box_open then
    st.close_sent = st.tick
    pcall(RunScript, CLOSE_BOX)
  end
elseif st.show then
  st.show = false -- pressed twice before the menu came up: stay closed
else
  st.show = true
end

if not st.registered then
  st.registered = true

  local SETTLE_FULL = 9 -- ticks after a box closed (7 measured clean; margin)
  local SETTLE_FAST = 3 -- after slot 1 was chosen (2 measured clean; margin)
  local RESEND_TICKS = 8 -- re-tap B if a box has not closed this long after a close request

  -- One tick. Every yield of the menu and its scripts goes through here so st.tick counts ticks.
  function st.wait()
    coroutine.yield()
    st.tick = st.tick + 1
  end

  -- Shows a game menu box and waits for the choice. Returns the 1-based entry chosen, 0 for B, or nil
  -- when F6 closed the menu (callers return at once and open nothing else).
  function st.box(title, entries)
    while st.tick < st.ready_tick do
      if st.closing then return nil end
      st.wait()
    end
    if st.closing then return nil end
    local args = { title }
    for i = 1, #entries do args[i + 1] = entries[i] end
    local okid, last_id = pcall(MessageEvents.GetMostRecentMessageID)
    if not okid or last_id == nil then return 0 end
    if not pcall(GUI.DisplayMenuBox, unpack(args, 1, #args)) then return 0 end
    st.box_open = true
    local resent = 0
    local choice = 0
    while true do
      local ok, posted, msg = pcall(MessageEvents.IsMessagePosted, EMessageEventType.MESSAGE_EVENT_MENUBOX,
                                    last_id)
      if not ok then break end
      if posted then
        local okc, c = pcall(function() return msg:GetExtraDataAsNumber() end)
        if okc and type(c) == "number" then choice = c end
        break
      end
      if st.closing and st.tick - st.close_sent >= RESEND_TICKS then
        -- Still up after a close request (F6 came while the box was opening and not yet taking
        -- input): tap B again, twice at most, then stop waiting (a box the game dropped never posts).
        if resent >= 2 then break end
        resent = resent + 1
        st.close_sent = st.tick
        pcall(RunScript, CLOSE_BOX)
      end
      st.wait()
    end
    st.box_open = false
    st.ready_tick = st.tick + ((choice == 1) and SETTLE_FAST or SETTLE_FULL)
    if st.closing then return nil end
    return choice
  end

  -- Shows a message as a menu box with one "OK" entry, so F6 closes it like any other box. (The game's
  -- message box cannot be closed from here: GUI.DisplayMessageBox returns no handle, it ignores B, and
  -- the game holds this script while it is up.) Returns nil when F6 closed it.
  function st.message(text)
    return st.box(text, { "OK" })
  end

  -- Runs a script from the game VFS in a coroutine of its own, one resume per tick. The game's
  -- loadfile resolves paths relative to data/ (io is disabled).
  local function run_script(path)
    local chunk, err = loadfile(path)
    if type(chunk) ~= "function" then
      local why = (err and err ~= "") and tostring(err)
          or "file not found in the game VFS (is it in data/dir.manifest?)"
      st.message("F6: could not load " .. path .. "\n" .. why)
      return
    end
    local co = coroutine.create(chunk)
    while true do
      local ok, e = coroutine.resume(co)
      if not ok then
        pcall(cprint, "[F6] " .. path .. ": " .. tostring(e))
        st.message("F6: " .. path .. " failed\n" .. tostring(e))
        return
      end
      if coroutine.status(co) == "dead" then return end
      -- The script yielded (st.wait counted the tick): wait for the next tick here.
      coroutine.yield()
    end
  end

  -- The menu page: "Leave Menu", our scripts, then "NONE" up to the full 5 slots (the game's idiom: a
  -- complete item array for its per-frame item pass).
  local PAGE_SIZE = 5
  local function page()
    local entries = { "Leave Menu" }
    for i = 2, PAGE_SIZE do
      local item = F6_MENU_ITEMS[i - 1]
      entries[i] = (item and item.name) or "NONE"
    end
    return entries
  end

  -- Persistent per-frame body, driven as a coroutine by the GeneralScriptManager.
  local function update()
    while true do
      if st.show then
        st.active = true
        -- A script returns to this menu by setting st.show (its "Back").
        while st.show and not st.closing do
          st.show = false
          local choice = st.box("Script Menu", page())
          local item = choice and F6_MENU_ITEMS[choice - 1]
          if item then run_script(item.path) end
        end
        st.show = false
        st.closing = false
        st.active = false
      end
      st.wait()
    end
  end

  local menutab = {}
  setmetatable(menutab, menutab)
  menutab.__index = _G
  menutab._G = _G
  menutab.Update = update
  GeneralScriptManager.AddScript(menutab)
end
