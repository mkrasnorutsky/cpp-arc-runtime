# CppARC

C++ ARC runtime adapted from Apple's objc4-818.2 with minimal changes to its core memory-management code. Objects are ordinary C++ allocations: no base class, `isa`, hidden header, boxed object, tagged pointer, or inline reference counter is required.

The library stores reference counts and destruction callbacks in striped side tables. It reuses Apple's `DenseMap` and full `AutoreleasePoolPage` implementation. The pool drains through the C++ scheduled-release path instead of `objc_release`.

## Open and build

1. Open `cpp-arc-runtime.xcodeproj`.
2. Select **cpp-arc-runtime → My Mac**.
3. **Cmd+B** builds `libcpparc.a` and the test executable.
4. **Cmd+R** runs the C++ runtime tests.

Uses Xcode's public macOS SDK, macOS 11.0 or later, C++17, and `arm64` / `x86_64`. No packages, internal SDK, signing team, or downloads are required. Products are in Xcode's configuration products directory (Products → Show in Finder).

```sh
xcodebuild -project cpp-arc-runtime.xcodeproj -scheme cpp-arc-runtime \
  -configuration Debug -destination 'platform=macOS' \
  -derivedDataPath build/CppARC build
build/CppARC/Build/Products/Debug/cpp-arc-tests
```

Universal Release build:

```sh
xcodebuild -project cpp-arc-runtime.xcodeproj -scheme cpp-arc-runtime \
  -configuration Release -destination 'generic/platform=macOS' \
  -derivedDataPath build/CppARC ARCHS='arm64 x86_64' ONLY_ACTIVE_ARCH=NO build
build/CppARC/Build/Products/Release/cpp-arc-tests
```

Library product names are distinct from Apple's system libraries:

| Product | Purpose |
| --- | --- |
| `libcpparc.a` | C++ ARC runtime |
| `libcpparc-reference.dylib` | Retained Objective-C reference build |
| `libcpparc-trampolines.dylib` | Reference build's block trampolines |

Reference dylibs use `@rpath` install names; no `libobjc` alias is produced.

## Terminal demo

Select the **cpparc-demo → My Mac** scheme in Xcode and press **Cmd+R**. The console shows `MyClass` construction, counts, and destruction across five examples:

1. Explicit `new`, `cpp_register_object`, `cpp_retain`, and `cpp_release`.
2. `MyOwner` retaining a `MyClass` child with `cpp_storeStrong`, including safe self-assignment.
3. Nested pools and reverse-order destruction.
4. An autoreleased factory return retained beyond its pool.
5. RAII pool cleanup during exception unwinding.

The demo also exercises all six topics from `EdgeCases.rtf`: same-stripe destructor chains, autoreleasing thousands of objects during a pop, concurrent striped-table use, invalid ownership and pool-token rejection, nested exception cleanup, and a retained cross-thread handoff. Expected failures run in isolated child processes and must produce the specific runtime diagnostic and expected fatal signal.

The runtime reserves existing ownership transferred to a pool without increasing the total reference count. Explicit release cannot consume reserved ownership, and a second autorelease requires another untransferred +1. See [docs/EDGE_CASES.md](docs/EDGE_CASES.md) for guarantees, limits, and the scenario mapping.

The demo uses explicit runtime calls and finishes with `Live MyClass objects: 0`. Its source is `examples/terminal/main.cpp`.

Run from Terminal:

```sh
./scripts/run-demo.sh
```

Or build and run directly:

```sh
xcodebuild -project cpp-arc-runtime.xcodeproj -scheme cpparc-demo \
  -configuration Debug -destination 'platform=macOS' \
  -derivedDataPath build/Demo build
build/Demo/Build/Products/Debug/cpparc-demo
```

## API and ownership

Include `<cpp_arc/arc.h>` for the C ABI, or `<cpp_arc/arc.hpp>` for the optional C++ allocation and pool helpers. Add this repository's `include` directory to your header search paths and link `libcpparc.a` with the C++ standard library. The C++ target has no libobjc or Foundation dependency.

| Function | Contract |
| --- | --- |
| `cpp_register_object(object, destroy)` | Registers an existing allocation at +1. Callback destroys **and deallocates** it. |
| `cpp_retain(object)` | Adds one reference and returns the same pointer. |
| `cpp_release(object)` | Removes one reference. At zero, removes metadata and invokes the callback outside the stripe lock. |
| `cpp_storeStrong(&slot, object)` | Retains new value, writes the initialized `void*` slot, releases old value. Self-assignment is safe. |
| `cpp_autorelease(object)` | Schedules one future release in this thread's pool; **does not retain**. |
| `cpp_autoreleasePoolPush()` / `cpp_autoreleasePoolPop(token)` | Nested, thread-local paged pools; opaque single-use tokens must be popped in LIFO order. |
| `cpp_autoreleaseReturnValue(object)` | Unoptimized equivalent of `cpp_autorelease`. |
| `cpp_retainAutoreleasedReturnValue(object)` | Unoptimized equivalent of `cpp_retain`. |
| `cpp_retainCount(object)` | Diagnostic count snapshot; caller must already own a reference. |

Object operations accept null. Each non-null object must be registered exactly once. Unregistered retain/release, duplicate registration, null callbacks, and count overflow abort; allocation failures or exceptions escaping callbacks terminate. The callback must not throw. `cpp_arc::make<T>(...)` uses `new T(...)` and registers a matching `delete T*` callback, including class-specific or over-aligned allocation.

```cpp
#include <cpp_arc/arc.hpp>

struct Widget { int value = 42; };

Widget* createWidget() {
    auto* widget = cpp_arc::make<Widget>(); // +1
    return static_cast<Widget*>(cpp_autoreleaseReturnValue(widget));
    // This ownership was transferred to the pool. Do not release it again here.
}

int main() {
    cpp_arc::autorelease_pool pool; // pops during exception unwinding too
    auto* widget = static_cast<Widget*>(
        cpp_retainAutoreleasedReturnValue(createWidget())); // caller's +1
    int result = widget->value;
    cpp_release(widget); // pool still has the scheduled release
    return result == 42 ? 0 : 1;
}
```

Use the same complete-allocation pointer for every runtime call; adjusted base-class pointers, interior pointers, and array elements are not aliases the runtime can discover. Each concurrent owner must hold a reference. Counts are synchronized, but object fields and shared strong-pointer slots require caller synchronization. The callback owns the allocation: do not also delete it directly.

Pool tokens are opaque, globally unique handles belonging to their creating thread. Pop each pushed pool exactly once, strictly in reverse order. Invalid, stale, cross-thread, out-of-order, and reentrant ancestor pops abort before any token-derived memory is accessed. Pools without an explicit boundary drain at pthread exit. Use an explicit pool on the main thread, since returning from `main` is process exit rather than pthread exit. RAII pools handle exception unwinding.

## Scope and validation

This implements the runtime part of the plan. Namespace directives and automatic compiler insertion of retains, releases, and exception cleanup require a separate compiler change. Raw C++ pointer assignment alone does not invoke ARC. Weak references, cycle collection, and the return-value TLS fast path are intentionally excluded from this first iteration.

Debug and universal Release builds were verified with Xcode 26.6 and the macOS 26.5 SDK. Tests run on the host's Intel architecture and cover ownership, self-assignment, aligned allocations, pool nesting and multi-page LIFO order, return-value ownership, exception unwinding, destructor reentrancy, concurrent retain/release, last-release races, and pthread-exit cleanup. AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer runs also pass. Apple Silicon is cross-built, not execution-tested here.

The original **cpparc-reference** scheme and imported Apple sources remain available as a reference build. Only the C++ scheme produces the C++ library. See [UPSTREAM.md](UPSTREAM.md) for source versions, licenses, and the small extraction patch.
