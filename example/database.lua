local service = require "service"
local redis = require "redis"

local conf = {
	host = "127.0.0.1" ,
	port = 6379 ,
	db = 0
}

local role_database_total = 3

service.setenv("role_database_total", role_database_total)

db = nil
M = {}

function init(i)
	if service.query("database") then
		require "database.database_player"
		conf.db = i
		service.name("database_role_"..i)
	else
		service.name "database"
		require "database.database_center"
		for idx=1, role_database_total do
			service.service(SERVICE_NAME, idx)
		end
	end
	db = redis.connect(conf)
end

function exit()

end

local function email_encode(email)
	return string.gsub(email, "([^%w_@.])", function(str)
		return string.format("%%%02X", string.byte(str))
	end)
end

local function email_decode(email)
	return string.gsub(str, "%%(%w%w)", function(str)
		return string.char(tonumber(str, 16))
	end)
end

local function password_encode(password)

	return password
end

function request:auth(username, password, ip)
	local encode_username = email_encode(username)
	local uid = db:get("account:email:"..encode_username)
	if not uid then
		return nil, "not exist email"
	end
	local encode_password = password_encode(password)
	local check_password = db:get("account:"..uid..":password")
	if encode_password ~= check_password then
		return nil, "error password"
	end
	local available = db:get("account:"..uid..":available")
	if available ~= "open" then
		return nil, available
	end
	local time = service.time()
	db:hmset("account:"..uid..":lastlogin", "ip", ip, "time", time)
	db:lpush("account:"..uid..":history", ip..":"..time)
	return uid
end

function request:regist(username, password, nickname, ip)
	local encode_username = email_encode(username)
	local ouid = db:get("account:email:"..encode_username)
	if ouid then
		return nil, "already exist email"
	end
	local time = service.time()
	local encode_password = password_encode(password)
	local uid = db:incr("account:count")

	db:sadd("account:userlist", uid)
	db:set("account:email:"..encode_username, uid)
	db:set("account:"..uid..":version", 1)
	db:set("account:"..uid..":email", username)
	db:set("account:"..uid..":password", encode_password)
	db:set("account:"..uid..":nickname", nickname)
	db:hmset("account:"..uid..":lastlogin", "ip", ip, "time", time)
	db:lpush("account:"..uid..":history", ip..":"..time)
	db:set("account:"..uid..":available", "open")
	return uid
end