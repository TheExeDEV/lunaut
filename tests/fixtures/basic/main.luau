--!strict
local util = require("./util")
local count: number = util.add(1, 2)
local wrong: string = count
print(util.greet("lunaut"), wrong)
local function unusedHelper() end
return {}
