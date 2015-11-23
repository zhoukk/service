local c = require "service.c"
local serial = require "service.serial"

local pcall = pcall
local assert = assert
local error = error
local debug_traceback = debug.traceback
local string_format = string.format
local math_floor = math.floor

local coroutine_resume = coroutine.resume
local coroutine_create = coroutine.create
local coroutine_yield = coroutine.yield
local coroutine_running = coroutine.running


local table_remove = table.remove
local table_insert = table.insert
local table_unpack = table.unpack
local table_concat = table.concat

local service = {
	proto_rep = 0,
	proto_err = 1,
	proto_socket = 2,
	proto_lua = 3,
	proto_client = 4,
	proto_debug = 5,
}

function service.log(...)
	return c.log(string_format(...))
end

function service.err(...)
	return c.log("[error] "..string_format(...))
end

service.query = c.query
service.name = function(name, handle)
	handle = handle or service.handle
	c.name(name, handle)
end

service.mqlen = c.mqlen
service.abort = c.abort
service.getenv = c.getenv
service.setenv = c.setenv

service.string = c.tostring
service.trash = c.trash
service.now = c.now
service.starttime = c.starttime
service.time = function()
	return math_floor(c.now()/100 + c.starttime())
end

service.pack = serial.pack
service.unpack = serial.unpack

local session_co = {}
local fork_co = {}
local co_param = {}
local call_session = {}
local sleep_session = {}
local wakeup_session = {}
local error_session = {}
local dead_source = {}
local watch_source = {}
local session_response = {}
local unresponse = {}

local co_pool = {}
local function co_create(f)
	local co = table_remove(co_pool)
	if not co then
		co = coroutine_create(function(...)
			f(...)
			while true do
				f = nil
				table_insert(co_pool, co)
				f = coroutine_yield("exit")
				f(coroutine_yield())
			end
		end)
	else
		coroutine_resume(co, f)
	end
	return co
end

local protocols = {}
function service.protocol(protocol)
	protocols[protocol.name] = protocol
	protocols[protocol.id] = protocol
end

function service.dispatch(proto, func)
	local protocol = assert(protocols[proto], proto)
	local f = protocol.dispatch
	protocol.dispatch = func
	return f
end

function service.send(d, proto, ...)
	if type(d) == "string" then
		local t = d
		d = c.query(d)
		if d == nil then
			error("service.send invalid service "..t)
		end
	end
	local protocol = assert(protocols[proto], proto)
	return c.send(d, protocol.id, 0, protocol.pack(...))
end

function service.call(d, proto, ...)
	if type(d) == "string" then
		local t = d
		d = c.query(d)
		if d == nil then
			error("service.call invalid service "..t)
		end
	end
	local protocol = assert(protocols[proto], proto)
	local session = c.send(d, protocol.id, c.session(), protocol.pack(...))
	if session <= 0 then
		error("service.call invalid service "..d)
	end
	call_session[session] = d
	local ok, msg, size = coroutine_yield("call", session)
	call_session[session] = nil
	if not ok then
		error("call failed "..d)
	end
	return protocol.unpack(msg, size)
end

function service.ret(...)
	local co = coroutine_running()
	local proto = co_param[co].proto
	local protocol = assert(protocols[proto], proto)
	return coroutine_yield("return", protocol.pack(...))
end

function service.response()
	local co = coroutine_running()
	local proto = co_param[co].proto
	local protocol = assert(protocols[proto], proto)
	return coroutine_yield("response", protocol.pack)
end

function service.timeout(ti, f)
	local exit = false
	local session = c.timeout(ti)
	session_co[session] = co_create(function()
		if not exit then f() end 
	end)
	return function() exit = true end
end

function service.sleep(ti)
	local session = c.timeout(ti)
	local ok, ret = coroutine_yield("sleep", session)
	sleep_session[coroutine_running()] = nil
	if ok then
		return
	end
	if ret == "BREAK" then
		return "BREAK"
	else
		error(ret)
	end
end

function service.wait(co)
	local session = c.session()
	local ok, ret = coroutine_yield("sleep", session)
	co = co or coroutine_running()
	sleep_session[co] = nil
	session_co[session] = nil
end

function service.wakeup(co)
	if sleep_session[co] and wakeup_session[co] == nil then
		wakeup_session[co] = true
		return true
	end
end

function service.fork(f, ...)
	local args = {...}
	local co = co_create(function()
		f(table_unpack(args))
	end)
	table_insert(fork_co, co)
	return co
end

local suspend
local function dispatch_wakeup()
	local co = next(wakeup_session)
	if co then
		wakeup_session[co] = nil
		local session = sleep_session[co]
		if session then
			session_co[session] = "BREAK"
			return suspend(co, coroutine_resume(co, false, "BREAK"))
		end
	end
end

local function dispatch_error_queue()
	local session = table_remove(error_session, 1)
	if session then
		local co = session_co[session]
		session_co[session] = nil
		return suspend(co, coroutine_resume(co, false))
	end
end

local function dispatch_error(session, source)
	if session == 0 then
		if watch_source[source] then
			dead_source[source] = true
		end
		for i, addr in pairs(call_session) do
			if addr == source then
				table_insert(error_session, i)
			end
		end
	else
		if call_session[session] then
			table_insert(error_session, session)
		end
	end
end

local function release_watch(source)
	local ref = watch_source[source]
	if ref then
		ref = ref - 1
		if ref > 0 then
			watch_source[source] = ref
		else
			watch_source[source] = nil
		end
	end
end

function suspend(co, ok, cmd, p1, p2, ...)
	if not ok then
		local p = co_param[co]
		if p then
			local source = p.source
			if p.session ~= 0 then
				c.send(source, service.proto_err, p.session)
			end
			co_param[co] = nil
		end
		error(debug_traceback(co, cmd))
	end

	if cmd == "call" then
		session_co[p1] = co
	elseif cmd == "sleep" then
		session_co[p1] = co
		sleep_session[co] = p1
	elseif cmd == "return" then
		local session = co_param[co].session
		local source = co_param[co].source
		local ret
		if not dead_source[source] then
			ret = c.send(source, service.proto_rep, session, p1, p2) ~= -1
			if not ret then
				c.send(source, service.proto_err, session)
			end
		elseif p2 ~= nil then
			c.trash(p1)
			ret = false
		end
		return suspend(co, coroutine_resume(co, ret))
	elseif cmd == "response" then
		local session = co_param[co].session
		local source = co_param[co].source
		if session_response[co] then
			error(debug_traceback(co))
		end
		local f = p1
		local function response(ok, ...)
			if ok == "TEST" then
				if dead_source[source] then
					release_watch(source)
					unresponse[response] = nil
					f = false
					return false
				else
					return true
				end
			end
			if not f then
				if f == false then
					f = nil
					return false
				end
				error "Can't response more than once"
			end

			local ret
			if not dead_source[source] then
				if ok then
					ret = c.send(source, service.proto_rep, session, f(...)) ~= -1
					if not ret then
						c.send(source, service.proto_err, session)
					end
				else
					ret = c.send(source, service.proto_err, session) ~= -1
				end
			else
				ret = false
			end
			release_watch(source)
			unresponse[response] = nil
			f = nil
			return ret
		end
		watch_source[source] = watch_source[source] + 1
		session_response[co] = true
		unresponse[response] = true
		return suspend(co, coroutine_resume(co, response))	
	elseif cmd == "exit" then
		local p = co_param[co]
		if p then
			release_watch(p.source)
			co_param[co] = nil
		end
		session_response[co] = nil
	elseif cmd == "quit" then
		return
	elseif cmd == nil then
		return
	else
		error("unknown command : "..cmd.."\n"..debug_traceback(co))
	end
	dispatch_wakeup()
	dispatch_error_queue()
end

local function unknown_request(session, source, msg, size, proto)
	service.err("unknown request (%s)\n", proto)
	error(string_format("unknown session :%d from %d\n", session, source))
end

local function unknown_response(session, source, msg, size)
	service.err("unknown response\n")
	error(string_format("unknown session :%d from %d\n", session, source))
end

function service.dispatch_unknown_request(unknown)
	local prev = unknown_request
	unknown_request = unknown
	return prev
end

function service.dispatch_unknown_response(unknown)
	local prev = unknown_response
	unknown_response = unknown
	return prev
end

local function raw_dispatch_message(proto, session, source, msg, size)
	if proto == service.proto_rep then
		local co = session_co[session]
		if co == "BREAK" then
			session_co[session] = nil
		elseif co == nil then
			unknown_response(session, source, msg, size)
		else
			session_co[session] = nil
			suspend(co, coroutine_resume(co, true, msg, size))
		end
	else
		local protocol = protocols[proto]
		if protocol == nil then
			if session ~= 0 then
				c.send(source, service.proto_err, session)
			else
				unknown_request(session, source, msg, size, proto)
			end
			return
		end
		local f = protocol.dispatch
		if f then
			local ref = watch_source[source]
			if ref then
				watch_source[source] = ref + 1
			else
				watch_source[source] = 1
			end
			co = co_create(f)
			co_param[co] = {session=session, source=source, proto=proto}
			suspend(co, coroutine_resume(co, session, source, protocol.unpack(msg, size)))
		else
			unknown_request(session, source, msg, size, proto)
		end
	end
end

function service.dispatch_message(...)
	local ok, err = pcall(raw_dispatch_message, ...)
	while true do
		local i, co = next(fork_co)
		if co == nil then
			break
		end
		fork_co[i] = nil
		local fok, ferr = pcall(suspend, co, coroutine_resume(co))
		if not fok then
			if ok then
				ok = false
				err = tostring(ferr)
			else
				err = tostring(err).."\n"..tostring(ferr)
			end
		end
	end
	assert(ok, tostring(err))
end

function service.launch(name, ...)
	return c.service(table_concat({name, ...}, " "))
end

function service.create(name, ...)
	return service.req("launch", "launch", name, ...)
end


function service.start(func)
	service.handle = c.start(service.dispatch_message)
	service.timeout(0, function()
		local ok, err = xpcall(func, debug.traceback)
		if not ok then
			service.err(err.."\n")
			service.send("launch", "lua", "error")
			service.exit()
		else
			service.send("launch", "lua", "launchok")
		end
	end)
end

function service.exit()
	fork_co = {}
	service.req("launch", "exit", service.handle)
	
	for co, p in pairs(co_param) do
		if p.session ~= 0 and p.source then
			c.send(p.source, service.proto_err, p.session)
		end
	end
	for resp in pairs(unresponse) do
		resp(false)
	end
	local tmp = {}
	for session, source in pairs(call_session) do
		tmp[source] = true
	end
	for source in pairs(tmp) do
		c.send(source, service.proto_err, 0)
	end
	c.exit()
	coroutine_yield("quit")
end

function service.task(ret)
	local t = 0
	for session, co in pairs(session_co) do
		if ret then
			ret[session] = debug_traceback(co)
		end
		t = t + 1
	end
	return t
end

function service.term(source)
	return dispatch_error(0, source)
end

service.protocol {
	id = service.proto_rep,
	name = "response",
	pack = function(...)
		return ...
	end,
	unpack = function(...)
		return ...
	end,
}

service.protocol {
	id = service.proto_lua,
	name = "lua",
	pack = serial.pack,
	unpack = serial.unpack,
}

service.protocol {
	id = service.proto_err,
	name = "error",
	pack = function(...) return ... end,
	unpack = function(...) return ... end,
	dispatch = dispatch_error
}

function service.req(d, ...)
	return service.call(d, "lua", ...)
end

service.noret = {}

function service.serve(h, p)
	p = p or "lua"
	return service.dispatch(p, function(session, source, cmd, ...)
		local func = assert(h[cmd], cmd)
		local args = {source=source, session=session}
		local function ret(ret, ...)
			if ret ~= service.noret then
				service.ret(ret, ...)
			end
		end
		ret(func(args, ...))
	end)
end

local dbg = require "service_debug"
dbg(service, {
	dispatch = service.dispatch_message,
	suspend = suspend,
	clear = function()
		co_pool = {}
	end,
})

return service