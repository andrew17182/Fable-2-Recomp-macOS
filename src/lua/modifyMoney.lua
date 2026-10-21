-- modifyMoney.lua: adjust the player's gold via a paginated sub-menu.
--
-- Run from the F6 menu (F6.lua). Opens a game menu box with +/- gold amounts. The box shows at most 5
-- entries, so the amounts are spread across pages:
--   * slot 1 is "Next" on every page (wraps around). It is slot 1 because the next box's cursor starts
--     there, so a page flip can reopen quickly (F6.lua);
--   * "Back" (page 1) returns to the F6 menu;
--   * B goes to the previous page (wrapping around to the last page);
--   * selecting an amount applies it and re-opens the sub-menu on the same page.
-- F6 closes it at any point (st.box returns nil).
--
-- Runs as a coroutine of the F6 menu (F6.lua's run_script); st.box waits across ticks. A yield cannot
-- cross pcall, so the loops stay outside pcall and each single game call is pcall'd.

local st = rawget(_G, "__f6_menu_state")

-- The gold amounts, in menu order. Positive = add, negative = subtract.
local AMOUNTS = { 100000, 10000, 1000, -1000, -10000, -100000 }

local function format_amount(amount)
  if amount > 0 then
    return "+" .. amount
  end
  return tostring(amount) -- negatives already carry a "-"
end

-- The content list: "Back" first, then the amounts (amount index = entry index - 1).
local ENTRIES = { "Back" }
for _, amount in ipairs(AMOUNTS) do
  ENTRIES[#ENTRIES + 1] = format_amount(amount)
end

-- Each page: "Next" in slot 1, then up to CONTENT_SIZE content entries.
local CONTENT_SIZE = 4
local PAGES = math.ceil(#ENTRIES / CONTENT_SIZE)

local function build_page(page_number)
  local page = { "Next" }
  for i = 1, CONTENT_SIZE do
    local entry = ENTRIES[i + page_number * CONTENT_SIZE]
    if not entry then break end
    page[#page + 1] = entry
  end
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
  local ok, hero = pcall(get_hero)
  if not ok or not hero then
    st.message("F6: could not get the hero")
    return
  end
  pcall(Money.Modify, hero, amount)
end

local function run()
  if not (st and st.box) then
    pcall(GUI.DisplayMessageBox, "Modify Money: open it from the F6 menu.")
    return
  end
  local page_number = 0
  while true do
    local page = build_page(page_number)
    local choice = st.box("Modify Money | Page " .. (page_number + 1) .. " of " .. PAGES, page)
    if choice == nil then return end -- F6 closed the menu
    if choice == 0 then
      page_number = (page_number - 1) % PAGES -- B: previous page
    elseif choice == 1 then
      page_number = (page_number + 1) % PAGES -- Next
    else
      local entry_index = (choice - 1) + page_number * CONTENT_SIZE
      if entry_index == 1 then
        st.show = true -- Back: return to the F6 menu
        return
      elseif entry_index >= 2 and entry_index <= #ENTRIES then
        apply_amount(AMOUNTS[entry_index - 1])
      else
        return
      end
    end
  end
end

run()
