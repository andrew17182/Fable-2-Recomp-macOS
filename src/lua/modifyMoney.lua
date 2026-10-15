-- modifyMoney.lua: adjust the player's gold via a paginated sub-menu.
--
-- Run via the F5 selector menu (F5.lua). Opens a native menu box with
-- +/- gold amounts. Because the game's menu box shows at most 5 entries per
-- page and we have more than that, the amounts are spread across pages:
--   * entry 1 is "Back" (the exit button, page 1 only) — selecting it returns
--     to the F5 menu,
--   * the LAST slot on every page is a "Next" entry that advances to the next
--     page (wrapping around to page 1),
--   * B goes to the PREVIOUS page (wrapping around to the last page),
--   * selecting an amount applies it and re-opens the sub-menu on the same
--     page, so you can keep adjusting.
--
-- This script runs inside the F5 menu's per-frame coroutine (F5.lua's
-- run_script calls the loaded chunk directly, NOT inside pcall), so
-- coroutine.yield() resumes that coroutine next frame — the sub-menu loop
-- below is driven frame-by-frame the same way the F5 menu itself is. A yield
-- cannot cross a C-call boundary, so the yield loops live directly in the
-- coroutine; each individual (non-yielding) API call is pcall'd instead.

local st = rawget(_G, "__f5_menu_state")

-- The gold amounts, in menu order. Positive = add, negative = subtract.
local AMOUNTS = { 100000, 10000, 1000, -1000, -10000, -100000 }

local function format_amount(amount)
  if amount > 0 then
    return "+" .. amount
  end
  return tostring(amount) -- negatives already carry a "-"
end

-- The full content list. Entry 1 is the "Back" exit button; the rest are the
-- gold amounts. This shifts every amount up by one, so an amount's index is
-- (entry_index - 1).
local ENTRIES = { "Back" }
for _, amount in ipairs(AMOUNTS) do
  ENTRIES[#ENTRIES + 1] = format_amount(amount)
end

-- Page geometry: the box shows at most 5 slots. Each page holds up to 4
-- content entries followed by a "Next" entry as its LAST slot (so a page is
-- never left with a trailing empty slot).
local CONTENT_SIZE = 4

-- Highest 0-based page index: content is CONTENT_SIZE entries per page.
local total_pages = math.ceil(#ENTRIES / CONTENT_SIZE) - 1

-- Build one page for a 0-based page number: the page's content entries
-- (up to CONTENT_SIZE, no padding) followed by a "Next" entry as the LAST
-- slot. Returns the page; the "Next" slot is #page.
local function build_page(page_number)
  local page = {}
  for i = 1, CONTENT_SIZE do
    local entry = ENTRIES[i + page_number * CONTENT_SIZE]
    if not entry then
      break
    end
    page[#page + 1] = entry
  end
  page[#page + 1] = "Next"
  return page
end

local function get_hero()
  local hero = QuestManager.HeroEntity
  if not hero then
    hero = (Debug and Debug.GetHero) and Debug.GetHero() or GetPlayerHero()
  end
  return hero
end

local function apply_amount(amount)
  local hero = get_hero()
  if not hero then
    pcall(GUI.DisplayMessageBox, "F5: could not get the hero")
    return
  end
  pcall(Money.Modify, hero, amount)
end

-- Let a just-closed menu box fully settle before re-opening it. The game
-- re-highlights whatever slot was hovered when the box closed if a new box is
-- opened before the close animation finishes (the "white highlight bug");
-- ~0.5 s at 60 fps clears it. Used on page changes, which reopen the box in
-- place (the amount/back paths already settle via run()'s teardown wait).
local function settle_box()
  for _ = 1, 30 do
    coroutine.yield()
  end
end

-- Open the sub-menu (starting on `start_page`) and drive it across pages until
-- the user makes a real selection. Returns (code, page_number) where code is:
--   -1          -> "Back" was chosen (return to the F5 menu)
--    1..#AMOUNTS -> the amount index to apply
--    0          -> timed out / unexpected slot (treat as cancel)
-- B (box choice 0) goes to the previous page; the "Next" entry (the box's last
-- slot) goes to the next page; both wrap around. The wait is bounded so a
-- box that never registers a selection can't wedge the F5 coroutine.
local function open_money_menu(start_page)
  local page_number = start_page or 0
  while true do
    local page = build_page(page_number)
    local next_slot = #page -- the "Next" entry is always the last slot
    local title = "Modify Money | Page "
        .. (page_number + 1) .. " of " .. (total_pages + 1)
    local args = { title }
    for i = 1, #page do
      args[i + 1] = page[i]
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
      return 0, page_number -- timed out: cancel
    end
    coroutine.yield() -- let the menu box finish closing.

    if choice == 0 then
      -- B: previous page (wrap to the last).
      if page_number == 0 then
        page_number = total_pages
      else
        page_number = page_number - 1
      end
      settle_box()
    elseif choice == next_slot then
      -- "Next": next page (wrap to the first).
      if page_number == total_pages then
        page_number = 0
      else
        page_number = page_number + 1
      end
      settle_box()
    else
      -- A content selection (slots 1..next_slot-1): map the box slot back to
      -- the absolute entry index for this page.
      local entry_index = choice + page_number * CONTENT_SIZE
      if entry_index == 1 then
        return -1, page_number -- "Back"
      elseif entry_index >= 2 and entry_index <= #ENTRIES then
        return entry_index - 1, page_number -- an amount
      else
        return 0, page_number -- unexpected slot: cancel
      end
    end
  end
end

-- Drive the sub-menu as a loop: apply amounts and re-open on the same page;
-- return to the F5 menu on "Back" or a cancel/timeout.
local function run()
  -- Let the F5 menu box fully tear down before opening the sub-menu box (the
  -- game drops a box opened on top of a live/tearing-down one).
  for _ = 1, 30 do
    coroutine.yield()
  end
  local page_number = 0
  while true do
    local choice, page = open_money_menu(page_number)
    page_number = page
    if choice == -1 then
      -- "Back": return to the F5 menu.
      if st then
        st.show = true
      end
      return
    elseif choice >= 1 and choice <= #AMOUNTS then
      apply_amount(AMOUNTS[choice])
      -- Let the menu box fully tear down before re-opening it (the game's
      -- menu box glitches if re-opened before its close animation finishes).
      for _ = 1, 30 do
        coroutine.yield()
      end
      -- loop: re-open the sub-menu on the same page
    else
      -- cancel/timeout: return to the F5 menu.
      if st then
        st.show = true
      end
      return
    end
  end
end

run()
