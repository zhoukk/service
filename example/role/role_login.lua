
function M.save_role()
	M.__init__()
end

function M.load_role_list()
	role.role_list = database.req.role_list(U.uid, role.server)
end

function REQUEST.role_list(req)
	local list = {}
	for _, id in pairs(role.role_list) do
		local data = {}
		local db = get_role_db(id)
		local info = db.req.role_info(id)

		data.id = tonumber(id)
		data.name = info.name
		data.job = tonumber(info.job)
		data.level = tonumber(info.level)

		table.insert(list, data)
	end

	return {list = list}
end

function REQUEST.create_role(req)
	local name = req.name
	local job = req.job
	local id = database.req.create_role(U.uid, name, role.server)
	local db = get_role_db(id)
	local ret = db.req.create_role(id, name, job)
	if ret > 0 then
		M.load_role_list()
	end
	return {ret = ret}
end

function REQUEST.delete_role(req)
	local db = get_role_db(req.id)
	local ret = db.req.delete_role(req.id)
	if ret > 0 then
		database.req.delete_role(req.id)
		M.load_role_list()
	end
	return {ret = ret}
end

function REQUEST.load_role(req)
	local id = req.id
	if not role.role_list[id] then
		return {ret = -1}
	end

	role.db = get_role_db(id)

	local info = role.db.req.role_info(id)
	role.id = id
	role.name = info.name
	role.level = tonumber(info.level)
	role.job = tonumber(info.job)
	role.gold = tonumber(info.gold) or 0

	role.x = info.x or 1
	role.y = info.y or 1
	role.create_time = info.create_time or 0
	role.create_ip = info.create_ip or ""
	role.last_login_time = info.last_login_time or 0
	role.last_login_ip = info.last_login_ip or ""
	role.last_logout_time = info.last_logout_time or 0

	return {ret = 0}
end

function REQUEST.role_info(req)
	return {
		name = role.name,
		job = role.job,
		level = role.level,
		gold = role.gold,
		scene = role.scene,
	}
end