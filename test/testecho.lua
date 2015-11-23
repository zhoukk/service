local service = require "service"

local mode = ...

if mode == "slave" then

	service.start(function()
		service.dispatch("lua", function(_,_,...)
			service.ret(...)
		end)
	end)

else

	service.start(function()

		local slave = service.create(SERVICE_NAME, "slave")
		local n = 100000
		local start = service.now()
		print("call salve", n, "times in queue")
		for i=1, n do
			service.call(slave, "lua")
		end
		print("qps = ", n / (service.now() - start) * 100)

		start = service.now()

		local worker = 10
		local task = n/worker
		print("call salve", n, "times in parallel, worker = ", worker)

		for i=1, worker do
			service.fork(function()
				for i=1,task do
					service.call(slave, "lua")
				end
				worker = worker -1
				if worker == 0 then
					print("qps = ", n/ (service.now() - start) * 100)
				end
			end)
		end
	end)

end
