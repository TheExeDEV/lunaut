--!priority 50
declare extern type Car extends Vehicle with
	brand: string
end

declare garage: { Car }
