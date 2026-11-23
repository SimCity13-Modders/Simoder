//! Namespaced `RegisterHook` custom event bus with bounded schemas, verified emitter identity,
//! deterministic subscriber ordering, fault isolation policy, and generation retirement.

use std::collections::BTreeMap;
use std::sync::Arc;

use serde::{Deserialize, Serialize};
use slotmap::{SlotMap, new_key_type};

use crate::config::{MAX_EVENT_PAYLOAD_BYTES, MAX_EVENT_SUBSCRIBERS};
use crate::hooks::{HookValue, rank_in_load_order};
use crate::types::{FaultPolicy, ModIdentity, ScriptError};

/// Synchronous listener invoked when a custom event is emitted.
pub type EventHandler = Arc<dyn Fn(&HookValue) -> Result<Option<HookValue>, ScriptError> + Send + Sync>;

new_key_type! {
    /// Generational token returned by [`EventBus::subscribe`] for owner-checked unsubscription.
    pub struct EventSubscriptionId;
}

/// Versioned schema describing a registered custom mod event (Beta rule H-011).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct EventSchema {
    /// Author-declared schema revision (`>= 1`).
    pub version: u32,
    /// Short human-readable summary of event semantics and expected payload shape.
    pub description: String,
    /// Whether subscribers may return values collected by [`EventBus::emit`].
    pub expects_return: bool,
}

impl EventSchema {
    /// Validate non-zero schema version.
    pub fn new(version: u32, description: impl Into<String>, expects_return: bool) -> Result<Self, ScriptError> {
        if version == 0 {
            return Err(ScriptError::EventError("event schema version must be >= 1".into()));
        }
        Ok(Self {
            version,
            description: description.into(),
            expects_return,
        })
    }
}

/// Verified owner-namespaced event identifier (`"<owner_mod_id>:<event_name>"`).
#[derive(Debug, Clone, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize, Deserialize)]
pub struct EventKey {
    /// Verified mod ID owning this event namespace.
    pub owner_mod_id: String,
    /// Event identifier unique within `owner_mod_id`.
    pub event_name: String,
}

impl EventKey {
    /// Construct and validate an owner-namespaced [`EventKey`].
    pub fn new(
        owner_mod_id: impl Into<String>,
        event_name: impl Into<String>,
    ) -> Result<Self, ScriptError> {
        let owner_mod_id = owner_mod_id.into();
        let event_name = event_name.into();
        if owner_mod_id.is_empty() || event_name.is_empty() {
            return Err(ScriptError::EventError(
                "event owner mod ID and event name must not be empty".into(),
            ));
        }
        if event_name.contains(':')
            || event_name.contains('/')
            || event_name.chars().any(char::is_whitespace)
        {
            return Err(ScriptError::EventError(format!(
                "invalid event name '{event_name}': must not contain ':', '/', or whitespace"
            )));
        }
        Ok(Self {
            owner_mod_id,
            event_name,
        })
    }

    /// Canonical namespaced representation `"<owner_mod_id>:<event_name>"`.
    pub fn canonical_name(&self) -> String {
        format!("{}:{}", self.owner_mod_id, self.event_name)
    }
}

struct EventSubscriber {
    event_key: EventKey,
    seq: u64,
    subscriber: ModIdentity,
    handler: EventHandler,
}

struct RegisteredEvent {
    owner: ModIdentity,
    schema: EventSchema,
}

/// Outcome summary returned by [`EventBus::emit`].
#[derive(Debug, Clone, PartialEq, Default)]
pub struct EventEmitOutcome {
    /// Count of subscriber callbacks executed (or attempted before isolating a fault).
    pub delivered: usize,
    /// Collected subscriber return values when `expects_return` is enabled.
    pub responses: Vec<(ModIdentity, HookValue)>,
    /// Diagnostics captured when individual subscribers failed under [`FaultPolicy::Regular`].
    pub diagnostics: Vec<String>,
}

/// Authoritative owner-namespaced custom event bus implementing `RegisterHook` (Beta rule H-011).
#[derive(Default)]
pub struct EventBus {
    events: BTreeMap<EventKey, RegisteredEvent>,
    subscribers: SlotMap<EventSubscriptionId, EventSubscriber>,
    next_seq: u64,
}

impl EventBus {
    /// Create an empty [`EventBus`].
    pub fn new() -> Self {
        Self::default()
    }

    /// Register a namespaced public event owned exclusively by `owner`.
    pub fn register_event(
        &mut self,
        owner: ModIdentity,
        event_name: &str,
        schema: EventSchema,
    ) -> Result<EventKey, ScriptError> {
        if schema.version == 0 {
            return Err(ScriptError::EventError("event schema version must be >= 1".into()));
        }
        let key = EventKey::new(owner.mod_id.clone(), event_name)?;
        if let Some(existing) = self.events.get(&key)
            && existing.owner.generation == owner.generation
        {
            return Err(ScriptError::EventError(format!(
                "duplicate event registration '{}' in generation {}",
                key.canonical_name(),
                owner.generation
            )));
        }
        self.events.insert(key.clone(), RegisteredEvent { owner, schema });
        Ok(key)
    }

    /// Inspect registered schema metadata for `key`.
    pub fn schema(&self, key: &EventKey) -> Option<&EventSchema> {
        self.events.get(key).map(|entry| &entry.schema)
    }

    /// Subscribe `subscriber` to an existing event registered under `key`.
    pub fn subscribe(
        &mut self,
        subscriber: ModIdentity,
        key: &EventKey,
        handler: EventHandler,
    ) -> Result<EventSubscriptionId, ScriptError> {
        if !self.events.contains_key(key) {
            return Err(ScriptError::EventError(format!(
                "unknown event '{}'",
                key.canonical_name()
            )));
        }
        let count = self
            .subscribers
            .values()
            .filter(|sub| &sub.event_key == key)
            .count();
        if count >= MAX_EVENT_SUBSCRIBERS {
            return Err(ScriptError::EventError(format!(
                "event '{}' reached maximum {} subscribers",
                key.canonical_name(),
                MAX_EVENT_SUBSCRIBERS
            )));
        }
        self.next_seq = self.next_seq.wrapping_add(1);
        Ok(self.subscribers.insert(EventSubscriber {
            event_key: key.clone(),
            seq: self.next_seq,
            subscriber,
            handler,
        }))
    }

    /// Unsubscribe `subscription_id` only when requested by the owning `subscriber`.
    pub fn unsubscribe(
        &mut self,
        subscriber: &ModIdentity,
        subscription_id: EventSubscriptionId,
    ) -> Result<(), ScriptError> {
        let Some(entry) = self.subscribers.get(subscription_id) else {
            return Err(ScriptError::StaleHandle(
                "event subscription not found or retired".into(),
            ));
        };
        if &entry.subscriber != subscriber {
            return Err(ScriptError::PermissionDenied(format!(
                "mod '{}' cannot unsubscribe listener owned by '{}'",
                subscriber.mod_id, entry.subscriber.mod_id
            )));
        }
        self.subscribers.remove(subscription_id);
        Ok(())
    }

    /// Emit `payload` for `key`, strictly verifying emitter ownership and enforcing bounded schemas.
    pub fn emit(
        &self,
        emitter: &ModIdentity,
        key: &EventKey,
        payload: &HookValue,
        load_order: &[String],
        fault_policy: FaultPolicy,
    ) -> Result<EventEmitOutcome, ScriptError> {
        let entry = self.events.get(key).ok_or_else(|| {
            ScriptError::EventError(format!("unknown event '{}'", key.canonical_name()))
        })?;
        if &entry.owner != emitter {
            return Err(ScriptError::PermissionDenied(format!(
                "mod '{}' (gen {}) cannot impersonate event emitter '{}' (gen {}) for '{}'",
                emitter.mod_id,
                emitter.generation,
                entry.owner.mod_id,
                entry.owner.generation,
                key.canonical_name()
            )));
        }
        let payload_bytes = payload.estimated_bytes()?;
        if payload_bytes > MAX_EVENT_PAYLOAD_BYTES {
            return Err(ScriptError::EventError(format!(
                "event '{}' payload {} bytes exceeds max {} bytes",
                key.canonical_name(),
                payload_bytes,
                MAX_EVENT_PAYLOAD_BYTES
            )));
        }

        let mut ordered: Vec<&EventSubscriber> = self
            .subscribers
            .values()
            .filter(|sub| &sub.event_key == key)
            .collect();
        ordered.sort_by(|a, b| {
            rank_in_load_order(load_order, &a.subscriber.mod_id)
                .cmp(&rank_in_load_order(load_order, &b.subscriber.mod_id))
                .then_with(|| a.subscriber.mod_id.cmp(&b.subscriber.mod_id))
                .then_with(|| a.seq.cmp(&b.seq))
        });

        let mut outcome = EventEmitOutcome::default();
        for sub in ordered {
            outcome.delivered += 1;
            let delivery = match (sub.handler)(payload) {
                Ok(Some(_)) if !entry.schema.expects_return => Err(ScriptError::EventError(format!(
                    "subscriber '{}' returned a value for void event '{}'",
                    sub.subscriber.mod_id,
                    key.canonical_name()
                ))),
                Ok(Some(ret)) => ret.estimated_bytes().map(|_| Some(ret)),
                Ok(None) => Ok(None),
                Err(err) => Err(err),
            };
            match delivery {
                Ok(Some(ret)) => outcome.responses.push((sub.subscriber.clone(), ret)),
                Ok(None) => {}
                Err(err) => match fault_policy {
                    FaultPolicy::Dev => return Err(err),
                    FaultPolicy::Regular => outcome.diagnostics.push(format!(
                        "{} (gen {}): {err}",
                        sub.subscriber.mod_id, sub.subscriber.generation
                    )),
                },
            }
        }
        Ok(outcome)
    }

    /// Retire all event registrations AND subscriptions owned by `identity`.
    pub fn retire_generation(&mut self, identity: &ModIdentity) {
        self.events.retain(|_, entry| &entry.owner != identity);
        self.subscribers
            .retain(|_, sub| &sub.subscriber != identity && self.events.contains_key(&sub.event_key));
    }
}
