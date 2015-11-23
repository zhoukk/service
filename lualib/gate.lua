--[[

Protocol:
	
	handshake (first package)
	Client -> Server :
		base64(uid)@base64(server)#base64(subid):index:base64(hmac)

	Server -> Client :
		Error Code
		404 User Not Found
		403 Index Expired
		401 Unauthorized
		400 Bad Request
		200 OK


API:
	gate.userid(username)
		return uid, subid, server

	gate.username(uid, subid, server)
		return username

	gate.login(username, secret)
		update user secret

	gate.logout(username)
		user logout

	gate.ip(username)
		return ip

	gate.start(handler)
		start gate server

Supported request
	kick username
	login username secret
	logout username
	open config

Supported handler
	login_handler(uid, secret) -> subid call when a new user login
	logout_handler(uid, subid) call when a user logout
	kick_handler(uid, subid) call when a user logout
	register_handler(servername) call when gate opened
	disconnect_handler(username) call when a connection disconnect (afk)
	request_handler(username, msg) call when a connection recv a package
]]


local service = require "service"
local socket = require "socket.c"
local crypt = require "crypt"
local netpack = require "netpack"

local b64encode = crypt.base64encode
local b64decode = crypt.base64decode

local connection = {}
local handshake = {}
local uid_login = {}
local user_login = {}
local fd_login = {}


local function openclient(fd)
	if connection[fd] then
		socket.start(fd)
	end
end

local function closeclient(fd)
	local c = connection[fd]
	if c then
		connection[fd] = false
		socket.close(fd)
	end
end


local gate = {}

function gate.username(uid, subid, servername)
	return string.format("%s@%s#%s", b64encode(uid), b64encode(servername), b64encode(tostring(subid)))
end

function gate.userid(username)
	-- base64(uid)@base64(server)#base64(subid)
	local uid, servername, subid = username:match "([^@]*)@([^#]*)#(.*)"
	return b64decode(uid), b64decode(subid), b64decode(servername)
end

function gate.ip(username)
	local u = user_login[username]
	if u and u.fd then
		return u.ip
	end
end

function gate.login(username, secret)
	assert(user_login[username] == nil)
	user_login[username] = {
		secret = secret,
		version = 0,
		index = 0,
		username = username,
		response = {},
	}
end

function gate.logout(username)
	local u = user_login[username]
	user_login[username] = nil
	if u.fd then
		closeclient(u.fd)
		fd_login[u.fd] = nil
	end
end

local listen

function gate.start(handler)
	local maxclient
	local nodelay
	local queue
	local expired_number
	local client_number = 0

	local request = {}

	function request:open(conf)
		local address = conf.address or "0.0.0.0"
		local port = assert(conf.port)
		local servername = conf.servername

		maxclient = conf.maxclient or 1024
		expired_number = conf.expired_number or 128
		nodelay = conf.nodelay
		
		listen = socket.listen(address, port)
		if listen == -1 then
			service.err("gated [%s] listen at %s:%d failed\n", servername, address, port)
			return
		else
			service.log("gated [%s] listen at %s:%d\n", servername, address, port)
		end
		socket.start(listen)
		handler.register_handler(servername)
	end

	function request:close()
		handler.unregister_handler()
	end

	function request:login(...)
		return handler.login_handler(...)
	end

	function request:logout(...)
		return handler.logout_handler(...)
	end

	function request:kick(...)
		return handler.kick_handler(...)
	end

	local SOCKET = {}

	function SOCKET.open(fd, addr)
		if client_number >= maxclient then
			socket.close(fd)
			return
		end
		if nodelay then
			socket.nodelay(fd)
		end
		connection[fd] = true
		client_number = client_number + 1

		handshake[fd] = addr
		openclient(fd)
	end

	local function close(fd)
		handshake[fd] = nil
		local c = fd_login[fd]
		if c then
			c.fd = nil
			fd_login[fd] = nil
			if handler.disconnect_handler then
				handler.disconnect_handler(c.username)
			end
		end
		if connection[fd] ~= nil then
			connection[fd] = nil
			client_number = client_number - 1
		end
	end

	function SOCKET.close(fd)
		close(fd)
	end

	function SOCKET.error(fd, msg)
		close(fd)
	end

	local function do_auth(fd, message, addr)
		local username, index, hmac = string.match(message, "([^:]*):([^:]*):([^:]*)")
		local u = user_login[username]
		if u == nil then
			return "404 User Not Found"
		end
		local idx = assert(tonumber(index))
		hmac = b64decode(hmac)

		if idx <= u.version then
			return "403 Index Expired"
		end

		local text = string.format("%s:%s", username, index)
		local v = crypt.hmac_hash(u.secret, text)	-- equivalent to crypt.hmac64(crypt.hashkey(text), u.secret)
		if v ~= hmac then
			return "401 Unauthorized"
		end
		u.version = idx
		u.fd = fd
		u.ip = addr
		fd_login[fd] = u
	end

	local function auth(fd, addr, msg, sz)
		local message = service.string(msg, sz)
		local ok, result = pcall(do_auth, fd, message, addr)
		if not ok then
			service.err("%s\n", result)
			result = "400 Bad Request"
		end
		local close = result ~= nil
		if result == nil then
			result = "200 OK"
		end
		socket.send(fd, string.pack(">s2", result))
		if close then
			closeclient(fd)
		else
			if handler.auth_handler then
				local u = fd_login[fd]
				handler.auth_handler(u.username, fd, addr)
			end
		end
	end

	local function do_request(fd, msg, sz)
		local u = assert(fd_login[fd], "invalid fd")
		handler.request_handler(u.username, msg, sz)
	end

	local function client_request(fd, msg, sz)
		local ok, err = pcall(do_request, fd, msg, sz)
		if not ok then
			service.err("invalid package %s from %d\n", err, fd)
			if fd_login[fd] then
				closeclient(fd)
			end
		end
	end

	local function dispatch_msg(fd, msg, sz)
		if connection[fd] then
			local addr = handshake[fd]
			if addr then
				auth(fd, addr, msg, sz)
				handshake[fd] = nil
			else
				client_request(fd, msg, sz)
			end
		else
			service.err("drop message from fd (%d)\n", fd)
		end
	end

	local function dispatch_queue()
		local fd, msg, sz = netpack.pop(queue)
		if fd then
			service.fork(dispatch_queue)
			dispatch_msg(fd, msg, sz)
			for fd, msg, sz in netpack.pop, queue do
				dispatch_msg(fd, msg, sz)
			end
		end
	end

	SOCKET.data = dispatch_msg
	SOCKET.more = dispatch_queue

	SOCKET.warning = function(id, size)
		service.err("%d K bytes send blocked on %d\n", size, id)
	end

	service.protocol {
		name = "socket",
		id = service.proto_socket,
		unpack = function(msg, sz)
			return netpack.filter(queue, msg, sz)
		end,
		dispatch = function(_,_,q,t, ...)
			queue = q
			if t then
				SOCKET[t](...)
			end
		end
	}

	service.start(function()
		service.serve(request)
	end)

end

function gate.exit()
	if listen then
		socket.close(listen)
	end
	service.exit()
end

return gate