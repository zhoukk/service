--[[

Protocol:

	line (\n) based text protocol

	1. Client->Server : base64(8bytes handshake client key)
	2. Server->Client : base64(8bytes random challenge)
	3. Server: Gen a 8bytes handshake server key
	4. Server->Client : base64(DH-Exchange(server key))
	5. Server/Client secret := DH-Secret(client key/server key)
	6. Client->Server : base64(HMAC(challenge, secret))
	7. Client->Server : DES(secret, base64(token))
	8. Server : call auth_handler(token) -> server, uid (A user defined method)
	9. Server : call login_handler(server, uid, secret) ->subid (A user defined method)
	10. Server->Client : 200 base64(subid)

Error Code:
	400 Bad Request . challenge failed
	401 Unauthorized . unauthorized by auth_handler
	403 Forbidden . login_handler failed
	406 Not Acceptable . already in login (disallow multi login)

Success:
	200 base64(subid)
]]

local service = require "service"
local socket = require "socket"
local crypt = require "crypt"

local socket_error = {}
local function send_package(fd, data)
	if not socket.write(fd, data.."\n") then
		error(socket_error)
	end
end

local function recv_package(fd)
	local v = socket.readline(fd)
	if v then
		return v
	else
		error(socket_error)
	end
end

local function launch_slave(auth_handler)
	local function auth(fd, addr)
		service.log("connect from %s (fd = %d)\n", addr, fd)
		socket.start(fd)
		socket.limit(fd, 8192)

		local clientkey = recv_package(fd)
		clientkey = crypt.base64decode(clientkey)
		if #clientkey ~= 8 then
			send_package(fd, "400 Bad Request")
			error("invalid clientkey")
		end
		local challenge = crypt.randomkey()
		send_package(fd, crypt.base64encode(challenge))
		local serverkey = crypt.randomkey()
		send_package(fd, crypt.base64encode(crypt.dhexchange(serverkey)))
		local secret = crypt.dhsecret(clientkey, serverkey)
		local hmac = crypt.hmac64(challenge, secret)
		local ret = recv_package(fd)
		if hmac ~= crypt.base64decode(ret) then
			send_package(fd, "400 Bad Request")
			error("challenge failed")
		end
		local check_token = recv_package(fd)
		local token = crypt.desdecode(secret, crypt.base64decode(check_token))
		local ok, server, uid = pcall(auth_handler, token)
		return ok, server, uid, secret
	end

	local function ret(fd, ok, err, ...)
		socket.abandon(fd)
		if ok then
			service.ret(err, ...)
		else
			if err == socket_error then
				service.ret(nil, "socket error")
			else
				service.ret(false, err)
			end
		end
	end

	service.dispatch("lua", function(_, _, fd, addr)
		ret(fd, pcall(auth, fd, addr))
	end)

end

local function launch_master(conf)
	local user_login = {}

	local function accept(conf, slave, fd, addr)
		local ok, server, uid, secret = service.req(slave, fd, addr)
		socket.start(fd)

		if not ok then
			if ok ~= nil then
				send_package(fd, "401 Unauthorized")
			end
			error(server)
		end

		if not conf.multi_login then
			if user_login[uid] then
				send_package(fd, "406 Not Acceptable")
				error(string.format("user %s is already login", uid))
			end
			user_login[uid] = true
		end

		local ok, err = pcall(conf.login_handler, server, uid, secret, addr)
		user_login[uid] = nil

		if ok then
			err = err or ""
			send_package(fd, "200 "..crypt.base64encode(err))
		else
			send_package(fd, "403 Forbidden")
			error(err)
		end
	end

	local balance = 1
	local host = conf.host or "0.0.0.0"
	local port = assert(conf.port)
	local instance = conf.instance or 8
	local slaves = {}

	service.serve(conf.request)

	for i=1, instance do
		local slave = service.create(SERVICE_NAME, "slave")
		table.insert(slaves, slave)
	end
	listen = socket.listen(host, port)
	if listen == -1 then
		service.err("login listen at %s:%d failed\n", host, port)
		return
	end
	service.log("login listen at %s:%d\n", host, port)
	socket.start(listen, function(fd, addr)
		local slave = slaves[balance]
		balance = balance + 1
		if balance > #slaves then
			balance = 1
		end
		local ok, err = pcall(accept, conf, slave, fd, addr)
		if not ok then
			service.err("login %d err:%s\n", fd, err)
			socket.start(fd)
		end
		socket.close(fd)

	end)
end

local login = {}

function login.start(conf)
	service.start(function()
		local address = service.query(conf.name)
		if address then
			launch_slave(conf.auth_handler)
		else
			service.name(conf.name)
			launch_master(conf)
		end
	end)
end

function login.exit()
	if listen then
		socket.close(listen)
	end
	service.exit()
end

return login