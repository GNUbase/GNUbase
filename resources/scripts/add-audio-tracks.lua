-- SPDX-License-Identifier: GPL-3.0-or-later
-- An example user script (docs/features/scripts.md): adds four audio tracks and names them. Whatever a script
-- changes is one undo step, named after the script, so one Undo removes all four tracks again.
for number = 1, 4 do
	local track = gnubase.track.add{type = "audio"}.track
	gnubase.track.set{track = track.id, name = "Take " .. number}
end
