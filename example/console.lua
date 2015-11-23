local service = require "service"
local socket = require "socket"

service.start(function()
	service.fork(function()
		local stdin = socket.stdin()
		socket.lock(stdin)
		while true do
			local cmdline = socket.readline(stdin, "\n")
			if cmdline ~= "" then
				cmdline = cmdline.." "
				local args = {}
				for m in cmdline:gmatch("(.-) ") do
					table.insert(args, m)
				end
				pcall(service.create, args[1], table.unpack(args, 2))
			end
		end
		socket.unlock(stdin)
	end)
end)