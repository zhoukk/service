

function REQUEST.enter_scene(req)
	role.aid = scene.req.enter(role.id, role.name, role.job, role.x, role.y)
	scene.req.speed(role.aid, 10)
	return {ret = 1}
end

function REQUEST.move(req)
	scene.req.move(role.aid, req.x, req.y)
end

function REQUEST.leave_scene(req)
	scene.req.leave(role.aid)
end