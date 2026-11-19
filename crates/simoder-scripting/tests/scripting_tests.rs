//! Integration tests for `simoder-scripting` Beta hook chain and localization fallback.

use std::collections::HashMap;

use simoder_scripting::{HookChain, LocaleFallbackChain, ResolvedText, ScriptError};

#[test]
fn hook_chain_enforces_pre_last_override_and_post_without_fallback() {
    let mut chain = HookChain::new(Box::new(|x: &mut i32| {
        *x += 100;
        Ok(*x)
    }));
    chain.add_pre_hook("mod-a", Box::new(|x: &mut i32| { *x += 1; Ok(()) }));
    chain.add_override("mod-b", Box::new(|x: &mut i32| Ok(*x * 2)));
    chain.add_override("mod-c", Box::new(|x: &mut i32| Ok(*x * 10)));
    chain.add_post_hook("mod-d", Box::new(|_, ret: i32| Ok(ret + 5)));

    assert_eq!(chain.active_override_owner(), Some("mod-c"));
    let mut arg = 2;
    assert_eq!(chain.invoke(&mut arg).unwrap(), 35);

    let mut failing = HookChain::new(Box::new(|_: &mut i32| Ok(999)));
    failing.add_override(
        "bad-mod",
        Box::new(|_| Err(ScriptError::InvocationFailed("boom".into()))),
    );
    failing.add_post_hook("post", Box::new(|_, _| panic!("post hook must not run")));
    assert!(failing.invoke(&mut 0).is_err());
}

#[test]
fn locale_fallback_chain_does_not_guess_unconfigured_variants() {
    let mut chain = LocaleFallbackChain::new("en-US", &["en-GB"], "pl-PL");
    chain.insert_dictionary(
        "en-GB".to_string(),
        HashMap::from([("road.title".into(), "Avenue".into())]),
    );
    chain.insert_dictionary(
        "pl-PL".to_string(),
        HashMap::from([("road.only_pl".into(), "Droga".into())]),
    );

    assert_eq!(
        chain.resolve("road.title"),
        ResolvedText::Found {
            locale: "en-GB",
            text: "Avenue"
        }
    );
    assert_eq!(
        chain.resolve("road.only_pl"),
        ResolvedText::Found {
            locale: "pl-PL",
            text: "Droga"
        }
    );
    assert_eq!(
        chain.resolve("missing.key"),
        ResolvedText::MissingDiagnostic("missing.key".into())
    );
}
