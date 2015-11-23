local service = require "service"

local mode = ...

if mode == "slave" then

	service.start(function()
		service.dispatch("lua", function(_,_,cmd,n,...)
			if cmd == "sum" then
				service.log("for loop begin %d\n", service.now())
				local s = 0
				for i = 1, n do
					s = s + i
				end
				service.log("for loop end %d\n", service.now())
			end
		end)
	end)

else

	service.start(function()

		local slave = service.create(SERVICE_NAME, "slave")
		for step = 1, 20 do
			service.log("overload test "..step.."\n")
			for i = 1, 512 * step do
				service.send(slave, "lua", "blackhole")
			end
			service.sleep(step)
		end
		local n = 1000000000
		service.log("endless test n=%d\n", n)
		service.send(slave, "lua", "sum", n)
	end)

end