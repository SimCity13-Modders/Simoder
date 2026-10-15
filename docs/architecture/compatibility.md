# API and format compatibility baseline

The prototype and alpha do not promise backwards compatibility for mod APIs or
file formats. Breaking changes may require authors to update their projects;
maintaining converters for retired alpha formats is not a release requirement.
Obsolete alpha migration tools are removed rather than preserved as products.

The first real public Beta release establishes the supported compatibility
baseline. From that release onward, API/format changes must account for existing
Beta consumers through explicit versioning, compatibility checks and an agreed
migration or deprecation path. Pre-Beta prototypes do not become supported legacy
formats solely because they once existed in repository history.

This policy does not relax current schema, input, integrity, game-build identity
or permission validation. Unsupported input must fail clearly; compatibility is
not permission to reinterpret malformed data or bypass security checks. Region
backups, approved migration/rollback procedures and native-mod trust boundaries
remain required wherever the corresponding operations are actually performed.

OpenSCP owns conversion and resource inspection. Assigning that responsibility
does not assert that every retired CLI operation is already implemented in its
UI. The retained authoring compiler and SDK describe only their existing tested
contracts; IDE build/debug integration requires a separately verified change.
