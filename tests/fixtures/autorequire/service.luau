--!strict

export type scheduler = {
	cancel: (self: scheduler) -> (),
}

return {
	make = function(): scheduler
		return {
			cancel = function(self: scheduler) end,
		}
	end,
}
