//! Shared fixtures, mock PROP host observer, and recording scheduler sink for `simoder-scripting` integration tests.

#![allow(dead_code)]

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use simoder_core::Tgi;
use simoder_scripting::{
    DueDelayedCall, FloatObservation, ModIdentity, PropObserver, PropertyAlias, SchedulerTickSink,
    ScriptDefinition, ScriptError, ScriptLanguage, ScriptLimits, ScriptModuleSource,
    ScriptPatchCommand, VmGeneration,
};

type PropObservationKey = ((u32, u32, u32), u32);

pub struct MockPropHost {
    observations: Mutex<BTreeMap<PropObservationKey, FloatObservation>>,
}

impl MockPropHost {
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            observations: Mutex::new(BTreeMap::new()),
        })
    }

    pub fn insert(&self, target: Tgi, prop_id: u32, obs: FloatObservation) {
        self.observations
            .lock()
            .unwrap()
            .insert((simoder_core::tgi_key(target), prop_id), obs);
    }
}

impl PropObserver for MockPropHost {
    fn read_float(
        &self,
        target: Tgi,
        property_id: u32,
    ) -> Result<Option<FloatObservation>, ScriptError> {
        Ok(self
            .observations
            .lock()
            .unwrap()
            .get(&(simoder_core::tgi_key(target), property_id))
            .cloned())
    }
}

pub struct RecordingTickSink {
    pub logs: Vec<(String, String)>,
    pub patches: Vec<(ModIdentity, Vec<ScriptPatchCommand>)>,
    pub delayed: Vec<String>,
    pub reject_patches_for: Option<String>,
}

impl RecordingTickSink {
    pub fn new() -> Self {
        Self {
            logs: Vec::new(),
            patches: Vec::new(),
            delayed: Vec::new(),
            reject_patches_for: None,
        }
    }
}

impl SchedulerTickSink for RecordingTickSink {
    fn on_log(&mut self, identity: &ModIdentity, message: &str) {
        self.logs.push((identity.mod_id.clone(), message.to_string()));
    }

    fn on_patch_commands(
        &mut self,
        identity: &ModIdentity,
        commands: Vec<ScriptPatchCommand>,
    ) -> Result<(), String> {
        if self.reject_patches_for.as_deref() == Some(identity.mod_id.as_str()) {
            return Err("simulated preflight conflict".into());
        }
        self.patches.push((identity.clone(), commands));
        Ok(())
    }

    fn on_delayed_call(
        &mut self,
        due: DueDelayedCall,
    ) -> Result<(), String> {
        self.delayed.push(format!("{}:{}", due.owner.mod_id, due.call_id));
        Ok(())
    }
}

pub fn sample_tgi() -> Tgi {
    Tgi {
        type_id: 0x00B1_B104,
        group: 0x61EF_C000,
        instance: 0x7194_36BD,
    }
}

pub fn vm_gen(raw: u64) -> VmGeneration {
    VmGeneration::new(raw).unwrap()
}

pub fn mod_id(name: &str, generation: u64) -> ModIdentity {
    ModIdentity::new(name, vm_gen(generation)).unwrap()
}

pub fn luau_def(mod_id_str: &str, entry_code: &str, modules: Vec<ScriptModuleSource>) -> ScriptDefinition {
    let mut resource_aliases = BTreeMap::new();
    resource_aliases.insert("sanitizer".to_string(), sample_tgi());
    let mut property_aliases = BTreeMap::new();
    property_aliases.insert(
        "operatingCost".to_string(),
        PropertyAlias {
            name: "operatingCost".into(),
            property_id: 0x09AE_19D7,
            source: "SimCity-EcoGame".into(),
        },
    );
    ScriptDefinition {
        mod_id: mod_id_str.to_string(),
        language: ScriptLanguage::Luau,
        entry: "scripts/main.luau".into(),
        entry_code: entry_code.into(),
        modules,
        limits: ScriptLimits::default(),
        resource_aliases,
        property_aliases,
        bootstrap_base: false,
    }
}

pub fn js_def(mod_id_str: &str, entry_code: &str, modules: Vec<ScriptModuleSource>) -> ScriptDefinition {
    let mut def = luau_def(mod_id_str, entry_code, modules);
    def.language = ScriptLanguage::JavaScript;
    def.entry = "scripts/main.js".into();
    def
}
