

function request:create_role(id, name, job)
	local key = "role:"..id..":data"
	db:hset(key, "name", name)
	db:hset(key, "job", job)
	db:hset(key, "level", 1)
	db:hset(key, "gold", 0)
	return id
end

function request:role_info(id)
	local data = db:hgetall("role:"..id..":data")
	local idx = 1
	local info = {}
	while idx < #data do
		info[data[idx]] = data[idx+1]
		idx = idx + 2
	end
	return info
end