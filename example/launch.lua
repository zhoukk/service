local service = require "service"

local services = {}
local instance = {}

local request = {}

function request:launch(name, ...)
	local handle = service.launch(name, ...)
	local param = table.concat({...}, " ")
	local response = service.response()
	if handle then
		services[handle] = name .. " " .. param
		instance[handle] = response
	else
		response(false)
	end
	return service.noret
end

function request:launchok()
	local handle = self.source
	local response = instance[handle]
	if response then
		response(true, handle)
		instance[handle] = nil
	end
	return service.noret
end

function request:error()
	local handle = self.source
	local response = instance[handle]
	if response then
		response(false)
		instance[handle] = nil
	end
	services[handle] = nil
	return service.noret
end

function request:exit(addr)
	services[addr] = nil
end

function request:list()
	local list = {}
	for k, v in pairs(services) do
		list[k] = v
	end
	return list
end

function request:stat()
	local list = {}
	for k, v in pairs(services) do
		local stat = service.call(k, "debug", "STAT")
		list[k] = stat
	end
	return list
end

function request:mem()
	local list = {}
	for k, v in pairs(services) do
		local kb, bytes = service.call(k, "debug", "MEM")
		list[k] = string.format("%.2f Kb (%s)", kb, v)
	end
	return list
end

function request:kill(address)
	-- service.kill(address)
	local ret = { [address] = tostring(services[address]) }
	services[address] = nil
	return ret
end

function request:gc()
	for k, v in pairs(services) do
		service.call(k, "debug", "GC")
	end
	return request:mem()
end

service.start(function()
	service.serve(request)
end)