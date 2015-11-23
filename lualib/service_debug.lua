
return function(service, export)

	local dbgcmd = {}

	function dbgcmd:MEM()
		local kb, bytes = collectgarbage "count"
		return kb, bytes
	end

	function dbgcmd:GC()
		export.clear()
		collectgarbage "collect"
	end

	function dbgcmd:STAT()
		local stat = {}
		stat.mqlen = service.mqlen()
		stat.task = service.task()
		return stat
	end

	function dbgcmd:TASK()
		local task = {}
		service.task(task)
		return task
	end

	function dbgcmd:TERM(source)
		service.term(source)
	end

	service.protocol {
		name = "debug",
		id = service.proto_debug,
		pack = service.pack,
		unpack = service.unpack,
	}

	service.serve(dbgcmd, "debug")
end