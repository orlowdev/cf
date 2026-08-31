//! The C! Zed extension's language-server glue: Zed calls `language_server_command` when a
//! `.cf` buffer opens, and we hand it the compiler itself — `cf lsp` speaks LSP over stdio.
//! The binary resolves from the worktree's PATH; the standard Zed `lsp.cf-lsp.binary`
//! settings override both path and arguments.

use zed_extension_api::{self as zed, settings::LspSettings, LanguageServerId, Result};

struct CfExtension;

impl zed::Extension for CfExtension {
    fn new() -> Self {
        CfExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let settings = LspSettings::for_worktree("cf-lsp", worktree)
            .ok()
            .and_then(|s| s.binary);

        let command = settings
            .as_ref()
            .and_then(|b| b.path.clone())
            .or_else(|| worktree.which("cf"))
            .ok_or_else(|| {
                "`cf` was not found on the worktree PATH — install the C! compiler, or set \
                 `lsp.cf-lsp.binary.path` in Zed settings"
                    .to_string()
            })?;

        let args = settings
            .and_then(|b| b.arguments)
            .unwrap_or_else(|| vec!["lsp".to_string()]);

        Ok(zed::Command {
            command,
            args,
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(CfExtension);
