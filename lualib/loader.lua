local args = {}
for word in string.gmatch(..., "%S+") do
	table.insert(args, word)
end

SERVICE_NAME = args[1]

local err = {}
for pat in string.gmatch(SERVICE_PATH, "([^;]+);*") do
	local filename = string.gsub(pat, "?", SERVICE_NAME)
	local f, msg = loadfile(filename)
	if not f then
		table.insert(err, msg)
	else
		main = f
		break
	end
end

package.path = LUA_PATH
package.cpath = LUA_CPATH

if not main then
	error(table.concat(err, "\n"))
end
main(select(2, table.unpack(args)))