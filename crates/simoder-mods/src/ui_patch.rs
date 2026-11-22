#![forbid(unsafe_code)]

//! Compositional declarative UI patch descriptors, deterministic multi-mod composition,
//! `replace_slot` conflict detection, and view/event envelopes for the EAWebKit DOM patch host.

use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_with::DeserializeFromStr;
use strum::{EnumString, IntoStaticStr, VariantArray};

use crate::types::{ModError, is_valid_mod_id};

const MAX_UI_NODE_DEPTH: usize = 12;
const MAX_UI_NODES_PER_PATCH: usize = 256;
const MAX_UI_TEXT_LEN: usize = 4096;
const MAX_UI_SELECT_OPTIONS: usize = 128;

/// Placement operation specifying how a [`UiPatchDescriptor`] composes into a `(view, slot)` container.
#[derive(
    Debug,
    Clone,
    Copy,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "snake_case", ascii_case_insensitive)]
#[serde(rename_all = "snake_case")]
pub enum UiPatchOp {
    /// Append the node to the end of the named slot.
    Append,
    /// Prepend the node to the beginning of the named slot.
    Prepend,
    /// Insert the node immediately before the sibling matching [`UiPatchDescriptor::target`].
    InsertBefore,
    /// Insert the node immediately after the sibling matching [`UiPatchDescriptor::target`].
    InsertAfter,
    /// Replace the target element/slot named by [`UiPatchDescriptor::target`]; concurrent writers record a conflict.
    ReplaceSlot,
}

impl UiPatchOp {
    /// Return the canonical snake_case operation identifier.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Closed set of safe declarative UI element kinds rendered by the EAWebKit DOM patch applicator.
#[derive(
    Debug,
    Clone,
    Copy,
    Default,
    PartialEq,
    Eq,
    Hash,
    Serialize,
    DeserializeFromStr,
    EnumString,
    IntoStaticStr,
    VariantArray,
)]
#[strum(serialize_all = "snake_case", ascii_case_insensitive)]
#[serde(rename_all = "snake_case")]
pub enum UiNodeKind {
    /// Clickable GlassBox-styled menu or dialog button.
    Button,
    /// Grouped vertical or horizontal container card.
    #[default]
    Panel,
    /// Top-level modal overlay dialog (`modal.overlay` slot).
    Modal,
    /// Single horizontal layout row inside a panel or modal.
    Row,
    /// Plain-text paragraph or heading (`textContent` only; zero raw HTML).
    Text,
    /// Compact pill status or counter badge.
    Badge,
    /// Boolean switch control emitting click/toggle events.
    Toggle,
    /// Visual horizontal rule separating groups.
    Divider,
    /// Single-line text input field emitting commit/change events.
    Input,
    /// Dropdown option selector pairing values and localized labels.
    Select,
    /// Bounded numeric range slider emitting change events.
    Slider,
    /// Standard labelled checkbox control emitting boolean string state.
    Checkbox,
    /// Tabbed container selecting one active [`UiNodeKind::Tab`] child by ID.
    Tabs,
    /// Tab strip page belonging inside a [`UiNodeKind::Tabs`] container.
    Tab,
    /// Bounded horizontal progress bar rendered from `min..max` and `value`.
    Progress,
}

impl UiNodeKind {
    /// Return the canonical snake_case element kind.
    pub fn as_str(self) -> &'static str {
        self.into()
    }
}

/// Option entry inside a [`UiNodeKind::Select`] dropdown node.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiSelectOption {
    /// Option identifier returned in [`UiPatchEvent::value`] when selected.
    pub value: String,
    /// Human-readable option label rendered via `textContent`.
    pub label: String,
}

/// Safe declarative UI node tree (zero raw HTML or inline JavaScript allowed).
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiNode {
    /// Element kind rendered via safe `document.createElement` + `textContent`.
    pub kind: UiNodeKind,
    /// Optional DOM sub-identifier used as `targetId` in click events and sibling anchors.
    #[serde(default)]
    pub id: Option<String>,
    /// Optional primary button/control label.
    #[serde(default)]
    pub label: Option<String>,
    /// Optional descriptive or body text content.
    #[serde(default)]
    pub text: Option<String>,
    /// Optional badge counter or status pill text (for example `"1"`).
    #[serde(default)]
    pub badge: Option<String>,
    /// Optional action identifier sent back in [`UiPatchEvent`] when clicked/changed.
    #[serde(default)]
    pub action: Option<String>,
    /// Optional visual tone hint (`"primary"`, `"danger"`, `"warning"`, `"muted"`, `"scroll"`, `"footer"`).
    #[serde(default)]
    pub tone: Option<String>,
    /// Optional toggle/checkbox checked state.
    #[serde(default)]
    pub checked: Option<bool>,
    /// Disables user interaction on clickable controls when `true`.
    #[serde(default)]
    pub disabled: bool,
    /// Optional current text/numeric/active-tab value for `input`, `select`, `slider`, `tabs`, or `progress`.
    #[serde(default)]
    pub value: Option<String>,
    /// Optional placeholder hint for [`UiNodeKind::Input`].
    #[serde(default)]
    pub placeholder: Option<String>,
    /// Optional inclusive minimum integer bound for [`UiNodeKind::Slider`] or [`UiNodeKind::Progress`].
    #[serde(default)]
    pub min: Option<i64>,
    /// Optional inclusive maximum integer bound for [`UiNodeKind::Slider`] or [`UiNodeKind::Progress`].
    #[serde(default)]
    pub max: Option<i64>,
    /// Optional positive integer step increment for [`UiNodeKind::Slider`].
    #[serde(default)]
    pub step: Option<i64>,
    /// Selectable entries for [`UiNodeKind::Select`].
    #[serde(default)]
    pub options: Vec<UiSelectOption>,
    /// Nested child declarative nodes.
    #[serde(default)]
    pub children: Vec<UiNode>,
}

/// Owner-scoped declarative UI patch targeting a game view and slot.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiPatchDescriptor {
    /// Globally namespaced patch identifier (`"<owner_mod_id>.<local_id>"`).
    pub patch_id: String,
    /// Host-verified owning mod ID.
    pub owner_mod_id: String,
    /// Host-verified owning mod generation (`>= 1`).
    pub owner_generation: u64,
    /// Logical game UI view identifier (for example `"launcher.menu"`, `"main.menu"`, `"ingame.options"`).
    pub view: String,
    /// Logical container slot inside [`Self::view`] (for example `"menu.actions"`, `"modal.overlay"`).
    pub slot: String,
    /// Insertion or slot-replacement operation.
    pub op: UiPatchOp,
    /// Optional sibling/slot anchor identifier used by `insert_before`, `insert_after`, or `replace_slot`.
    #[serde(default)]
    pub target: Option<String>,
    /// Deterministic ordering weight (ascending order; ties break by `owner_mod_id` then `patch_id`).
    #[serde(default)]
    pub priority: i32,
    /// Root declarative [`UiNode`] rendered when this patch is active.
    pub node: UiNode,
}

/// Recorded conflict when multiple active patches issue [`UiPatchOp::ReplaceSlot`] on the same target.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiPatchConflict {
    /// Target UI view identifier.
    pub view: String,
    /// Target slot identifier.
    pub slot: String,
    /// Contested replacement target key.
    pub target: String,
    /// Patch that won deterministic ordering (`winner_patch_id`).
    pub winner_patch_id: String,
    /// Overridden competing patch (`loser_patch_id`).
    pub loser_patch_id: String,
}

/// Interaction event raised by the ES5 DOM applicator back to `Simoder-Base` / owning mods.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiPatchEvent {
    /// Namespaced patch identifier owning the interacted control.
    pub patch_id: String,
    /// Declared action string from the clicked control's [`UiNode::action`].
    pub action: String,
    /// Optional sub-control [`UiNode::id`] within the patch subtree.
    #[serde(default)]
    pub target_id: Option<String>,
    /// Optional string payload (for example toggle state `"true"` / `"false"`).
    #[serde(default)]
    pub value: Option<String>,
}

/// Current EAWebKit UI view observation reported back from the game's UI thread.
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UiViewObservation {
    /// High-level phase (`"cold"`, `"launcher.menu"`, `"main.menu"`, `"ingame.options"`).
    pub phase: String,
    /// Currently visible logical view identifiers.
    #[serde(default)]
    pub active_views: Vec<String>,
    /// Active UI locale tag reported by `scrui.gUIManager.mLocaleManager` (for example `"en-us"`, `"pl-pl"`).
    #[serde(default)]
    pub locale: String,
}

/// Deterministic multi-mod UI patch composer tracking generations, revisions, and slot conflicts.
#[derive(Debug, Default)]
pub struct UiPatchRegistry {
    revision: u64,
    patches: Vec<UiPatchDescriptor>,
}

impl UiPatchRegistry {
    /// Create an empty [`UiPatchRegistry`].
    pub const fn new() -> Self {
        Self {
            revision: 0,
            patches: Vec::new(),
        }
    }

    /// Return the current monotonic UI patch revision (`0` when unchanged).
    pub const fn revision(&self) -> u64 {
        self.revision
    }

    /// Validate and register (or replace same-ID owned) declarative [`UiPatchDescriptor`].
    pub fn register_patch(&mut self, mut patch: UiPatchDescriptor) -> Result<u64, ModError> {
        let raw_id = patch.patch_id.trim().to_string();
        if let Some(other) = self
            .patches
            .iter()
            .find(|p| p.patch_id == raw_id && p.owner_mod_id != patch.owner_mod_id)
        {
            return Err(ModError::Lifecycle(format!(
                "UI patch ID collision '{}': owned by '{}', rejected for '{}'",
                raw_id, other.owner_mod_id, patch.owner_mod_id
            )));
        }
        validate_patch_descriptor(&mut patch)?;
        if let Some(existing) = self.patches.iter_mut().find(|p| p.patch_id == patch.patch_id) {
            if existing.owner_mod_id != patch.owner_mod_id {
                return Err(ModError::Lifecycle(format!(
                    "UI patch ID collision '{}': owned by '{}', rejected for '{}'",
                    patch.patch_id, existing.owner_mod_id, patch.owner_mod_id
                )));
            }
            *existing = patch;
        } else {
            self.patches.push(patch);
        }
        self.revision = self.revision.saturating_add(1).max(1);
        Ok(self.revision)
    }

    /// Update an existing patch's [`UiNode`] tree, enforcing matching owner mod ID and generation.
    pub fn update_patch(
        &mut self,
        owner_mod_id: &str,
        owner_generation: u64,
        patch_id: &str,
        node: UiNode,
    ) -> Result<u64, ModError> {
        validate_node_tree(&node)?;
        let Some(existing) = self.patches.iter_mut().find(|p| p.patch_id == patch_id) else {
            return Err(ModError::Lifecycle(format!("unknown UI patch '{patch_id}'")));
        };
        if existing.owner_mod_id != owner_mod_id || existing.owner_generation != owner_generation {
            return Err(ModError::Lifecycle(format!(
                "stale or unauthorized owner for UI patch '{patch_id}'"
            )));
        }
        existing.node = node;
        self.revision = self.revision.saturating_add(1).max(1);
        Ok(self.revision)
    }

    /// Remove a single patch owned by `owner_mod_id`.
    pub fn remove_patch(&mut self, owner_mod_id: &str, patch_id: &str) -> bool {
        let before = self.patches.len();
        self.patches
            .retain(|p| !(p.patch_id == patch_id && p.owner_mod_id == owner_mod_id));
        if self.patches.len() != before {
            self.revision = self.revision.saturating_add(1).max(1);
            true
        } else {
            false
        }
    }

    /// Remove every UI patch belonging to `(owner_mod_id, owner_generation)` on mod disable or reload.
    pub fn revoke_generation(&mut self, owner_mod_id: &str, owner_generation: u64) -> usize {
        let before = self.patches.len();
        self.patches.retain(|p| {
            !(p.owner_mod_id == owner_mod_id && p.owner_generation == owner_generation)
        });
        let removed = before - self.patches.len();
        if removed > 0 {
            self.revision = self.revision.saturating_add(1).max(1);
        }
        removed
    }

    /// Replace the registry contents with a pre-composed batch committed by `Simoder-Base`'s `ui-manager`.
    pub fn replace_composed_snapshot(
        &mut self,
        revision_hint: u64,
        mut patches: Vec<UiPatchDescriptor>,
    ) -> Result<u64, ModError> {
        for patch in &mut patches {
            validate_patch_descriptor(patch)?;
        }
        self.patches = patches;
        self.revision = self.revision.saturating_add(1).max(revision_hint.max(1));
        Ok(self.revision)
    }

    /// Compose deterministically ordered patches and explicit `replace_slot` conflicts for `view`.
    pub fn composed_patches_for_view(
        &self,
        view: &str,
    ) -> (u64, Vec<UiPatchDescriptor>, Vec<UiPatchConflict>) {
        let filtered: Vec<UiPatchDescriptor> = self
            .patches
            .iter()
            .filter(|p| p.view == view)
            .cloned()
            .collect();
        let (composed, conflicts) = compose_sorted(filtered);
        (self.revision, composed, conflicts)
    }

    /// Compose all registered patches across every view and record any `replace_slot` conflicts.
    pub fn all_composed(&self) -> (u64, Vec<UiPatchDescriptor>, Vec<UiPatchConflict>) {
        let (composed, conflicts) = compose_sorted(self.patches.clone());
        (self.revision, composed, conflicts)
    }
}

fn compose_sorted(
    mut list: Vec<UiPatchDescriptor>,
) -> (Vec<UiPatchDescriptor>, Vec<UiPatchConflict>) {
    list.sort_by(|a, b| {
        a.view
            .cmp(&b.view)
            .then_with(|| a.slot.cmp(&b.slot))
            .then_with(|| a.priority.cmp(&b.priority))
            .then_with(|| a.owner_mod_id.cmp(&b.owner_mod_id))
            .then_with(|| a.patch_id.cmp(&b.patch_id))
    });

    let mut winners: BTreeMap<(String, String, String), String> = BTreeMap::new();
    let mut composed = Vec::with_capacity(list.len());
    let mut conflicts = Vec::new();

    for patch in list {
        if patch.op == UiPatchOp::ReplaceSlot {
            let target_key = patch
                .target
                .clone()
                .unwrap_or_else(|| "__default__".to_string());
            let key = (patch.view.clone(), patch.slot.clone(), target_key.clone());
            if let Some(winner_id) = winners.get(&key) {
                conflicts.push(UiPatchConflict {
                    view: patch.view,
                    slot: patch.slot,
                    target: target_key,
                    winner_patch_id: winner_id.clone(),
                    loser_patch_id: patch.patch_id,
                });
                continue;
            }
            winners.insert(key, patch.patch_id.clone());
        }
        composed.push(patch);
    }

    (composed, conflicts)
}

fn validate_patch_descriptor(patch: &mut UiPatchDescriptor) -> Result<(), ModError> {
    if !is_valid_mod_id(&patch.owner_mod_id) {
        return Err(ModError::InvalidModId(patch.owner_mod_id.clone()));
    }
    if patch.owner_generation == 0 {
        return Err(ModError::Lifecycle(
            "UI patch requires owner_generation >= 1".into(),
        ));
    }
    let local = patch.patch_id.trim();
    if local.is_empty() || local.len() > 128 {
        return Err(ModError::ManifestValidation(
            "UI patch_id must be 1..=128 bytes".into(),
        ));
    }
    let prefix = format!("{}.", patch.owner_mod_id);
    patch.patch_id = if local.starts_with(&prefix) {
        local.to_string()
    } else {
        format!("{prefix}{local}")
    };
    if patch.view.trim().is_empty()
        || patch.view.len() > 128
        || patch.slot.trim().is_empty()
        || patch.slot.len() > 128
    {
        return Err(ModError::ManifestValidation(
            "UI patch view and slot must be 1..=128 bytes".into(),
        ));
    }
    validate_node_tree(&patch.node)?;
    Ok(())
}

fn validate_node_tree(root: &UiNode) -> Result<(), ModError> {
    fn walk(node: &UiNode, depth: usize, count: &mut usize) -> Result<(), ModError> {
        if depth > MAX_UI_NODE_DEPTH {
            return Err(ModError::ManifestValidation(format!(
                "UI node tree exceeds max depth {MAX_UI_NODE_DEPTH}"
            )));
        }
        *count += 1;
        if *count > MAX_UI_NODES_PER_PATCH {
            return Err(ModError::ManifestValidation(format!(
                "UI node tree exceeds max node count {MAX_UI_NODES_PER_PATCH}"
            )));
        }
        for opt in [
            &node.id,
            &node.label,
            &node.text,
            &node.badge,
            &node.action,
            &node.tone,
            &node.value,
            &node.placeholder,
        ]
        .into_iter()
        .flatten()
        {
            if opt.len() > MAX_UI_TEXT_LEN {
                return Err(ModError::ManifestValidation(format!(
                    "UI node text field exceeds {MAX_UI_TEXT_LEN} bytes"
                )));
            }
        }
        if node.options.len() > MAX_UI_SELECT_OPTIONS {
            return Err(ModError::ManifestValidation(format!(
                "UI select options exceed max count {MAX_UI_SELECT_OPTIONS}"
            )));
        }
        for sel in &node.options {
            if sel.value.len() > MAX_UI_TEXT_LEN || sel.label.len() > MAX_UI_TEXT_LEN {
                return Err(ModError::ManifestValidation(format!(
                    "UI select option text exceeds {MAX_UI_TEXT_LEN} bytes"
                )));
            }
        }
        if let (Some(lo), Some(hi)) = (node.min, node.max)
            && lo > hi
        {
            return Err(ModError::ManifestValidation(
                "UI range min cannot exceed max".into(),
            ));
        }
        if let Some(step) = node.step
            && step <= 0
        {
            return Err(ModError::ManifestValidation(
                "UI range step must be greater than zero".into(),
            ));
        }
        for child in &node.children {
            walk(child, depth + 1, count)?;
        }
        Ok(())
    }
    let mut count = 0;
    walk(root, 0, &mut count)
}
