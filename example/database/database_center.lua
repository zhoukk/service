
function request:create_role(uid, name, server)
	local id = db:incr("role:id")
	db:sadd("account:"..uid..":roles", id)
	db:set("role:"..id..":account", uid)
	db:set("role:"..id..":available", "open")
	return id
end

function request:role_list(uid, server)
	local ids = db:smembers("account:"..uid..":roles")
	local ret = {}
	for _, v in pairs(ids) do
		v = tonumber(v)
		ret[v] = v
	end
	return ret
end

function request:delete_role(id)
	local uid = db:get("role:"..id..":account")
	db:set("role:"..id..":available", "delete")
	db:srem("role:"..id..":account", id)
end