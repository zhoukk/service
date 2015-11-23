local c = require "socket.c"
local service = require "service"
local assert = assert
local pairs = pairs
local coroutine_running = coroutine.running
local table_insert = table.insert
local table_remove = table.remove

local socket = {}
local socket_pool = setmetatable({},{
	__gc = function(p)
		for id, v in pairs(p) do
			c.close(id)
			p[id] = nil
		end
	end
})

local function wakeup(s)
	local co = s.co
	if co then
		s.co = nil
		service.wakeup(co)
	end
end

local function suspend(s)
	assert(not s.co)
	s.co = coroutine_running()
	service.wait(s.co)
	if s.closing then
		service.wakeup(s.closing)
	end
end

local socket_handle = {}

--close
socket_handle[1] = function(id)
	local s = socket_pool[id]
	if s == nil then
		return
	end
	s.valid = false
	wakeup(s)
end

--open
socket_handle[2] = function(id,_,addr)
	local s = socket_pool[id]
	if s == nil then
		return
	end
	s.valid = true
	wakeup(s)
end

--data
socket_handle[3] = function(id, size, data)
	local s = socket_pool[id]
	if s == nil then
		service.log("socket: drop package from %d\n", id)
		service.trash(data)
		return
	end
	local size = s.buffer:push(data, size)
	local rr = s.need_read
	local rrt = type(rr)
	if rrt == "number" then
		if size >= rr then
			s.need_read = nil
			wakeup(s)
		end
	else
		if s.buffer_limit and size > s.buffer_limit then
			service.log("socket: buffer overflow fd=%d size=%d\n", id, size)
			s.buffer:free()
			c.close(id)
			return
		end
		if rrt == "string" then
			if s.buffer:readline(rr, 1) then
				s.need_read = nil
				wakeup(s)
			end
		end
	end
end

--accept
socket_handle[4] = function(id, newid, addr)
	local s = socket_pool[id]
	if s == nil then
		c.close(newid)
	end
	s.callback(newid, addr)
end

--error
socket_handle[5] = function(id, _, err)
	local s = socket_pool[id]
	if s == nil then
		service.log("socket: error on unknown %d, %s\n", id, err)
		return
	end
	if s.valid then
		service.log("socket: error on %d, %s\n", id, err)
	end
	s.valid = false
	wakeup(s)
end

--udp
socket_handle[6] = function(id, size, data, address)
	local s = socket_pool[id]
	if s == nil or s.callback == nil then
		service.log("socket: drop udp package from %d\n", id)
		service.trash(data)
		return
	end
	local str = service.string(data, size)
	s.callback(str, address)
end

local function default_warning(id, size)
	local s = socket_pool[id]
	local last = s.warningsize or 0
	if last + 64 < size then	-- if size increase 64K
		s.warningsize = size
		service.log("WARNING: %d K bytes need to send out (fd = %d)", size, id)
	end
	s.warningsize = size
end

--warning
socket_handle[7] = function(id, size)
	local s = socket_pool[id]
	if s then
		local warning = s.warning or default_warning
		warning(id, size)
	end
end

service.protocol {
	name = "socket",
	id = service.proto_socket,
	unpack = c.unpack,
	dispatch = function(_,_,t,...)
		socket_handle[t](...)
	end
}

local function socket_new(id, func)
	local buffer
	if func == nil then
		buffer = c.buffer()
	end
	local s = {
		id = id,
		buffer = buffer,
		valid = false,
		need_read = false,
		co = false,
		callback = func,
		protocol = "TCP",
	}
	socket_pool[id] = s
	suspend(s)
	if s.valid then
		return id
	else
		socket_pool[id] = nil
	end
end

function socket.open(addr, port)
	if port == nil then
		addr, port = string.match(addr, "([^:]+):(.+)$")
		port = tonumber(port)
	end
	local id = c.open(addr, port)
	return socket_new(id)
end

function socket.bind(fd)
	local id = c.bind(fd)
	return socket_new(id)
end

function socket.stdin()
	return socket.bind(0)
end

function socket.start(id, func)
	c.start(id)
	return socket_new(id, func)
end

function socket.shutdown(id)
	local s = socket_pool[id]
	if s then
		if s.buffer then
			s.buffer:free()
		end
		if s.valid then
			c.close(id)
		end
	end
end

function socket.close(id)
	local s = socket_pool[id]
	if s == nil then
		return
	end
	if s.valid then
		c.close(s.id)
		if s.co then
			assert(not s.closing)
			s.closing = coroutine_running()
			service.wait(s.closing)
		else
			suspend(s)
		end
		s.valid = false
	end
	socket.shutdown(id)
	socket_pool[id] = nil
end

function socket.listen(host, port, backlog)
	if port == nil then
		host, port = string.match(host, "([^:]+):(.+)$")
		port = tonumber(port)
	end
	return c.listen(host, port, backlog)
end

function socket.read(id, size)
	local s = socket_pool[id]
	assert(s)
	if size == nil then
		local ret = s.buffer:readall()
		if ret ~= "" then
			return ret
		end
		if not s.valid then
			return false, ret
		end
		assert(not s.need_read)
		s.need_read = 0
		suspend(s)
		ret = s.buffer:readall()
		if ret ~= "" then
			return ret
		else
			return false, ret
		end
	end
	local ret = s.buffer:pop(size)
	if ret then
		return ret
	end
	if not s.valid then
		return false, s.buffer:readall()
	end
	assert(not s.need_read)
	s.need_read = size
	suspend(s)
	ret = s.buffer:pop(size)
	if ret then
		return ret
	else
		return false, s.buffer:readall()
	end
end

function socket.readall(id)
	local s = socket_pool[id]
	assert(s)
	if not s.valid then
		local r = s.buffer:readall()
		return r ~= "" and r
	end
	assert(not s.need_read)
	s.need_read = true
	suspend(s)
	assert(s.valid == false)
	return s.buffer:readall()
end

function socket.readline(id, sep)
	sep = sep or "\n"
	local s = socket_pool[id]
	assert(s)
	local ret = s.buffer:readline(sep)
	if ret then
		return ret
	end
	if not s.valid then
		return false, s.buffer:readall()
	end
	assert(not s.need_read)
	s.need_read = sep
	suspend(s)
	if s.valid then
		return s.buffer:readline(sep)
	else
		return false, s.buffer:readall()
	end
end

function socket.invalid(id)
	return socket_pool[id] == nil
end

function socket.limit(id, limit)
	local s = assert(socket_pool[id])
	s.buffer_limit = limit
end

function socket.lock(id)
	local s = socket_pool[id]
	assert(s)
	local lock_set = s.lock
	if not lock_set then
		lock_set = {}
		s.lock = lock_set
	end
	if #lock_set == 0 then
		lock_set[1] = true
	else
		local co = coroutine_running()
		table_insert(lock_set, co)
		service.wait(co)
	end
end

function socket.unlock(id)
	local s = socket_pool[id]
	assert(s)
	local lock_set = assert(s.lock)
	table_remove(lock_set, 1)
	local co = lock_set[1]
	if co then
		service.wakeup(co)
	end
end

function socket.abandon(id)
	local s = socket_pool[id]
	if s and s.buffer then
		s.buffer:free()
	end
	socket_pool[id] = nil
end

function socket.block(id)
	local s = socket_pool[id]
	if not s or not s.valid then
		return false
	end
	assert(not s.need_read)
	s.need_read = 0
	suspend(s)
	return s.valid
end

socket.write = assert(c.send)

local function udp_new(id, cb)
	socket_pool[id] = {
		id = id,
		valid = true,
		protocol = "UDP",
		callback = cb,
	}
end


function socket.udp(func, host, port)
	local id = c.udp(host, port)
	udp_new(id, func)
	return id
end

function socket.udp_open(id, addr, port, func)
	local s = socket_pool[id]
	if s then
		assert(s.protocol == "UDP")
		if func then
			s.callback = func
		end
	else
		udp_new(id, func)
	end
	c.udp_open(id, addr, port)
end

function socket.warning(id, callback)
	local s = socket_pool[id]
	assert(s)
	s.warning = callback
end

socket.sendto = assert(c.udp_send)
socket.udp_address = assert(c.udp_address)
socket.nodelay = assert(c.nodelay)

return socket