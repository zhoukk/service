local service = require "service"
local socket = require "socket"

local mode, id = ...

if mode == "agent" then
	local function echo(id)
		socket.start(id)

		while true do
			local str = socket.read(id)
			if str then
				socket.write(id, str)
			else
				socket.close(id)
				return
			end
		end
	end
	service.start(function()
		service.fork(function()
			echo(tonumber(id))
			service.exit()
		end)
	end)
else
	local function accept(id)
		socket.start(id)
		socket.write(id, "Hello service\n")
		service.create(SERVICE_NAME, "agent", id)
		-- notice: Some data on this connection(id) may lost before new service start.
		-- So, be careful when you want to use start / abandon / start .
		socket.abandon(id)
	end

	service.start(function()

		local id = socket.listen("127.0.0.1", 8002)
		print("Listen socket :", "127.0.0.1", 8002, id)

		socket.start(id , function(id, addr)
			print("connect from " .. addr .. " " .. id)
			-- you have choices :
			-- 1. service.newservice("testsocket", "agent", id)
			-- 2. service.fork(echo, id)
			-- 3. accept(id)
			accept(id)
		end)
	end)
end