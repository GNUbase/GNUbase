-- SPDX-License-Identifier: GPL-3.0-or-later
-- gnubase.test, GNUbase's Lua test framework (#206): test cases, assertions, fixtures and golden files. The runtime
-- loads it into every Lua state, trusted or not, and calls the second function it returns when a script ends, which
-- runs the test cases the script registered. docs/lua.md describes it (src/scripting/reference.cpp).

-- The runtime's message handler: returns the message with a traceback. The sandbox has no debug library.
local traceback = ...

-- Taken now, so that a test can't break the framework by replacing them. The sandbox has neither io nor os.
local io, getenv = io, os and os.getenv
local error, ipairs, pairs, pcall, print, select, tonumber, tostring, type, xpcall =
	error, ipairs, pairs, pcall, print, select, tonumber, tostring, type, xpcall
local format, find, gmatch, match, pack, sub, unpack =
	string.format, string.find, string.gmatch, string.match, string.pack, string.sub, string.unpack
local concat, sort = table.concat, table.sort
local abs, log, mathType = math.abs, math.log, math.type

local test = {}

local cases = {} -- registered and not run yet: {name = ..., fn = ...}
local beforeHooks, afterHooks = {}, {}
local current -- the running test case: {cleanups = {...}, fixtures = {...}}

-- Values in messages

-- Floats as Lua prints them, unless that hides a difference: then with every digit.
local function describeNumber(value)
	local text = tostring(value)
	if mathType(value) == "float" and value == value and tonumber(text) ~= value then
		text = format("%.17g", value)
	end
	return text
end

-- Numbers first, then strings, then the rest.
local function keyOrder(a, b)
	local typeA, typeB = type(a), type(b)
	if typeA ~= typeB then
		return typeA < typeB
	end
	if typeA == "number" or typeA == "string" then
		return a < b
	end
	return tostring(a) < tostring(b)
end

-- The keys of all the given tables, once each, in keyOrder.
local function orderedKeys(...)
	local keys, seen = {}, {}
	for i = 1, select("#", ...) do
		for key in pairs((select(i, ...))) do
			if not seen[key] then
				seen[key] = true
				keys[#keys + 1] = key
			end
		end
	end
	sort(keys, keyOrder)
	return keys
end

local function isIdentifier(key)
	return type(key) == "string" and match(key, "^[%a_][%w_]*$") ~= nil
end

local describe

-- A value as Lua source, shortened: {1, 2, name = "x"}.
function describe(value, depth, seen)
	local kind = type(value)
	if kind == "string" then
		return format("%q", value)
	elseif kind == "number" then
		return describeNumber(value)
	elseif kind ~= "table" then
		return tostring(value)
	end
	depth, seen = depth or 0, seen or {}
	if depth >= 3 or seen[value] then
		return "{...}"
	end
	seen[value] = true
	local parts, length, shown = {}, #value, 0
	for _, key in ipairs(orderedKeys(value)) do
		shown = shown + 1
		if shown > 10 then
			parts[#parts + 1] = "..."
			break
		end
		local text = describe(value[key], depth + 1, seen)
		if mathType(key) == "integer" and key >= 1 and key <= length then
			parts[#parts + 1] = text
		elseif isIdentifier(key) then
			parts[#parts + 1] = key .. " = " .. text
		else
			parts[#parts + 1] = "[" .. describe(key) .. "] = " .. text
		end
	end
	seen[value] = nil
	return "{" .. concat(parts, ", ") .. "}"
end

-- Where a nested value is: track.name, [3].name, ["two words"].
local function pathTo(path, key)
	if isIdentifier(key) then
		return path == "" and key or path .. "." .. key
	end
	return path .. "[" .. describe(key) .. "]"
end

-- The first difference between two values, {path = ..., actual = ..., expected = ...}, or nil if they are equal.
-- Tables are compared by their contents.
local function difference(actual, expected, path, seen)
	if actual == expected then
		return nil
	end
	if type(actual) ~= "table" or type(expected) ~= "table" then
		return {path = path, actual = actual, expected = expected}
	end
	seen = seen or {}
	if seen[actual] == expected then
		return nil -- compared already, or being compared: the tables contain themselves
	end
	seen[actual] = expected
	for _, key in ipairs(orderedKeys(expected, actual)) do
		local found = difference(actual[key], expected[key], pathTo(path, key), seen)
		if found then
			return found
		end
	end
	return nil
end

-- Failures

-- Raises a failed assertion at the line that called the assertion: level 3 is the caller of fail's caller. So call it
-- as a statement, never as "return fail(...)", which would replace the assertion's level.
local function fail(message, note)
	if note ~= nil then
		message = tostring(note) .. ": " .. message
	end
	error(message, 3)
end

-- For argument checks in the functions of test: level 3 is their caller.
local function expectType(value, kind, what)
	if type(value) ~= kind then
		error(format("%s must be a %s, got %s", what, kind, describe(value)), 3)
	end
end

local function outsideTestCases(name)
	if current then
		error(name .. " can't be called inside a test case", 3)
	end
end

local function needFiles(what)
	if not io then
		error(what .. " need a trusted script: untrusted ones have no files", 3)
	end
end

-- Assertions. Each takes an optional message last, which goes in front of the failure.

function test.ok(value, message)
	if not value then
		fail("expected a true value, got " .. describe(value), message)
	end
	return value
end

function test.equal(actual, expected, message)
	local found = difference(actual, expected, "")
	if found then
		local text = format("expected %s, got %s", describe(found.expected), describe(found.actual))
		if found.path ~= "" then
			text = "at " .. found.path .. ": " .. text
		end
		fail(text, message)
	end
end

function test.notEqual(actual, unexpected, message)
	if not difference(actual, unexpected, "") then
		fail("expected a value other than " .. describe(unexpected), message)
	end
end

-- NaN is near nothing.
local function isNear(actual, expected, tolerance)
	return abs(actual - expected) <= tolerance
end

function test.near(actual, expected, tolerance, message)
	expectType(tolerance, "number", "the tolerance")
	if type(actual) == "number" and type(expected) == "number" then
		if not isNear(actual, expected, tolerance) then
			fail(format("expected %s within %s, got %s", describe(expected), describe(tolerance), describe(actual)),
				message)
		end
		return
	end
	if type(actual) ~= "table" or type(expected) ~= "table" then
		fail("expected a number or a list of numbers, got " .. describe(actual), message)
	end
	if #actual ~= #expected then
		fail(format("expected %d values, got %d", #expected, #actual), message)
	end
	for i = 1, #expected do
		if type(actual[i]) ~= "number" or not isNear(actual[i], expected[i], tolerance) then
			fail(format("at [%d]: expected %s within %s, got %s", i, describe(expected[i]), describe(tolerance),
				describe(actual[i])), message)
		end
	end
end

function test.matches(text, pattern, message)
	if type(text) ~= "string" then
		fail("expected a string, got " .. describe(text), message)
	end
	if not find(text, pattern) then
		fail(format("expected %s to match %s", describe(text), describe(pattern)), message)
	end
end

-- Returns the error's message. contains is plain text, not a pattern.
function test.raises(fn, contains, message)
	expectType(fn, "function", "the function")
	local ok, problem = pcall(fn)
	if ok then
		fail("expected an error", message)
	end
	local text = type(problem) == "string" and problem or describe(problem)
	if contains ~= nil and not find(text, contains, 1, true) then
		fail(format("expected an error containing %s, got %s", describe(contains), describe(text)), message)
	end
	return text
end

function test.fail(message)
	fail(message ~= nil and tostring(message) or "failed")
end

-- Test cases and fixtures

function test.case(name, fn)
	outsideTestCases("test.case")
	expectType(name, "string", "the name of a test case")
	expectType(fn, "function", "a test case")
	for _, case in ipairs(cases) do
		if case.name == name then
			error(format("there already is a test case named %s", describe(name)), 2)
		end
	end
	cases[#cases + 1] = {name = name, fn = fn}
end

function test.before(fn)
	outsideTestCases("test.before")
	expectType(fn, "function", "a before hook")
	beforeHooks[#beforeHooks + 1] = fn
end

function test.after(fn)
	outsideTestCases("test.after")
	expectType(fn, "function", "an after hook")
	afterHooks[#afterHooks + 1] = fn
end

-- Runs fn when the running test case ends, after it failed too. The last one registered runs first.
function test.cleanup(fn)
	if not current then
		error("test.cleanup works inside test cases only", 2)
	end
	expectType(fn, "function", "a cleanup")
	current.cleanups[#current.cleanups + 1] = fn
end

-- Returns a function that gives the fixture's value: setup runs the first time a test case asks for it, and setup's
-- second result, if any, runs as a cleanup when the test case ends.
function test.fixture(setup)
	expectType(setup, "function", "a fixture's setup")
	local key = {}
	return function()
		if not current then
			error("fixtures work inside test cases only", 2)
		end
		local made = current.fixtures[key]
		if not made then
			local value, teardown = setup()
			if teardown ~= nil then
				if type(teardown) ~= "function" then
					error("a fixture's setup returns its value and, if it needs one, a teardown function", 2)
				end
				current.cleanups[#current.cleanups + 1] = teardown
			end
			made = {value = value}
			current.fixtures[key] = made
		end
		return made.value
	end
end

-- Running

-- The message handler for test cases. An error without a position, like those of commands and assert, gets the line
-- in the test that caused it, from the traceback.
local function located(problem)
	local message = type(problem) == "string" and problem or describe(problem)
	if find(message, "^[^\n]-:%d+: ") then
		return message
	end
	for line in gmatch(sub(traceback(message), #message + 1), "[^\n]+") do
		local source, number = match(line, "^%s*(.-):(%d+): in ")
		if source and source ~= "gnubase.test" then
			return source .. ":" .. number .. ": " .. message
		end
	end
	return message
end

-- Runs a test case with its hooks and cleanups; returns what went wrong.
local function runCase(case, befores, afters)
	current = {cleanups = {}, fixtures = {}}
	local problems = {}
	local function attempt(fn)
		local ok, problem = xpcall(fn, located)
		if not ok then
			problems[#problems + 1] = problem
		end
		return ok
	end
	local ready = true
	for _, hook in ipairs(befores) do
		if not attempt(hook) then
			ready = false
			break
		end
	end
	if ready then
		attempt(case.fn)
	end
	local cleanups = current.cleanups
	for i = #cleanups, 1, -1 do
		attempt(cleanups[i])
	end
	for _, hook in ipairs(afters) do
		attempt(hook)
	end
	current = nil
	return problems
end

-- Runs the registered test cases and reports them in TAP (Test Anything Protocol) format. Raises an error if any
-- failed. A script doesn't need to call it: the test cases it registered run when it ends.
local function run()
	outsideTestCases("test.run")
	local list, befores, afters = cases, beforeHooks, afterHooks
	cases, beforeHooks, afterHooks = {}, {}, {}
	print(format("1..%d", #list))
	local failed = {}
	for number, case in ipairs(list) do
		local problems = runCase(case, befores, afters)
		if #problems == 0 then
			print(format("ok %d - %s", number, case.name))
		else
			print(format("not ok %d - %s", number, case.name))
			for _, problem in ipairs(problems) do
				for line in gmatch(problem, "[^\n]+") do
					print("# " .. line)
				end
			end
			failed[#failed + 1] = case.name
		end
	end
	if #failed > 0 then
		error(format("%d of %d test cases failed: %s", #failed, #list, concat(failed, ", ")), 0)
	end
end
test.run = run

-- Audio files and golden files

-- 32-bit float WAV files, in the format of the C++ helpers in tests/testutils.
local function writeWav(path, audio)
	local samples, channels, rate = audio.samples, audio.channels, audio.sampleRate
	local count = #samples
	local parts = {
		"RIFF", pack("<I4", 4 + (8 + 18) + (8 + 4) + (8 + count * 4)), "WAVE",
		"fmt ", pack("<I4I2I2I4I4I2I2I2", 18, 3, channels, rate, rate * channels * 4, channels * 4, 32, 0),
		"fact", pack("<I4I4", 4, count // channels),
		"data", pack("<I4", count * 4),
	}
	for i = 1, count do
		parts[#parts + 1] = pack("<f", samples[i])
	end
	local file, problem = io.open(path, "wb")
	if not file then
		error("can't write " .. problem, 0)
	end
	file:write(concat(parts))
	file:close()
end

local function readWav(path)
	local file, problem = io.open(path, "rb")
	if not file then
		error("can't read " .. problem, 0)
	end
	local data = file:read("a")
	file:close()
	if sub(data, 1, 4) ~= "RIFF" or sub(data, 9, 12) ~= "WAVE" then
		error(path .. " is not a WAV file", 0)
	end
	local audio, position = {samples = {}}, 13
	while position + 7 <= #data do
		local id, size = sub(data, position, position + 3), unpack("<I4", data, position + 4)
		local body = position + 8
		if id == "fmt " then
			local encoding, channels, rate = unpack("<I2I2I4", data, body)
			if encoding ~= 3 or unpack("<I2", data, body + 14) ~= 32 then
				error(path .. ": only 32-bit float WAV files are supported", 0)
			end
			audio.channels, audio.sampleRate = channels, rate
		elseif id == "data" then
			local samples = audio.samples
			for i = 0, size // 4 - 1 do
				samples[i + 1] = unpack("<f", data, body + i * 4)
			end
		end
		position = body + size + size % 2
	end
	if not audio.channels then
		error(path .. " has no format chunk", 0)
	end
	return audio
end

local function sameFormat(actual, expected)
	return actual.channels == expected.channels and actual.sampleRate == expected.sampleRate and
		#actual.samples == #expected.samples
end

-- What differs, in the words of compareAudio in tests/testutils, or nil if every sample is within the tolerance.
local function compareAudio(actual, expected, tolerance)
	local channels = actual.channels
	local function frames(audio)
		return #audio.samples // audio.channels
	end
	if not sameFormat(actual, expected) then
		return format("the golden file has %d channels at %d Hz and %d frames, the actual audio %d channels at %d Hz "
			.. "and %d frames.", expected.channels, expected.sampleRate, frames(expected), channels, actual.sampleRate,
			frames(actual))
	end
	local got, wanted = actual.samples, expected.samples
	local mismatches, first, largest, largestAt = 0, nil, 0, nil
	for i = 1, #got do
		local distance = abs(got[i] - wanted[i])
		if not (distance <= tolerance) then
			mismatches = mismatches + 1
			first = first or i
			if not (distance <= largest) then
				largest, largestAt = distance, i
			end
		end
	end
	if mismatches == 0 then
		return nil
	end
	local function describeSample(i)
		local index = i - 1
		local frame = index // channels
		return format("sample %d (frame %d, channel %d, %s s): actual %s, expected %s", index, frame, index % channels,
			describeNumber(frame / actual.sampleRate), describeNumber(got[i]), describeNumber(wanted[i]))
	end
	return format("%d of %d samples differ by more than %s. First: %s. Largest difference %s (%s dB) at %s.",
		mismatches, #got, describeNumber(tolerance), describeSample(first), describeNumber(largest),
		describeNumber(20 * log(largest, 10)), describeSample(largestAt))
end

-- audio is {samples = {...}, channels = 1, sampleRate = 48000}, with interleaved samples; channels and sampleRate
-- have these defaults.
local function audioOf(audio)
	return {samples = audio.samples, channels = audio.channels or 1, sampleRate = audio.sampleRate or 48000}
end

function test.readWav(path)
	needFiles("WAV files")
	expectType(path, "string", "the path")
	return readWav(path)
end

function test.writeWav(path, audio)
	needFiles("WAV files")
	expectType(path, "string", "the path")
	expectType(audio, "table", "the audio")
	expectType(audio.samples, "table", "the audio's samples")
	writeWav(path, audioOf(audio))
end

-- Compares audio with the golden file <goldenDir>/<name>.wav. options: goldenDir (default $GNUBASE_GOLDEN_DIR, else
-- tests/golden), outputDir for the actual audio and the difference (default $GNUBASE_TEST_OUTPUT_DIR, else the
-- current folder), and update (default: whether $GNUBASE_UPDATE_GOLDEN is 1), which writes the golden file instead.
function test.golden(name, audio, tolerance, options)
	needFiles("golden files")
	expectType(name, "string", "the name of the golden file")
	expectType(audio, "table", "the audio")
	expectType(audio.samples, "table", "the audio's samples")
	expectType(tolerance, "number", "the tolerance")
	options = options or {}
	local goldenDir = options.goldenDir or getenv("GNUBASE_GOLDEN_DIR") or "tests/golden"
	local outputDir = options.outputDir or getenv("GNUBASE_TEST_OUTPUT_DIR") or "."
	local update = options.update
	if update == nil then
		update = getenv("GNUBASE_UPDATE_GOLDEN") == "1"
	end
	local actual = audioOf(audio)
	local golden = goldenDir .. "/" .. name .. ".wav"
	if update then
		writeWav(golden, actual)
		return
	end

	local actualFile = outputDir .. "/" .. name .. ".actual.wav"
	local accept = " Run the tests with GNUBASE_UPDATE_GOLDEN=1 to accept the new audio."
	local file = io.open(golden, "rb")
	if not file then
		writeWav(actualFile, actual)
		fail(format("%s doesn't exist; the actual audio is in %s.%s", golden, actualFile, accept))
	end
	file:close()
	local expected = readWav(golden)
	local report = compareAudio(actual, expected, tolerance)
	if report then
		writeWav(actualFile, actual)
		report = format("%s: %s Golden file: %s. Actual audio: %s.", name, report, golden, actualFile)
		if sameFormat(actual, expected) then
			local samples = {}
			for i = 1, #actual.samples do
				samples[i] = actual.samples[i] - expected.samples[i]
			end
			local diffFile = outputDir .. "/" .. name .. ".diff.wav"
			writeWav(diffFile, {samples = samples, channels = actual.channels, sampleRate = actual.sampleRate})
			report = report .. " Difference: " .. diffFile .. "."
		end
		fail(report .. accept)
	end
end

-- Called by the runtime when a script ends: runs the test cases it registered, or drops them if the script failed.
local function finish(succeeded)
	if succeeded and #cases > 0 then
		run()
	end
	cases, beforeHooks, afterHooks, current = {}, {}, {}, nil
end

return test, finish
