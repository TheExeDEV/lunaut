--!strict

local Settings = require(script.Parent.Settings)

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

local function isExcluded(filter: Settings.Filter?, instance: Instance): boolean
	if filter == nil then
		return false
	end

	for _, excluded in filter.exclude do
		if instance == excluded then
			return true
		end
	end

	return false
end

function Tree.build(root: DataModel, limit: number?, filter: Settings.Filter?): (Node, number)
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

			if not isExcluded(filter, child) then
				table.insert(nodes, visit(child))
			end
		end

		node.Children = nodes
		return node
	end

	local tree: Node = { Name = "game", ClassName = "DataModel", Children = {} }
	local include = filter and filter.include

	if include == nil then
		for _, name in Tree.services do
			local ok, service = pcall(root.FindService, root, name)

			if ok and service and not isExcluded(filter, service) then
				table.insert(tree.Children :: { Node }, visit(service))
			end
		end

		return tree, count
	end

	local made: { [Instance]: Node } = { [root] = tree }

	local function getNode(instance: Instance): Node?
		local existing = made[instance]

		if existing then
			return existing
		end

		local parent = instance.Parent

		if parent == nil then
			return nil
		end

		local parentNode = getNode(parent)

		if parentNode == nil then
			return nil
		end

		count += 1
		local node: Node = { Name = instance.Name, ClassName = instance.ClassName }
		parentNode.Children = parentNode.Children or {}
		table.insert(parentNode.Children :: { Node }, node)
		made[instance] = node
		return node
	end

	for _, included in include do
		local covered = false

		for _, other in include do
			if other ~= included and included:IsDescendantOf(other) then
				covered = true
				break
			end
		end

		if covered or included == root or not included:IsDescendantOf(root) or not Settings.isWatched(filter, included) then
			continue
		end

		local parent = included.Parent
		local parentNode = parent and getNode(parent)

		if parentNode and count < maximum then
			local node = visit(included)
			parentNode.Children = parentNode.Children or {}
			table.insert(parentNode.Children :: { Node }, node)
			made[included] = node
		end
	end

	return tree, count
end

return Tree
