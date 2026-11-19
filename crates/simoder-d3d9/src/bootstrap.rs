//! Single-owner pinned bootstrap coordinator and trusted Windows path layout for `simoder-d3d9`.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};

use simoder_formats::{LoaderConfig, load_or_create_config};

use crate::config::{PROXY_DISABLED_MARKER_FILE, SIMODER_DATA_DIR_NAME, SYSTEM_D3D9_DLL_NAME};
use crate::pe::{BuildAssessment, assess_game_executable};
use crate::{D3d9Error, is_simcity_executable};

/// Resolved filesystem layout anchoring the proxy DLL, host game root, writable `simoder/` data folder,
/// and genuine Windows system `d3d9.dll`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstallPaths {
    /// Absolute path to the integrated `d3d9.dll` beside `SimCity.exe`.
    pub proxy_dll_path: PathBuf,
    /// Parent installation root containing `SimCity/` and `simoder/`.
    pub game_root: PathBuf,
    /// Writable Simoder data directory (`<game_root>/simoder`).
    pub data_root: PathBuf,
    /// Absolute path to the genuine OS Direct3D 9 DLL (`<system_dir>/d3d9.dll`).
    pub system_d3d9_path: PathBuf,
}

impl InstallPaths {
    /// Resolve and validate installation and system DLL paths without consulting `PATH` or `CWD`.
    ///
    /// Rejects layouts missing a game root directory or pointing `proxy_dll_path` directly at `system_d3d9_path`
    /// (preventing infinite proxy self-forwarding).
    pub fn resolve(proxy_dll_path: &Path, system_dir: &Path) -> Result<Self, D3d9Error> {
        if system_dir.as_os_str().is_empty() {
            return Err(D3d9Error::InvalidInstallPath(
                "system directory cannot be empty".to_string(),
            ));
        }
        let proxy_dir = proxy_dll_path.parent().ok_or_else(|| {
            D3d9Error::InvalidInstallPath("proxy DLL path has no parent directory".to_string())
        })?;
        let game_root = proxy_dir
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .ok_or_else(|| {
                D3d9Error::InvalidInstallPath(
                    "proxy DLL parent has no install root directory".to_string(),
                )
            })?
            .to_path_buf();

        let system_d3d9_path = system_dir.join(SYSTEM_D3D9_DLL_NAME);
        if normalized_windows_path(proxy_dll_path) == normalized_windows_path(&system_d3d9_path) {
            return Err(D3d9Error::InvalidInstallPath(
                "proxy DLL path cannot match genuine system d3d9.dll".to_string(),
            ));
        }

        let data_root = game_root.join(SIMODER_DATA_DIR_NAME);
        Ok(Self {
            proxy_dll_path: proxy_dll_path.to_path_buf(),
            game_root,
            data_root,
            system_d3d9_path,
        })
    }
}

/// Outcome returned by [`BootstrapCoordinator::try_begin_bootstrap`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum BootstrapOutcome {
    /// First-time initialization succeeded for `SimCity.exe` with `config.toon` loaded and host pinned.
    Initialized {
        /// Validated filesystem layout.
        paths: InstallPaths,
        /// Loaded (or newly seeded) [`LoaderConfig`] from `simoder/config.toon`.
        config: LoaderConfig,
        /// On-disk PE32 build assessment when `executable_path` exists on disk.
        build: Option<BuildAssessment>,
    },
    /// A previous call in this process already claimed the one-shot bootstrap slot.
    SkippedAlreadyRequested,
    /// Host executable was not `SimCity.exe`; graphics forwarding remains active without initializing Simoder.
    DisabledNonSimCityProcess {
        /// Observed process executable filename.
        executable_name: String,
    },
    /// Presence of `simoder/proxy.disabled` suppressed runtime startup cleanly.
    DisabledByMarkerFile {
        /// Path to the active disable marker file.
        marker_path: PathBuf,
    },
}

/// Process-lifetime single-owner coordinator enforcing one-shot bootstrap and module pinning.
///
/// Mirrors the stability invariant documented in `Simoder-OpenSCP/docs/re/simcity-frida-crash-20261023.md`:
/// a single bootstrap owner claims initialization once (`bootstrap_requested`) and keeps the DLL pinned
/// (`module_pinned`, modeling `GET_MODULE_HANDLE_EX_FLAG_PIN`) to prevent `LdrUnloadDll` unload races.
#[derive(Debug, Default)]
pub struct BootstrapCoordinator {
    bootstrap_requested: AtomicBool,
    module_pinned: AtomicBool,
}

impl BootstrapCoordinator {
    /// Create a fresh unrequested [`BootstrapCoordinator`].
    pub const fn new() -> Self {
        Self {
            bootstrap_requested: AtomicBool::new(false),
            module_pinned: AtomicBool::new(false),
        }
    }

    /// Return `true` after [`Self::try_begin_bootstrap`] has been entered once.
    pub fn is_requested(&self) -> bool {
        self.bootstrap_requested.load(Ordering::Acquire)
    }

    /// Return `true` once the proxy has claimed process-lifetime pinning for `SimCity.exe`.
    pub fn is_module_pinned(&self) -> bool {
        self.module_pinned.load(Ordering::Acquire)
    }

    /// Execute the bounded one-shot preflight and bootstrap sequence.
    ///
    /// Preserves non-`SimCity.exe` graphics callers without writing `config.toon`, honors `proxy.disabled`,
    /// seeds/loads `simoder/config.toon`, and assesses `executable_path` if present on disk.
    pub fn try_begin_bootstrap(
        &self,
        proxy_dll_path: &Path,
        executable_path: &Path,
        system_dir: &Path,
    ) -> Result<BootstrapOutcome, D3d9Error> {
        if self
            .bootstrap_requested
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return Ok(BootstrapOutcome::SkippedAlreadyRequested);
        }

        let paths = InstallPaths::resolve(proxy_dll_path, system_dir)?;
        if !is_simcity_executable(executable_path) {
            let executable_name = executable_path
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("")
                .to_string();
            return Ok(BootstrapOutcome::DisabledNonSimCityProcess { executable_name });
        }

        self.module_pinned.store(true, Ordering::Release);

        let marker_path = paths.data_root.join(PROXY_DISABLED_MARKER_FILE);
        if marker_path.exists() {
            return Ok(BootstrapOutcome::DisabledByMarkerFile { marker_path });
        }

        let config = load_or_create_config(&paths.data_root)?;
        let build = if executable_path.is_file() {
            Some(assess_game_executable(executable_path)?)
        } else {
            None
        };

        Ok(BootstrapOutcome::Initialized {
            paths,
            config,
            build,
        })
    }
}

fn normalized_windows_path(path: &Path) -> String {
    path.to_string_lossy()
        .replace('/', "\\")
        .to_ascii_lowercase()
}
