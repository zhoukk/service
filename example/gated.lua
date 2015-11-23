local service = require "service"
local gate = require "gate"

local users = {}
local username_map = {}
local internal_id = 0

local gated = {}

local agentpool

--call by login server
function gated.login_handler(uid, secret)
	if users[uid] then
		error(string.format("%s is already login", uid))
	end

	internal_id = internal_id + 1
	local id = internal_id	-- don't use internal_id directly
	local username = gate.username(uid, id, servername)

	-- you can use a pool to alloc new agent
	local agent = nil
	if agentpool then
		agent = service.req(agentpool)
		print("agentpool get", agent)
		if not agent then
			agentpool = nil
		end
	end
	if not agent then
		agent = service.create "agent"
	end
	local u = {
		username = username,
		agent = agent,
		uid = uid,
		subid = id,
	}

	-- trash subid (no used)
	service.req(agent, "login", uid, id, secret)

	users[uid] = u
	username_map[username] = u

	gate.login(username, secret)

	-- you should return unique subid
	return id
end


--call by agent
function gated.logout_handler(uid, subid)
	local u = users[uid]
	if u then
		local username = gate.username(uid, subid, servername)
		assert(u.username == username)
		gate.logout(u.username)
		users[uid] = nil
		username_map[u.username] = nil
		service.req(logind, "logout", uid, subid)
	end
end

--call by login server
function gated.kick_handler(uid, subid)
	local u = users[uid]
	if u then
		local username = gate.username(uid, subid, servername)
		assert(u.username == username)
		pcall(service.req, u.agent, "logout")
	end
end

--call by self when socket disconnect
function gated.disconnect_handler(username)
	local u = username_map[username]
	if u then
		service.req(u.agent, "afk")
	end
end


--call by self when a user auth ok
function gated.auth_handler(username, fd, addr)
	local u = username_map[username]
	if u then
		service.req(u.agent, "cbk", fd, addr)
	end
end

service.protocol {
	name = "client",
	id = service.proto_client,
	pack = function(...)
		return ...
	end,
}

--call by self when recv a request from client
function gated.request_handler(username, msg, sz)
	local u = username_map[username]
	return service.send(u.agent, "client", msg, sz)
end

--call by self when gate open
function gated.register_handler(name)
	servername = name
	service.req(logind, "regist", servername, service.handle)
end

function gated.unregister_handler()
	service.req(logind, "unregist", servername)
end

logind = service.query("LOGIND")
agentpool = service.query("agentpool")
gate.start(gated)
