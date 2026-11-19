#![forbid(unsafe_code)]

//! Beta script-extension contracts: deterministic hook chain orchestration and localization fallback.

use std::collections::HashMap;

/// Errors raised by script hook or override execution.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum ScriptError {
    /// The invoked body or hook returned an explicit error.
    #[error("hook or body execution failed: {0}")]
    InvocationFailed(String),
}

/// Callback type executed before the active body inside [`HookChain`].
pub type PreHookFn<A> = Box<dyn FnMut(&mut A) -> Result<(), ScriptError>>;

/// Callback type for an original function body or override inside [`HookChain`].
pub type BodyFn<A, R> = Box<dyn FnMut(&mut A) -> Result<R, ScriptError>>;

/// Callback type executed after a successful body inside [`HookChain`].
pub type PostHookFn<A, R> = Box<dyn FnMut(&mut A, R) -> Result<R, ScriptError>>;

/// Ordered hook chain enforcing Beta rules H-003, H-004, H-006, and H-007.
pub struct HookChain<A, R> {
    pre_hooks: Vec<(String, PreHookFn<A>)>,
    overrides: Vec<(String, BodyFn<A, R>)>,
    post_hooks: Vec<(String, PostHookFn<A, R>)>,
    original: BodyFn<A, R>,
}

impl<A, R> HookChain<A, R> {
    /// Create a [`HookChain`] wrapping an `original` function body with no hooks or overrides.
    pub fn new(original: BodyFn<A, R>) -> Self {
        Self {
            pre_hooks: Vec::new(),
            overrides: Vec::new(),
            post_hooks: Vec::new(),
            original,
        }
    }

    /// Append a `PreHook` executed before the selected body.
    pub fn add_pre_hook(&mut self, owner: impl Into<String>, hook: PreHookFn<A>) {
        self.pre_hooks.push((owner.into(), hook));
    }

    /// Append an `OverrideFunction` candidate; the last registered override wins deterministically.
    pub fn add_override(&mut self, owner: impl Into<String>, body: BodyFn<A, R>) {
        self.overrides.push((owner.into(), body));
    }

    /// Append a `PostHook` executed only on body success (Beta rule H-007) that may transform the return value (H-005).
    pub fn add_post_hook(&mut self, owner: impl Into<String>, hook: PostHookFn<A, R>) {
        self.post_hooks.push((owner.into(), hook));
    }

    /// Return the active override owner, or `None` when the original body runs.
    pub fn active_override_owner(&self) -> Option<&str> {
        self.overrides.last().map(|(owner, _)| owner.as_str())
    }

    /// Execute `PreHooks -> last Override (or original, never falling back on error) -> PostHooks`.
    pub fn invoke(&mut self, args: &mut A) -> Result<R, ScriptError> {
        for (_, pre) in &mut self.pre_hooks {
            pre(args)?;
        }

        // Beta H-004/H-006: last override wins and failures never fall back to the original body.
        let mut result = if let Some((_, override_body)) = self.overrides.last_mut() {
            override_body(args)?
        } else {
            (self.original)(args)?
        };

        // Beta H-007: PostHooks execute only after a successful body invocation.
        for (_, post) in &mut self.post_hooks {
            result = post(args, result)?;
        }

        Ok(result)
    }
}

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
