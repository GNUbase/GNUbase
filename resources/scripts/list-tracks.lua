-- SPDX-License-Identifier: GPL-3.0-or-later
-- An example user script (docs/features/scripts.md): prints every track with its type into the script console. The
-- Lua API reference, which the console's Lua API Reference button opens, lists every gnubase function.
local project = gnubase.project.get()
if #project.tracks == 0 then
	print("The project has no tracks.")
end
for index, track in ipairs(project.tracks) do
	print(index .. ". " .. track.name .. " (" .. track.type .. ")")
end
