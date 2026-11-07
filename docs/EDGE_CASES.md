# Edge cases from EdgeCases.rtf

Run `./scripts/run-demo.sh`, or select **cpparc-demo → My Mac** and press **Cmd+R**. Sections 6–11 exercise the six document topics. `./scripts/test-runtime.sh` runs these same scenarios alongside the ownership regression tests. Checks remain enabled in Release builds.

| Document topic | Implementation and demonstrated check |
| --- | --- |
| Destructor reentrancy | Final release erases metadata and unlocks the stripe before invoking the callback. Three custom objects are deliberately chosen from the **same** stripe and chained A → B → C; releasing A destroys all three without recursively locking a held stripe. |
| Pool mutation during pop | Apple's paged pool restarts at the hot page after each release. A custom destructor autoreleases 5,000 objects, forcing additional pages, and pushes/pops a nested pool. All 5,002 destructors run both on explicit pop and at pthread exit. |
| Multiple side tables | The original 64 cache-aligned stripes and pointer indexing are preserved. Eight threads balance references to one shared object and create/destroy 4,000 independent objects; final counts and destruction totals are checked. This is a correctness test, not a scalability benchmark. |
| Unbalanced autorelease / double pop | Pending releases reserve existing ownership. An explicit release without another +1 and an overscheduled autorelease abort **before** destroying the object. Public pool handles are never reused; stale, invalid, null, double, wrong-thread, out-of-order, and reentrant ancestor pops abort before reading page memory. Empty-placeholder and used-page reuse are both tested. |
| Exceptions | Two nested RAII pools unwind through a thrown C++ exception, destroying both objects before catch. Callbacks must not throw; an exception escaping a runtime callback terminates because the ABI is noexcept. |
| Thread transfer | The producer retains a consumer reference **before publishing** the pointer through a promise. The consumer waits until the producer pool has drained, verifies the object is alive at +1, then performs its final release. A missing-retain variant is rejected by a subsequent runtime retain after the producer exits. |

## Ownership correction

The attachment's assertion that autorelease itself adds a reference or automatically makes arbitrary early release safe is incorrect. Autorelease transfers an already-owned +1 to a future pool release; it does not increase the total count. The checked implementation tracks that transfer and rejects an attempt to consume it a second time. A separate retained +1 can still be explicitly released before pool pop.

Tokens are opaque and strictly LIFO. Unlike Apple's low-level raw API, popping an ancestor while child pools remain active is rejected. Destructors may create and pop **new** nested pools while an existing pool drains. Preexisting ancestor tokens cannot be popped during explicit or pthread-exit draining.

## Expected failures are isolated

Misuse examples execute a fresh copy of the demo/test binary with an internal scenario argument. The parent checks both the expected fatal signal and the expected diagnostic; a crash for another reason, missing guard, or timeout fails the scenario. Child core dumps are disabled and each child has a ten-second timeout. Normal guards terminate with SIGABRT; Darwin may use its SIGILL abort fallback during pthread teardown, which is accepted only for that scenario and only with the expected diagnostic. These children deliberately violate the API contract; they are not normal recoverable library errors.

## Limits of a raw-pointer runtime

The library cannot intercept ordinary pointer dereferences, direct `delete`, or compiler scope exits. It cannot make an unretained cross-thread pointer safe. Retain before publication, synchronize access, and release each owned reference exactly once. Using the complete registered pointer remains required; the runtime does not canonicalize interior or adjusted base pointers. A stale object pointer whose address has been reused for a different registered object cannot be distinguished using a raw pointer alone. The new pool handles avoid this ambiguity for pool tokens, and reserved pool ownership prevents pool entries from becoming stale through an early runtime release.

Cycles still require application-level ownership design; weak references and compiler integration remain outside this iteration. Exception-safe scope cleanup for ordinary owned references must likewise be emitted by the compiler or explicitly arranged by the caller. The pool RAII helper only releases ownership transferred to that pool.
