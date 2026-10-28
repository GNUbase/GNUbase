-- SPDX-License-Identifier: GPL-3.0-or-later
-- User settings through the Lua API. ctest points GNUBASE_SETTINGS_DIR at the build's test output folder.
local test = gnubase.test

test.before(function() gnubase.settings.reset() end)

test.case("settings start with their defaults", function()
	test.equal(gnubase.settings.get{key = "ui.scale"}.value, 1)
	test.equal(gnubase.settings.get{key = "audio.blockSize"}.value, 256)
end)

test.case("a changed setting is announced and kept", function()
	gnubase.events() -- forgets what earlier commands announced
	test.equal(gnubase.settings.set{key = "ui.theme", value = "dark"}, {key = "ui.theme", value = "dark"})
	local event = gnubase.waitFor("settings.changed", 0)
	test.ok(event, "settings.changed")
	test.equal(event.data.values, {["ui.theme"] = "dark"})
	test.equal(gnubase.settings.get{key = "ui.theme"}.value, "dark")
end)

test.case("values are checked against the setting's schema", function()
	test.raises(function() gnubase.settings.set{key = "ui.scale", value = 10} end, "ui.scale must be at most 4")
	test.raises(function() gnubase.settings.set{key = "audio.blockSize", value = "big"} end,
		"audio.blockSize must be integer")
	test.equal(gnubase.settings.get{key = "ui.scale"}.value, 1)
end)

test.case("reset puts a setting back to its default", function()
	gnubase.settings.set{key = "ui.scale", value = 2}
	test.equal(gnubase.settings.reset{key = "ui.scale"}.values, {["ui.scale"] = 1})
	test.equal(gnubase.settings.get{key = "ui.scale"}.value, 1)
end)

test.case("settings.list describes every setting and where the file is", function()
	local list = gnubase.settings.list()
	test.equal(list.version, 1)
	test.ok(list.file:find("settings.json", 1, true), list.file)
	local keys = {}
	for _, setting in ipairs(list.settings) do
		keys[setting.key] = setting
	end
	test.equal(keys["ui.scale"].default, 1)
	test.equal(keys["ui.scale"].schema.maximum, 4)
end)
