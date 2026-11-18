--!strict

local function get_kind(variant: enumitem_shared_net_variant?)
	const kind = variant or Safe
	print(kind)
end

print(get_kind)
