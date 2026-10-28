-- SPDX-License-Identifier: GPL-3.0-or-later
-- The assertions of gnubase.test, the Lua test framework: what they accept, and what a failure says.
local test = gnubase.test

test.case("ok accepts every value but false and nil", function()
	test.ok(true)
	test.ok(0)
	test.ok("")
	test.raises(function() test.ok(false) end, "expected a true value, got false")
	test.raises(function() test.ok(nil, "the track") end, "the track: expected a true value, got nil")
end)

test.case("equal compares tables deeply and names the first difference", function()
	test.equal({1, 2, {name = "x"}}, {1, 2, {name = "x"}})
	test.equal(1, 1.0)
	test.raises(function() test.equal({1, 2, {name = "x"}}, {1, 2, {name = "y"}}) end,
		'at [3].name: expected "y", got "x"')
	test.raises(function() test.equal({1, 2}, {1, 2, 3}) end, "at [3]: expected 3, got nil")
	test.raises(function() test.equal({track = {id = 1}}, {}) end, "at track: expected nil, got {id = 1}")
	test.raises(function() test.equal({["two words"] = 1}, {}) end, 'at ["two words"]: expected nil, got 1')
	test.raises(function() test.equal("1", 1) end, 'expected 1, got "1"')
	test.raises(function() test.equal(0.1 + 0.2, 0.3) end, "expected 0.3, got 0.30000000000000004")
	test.raises(function() test.equal(1, 2, "track count") end, "track count: expected 2, got 1")
end)

test.case("notEqual fails for equal values and shows them", function()
	test.notEqual({1}, {2})
	test.notEqual(1, "1")
	test.raises(function() test.notEqual({list = {1, 2}, name = "x"}, {list = {1, 2}, name = "x"}) end,
		'expected a value other than {list = {1, 2}, name = "x"}')
end)

test.case("near compares numbers and lists of numbers within a tolerance", function()
	test.near(0.5, 0.501, 0.01)
	test.near({0.5, 0.25}, {0.501, 0.249}, 0.01)
	test.raises(function() test.near(0.5, 0.6, 0.01) end, "expected 0.6 within 0.01, got 0.5")
	test.raises(function() test.near({0.5, 0.25}, {0.5, 0.3}, 0.01) end, "at [2]: expected 0.3 within 0.01, got 0.25")
	test.raises(function() test.near({0.5}, {0.5, 0.3}, 0.01) end, "expected 2 values, got 1")
	test.raises(function() test.near(0 / 0, 0, 1) end, "expected 0 within 1, got") -- NaN is near nothing
	test.raises(function() test.near("0.5", 0.5, 1) end, 'expected a number or a list of numbers, got "0.5"')
end)

test.case("matches checks text against a Lua pattern", function()
	test.matches("Audio 01", "^Audio %d+$")
	test.raises(function() test.matches("MIDI 01", "^Audio") end, 'expected "MIDI 01" to match "^Audio"')
	test.raises(function() test.matches(42, "4") end, "expected a string, got 42")
end)

test.case("raises checks that a function fails, and returns the error", function()
	test.equal(test.raises(function() error("broken", 0) end, "broken"), "broken")
	test.raises(function() test.raises(function() end) end, "expected an error")
	test.raises(function() test.raises(function() error("other", 0) end, "broken") end,
		'expected an error containing "broken", got "other"')
end)

test.case("fail always fails", function()
	test.raises(function() test.fail("on purpose") end, "on purpose")
end)

test.case("a failure names the file and line of the failing call", function()
	local message = test.raises(function() test.equal(1, 2) end)
	test.matches(message, "tests/lua/framework%.lua:%d+: expected 2, got 1$")
end)

test.case("test cases are registered and run outside of test cases only", function()
	test.raises(function() test.case("nested", function() end) end, "inside a test case")
	test.raises(function() test.run() end, "inside a test case")
end)
