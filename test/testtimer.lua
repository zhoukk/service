local service = require "service"

local function timeout(t)
	print(t)
end

local function wakeup(co)
	for i=1,5 do
		service.sleep(50)
		service.wakeup(co)
	end
end

local function test()
	service.timeout(10, function() print("test timeout 10") end)
	for i=1,10 do
		print("test sleep",i,service.now())
		service.sleep(1)
	end
end

service.start(function()
	test()

	service.fork(wakeup, coroutine.running())
	service.timeout(300, function() timeout "Hello World" end)
	for i = 1, 10 do
		print(i, service.now())
		print(service.sleep(100))
	end
	service.exit()
	print("Test timer exit")
end)

