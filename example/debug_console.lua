local service = require "service"
local c = require "service.c"
local socket = require "socket"

local port = ...

local COMMAND = {}

local function format_table(t)
	local index = {}
	for k in pairs(t) do
		table.insert(index, k)
	end
	table.sort(index)
	local result = {}
	for _,v in ipairs(index) do
		table.insert(result, string.format("%s:%s",v,tostring(t[v])))
	end
	return table.concat(result,"\t")
end

local function dump_line(print, key, value)
	if type(value) == "table" then
		print(key, format_table(value))
	else
		print(key,tostring(value))
	end
end

local function dump_list(print, list)
	local index = {}
	for k in pairs(list) do
		table.insert(index, k)
	end
	table.sort(index)
	for _,v in ipairs(index) do
		dump_line(print, v, list[v])
	end
	print("OK")
end

local function split_cmdline(cmdline)
	local split = {}
	for i in string.gmatch(cmdline, "%S+") do
		table.insert(split,i)
	end
	return split
end

local function docmd(cmdline, print, fd)
	local split = split_cmdline(cmdline)
	local command = split[1]
	if command == "debug" then
		table.insert(split, fd)
	end
	local cmd = COMMAND[command]
	local ok, list
	if cmd then
		ok, list = pcall(cmd, select(2, table.unpack(split)))
	else
		print("Invalid command, type help for command list")
	end

	if ok then
		if list then
			if type(list) == "string" then
				print(list)
			else
				dump_list(print, list)
			end
		else
			print("OK")
		end
	else
		print("Error:", list)
	end
end

local function console_main_loop(stdin, print)
	socket.lock(stdin)
	print("Welcome to service console")
	while true do
		local cmdline = socket.readline(stdin, "\n")
		if not cmdline then
			break
		end
		if cmdline ~= "" then
			docmd(cmdline, print, stdin)
		end
	end
	socket.unlock(stdin)
end

local listen_socket

service.start(function()
	port = tonumber(port)
	listen_socket = socket.listen("127.0.0.1", port)
	if listen_socket < 0 then
		service.exit()
		return
	end
	service.log("Start debug console at 127.0.0.1 %d\n", port)
	socket.start(listen_socket , function(id, addr)
		local function print(...)
			local t = { ... }
			for k,v in ipairs(t) do
				t[k] = tostring(v)
			end
			socket.write(id, table.concat(t,"\t"))
			socket.write(id, "\r\n")
		end
		socket.start(id)
		service.fork(console_main_loop, id , print)
	end)
end)

function exit()
	if listen_socket >= 0 then
		socket.close(listen_socket)
		listen_socket = nil
	end
end

function COMMAND.help()
	return {
		help = "this help message",
		list = "list all the service",
		stat = "dump all stats",
		info = "info address : get service infomation",
		exit = "exit address : kill a lua service",
		kill = "kill address : kill service",
		mem = "mem : show memory status",
		gc = "gc : force every lua service do garbage collect",
		start = "lanuch a new lua service",
		task = "task address : show service task detail",
		inject = "inject address luascript.lua",
		logon = "logon address",
		logoff = "logoff address",
		debug = "debug address : debug a lua service",
	}
end

function COMMAND.start(...)
	local ok, addr = pcall(service.create, ...)
	if ok then
		if addr then
			return { [addr] = ... }
		else
			return "OK"
		end
	else
		return "Failed"
	end
end

function COMMAND.list()
	return service.req("launch", "list")
end

function COMMAND.stat()
	return service.req("launch", "stat")
end

function COMMAND.mem()
	return service.req("launch", "mem")
end

function COMMAND.kill(address)
	return service.req("launch", "kill", address)
end

function COMMAND.gc()
	return service.req("launch", "gc")
end

function COMMAND.exit(address)
	address = tonumber(address)
	service.call(address, "debug", "EXIT")
end

function COMMAND.inject(address, filename)
	address = tonumber(address)
	local f = io.open(filename, "rb")
	if not f then
		return "Can't open " .. filename
	end
	local source = f:read "*a"
	f:close()
	return service.call(address, "debug", "RUN", source, filename)
end

function COMMAND.task(address)
	address = tonumber(address)
	return service.call(address,"debug","TASK")
end

function COMMAND.info(address)
	address = tonumber(address)
	return service.call(address,"debug","INFO")
end

function COMMAND.debug(address, fd)
	address = tonumber(address)
	local agent = service.create "debug_agent"
	local stop
	service.fork(function()
		repeat
			local cmdline = socket.readline(fd, "\n")
			if not cmdline then
				agent.post.cmd("cont")
				break
			end
			agent.post.cmd(cmdline)
		until stop or cmdline == "cont"
	end)
	agent.req.start(address, fd)
	stop = true
end

function COMMAND.logon(address)
	address = tonumber(address)
	c.logon(address)
end

function COMMAND.logoff(address)
	address = tonumber(address)
	c.logoff(address)
end
