local service = require "service"
local sprotoloader = require "sprotoloader"
local proto = require "example.proto"

service.start(function()
	sprotoloader.save(proto.c2s, 1)
	sprotoloader.save(proto.s2c, 2)
end)

-- sprotoloader.delete(1)
-- sprotoloader.delete(2)