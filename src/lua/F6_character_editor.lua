-- F6_character_editor.lua: the F6 entry point while the Character Editor mod is on.
--
-- The host (src/core/fable2_character_editor.h) runs this file instead of F6.lua for the F6 key only
-- when FABLE2_CHARACTER_EDITOR=1 (launcher: General -> Mods -> Character Editor). It marks the mod as
-- on and runs the normal F6 menu, which then lists "Character Editor". With the mod off the host never
-- runs this file. docs/character-editor.md.
rawset(_G, "__recomp_character_editor", true)
local chunk, err = loadfile("scripts/recomp/F6.lua")
if type(chunk) ~= "function" then
  pcall(GUI.DisplayMessageBox, "F6: could not load scripts/recomp/F6.lua\n" .. tostring(err))
  return
end
chunk()
