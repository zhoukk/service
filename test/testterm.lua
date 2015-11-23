local service = require "service"

local function term()
	service.err("Sleep one second, and term the call to UNEXIST\n")
	service.sleep(100)
	service.call(service.handle, "debug", "TERM", "UNEXIST")
end

service.start(function()
	service.fork(term)
	service.err("call an unexist named service UNEXIST, may block\n")
	print(pcall(service.call, "UNEXIST", "lua", "test"))
	service.err("unblock the unexisted service call\n")
end)
