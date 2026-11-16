--!strict

export type Base = {
	count: number,
}

export type Extra = {
	scale: number,
}

-- Intersected shape.
export type Shape = Base & Extra

local shape: Shape = { count = 1, scale = 2 }

return shape