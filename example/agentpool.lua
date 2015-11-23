local service = require "service"

local pool_num = ...

local pool = {}

service.start(function()
	service.dispatch("lua", function(...)
		local agent = table.remove(pool)
		if not agent then
			service.timeout(0, service.exit)
		end
		service.ret(agent)
	end)

	pool_num = tonumber(pool_num)
	for i=1, pool_num do
		local agent = service.create("agent")
		table.insert(pool, agent)
	end

	service.name "agentpool"
end)