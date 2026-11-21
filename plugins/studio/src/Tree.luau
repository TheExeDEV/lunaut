--!strict

export type Node = {
	Name: string,
	ClassName: string,
	Children: { Node }?,
}

local Tree = {}

Tree.services = {
	"Workspace",
	"Players",
	"Lighting",
	"MaterialService",
	"ReplicatedFirst",
	"ReplicatedStorage",
	"ServerScriptService",
	"ServerStorage",
	"StarterGui",
	"StarterPack",
	"StarterPlayer",
	"SoundService",
	"Teams",
	"TextChatService",
	"Chat",
	"LocalizationService",
	"TestService",
}

Tree.limit = 200000

function Tree.build(root: DataModel, limit: number?): (Node, number)
	local maximum = limit or Tree.limit
	local count = 0

	local function visit(instance: Instance): Node
		count += 1
		local node: Node = { Name = instance.Name, ClassName = instance.ClassName }

		if count >= maximum then
			return node
		end

		local ok, children = pcall(instance.GetChildren, instance)

		if not ok or #children == 0 then
			return node
		end

		local nodes = {}

		for _, child in children do
			if count >= maximum then
				break
			end

			table.insert(nodes, visit(child))
		end

		node.Children = nodes
		return node
	end

	local services = {}

	for _, name in Tree.services do
		local ok, service = pcall(root.FindService, root, name)

		if ok and service then
			table.insert(services, visit(service))
		end
	end

	return { Name = "game", ClassName = "DataModel", Children = services }, count
end

return Tree
