local service = require "service"

service.start(function()
	local launch = service.launch("launch")
	service.name("launch", launch)
	service.create("console")
	service.create("debug_console", 6000)
	service.create "protoloader"
	service.create "simpledb"
	-- service.create "database"
	-- service.create "scene"
	service.create("agentpool", 10)

	service.create("logind", 8000)
	local gate = service.create("gated")
	service.req(gate, "open", {
		port = 8001,
		maxclient = 64,
		servername = "sample",
	})
	service.exit()
end)

