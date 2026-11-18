type function negate(ty)
	return types.negationof(ty)
end

export type not_number = negate<number>
