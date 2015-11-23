local service = require "service"
local socket = require "socket"

local function server()
	local host
	host = socket.udp(function(str, from)
		print("server recv", str, socket.udp_address(from))
		socket.sendto(host, from, "OK " .. str)
	end , "127.0.0.1", 8765)	-- bind an address
end

local function client()
	local c = socket.udp(function(str, from)
		print("client recv", str, socket.udp_address(from))
	end)
	socket.udp_open(c, "127.0.0.1", 8765)
	for i=1,20 do
		socket.write(c, "hello " .. i)	-- write to the address by udp_connect binding
	end
end

service.start(function()
	service.fork(server)
	service.fork(client)
end)