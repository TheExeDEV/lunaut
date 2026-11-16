--!strict

export type Config = {
	maxPlayers: number,
	mode: string,
}

local alpha: Config = {
	maxPlayers = 10,
	mode = "fast",
}

return alpha
