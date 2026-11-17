local Item = require("./item")

local sample: marked = {
	owner = nil,
	tag = "sample",
	name = "sample",
	id = "sample",
}

function get_marked<T>(): { [unknown]: marked }
	return { sample }
end

for _, entry in get_marked() do
	print(entry.tag, Item)
end
