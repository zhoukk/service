local service = require "service"

local mode = ...

if mode == "test" then
	service.start(function()
		service.dispatch("lua", function (...)
			print("====>", ...)
			service.exit()
		end)
	end)
elseif mode == "dead" then
	service.start(function()
		service.dispatch("lua", function (...)
			service.sleep(100)
			print("return", service.ret "")
		end)
	end)
else
	service.start(function()
		local test = service.create(SERVICE_NAME, "test")	-- launch self in test mode
		print(pcall(function() service.call(test, "lua", "dead call") end))
		local dead = service.create(SERVICE_NAME, "dead")	-- launch self in dead mode
		service.timeout(0, service.exit)	-- exit after a while, so the call never return
		service.call(dead, "lua", "whould not return")
	end)
end

