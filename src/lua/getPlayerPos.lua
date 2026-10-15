-- getPlayerPos.lua: display the player's current (x, y, z) position in a
-- message box. Run directly, or via the F5 selector menu (F5.lua).
--
-- Position comes from the hero entity: QuestManager.HeroEntity:GetPosition()
-- (equivalently GetPlayerHero():GetPosition() or Debug.GetHero():GetPosition()).
--
-- GetPosition() returns a CVector3 userdata. Its numeric fields are not
-- accessible from Lua, but tostring(vec) yields "CVector3(x,y, z)", so we
-- parse that.

local function parse_vector3(text)
  -- CVector3(201.295682,91.704979, 52.825989) -> x, y, z
  local x, y, z = text:match("CVector3%((%-?%d+%.?%d*),%s*(%-?%d+%.?%d*),%s*(%-?%d+%.?%d*)")
  if not x then return nil end
  return tonumber(x), tonumber(y), tonumber(z)
end

-- NOTE: pcall only catches Lua errors. If the entity wrapped by `hero`
-- is NULL/dead, hero:GetPosition() faults INSIDE the C++ binding (a guest
-- access violation, not a Lua error), so the guards below are best-effort:
-- prefer a live hero reference and bail on anything that isn't a usable
-- entity userdata before touching GetPosition.
local function player_position()
  local hero = nil
  if type(QuestManager) == "table" then
    hero = QuestManager.HeroEntity
  end
  if not hero and type(Debug) == "table" and type(Debug.GetHero) == "function" then
    local ok, h = pcall(Debug.GetHero)
    if ok then hero = h end
  end
  if not hero then
    local ok, h = pcall(GetPlayerHero)
    if ok then hero = h end
  end
  if type(hero) ~= "userdata" then return nil end
  -- Optional liveness probe: only if the binding exposes IsValid(), and only
  -- trust a definitive false.
  if type(hero.IsValid) == "function" then
    local ok, valid = pcall(hero.IsValid, hero)
    if ok and valid == false then return nil end
  end
  local ok, pos = pcall(function() return hero:GetPosition() end)
  if not ok or type(pos) ~= "userdata" then return nil end
  return parse_vector3(tostring(pos))
end

local x, y, z = player_position()
if x then
  GUI.DisplayMessageBox(
      "Player position\nX: " .. string.format("%.3f", x) ..
      "\nY: " .. string.format("%.3f", y) ..
      "\nZ: " .. string.format("%.3f", z))
else
  GUI.DisplayMessageBox("Could not get player position")
end
