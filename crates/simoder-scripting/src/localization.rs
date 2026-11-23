//! Bounded mod localization dictionaries, explicit author fallback chains, atomic reload, and `0x0A98EAF0` integration.

use std::collections::{BTreeMap, BTreeSet, HashMap};

use oxilangtag::LanguageTag;
use serde::Deserialize;
use simoder_formats::{LocaleItem, mod_locale_table_id, parse_locale_items, serialize_locale_items};

use crate::config::{
    MAX_LOCALE_ENTRIES_PER_FILE, MAX_LOCALE_FALLBACK_DEPTH, MAX_LOCALE_FILES_PER_MOD,
    MAX_LOCALE_FILE_BYTES, VANILLA_LOCALE_TABLE_IDS,
};
use crate::types::{ScriptError, normalize_contained_posix_path};

/// Outcome of resolving a localization key along the explicit Beta fallback chain (H-009).
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ResolvedText<'a> {
    /// Key resolved cleanly within one configured locale dictionary.
    Found {
        /// Matched BCP-47/game locale tag.
        locale: &'a str,
        /// Localized text value.
        text: &'a str,
    },
    /// Key was absent across every configured dictionary; contains diagnostic key identifier.
    MissingDiagnostic(String),
}

/// Explicit dictionary lookup implementing `game locale -> author fallbacks -> mod default -> key`.
#[derive(Debug, Default, Clone)]
pub struct LocaleFallbackChain {
    dictionaries: HashMap<String, HashMap<String, String>>,
    fallback_order: Vec<String>,
}

impl LocaleFallbackChain {
    /// Build a deterministic lookup chain with explicit ordered locale tags (no implicit guessing).
    pub fn new(game_locale: &str, author_fallbacks: &[&str], mod_default: &str) -> Self {
        let mut fallback_order = Vec::new();
        for tag in std::iter::once(game_locale)
            .chain(author_fallbacks.iter().copied())
            .chain(std::iter::once(mod_default))
        {
            if !tag.is_empty() && !fallback_order.iter().any(|seen| seen == tag) {
                fallback_order.push(tag.to_string());
            }
        }
        Self {
            dictionaries: HashMap::new(),
            fallback_order,
        }
    }

    /// Insert a bounded key-value translation dictionary for one exact `locale` tag.
    pub fn insert_dictionary(&mut self, locale: impl Into<String>, entries: HashMap<String, String>) {
        self.dictionaries.insert(locale.into(), entries);
    }

    /// Resolve one translation `key` without implicit language guessing (Beta rule H-009).
    pub fn resolve(&self, key: &str) -> ResolvedText<'_> {
        for locale in &self.fallback_order {
            if let Some(text) = self.dictionaries.get(locale).and_then(|map| map.get(key)) {
                return ResolvedText::Found {
                    locale: locale.as_str(),
                    text: text.as_str(),
                };
            }
        }
        ResolvedText::MissingDiagnostic(key.to_string())
    }
}

#[derive(Deserialize)]
struct BoundedLocaleMapDto(
    #[serde(with = "serde_with::rust::maps_duplicate_key_is_error")] BTreeMap<String, String>,
);

/// Authoritative RFC 5646 / BCP-47 locale identifier validation via [`oxilangtag::LanguageTag`].
pub fn is_valid_locale_tag(tag: &str) -> bool {
    (2..=35).contains(&tag.len()) && LanguageTag::parse(tag).is_ok()
}

/// Validate a normalized POSIX translation file path under `"locales/"`.
pub fn validate_locale_file_path(relative: &str) -> Result<(), ScriptError> {
    normalize_contained_posix_path("", relative, "locales/")
        .map(|_| ())
        .map_err(|msg| ScriptError::LocalizationError(format!("invalid locale path: {msg}")))
}

/// Validate and decode a bounded JSON translation object rejecting duplicate keys and empty keys.
pub fn parse_bounded_locale_json(raw: &[u8]) -> Result<BTreeMap<String, String>, ScriptError> {
    if raw.len() > MAX_LOCALE_FILE_BYTES {
        return Err(ScriptError::LocalizationError(format!(
            "locale dictionary size {} bytes exceeds max {} bytes",
            raw.len(),
            MAX_LOCALE_FILE_BYTES
        )));
    }
    let BoundedLocaleMapDto(entries) = serde_json::from_slice(raw).map_err(|err| {
        ScriptError::LocalizationError(format!("invalid JSON translation dictionary: {err}"))
    })?;
    if entries.len() > MAX_LOCALE_ENTRIES_PER_FILE {
        return Err(ScriptError::LocalizationError(format!(
            "locale entry count {} exceeds max {}",
            entries.len(),
            MAX_LOCALE_ENTRIES_PER_FILE
        )));
    }
    if entries.contains_key("") {
        return Err(ScriptError::LocalizationError(
            "locale key must not be empty".into(),
        ));
    }
    Ok(entries)
}

/// Configuration specifying one mod's explicit locale chain (`game_locale -> author_fallbacks -> mod_default`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModLocalizationConfig {
    /// Verified mod identifier used to namespace keys cleanly.
    pub mod_id: String,
    /// Exact game locale tag requested for lookup.
    pub game_locale: String,
    /// Author-declared fallback locale tags without duplicates or cycles.
    pub author_fallbacks: Vec<String>,
    /// Mandatory final mod default locale tag.
    pub mod_default: String,
}

impl ModLocalizationConfig {
    /// Validate locale identifiers, bounded fallback depth, and explicit acyclic fallback graph.
    pub fn validate(&self) -> Result<(), ScriptError> {
        if self.mod_id.is_empty() {
            return Err(ScriptError::LocalizationError(
                "localization mod_id must not be empty".into(),
            ));
        }
        if !is_valid_locale_tag(&self.game_locale) || !is_valid_locale_tag(&self.mod_default) {
            return Err(ScriptError::LocalizationError(format!(
                "invalid game ('{}') or mod default ('{}') locale tag",
                self.game_locale, self.mod_default
            )));
        }
        if self.author_fallbacks.len() > MAX_LOCALE_FALLBACK_DEPTH {
            return Err(ScriptError::LocalizationError(format!(
                "author fallback chain length {} exceeds max {}",
                self.author_fallbacks.len(),
                MAX_LOCALE_FALLBACK_DEPTH
            )));
        }
        let mut seen = BTreeSet::new();
        for tag in &self.author_fallbacks {
            if !is_valid_locale_tag(tag) {
                return Err(ScriptError::LocalizationError(format!(
                    "invalid fallback locale tag '{tag}'"
                )));
            }
            if tag == &self.mod_default || !seen.insert(tag.clone()) {
                return Err(ScriptError::LocalizationError(format!(
                    "cyclic or duplicate locale fallback tag '{tag}'"
                )));
            }
        }
        Ok(())
    }
}

/// Input dictionary source staged for atomic [`ModLocalizationCatalog`] validation.
#[derive(Debug, Clone, Copy)]
pub struct StagedLocaleFile<'a> {
    /// Exact locale tag served by this file (for example `"pl-PL"` or `"en-GB"`).
    pub locale: &'a str,
    /// Normalized POSIX path relative to mod root (under `"locales/"`).
    pub relative_path: &'a str,
    /// Raw UTF-8 JSON dictionary bytes.
    pub bytes: &'a [u8],
}

/// Validated multi-file mod localization catalog supporting atomic staged reload and explicit fallbacks.
#[derive(Debug, Clone)]
pub struct ModLocalizationCatalog {
    mod_id: String,
    generation: u64,
    chain: LocaleFallbackChain,
}

impl ModLocalizationCatalog {
    /// Build and validate a fresh catalog from `config` and `files` without modifying prior state.
    pub fn load(
        config: &ModLocalizationConfig,
        files: &[StagedLocaleFile<'_>],
        generation: u64,
    ) -> Result<Self, ScriptError> {
        config.validate()?;
        if files.len() > MAX_LOCALE_FILES_PER_MOD {
            return Err(ScriptError::LocalizationError(format!(
                "mod '{}' locale file count {} exceeds max {}",
                config.mod_id,
                files.len(),
                MAX_LOCALE_FILES_PER_MOD
            )));
        }

        let fallback_refs: Vec<&str> = config.author_fallbacks.iter().map(String::as_str).collect();
        let mut chain =
            LocaleFallbackChain::new(&config.game_locale, &fallback_refs, &config.mod_default);
        let mut seen_locales = BTreeSet::new();

        for file in files {
            if !is_valid_locale_tag(file.locale) {
                return Err(ScriptError::LocalizationError(format!(
                    "invalid dictionary locale tag '{}'",
                    file.locale
                )));
            }
            validate_locale_file_path(file.relative_path)?;
            if !seen_locales.insert(file.locale) {
                return Err(ScriptError::LocalizationError(format!(
                    "overlapping translation files for locale '{}'",
                    file.locale
                )));
            }
            let parsed = parse_bounded_locale_json(file.bytes)?;
            let hash_entries: HashMap<String, String> = parsed.into_iter().collect();
            chain.insert_dictionary(file.locale.to_string(), hash_entries);
        }

        Ok(Self {
            mod_id: config.mod_id.clone(),
            generation,
            chain,
        })
    }

    /// Atomically validate a replacement generation and publish it into `self` only on full success.
    pub fn stage_and_publish(
        &mut self,
        config: &ModLocalizationConfig,
        files: &[StagedLocaleFile<'_>],
    ) -> Result<u64, ScriptError> {
        let next_gen = self.generation.saturating_add(1);
        *self = Self::load(config, files, next_gen)?;
        Ok(self.generation)
    }

    /// Return the current committed catalog generation counter.
    pub const fn generation(&self) -> u64 {
        self.generation
    }

    /// Resolve a mod translation key using only exact locale tags and explicit author fallbacks.
    pub fn resolve(&self, key: &str) -> ResolvedText<'_> {
        self.chain.resolve(key)
    }

    /// Format a namespaced translation key identifier (`"<mod_id>:<key>"`).
    pub fn namespaced_key(&self, key: &str) -> String {
        format!("{}:{key}", self.mod_id)
    }
}

/// Serialized DBPF `0x0A98EAF0` mod locale table bound to a collision-free FNV-1 instance ID.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModLocaleResource {
    /// Collision-free FNV-1 instance ID computed from `"<mod_id>/strings"`.
    pub table_id: u32,
    /// Serialized `0x0A98EAF0` resource payload bytes (`3`-byte prefix + JSON string map).
    pub payload: Vec<u8>,
}

/// Encode `entries` for `mod_id` into a custom `0x0A98EAF0` DBPF locale table payload.
pub fn export_mod_locale_resource(
    mod_id: &str,
    entries: &[LocaleItem],
) -> Result<ModLocaleResource, ScriptError> {
    if mod_id.is_empty() {
        return Err(ScriptError::LocalizationError(
            "mod_id must not be empty when exporting locale resource".into(),
        ));
    }
    let table_id = mod_locale_table_id(mod_id);
    if VANILLA_LOCALE_TABLE_IDS.contains(&table_id) {
        return Err(ScriptError::LocalizationError(format!(
            "mod '{mod_id}' derived table ID {table_id:#010X} collides with base-game locale table"
        )));
    }
    let payload = serialize_locale_items(entries).map_err(ScriptError::LocalizationError)?;
    Ok(ModLocaleResource { table_id, payload })
}

/// Decode a binary `0x0A98EAF0` DBPF string table payload using [`simoder_formats`].
pub fn decode_mod_locale_resource(bytes: &[u8]) -> Result<Vec<LocaleItem>, ScriptError> {
    parse_locale_items(bytes).map_err(ScriptError::LocalizationError)
}
