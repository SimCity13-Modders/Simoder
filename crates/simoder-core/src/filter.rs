//! Optional subset TGI filter used for mod resource matching and inspection.

use crate::Tgi;

/// Selects DBPF resources by any optional subset of semantic `(type_id, group, instance)` fields.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct ResourceFilter {
    /// Required DBPF resource type ID, or `None` to match any type.
    pub type_id: Option<u32>,
    /// Required DBPF resource group ID, or `None` to match any group.
    pub group: Option<u32>,
    /// Required DBPF resource instance ID, or `None` to match any instance.
    pub instance: Option<u32>,
}

impl ResourceFilter {
    /// Construct a resource filter from optional `(type_id, group, instance)` fields (`None` acts as wildcard).
    pub const fn new(type_id: Option<u32>, group: Option<u32>, instance: Option<u32>) -> Self {
        Self {
            type_id,
            group,
            instance,
        }
    }

    /// Construct a resource filter matching one exact [`Tgi`].
    pub const fn exact(tgi: Tgi) -> Self {
        Self {
            type_id: Some(tgi.type_id),
            group: Some(tgi.group),
            instance: Some(tgi.instance),
        }
    }

    /// Report whether every configured field matches the supplied [`Tgi`] identifier.
    pub fn matches(&self, tgi: Tgi) -> bool {
        self.type_id.is_none_or(|expected| expected == tgi.type_id)
            && self.group.is_none_or(|expected| expected == tgi.group)
            && self.instance.is_none_or(|expected| expected == tgi.instance)
    }
}
