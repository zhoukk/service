local pixel = require "pixel"

local aoi = require "aoi"

local objs = {}

local map = aoi.new(100, 100)

function init()
	pixel.name "scene"
	pixel.fork(function()
		while true do
			for id, v in pairs(objs) do
				map:update(id, 1)
				v.x, v.y = map:pos(id)
				map:around(id, 10, 10, function(tid, what)
					print(tid, what)
				end)
			end
			pixel.sleep(10)
		end
	end)
end

function exit()

end

function request:enter(roleid, name, job, x, y)
	local id = map:enter()
	map:locate(id, x, y)
	objs[id] = {rid = roleid, name = name, job = job}
	return id
end

function request:level(id)
	map:leave(id)
end

function request:move(id, x, y)
	map:move(id, x, y)
end

function request:speed(id, speed)
	map:speed(id, speed)
end