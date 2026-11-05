# Test report

- Date: 2026-11-05 20:54:16
- Binary: `C:\Users\win11\Desktop\lusp\build\lunaut.exe`
- Version: `lunaut 0.1.0 (Luau 0.742, GNU 16.2.0)`
- Result: **62/62 passed** in 2.5s

| Test | Status | Time |
| --- | --- | --- |
| `test_cli.CliTests.test_analyze_clean_file` | PASS | 1.94s |
| `test_cli.CliTests.test_analyze_reports_errors` | PASS | 0.03s |
| `test_cli.CliTests.test_analyze_unknown_require` | PASS | 0.03s |
| `test_cli.CliTests.test_help` | PASS | 0.01s |
| `test_cli.CliTests.test_unknown_option` | PASS | 0.01s |
| `test_cli.CliTests.test_version` | PASS | 0.01s |
| `test_config.ConfigTests.test_base_luaurc_and_lua_files` | PASS | 0.03s |
| `test_config.ConfigTests.test_config_luau_aliases` | PASS | 0.03s |
| `test_config.ConfigTests.test_require_path_completion` | PASS | 0.03s |
| `test_definitions.DefinitionTests.test_cli_duplicate_keeps_config_priority` | PASS | 0.03s |
| `test_definitions.DefinitionTests.test_priority_order_loads_dependent_files` | PASS | 0.03s |
| `test_definitions.DefinitionTests.test_wrong_priority_fails` | PASS | 0.02s |
| `test_fflags.FFlagTests.test_cli_override` | PASS | 0.01s |
| `test_fflags.FFlagTests.test_flags_enabled_by_default` | PASS | 0.01s |
| `test_fflags.FFlagTests.test_invalid_value` | PASS | 0.01s |
| `test_fflags.FFlagTests.test_json_output` | PASS | 0.01s |
| `test_fflags.FFlagTests.test_no_flags_enabled` | PASS | 0.01s |
| `test_fflags.FFlagTests.test_settings_override_over_lsp` | PASS | 0.02s |
| `test_fflags.FFlagTests.test_unknown_flag` | PASS | 0.01s |
| `test_lsp.LspTests.test_alias_require` | PASS | 0.00s |
| `test_lsp.LspTests.test_capabilities` | PASS | 0.00s |
| `test_lsp.LspTests.test_completion_members` | PASS | 0.00s |
| `test_lsp.LspTests.test_definition_cross_module` | PASS | 0.00s |
| `test_lsp.LspTests.test_definition_local` | PASS | 0.00s |
| `test_lsp.LspTests.test_definition_require` | PASS | 0.00s |
| `test_lsp.LspTests.test_diagnostics` | PASS | 0.00s |
| `test_lsp.LspTests.test_diagnostics_update_on_change` | PASS | 0.00s |
| `test_lsp.LspTests.test_document_symbols` | PASS | 0.00s |
| `test_lsp.LspTests.test_hover_function` | PASS | 0.00s |
| `test_lsp.LspTests.test_hover_local` | PASS | 0.00s |
| `test_lsp.LspTests.test_signature_help` | PASS | 0.00s |
| `test_lsp.LspTests.test_status` | PASS | 0.00s |
| `test_lsp.LspTests.test_unknown_method` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_local_references` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_prepare_rename` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_property_references_across_files` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_references_from_table_key_find_unopened_files` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_references_without_declaration` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_rename_local` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_rename_module_function` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_rename_rejects_bad_names` | PASS | 0.00s |
| `test_references.NewSolverReferenceTests.test_rename_table_key` | PASS | 0.00s |
| `test_references.ReferenceTests.test_local_references` | PASS | 0.00s |
| `test_references.ReferenceTests.test_prepare_rename` | PASS | 0.00s |
| `test_references.ReferenceTests.test_property_references_across_files` | PASS | 0.00s |
| `test_references.ReferenceTests.test_references_from_table_key_find_unopened_files` | PASS | 0.00s |
| `test_references.ReferenceTests.test_references_without_declaration` | PASS | 0.00s |
| `test_references.ReferenceTests.test_rename_local` | PASS | 0.00s |
| `test_references.ReferenceTests.test_rename_module_function` | PASS | 0.00s |
| `test_references.ReferenceTests.test_rename_rejects_bad_names` | PASS | 0.00s |
| `test_references.ReferenceTests.test_rename_table_key` | PASS | 0.00s |
| `test_rojo.RojoTests.test_completion_from_sourcemap_children` | PASS | 0.00s |
| `test_rojo.RojoTests.test_definition_into_sourcemap_module` | PASS | 0.00s |
| `test_rojo.RojoTests.test_definitions_loaded` | PASS | 0.00s |
| `test_rojo.RojoTests.test_hover_on_required_module` | PASS | 0.00s |
| `test_rojo.RojoTests.test_instance_requires_resolve` | PASS | 0.00s |
| `test_rojo.RojoTests.test_references_through_sourcemap` | PASS | 0.00s |
| `test_rojo.RojoTests.test_required_types_flow` | PASS | 0.00s |
| `test_rojo.RojoTests.test_roblox_magic_functions` | PASS | 0.00s |
| `test_rojo.RojoTests.test_roblox_magic_new_solver` | PASS | 0.03s |
| `test_rojo.RojoTests.test_script_instance_type` | PASS | 0.00s |
| `test_rojo.RojoTests.test_status` | PASS | 0.00s |
