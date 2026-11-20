use std::fs;

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

const REPOSITORY: &str = "TheExeDEV/lunaut";
const SERVER_NAME: &str = "lunaut";

struct LunautExtension {
    cached_binary_path: Option<String>,
}

impl LunautExtension {
    fn get_asset_name(platform: zed::Os, architecture: zed::Architecture) -> Result<&'static str> {
        match (platform, architecture) {
            (zed::Os::Windows, zed::Architecture::X8664) => Ok("lunaut-win-x64.exe"),
            (zed::Os::Linux, zed::Architecture::X8664) => Ok("lunaut-linux-x64"),
            (zed::Os::Mac, zed::Architecture::X8664) => Ok("lunaut-osx-x64"),
            (zed::Os::Mac, zed::Architecture::Aarch64) => Ok("lunaut-osx-arm64"),
            _ => Err(format!("lunaut has no build for {platform:?} {architecture:?}, put lunaut on PATH")),
        }
    }

    fn is_file(path: &str) -> bool {
        fs::metadata(path).is_ok_and(|metadata| metadata.is_file())
    }

    fn remove_old_versions(current: &str) {
        let Ok(entries) = fs::read_dir(".") else {
            return;
        };

        for entry in entries.flatten() {
            let name = entry.file_name().to_string_lossy().to_string();

            if name.starts_with("lunaut-") && name != current {
                fs::remove_dir_all(entry.path()).ok();
            }
        }
    }

    fn get_binary_path(&mut self, language_server_id: &LanguageServerId, worktree: &zed::Worktree) -> Result<String> {
        if let Some(path) = &self.cached_binary_path {
            if Self::is_file(path) {
                return Ok(path.clone());
            }
        }

        match self.download_latest(language_server_id) {
            Ok(path) => Ok(path),
            Err(error) => {
                zed::set_language_server_installation_status(language_server_id, &zed::LanguageServerInstallationStatus::None);
                worktree.which(SERVER_NAME).ok_or(error)
            }
        }
    }

    fn download_latest(&mut self, language_server_id: &LanguageServerId) -> Result<String> {
        zed::set_language_server_installation_status(language_server_id, &zed::LanguageServerInstallationStatus::CheckingForUpdate);

        let release = zed::latest_github_release(
            REPOSITORY,
            zed::GithubReleaseOptions {
                require_assets: true,
                pre_release: false,
            },
        )?;
        let (platform, architecture) = zed::current_platform();
        let asset_name = Self::get_asset_name(platform, architecture)?;
        let asset = release
            .assets
            .iter()
            .find(|asset| asset.name == asset_name)
            .ok_or_else(|| format!("release {} has no {asset_name}", release.version))?;
        let directory = format!("lunaut-{}", release.version);
        let executable = if platform == zed::Os::Windows { "lunaut.exe" } else { "lunaut" };
        let binary_path = format!("{directory}/{executable}");

        if !Self::is_file(&binary_path) {
            zed::set_language_server_installation_status(language_server_id, &zed::LanguageServerInstallationStatus::Downloading);
            fs::create_dir_all(&directory).map_err(|error| format!("cannot create {directory}: {error}"))?;
            zed::download_file(&asset.download_url, &binary_path, zed::DownloadedFileType::Uncompressed)
                .map_err(|error| format!("cannot download {asset_name}: {error}"))?;
            zed::make_file_executable(&binary_path)?;
            Self::remove_old_versions(&directory);
        }

        zed::set_language_server_installation_status(language_server_id, &zed::LanguageServerInstallationStatus::None);
        self.cached_binary_path = Some(binary_path.clone());
        Ok(binary_path)
    }
}

impl zed::Extension for LunautExtension {
    fn new() -> Self {
        Self { cached_binary_path: None }
    }

    fn language_server_command(&mut self, language_server_id: &LanguageServerId, worktree: &zed::Worktree) -> Result<zed::Command> {
        let binary = LspSettings::for_worktree(SERVER_NAME, worktree).ok().and_then(|settings| settings.binary);

        if let Some(path) = binary.as_ref().and_then(|binary| binary.path.clone()) {
            return Ok(zed::Command {
                command: path,
                args: binary.as_ref().and_then(|binary| binary.arguments.clone()).unwrap_or_else(|| vec!["lsp".into()]),
                env: binary.and_then(|binary| binary.env).map(|env| env.into_iter().collect()).unwrap_or_default(),
            });
        }

        Ok(zed::Command {
            command: self.get_binary_path(language_server_id, worktree)?,
            args: vec!["lsp".into()],
            env: Default::default(),
        })
    }

    fn language_server_initialization_options(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Option<zed::serde_json::Value>> {
        Ok(LspSettings::for_worktree(SERVER_NAME, worktree).ok().and_then(|settings| settings.initialization_options))
    }

    fn language_server_workspace_configuration(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<Option<zed::serde_json::Value>> {
        Ok(LspSettings::for_worktree(SERVER_NAME, worktree).ok().and_then(|settings| settings.settings))
    }
}

zed::register_extension!(LunautExtension);
