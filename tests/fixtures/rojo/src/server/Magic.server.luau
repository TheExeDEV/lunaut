--!strict
local part = Instance.new("Part")
part.Anchored = true
local copy = part:Clone()
copy.Material = Enum.Material.Wood
local material: Enum.Material = Enum.Material.Plastic
local thing: Instance = game
if thing:IsA("Part") then
	thing.Anchored = false
end
local found = game:FindFirstChildWhichIsA("Folder")
print(material, found)
