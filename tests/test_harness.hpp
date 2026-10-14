#pragma once

namespace sc13::tests {
/** @summary Record an expectation without aborting independent scenarios.
 * @param condition Observed test result.
 * @param expression Source expression identifying a failure.
 */
void Expect(bool condition, const char* expression);
/** @summary Return the number of failed expectations across all test modules. */
[[nodiscard]] int FailureCount() noexcept;
/** @summary Run game-independent identity, matching and signature tests. */
void TestCorePrimitives();
/** @summary Run current PROP table structure, float and isolation scenarios. */
void TestRuntimeTable();
/** @summary Run bounded codec and document-format scenarios. */
void TestFormats();
/** @summary Run current manifest identity, alias and invalid-input scenarios. */
void TestModDefinitionParser();
/** @summary Run patch ownership, conflict ordering, invalidation and vanilla restoration scenarios. */
void TestPatchOwnership();
/** @summary Run configuration, log classification and persisted activation-order contracts. */
void TestConfiguration();
/** @summary Run discovery, persistence, invalid changes and owned mod-removal scenarios. */
void TestModLifecycle();
/** @summary Run synthetic package extraction, source validation and resource capability gates. */
void TestResourceIntegration();
}

#define SC13_EXPECT(expression) ::sc13::tests::Expect((expression), #expression)
