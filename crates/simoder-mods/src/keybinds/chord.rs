//! Key-chord parsing, canonicalization, hierarchical contexts, and vanilla SimCity reserved shortcuts.

use std::fmt;

use serde::{Deserialize, Serialize};

pub(super) const MAX_CONTEXT_LEN: usize = 64;

/// Canonical modifier + key chord (for example `"Ctrl+Alt+C"`, `"F9"`, or `"E"`).
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct KeyChord {
    /// Whether `Ctrl` is required.
    pub ctrl: bool,
    /// Whether `Alt` is required.
    pub alt: bool,
    /// Whether `Shift` is required.
    pub shift: bool,
    /// Normalized primary key token (e.g., `"C"`, `"F9"`, `"Esc"`, `"Backquote"`).
    pub key: String,
}

impl KeyChord {
    /// Parse and normalize a human-readable key chord string such as `"ctrl+alt+c"` or `"F9"`.
    pub fn parse(raw: &str) -> Result<Self, String> {
        let trimmed = raw.trim();
        if trimmed.is_empty() {
            return Err("Key chord cannot be empty".to_string());
        }

        let mut ctrl = false;
        let mut alt = false;
        let mut shift = false;
        let mut key: Option<String> = None;

        for token in trimmed.split('+') {
            let part = token.trim();
            if part.is_empty() {
                return Err(format!("Invalid empty segment in key chord '{raw}'"));
            }
            let lower = part.to_ascii_lowercase();
            match lower.as_str() {
                "ctrl" | "control" => ctrl = true,
                "alt" => alt = true,
                "shift" => shift = true,
                _ => {
                    if key.is_some() {
                        return Err(format!(
                            "Key chord '{raw}' contains multiple primary keys"
                        ));
                    }
                    key = Some(normalize_key_token(part)?);
                }
            }
        }

        let primary = key.ok_or_else(|| {
            format!("Key chord '{raw}' must include a primary key in addition to modifiers")
        })?;

        Ok(Self {
            ctrl,
            alt,
            shift,
            key: primary,
        })
    }

    /// Return the canonical string representation (`Ctrl+Alt+Shift+<Key>`).
    pub fn canonical(&self) -> String {
        self.to_string()
    }
}

impl fmt::Display for KeyChord {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut first = true;
        for (active, label) in [
            (self.ctrl, "Ctrl"),
            (self.alt, "Alt"),
            (self.shift, "Shift"),
        ] {
            if active {
                if !first {
                    f.write_str("+")?;
                }
                f.write_str(label)?;
                first = false;
            }
        }
        if !first {
            f.write_str("+")?;
        }
        f.write_str(&self.key)
    }
}

fn normalize_key_token(token: &str) -> Result<String, String> {
    let trimmed = token.trim();
    if trimmed.len() == 1 {
        let ch = trimmed.chars().next().unwrap_or_default();
        if ch.is_ascii_alphabetic() {
            return Ok(ch.to_ascii_uppercase().to_string());
        }
        if ch.is_ascii_digit() {
            return Ok(ch.to_string());
        }
    }

    let lower = trimmed.to_ascii_lowercase();
    if let Some(num_str) = lower.strip_prefix('f')
        && let Ok(num) = num_str.parse::<u8>()
        && (1..=12).contains(&num)
    {
        return Ok(format!("F{num}"));
    }

    let normalized = match lower.as_str() {
        "space" | " " => "Space",
        "enter" | "return" => "Enter",
        "tab" => "Tab",
        "esc" | "escape" => "Esc",
        "backspace" | "bksp" => "Backspace",
        "delete" | "del" => "Delete",
        "insert" | "ins" => "Insert",
        "home" => "Home",
        "end" => "End",
        "pageup" | "pgup" => "PageUp",
        "pagedown" | "pgdn" => "PageDown",
        "up" | "arrowup" => "Up",
        "down" | "arrowdown" => "Down",
        "left" | "arrowleft" => "Left",
        "right" | "arrowright" => "Right",
        "backquote" | "grave" | "~" | "`" => "Backquote",
        "minus" | "-" => "Minus",
        "equal" | "equals" | "=" => "Equal",
        "bracketleft" | "[" => "BracketLeft",
        "bracketright" | "]" => "BracketRight",
        "semicolon" | ";" => "Semicolon",
        "quote" | "'" => "Quote",
        "comma" | "," => "Comma",
        "period" | "." => "Period",
        "slash" | "/" => "Slash",
        "backslash" | "\\" => "Backslash",
        _ => return Err(format!("Unsupported key token '{trimmed}'")),
    };
    Ok(normalized.to_string())
}

/// Validated hierarchical keybind context (e.g., `"global"`, `"main_menu"`, `"city"`, `"city.build"`).
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct KeybindContext(String);

impl KeybindContext {
    /// Validate and construct a [`KeybindContext`] from a dot-separated lowercase identifier.
    pub fn parse(raw: &str) -> Result<Self, String> {
        let trimmed = raw.trim();
        if trimmed.is_empty() || trimmed.len() > MAX_CONTEXT_LEN {
            return Err(format!(
                "Keybind context '{raw}' must be 1..={MAX_CONTEXT_LEN} characters"
            ));
        }
        if trimmed.starts_with('.') || trimmed.ends_with('.') || trimmed.contains("..") {
            return Err(format!(
                "Keybind context '{raw}' cannot have leading, trailing, or consecutive dots"
            ));
        }
        if !trimmed
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || matches!(c, '.' | '_' | '-'))
        {
            return Err(format!(
                "Keybind context '{raw}' must use lowercase ASCII letters, digits, '.', '_', or '-'"
            ));
        }
        Ok(Self(trimmed.to_string()))
    }

    /// Return the underlying context string slice.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Return `true` when two contexts can be active simultaneously and therefore collide.
    ///
    /// `"global"` overlaps every context, and a parent context (e.g. `"city"`) overlaps its
    /// descendant sub-contexts (e.g. `"city.build"`). Disjoint contexts (`"main_menu"` vs `"city.build"`,
    /// or `"city.build"` vs `"city.camera"`) do not overlap.
    pub fn overlaps(&self, other: &Self) -> bool {
        if self.0 == other.0 || self.0 == "global" || other.0 == "global" {
            return true;
        }
        is_dot_prefix(&self.0, &other.0) || is_dot_prefix(&other.0, &self.0)
    }
}

fn is_dot_prefix(prefix: &str, full: &str) -> bool {
    full.strip_prefix(prefix)
        .is_some_and(|rest| rest.starts_with('.'))
}

impl fmt::Display for KeybindContext {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

/// Built-in retail SimCity 2013 shortcut reserved by the game engine.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct VanillaReservedKeybind {
    /// Canonical key chord string.
    pub chord: &'static str,
    /// Active SimCity context for this vanilla shortcut.
    pub context: &'static str,
    /// Human-readable game feature owning the shortcut.
    pub title: &'static str,
}

/// Known retail SimCity 2013 shortcuts checked during conflict detection.
pub const VANILLA_RESERVED_KEYBINDS: &[VanillaReservedKeybind] = &[
    VanillaReservedKeybind {
        chord: "Ctrl+Shift+C",
        context: "global",
        title: "SimCity Cheat Console",
    },
    VanillaReservedKeybind {
        chord: "Esc",
        context: "global",
        title: "Game Menu / Cancel",
    },
    VanillaReservedKeybind {
        chord: "Backquote",
        context: "city",
        title: "Pause Simulation",
    },
    VanillaReservedKeybind {
        chord: "1",
        context: "city",
        title: "Turtle Speed",
    },
    VanillaReservedKeybind {
        chord: "2",
        context: "city",
        title: "Llama Speed",
    },
    VanillaReservedKeybind {
        chord: "3",
        context: "city",
        title: "Cheetah Speed",
    },
    VanillaReservedKeybind {
        chord: "W",
        context: "city.camera",
        title: "Camera Forward",
    },
    VanillaReservedKeybind {
        chord: "A",
        context: "city.camera",
        title: "Camera Left",
    },
    VanillaReservedKeybind {
        chord: "S",
        context: "city.camera",
        title: "Camera Back",
    },
    VanillaReservedKeybind {
        chord: "D",
        context: "city.camera",
        title: "Camera Right",
    },
    VanillaReservedKeybind {
        chord: "Q",
        context: "city.camera",
        title: "Camera Rotate Left",
    },
    VanillaReservedKeybind {
        chord: "E",
        context: "city.camera",
        title: "Camera Rotate Right",
    },
    VanillaReservedKeybind {
        chord: "B",
        context: "city.build",
        title: "Bulldoze Tool",
    },
    VanillaReservedKeybind {
        chord: "R",
        context: "city.build",
        title: "Roads Menu",
    },
    VanillaReservedKeybind {
        chord: "P",
        context: "city.build",
        title: "Power Menu",
    },
];
