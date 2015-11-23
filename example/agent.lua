local service = require "service"
local socket = require "socket"
local sprotoloader = require "sprotoloader"

local host = sprotoloader.load(1):host "package"
local sender = host:attach(sprotoloader.load(2))

U = {}

local function send(s)
	if U.fd then
		socket.write(U.fd, string.pack(">s2", s))
	end
end

function send_package(proto, args)
	send(sender(proto, args))
end

REQUEST = {}

local function client_request(name, args, response)
	service.log("%d client:%s\n", service.time(), name)
	local f = assert(REQUEST[name], "not support client request:"..name)
	local r = f(args)
	if response then
		return response(r)
	end
end

service.protocol {
	name = "client",
	id = service.proto_client,
	unpack = function(msg, sz)
		return host:dispatch(msg, sz)
	end,
	dispatch = function(_,_,type,...)
		if type == "REQUEST" then
			local ok, result = pcall(client_request, ...)
			if ok then
				if result then
					send(result)
				end
			else
				service.err("error:%s\n", result)
			end
		end
	end
}

local request = {}

service.start(function()
	service.serve(request)
end)

role = {}
M = {}
-- require "role.role_base"
-- require "role.role_login"
-- require "role.role_aoi"

local role_database_total = service.getenv("role_database_total")
function get_role_db(role_id)
	local idx = math.floor(role_id % role_database_total)
	if idx == 0 then
		idx = role_database_total
	end
	return service.query("database_role_"..idx)
end

--rpc request

local function logout()

	-- M.save_role()

	service.log("%s is logout\n", U.uid)
	service.req(gated, "logout", U.uid, U.sid)
	if U.cancel_timer then
		U.cancel_timer()
		U.cancel_timer = nil
	end
	service.exit()
end

function request:login(uid, sid, secret, addr)
	service.log("%s is login, sid:%d addr:%s\n", uid, sid, addr)
	gated = self.source
	U.uid = uid
	U.sid = sid
	U.secret = secret

	role.server = self.source
	role.login_ip = addr
	role.login_time = service.time()

	-- M.__init__()

	-- M.load_role_list()

	service.fork(function()
		while true do
			send_package("heartbeat", {time = service.now()})
			service.sleep(100)
		end
	end)
	U.cancel_timer = service.timeout(500, logout)
end

function request:logout()
	logout()
end

function request:afk()
	U.fd = nil
	service.log("AFK\n")
	U.cancel_timer = service.timeout(500, logout)
end

function request:cbk(fd, addr)
	U.fd = fd
	service.log("CBK fd:%d addr:%s\n", fd, addr)
	if U.cancel_timer then
		U.cancel_timer()
		U.cancel_timer = nil
	end
end

function request:send(...)
	send_package(...)
end


--client request

function REQUEST.handshake()
	return {msg = "welcome to service"}
end

function REQUEST.quit()
	service.req(gated, "kick", U.uid, U.sid)
end