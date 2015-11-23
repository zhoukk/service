local service = require "service"

service.start(function()
	print(service.starttime())
	print(service.now())
	print(service.time())

	service.timeout(1, function()
		print("in 1", service.now())
	end)
	service.timeout(2, function()
		print("in 2", service.now())
	end)
	service.timeout(3, function()
		print("in 3", service.now())
	end)

	service.timeout(4, function()
		print("in 4", service.now())
	end)
	service.timeout(100, function()
		print("in 100", service.now())
	end)
end)