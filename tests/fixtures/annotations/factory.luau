--!strict

export type Shape = {
	count: number,
} & {
	scale: number,
}

-- Creates a shape.
function make(): Shape
	return { count = 1, scale = 2 }
end

return make
