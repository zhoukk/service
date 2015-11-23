local sprotoparser = require "sprotoparser"

local proto = {}

proto.type = [[

.userdata {
	boolval 0 : boolean
	intval 1 : integer
	strval 2 : string
	arrval 3: *string
}

]]

proto.c2s = sprotoparser.parse (proto.type..[[
.package {
	type 0 : integer
	session 1 : integer
}

handshake 1 {
	response {
		msg 0 : string
		data 1 : userdata
	}
}

quit 2 {}

set 3 {
	request {
		key 0 : string
		value 1 : string
	}
	response {
		value 0 : string
	}
}

get 4 {
	request {
		key 0 : string
	}
	response {
		value 0 : string
	}
}

]])

proto.s2c = sprotoparser.parse (proto.type..[[
.package {
	type 0 : integer
	session 1 : integer
}

heartbeat 1 {
	request {
		time 0 : integer
	}
}

]])

return proto
