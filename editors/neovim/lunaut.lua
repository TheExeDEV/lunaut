vim.filetype.add({ extension = { luau = "luau" } })

vim.lsp.config("lunaut", {
	cmd = { "lunaut", "lsp" },
	filetypes = { "luau" },
	root_markers = { "default.project.json", ".luaurc", ".config.luau", ".git" },
	init_options = {
		vscode = {
			["lunaut.inlayHints.parameterNames"] = "literals",
			["lunaut.inlayHints.variableTypes"] = true,
			["lunaut.completion.autocompleteEnd"] = true,
		},
	},
})

vim.lsp.enable("lunaut")

vim.api.nvim_create_autocmd("LspAttach", {
	callback = function(args)
		local client = vim.lsp.get_client_by_id(args.data.client_id)

		if client and client.name == "lunaut" and client:supports_method("textDocument/inlayHint") then
			vim.lsp.inlay_hint.enable(true, { bufnr = args.buf })
		end
	end,
})
