-- recomp_notify.lua: show a host-supplied message through the game's own
-- on-screen message system (the timed, button-less info box).
--
-- Run by the host (src/core/fable2_game_message.h) on the guest main thread via
-- CScriptManager::RunScript, the same path as the F6 Lua menu. The text comes
-- from recomp_notify_text.lua, which the host rewrites right before each run:
--   return "<text>", <lifetime seconds>
--
-- Safety gate: the message is only shown in normal gameplay. During front end,
-- loading, screen fades and while another UI is active it is declined, and the
-- host falls back to its own overlay. The script reports the outcome by running
-- one of two no-op files, which the host's RunScript dispatcher probe observes:
--   scripts/recomp/recomp_notify_shown.lua     the message was queued
--   scripts/recomp/recomp_notify_declined.lua  not shown (host uses overlay)

local function ack(what)
  pcall(RunScript, "scripts/recomp/recomp_notify_" .. what .. ".lua")
end

-- Progress mark (diagnostics): each mark is logged by the host as
-- "[notify] ack '...recomp_notify_mark.lua'". Order: 1 start, 2 text loaded,
-- 3 gameplay check passed, 4 fade checks done, 5 UI check done, 6 display call returned.
local function mark()
  pcall(RunScript, "scripts/recomp/recomp_notify_mark.lua")
end

local function query(f, ...)
  if type(f) ~= "function" then return nil end
  local ok, v = pcall(f, ...)
  if ok then return v end
  return nil
end

local function decline(why)
  pcall(cprint, "[recomp_notify] declined: " .. tostring(why))
  ack("declined")
end

mark()
local chunk = loadfile("scripts/recomp/recomp_notify_text.lua")
if type(chunk) ~= "function" then return decline("no text slot") end
local ok, text, lifetime = pcall(chunk)
if not ok or type(text) ~= "string" or text == "" then return decline("empty text") end
lifetime = tonumber(lifetime) or 2
mark()

if type(GUI) ~= "table" or type(QuestManager) ~= "table" or QuestManager.HeroEntity == nil then
  return decline("not in gameplay")
end
mark()
if query(GUI.IsScreenFading) or query(GUI.IsScreenFullyFaded) then
  return decline("screen fade / loading")
end
mark()
if query(GUI.IsUIActive) then return decline("UI active") end
mark()

local shown = false
if type(GUI.DisplayInfoBoxParams) == "function" and type(EDisplayBoxStyle) == "table" then
  shown = pcall(GUI.DisplayInfoBoxParams,
                { ShowYButton = false, ShowAButton = false, LifeTime = lifetime,
                  DisplayBoxStyle = EDisplayBoxStyle.DBS_INFO_BOX },
                text)
end
if not shown and type(GUI.DisplayInfoBox) == "function" then
  shown = pcall(GUI.DisplayInfoBox, text)
end
mark()
if shown then ack("shown") else decline("display API error") end
