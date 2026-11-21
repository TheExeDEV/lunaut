--!strict

local Ui = {}

export type Theme = {
	background: Color3,
	panel: Color3,
	border: Color3,
	text: Color3,
	dimmed: Color3,
	accent: Color3,
	danger: Color3,
	field: Color3,
}

export type Checkbox = {
	frame: Frame,
	set: (checked: boolean) -> (),
	changed: BindableEvent,
}

local accent = Color3.fromRGB(11, 132, 255)
local danger = Color3.fromRGB(214, 69, 69)

function Ui.getTheme(): Theme
	local studio = settings().Studio.Theme :: StudioTheme

	return {
		background = studio:GetColor(Enum.StudioStyleGuideColor.MainBackground),
		panel = studio:GetColor(Enum.StudioStyleGuideColor.Titlebar),
		border = studio:GetColor(Enum.StudioStyleGuideColor.Border),
		text = studio:GetColor(Enum.StudioStyleGuideColor.MainText),
		dimmed = studio:GetColor(Enum.StudioStyleGuideColor.DimmedText),
		accent = accent,
		danger = danger,
		field = studio:GetColor(Enum.StudioStyleGuideColor.InputFieldBackground),
	}
end

function Ui.corner(parent: Instance, radius: number)
	local corner = Instance.new("UICorner")
	corner.CornerRadius = UDim.new(0, radius)
	corner.Parent = parent
end

function Ui.label(parent: Instance, text: string, size: number, bold: boolean, order: number): TextLabel
	local label = Instance.new("TextLabel")
	label.BackgroundTransparency = 1
	label.Size = UDim2.new(1, 0, 0, 0)
	label.AutomaticSize = Enum.AutomaticSize.Y
	label.Font = if bold then Enum.Font.SourceSansBold else Enum.Font.SourceSans
	label.TextSize = size
	label.TextWrapped = true
	label.TextXAlignment = Enum.TextXAlignment.Left
	label.Text = text
	label.LayoutOrder = order
	label.Parent = parent
	return label
end

function Ui.button(parent: Instance, text: string, height: number, order: number): TextButton
	local button = Instance.new("TextButton")
	button.Size = UDim2.new(1, 0, 0, height)
	button.AutoButtonColor = true
	button.Font = Enum.Font.SourceSansSemibold
	button.TextSize = 18
	button.Text = text
	button.LayoutOrder = order
	button.BorderSizePixel = 0
	Ui.corner(button, 4)
	button.Parent = parent
	return button
end

function Ui.checkbox(parent: Instance, title: string, description: string, order: number): Checkbox
	local frame = Instance.new("Frame")
	frame.BackgroundTransparency = 1
	frame.Size = UDim2.new(1, 0, 0, 0)
	frame.AutomaticSize = Enum.AutomaticSize.Y
	frame.LayoutOrder = order
	frame.Parent = parent

	local box = Instance.new("TextButton")
	box.Name = "Box"
	box.Size = UDim2.fromOffset(26, 26)
	box.Text = ""
	box.AutoButtonColor = false
	box.BorderSizePixel = 0
	Ui.corner(box, 4)
	box.Parent = frame

	local stroke = Instance.new("UIStroke")
	stroke.Thickness = 1
	stroke.ApplyStrokeMode = Enum.ApplyStrokeMode.Border
	stroke.Parent = box

	local name = Instance.new("TextButton")
	name.Name = "Title"
	name.BackgroundTransparency = 1
	name.Position = UDim2.fromOffset(38, 0)
	name.Size = UDim2.new(1, -38, 0, 26)
	name.Font = Enum.Font.SourceSans
	name.TextSize = 18
	name.TextXAlignment = Enum.TextXAlignment.Left
	name.Text = title
	name.Parent = frame

	local hint = Instance.new("TextLabel")
	hint.Name = "Description"
	hint.BackgroundTransparency = 1
	hint.Position = UDim2.fromOffset(0, 30)
	hint.Size = UDim2.new(1, 0, 0, 0)
	hint.AutomaticSize = Enum.AutomaticSize.Y
	hint.Font = Enum.Font.SourceSans
	hint.TextSize = 15
	hint.TextWrapped = true
	hint.TextXAlignment = Enum.TextXAlignment.Left
	hint.Text = description
	hint.Parent = frame

	local changed = Instance.new("BindableEvent")
	local checked = false

	local function set(value: boolean)
		checked = value
		box:SetAttribute("Checked", value)
	end

	local function toggle()
		set(not checked)
		changed:Fire(checked)
	end

	box.Activated:Connect(toggle)
	name.Activated:Connect(toggle)

	return { frame = frame, set = set, changed = changed }
end

function Ui.paint(root: Instance, theme: Theme)
	for _, item in root:GetDescendants() do
		local role = item:GetAttribute("Role")

		if item:IsA("TextLabel") or item:IsA("TextButton") or item:IsA("TextBox") then
			item.TextColor3 = if item.Name == "Description" or role == "Hint" then theme.dimmed else theme.text
		end

		if item:IsA("UIStroke") then
			item.Color = theme.border
		end

		if item.Name == "Box" and item:IsA("TextButton") then
			item.BackgroundColor3 = if item:GetAttribute("Checked") then theme.accent else theme.field
		elseif role == "Primary" and item:IsA("TextButton") then
			item.BackgroundColor3 = if item:GetAttribute("Active") then theme.danger else theme.accent
			item.TextColor3 = Color3.new(1, 1, 1)
		elseif role == "Secondary" and item:IsA("TextButton") then
			item.BackgroundColor3 = theme.panel
		elseif role == "Field" and item:IsA("TextBox") then
			item.BackgroundColor3 = theme.field
		elseif role == "Background" and item:IsA("GuiObject") then
			item.BackgroundColor3 = theme.background
		end
	end
end

return Ui
