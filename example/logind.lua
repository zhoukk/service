local service = require "service"
local crypt = require "crypt"
local login = require "login"

local gameserver_list = {}
local user_online = {}

local logind = {
	host = "0.0.0.0",
	port = 8000,
	instance = 2,
	multi_login = false,
	name = "LOGIND",
	request={},
}

local port = ...

function logind.auth_handler(token)
	-- the token is base64(user)@base64(server):base64(password)
	local user, server, password = token:match("([^@]+)@([^:]+):(.+)")
	user = crypt.base64decode(user)
	server = crypt.base64decode(server)
	password = crypt.base64decode(password)
	assert(password == "password")
	return server, user
end

function logind.login_handler(server, uid, secret)
	service.log("%s@%s is login, secret:%s\n", uid, server, crypt.hexencode(secret))
	local gameserver = assert(gameserver_list[server], "unknown gameserver")
	local last = user_online[uid]
	if last then
		service.req(last.address, "kick", uid, last.subid)
	end
	if user_online[uid] then
		error(string.format("user:%s is already online\n", uid))
	end
	local subid = service.req(gameserver, "login", uid, secret)
	user_online[uid] = {address = gameserver, subid = subid, server = server}
	return subid
end

function logind.request:regist(server, address)
	gameserver_list[server] = address
	service.log("gated [%s] regist, address:%d\n", server, address)
end

function logind.request:unregist(server)
	service.log("gated [%s] unregist, address:%d\n", server, gameserver_list[server])
	for i, v in pairs(user_online) do
		if v.server == server then
			user_online[i] = nil
		end
	end 
	gameserver_list[server] = nil
end

function logind.request:logout(uid, subid)
	local u = user_online[uid]
	if u then
		service.log("%s@%s is logout\n", uid, u.server)
		user_online[uid] = nil
	end
end

logind.port = tonumber(port)
login.start(logind)
