--!strict

local Service = require("./service")

return function(): Service.scheduler
	return Service.make()
end
