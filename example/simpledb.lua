local service = require "service"

local db = {}
local request = {}

function request:set(key, val)
	local last = db[key]
	db[key] = val
	return last
end

function request:get(key)
	return db[key]
end


service.start(function()
	service.name "SIMPLEDB"
	service.serve(request)
end)

