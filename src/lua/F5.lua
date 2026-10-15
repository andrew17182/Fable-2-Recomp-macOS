-- F5.lua is no longer run by default.
--
-- F5 / number-row 5 is now a native quick save (src/core/fable2_quick_save.h): it
-- saves exactly like the pause menu's "Save > Save and Continue". The Lua script
-- selector menu that used to live here moved to F6 / number-row 6 (F6.lua).
--
-- This file stays (it is listed in data/dir.manifest) and only runs when the
-- `f5_lua_path` cvar points at it. Do not call AutoSaveGame() from a host-run
-- script: tested 2026-10-03, it crashed the game once and hung it on a loading
-- screen with a half-written mainsave.bin the other time.
pcall(GUI.DisplayMessageBox,
      "F5 is now a quick save. The script menu moved to F6.")
