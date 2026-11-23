# Scripting

Simoder runs mod scripts in isolated Luau or JavaScript ES2023 virtual machines.
Luau uses `mlua`; JavaScript uses `rquickjs`. A script mod declares its language
in `mod.toml`; the host validates the manifest and initializes each VM through
the same mod lifecycle.

```toml
[mod]
id = "example.hello"
name = "Hello"
version = "1.0.0"
author = "Example"
language = "js"
entry = "main.js"
```

JavaScript modules use CommonJS exports:

```javascript
module.exports = {
  onEnable() {
    simoder.log("Hello from Simoder");
  },
};
```

Luau scripts return lifecycle callbacks:

```luau
return {
    onEnable = function()
        simoder.log("Hello from Simoder")
    end,
}
```

The two VMs share the host API and scheduler, but language-specific syntax and
module loading remain distinct. `Simoder-Base` is itself a Luau mod and
provides higher-level services and UI abstractions. The APIs available to
individual scripts are defined by runtime bindings and Base; older
TypeScript-to-Luau compiler guides describe a retired authoring route.

## Runtime boundaries

Scripts execute under scheduler quotas and lifecycle rules. API calls that
change game state are staged and committed at the host's defined lifecycle or
tick boundaries. Available capabilities depend on the active host; successful
compilation alone does not imply that a game-facing capability is available.

See [mod lifecycle](mod-lifecycle.md), [PROP API](script-prop-api.md), and the
[current Rust architecture](../README.md) for implemented behavior. Synthetic
tests do not replace live-game acceptance.
