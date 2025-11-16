local script_path = debug.getinfo(1, "S").source:sub(2)
local dir = script_path:match("(.*/)")
if dir then
	package.cpath = package.cpath .. ";" .. dir .. "?.so"
end

local c = require("c")

---@type fun(cmd: string, stdin: string): string?, integer, integer
c.exec = c.exec

---@class State
---@field text string
---@field pipe string

---@class Action
---@field description string
---@field run fun(State)

---@param list Action[]
local function query(list)
	io.write("What to do next?\n")
	print([[0 or "" to quit]])
	for i, action in ipairs(list) do
		print(("%d (%s)"):format(i, action.description))
	end
	io.write(": ")
	return io.read("l*")
end

---@param state State
---@param list Action[]
---@return boolean -- to continue
local function main_loop(state, list)
	local match = query(list):match("^(%d*)$")
	if not match then
		print("invalid input")
		return true
	end
	if match == "" then
		return false
	end
	local index = tonumber(match)
	if index == 0 then
		return false
	end
	if index > #list then
		print("invalid input")
		return true
	end
	local action = list[index]
	action.run(state)
	return true
end

---@return boolean
local function query_commit()
	while true do
		io.write("commit? y/n (default=y): ")
		local response = io.read("l*")
		if response == "" or response:sub(1, 1) == "y" then
			return true
		end
		if response:sub(1, 1) == "n" then
			return false
		end
		print("invalid input")
	end
end

---@param state State
local function chain_pipe_action(state)
	io.write("enter next cmd: ")
	local cmd = io.read("l*")
	local stdout, status, exit_type = c.exec(cmd, state.text)
	if exit_type == 1 then
		print("Interrupted!")
		return
	end
	assert(stdout)
	io.write(stdout)
	io.write(("\nSTATUS : %d\n"):format(status))
	if query_commit() then
		state.text = stdout
		if state.pipe == "" then
			state.pipe = cmd
		else
			state.pipe = state.pipe .. " | " .. cmd
		end
	end
end

---@param state State
local function print_text_action(state)
	io.write(state.text)
	io.write("\n")
end

---@param state State
local function print_pipe_action(state)
	io.write(state.pipe)
	io.write("\n")
end

local function open_text_in_less_action(state)
		local file = assert(io.popen("less", "w"))
		file:write(state.text)
		file:close()
end

local function open_pipe_in_less_action(state)
		local file = assert(io.popen("less", "w"))
		file:write(state.pipe)
		file:close()
end

---@param buffer string
---@return string?
local function edit_buffer(buffer)
	local editor = os.getenv("EDITOR")
	if not editor then
		print("$EDITOR enviroment variable isn't set")
		return nil
	end
	local path = os.tmpname()
	local file = io.open(path, "w+")
	assert(file)
	file:write(buffer)
	file:flush()
	os.execute("$EDITOR " .. path)
	file:seek("set", 0)
	local value = assert(file:read("*a"))
	file:close()
	return value
end

---@param state State
local function edit_text_action(state)
	local new_text = edit_buffer(state.text)
	if new_text then
		state.text = new_text
	end
end

---@param state State
local function edit_pipe_action(state)
	local new_pipe = edit_buffer(state.pipe)
	if new_pipe then
		state.pipe = new_pipe
	end
end

---@param buffer string
local function copy_buffer_to_clipboard(buffer)
	local file = io.popen("wl-copy", "w")
	if not file then
		file = io.popen("xclip -selection clipboard")
	end
	if not file then
		print("couldn't find suitable clipboard")
		return
	end
	file:write(buffer)
	file:close()
end

---@param state State
local function copy_text_action(state)
	copy_buffer_to_clipboard(state.text)
end

---@param state State
local function copy_pipe_action(state)
	copy_buffer_to_clipboard(state.pipe)
end

---@param state State
local function reexecute_pipe_action(state)
	local stdout, status, exit_type = c.exec(state.pipe, "")
	assert(stdout)
	if exit_type == 1 then
		print("Interrupted!")
		return
	end
	io.write(stdout)
	io.write(("\nSTATUS : %d\n"):format(status))
	if query_commit() then
		state.text = stdout
	end
end

---@param state State
local function save_file_action(state)
	io.write("enter file name: ")
	local path = io.read("l*")
	local file = io.open(path, "wb")
	if not file then
		print(("could not open file [%s]"):format(path))
		return
	end
	file:write(state.text)
	file:close()
end

---@param state State
local function save_pipe_action(state)
	io.write("enter file name: ")
	local path = io.read("l*")
	local file = io.open(path, "wb")
	if not file then
		print(("could not open file [%s]"):format(path))
		return
	end
	file:write(state.pipe)
	file:close()
end

---@param state State
local function reset_state_action(state)
	state.text = ""
	state.pipe = ""
end

local actions = { ---@type Action[]
	{ description = "chain pipe", run = chain_pipe_action },
	{ description = "print text", run = print_text_action },
	{ description = "print pipe", run = print_pipe_action },
	{ description = "open text in less", run = open_text_in_less_action },
	{ description = "open pipe in less", run = open_pipe_in_less_action },
	{ description = "edit text with $EDITOR", run = edit_text_action },
	{ description = "edit pipe with $EDITOR", run = edit_pipe_action },
	{ description = "copy text to clipboard", run = copy_text_action },
	{ description = "copy pipe to clipboard", run = copy_pipe_action },
	{ description = "re-execute pipe", run = reexecute_pipe_action },
	{ description = "save file", run = save_file_action },
	{ description = "save pipe", run = save_pipe_action },
	{ description = "clear everything", run = reset_state_action },
}

local state = { ---@type State
	text = "",
	pipe = ""
}

while main_loop(state, actions) do end
