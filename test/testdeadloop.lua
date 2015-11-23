local service = require "service"

local function dead_loop()
    while true do
        service.sleep(0)
    end
end

service.start(function()
	service.fork(dead_loop)
end)