# Source provenance

Apple source: https://github.com/apple-oss-distributions/objc4/tree/objc4-818.2
Commit: 62b60ba0e56440e265ca576cc9f197e9af54c1bd

Compatibility reference: https://github.com/showxu/objc4/tree/75f57479d4416815ac43e81848db76d101f86eab
Commit: 75f57479d4416815ac43e81848db76d101f86eab

Apple's files were copied directly from the release (excluding Git metadata and .DS_Store). The six runtime files modified by the reference are NSObject.mm, objc-cache.mm, objc-os.h, objc-os.mm, objc-runtime-new.mm, and objc-runtime.mm. They remove unavailable Cambria/feature headers and unsupported private calls or checks, and use the bundled dyld and OS declarations. The reference's disabled static assertion and private feature checks are retained; this build is not a claim of compatibility with the current system dyld.

The Xcode project and objc.xcconfig start from the reference. Local changes:

- Move the project to the repository root; keep SRCROOT at objc4 and add a shared cpparc-reference scheme.
- Build against the public macOS SDK with a macOS 11 deployment target.
- Write products into the ordinary configuration products directory instead of /tmp/objc.dst.
- Link the trampolines library with libSystem; modern linkers reject a dylib with no libSystem dependency.
- Disable internal text-based API generation, which assumes Apple's install layout.
- Compile markgc for the host architecture rather than hard-coding Intel. Retain Apple's post-link initializer marking.
- Include <utility> explicitly for std::move; newer libc++ no longer supplies it transitively through <atomic>.
- Disable legacy header searching and user script sandboxing for the upstream post-link tool.

## Licenses

Apple's runtime is covered by the Apple Public Source License 2.0 in objc4/APPLE_LICENSE and the notices in individual files. Bundled compatibility headers retain their original notices. The reference repository's MIT license is retained in LICENSE.showxu. Preserve these licenses and notices when redistributing.

## C++ ARC adaptation

The final strategy in Plan.rtfd is implemented as a separate pure-C++ build target using the imported Apple sources. Existing Objective-C files are preserved for comparison and the original scheme still builds.

| Source | Reuse / change |
| --- | --- |
| `objc4/runtime/NSObject.mm` → `runtime/AutoreleasePool.cpp` | Full `AutoreleasePoolPage` class retained. Route releases through the C++ scheduled-release helper, retain internal pop calls, replace private fixed TLS key with `pthread_key_create`, remove class/tagged-pointer diagnostics. `id` locally means `void*`. Page allocation, boundaries, placeholder optimization, traversal, reentrant drain, cleanup hysteresis and TLS destructor stay Apple's. |
| `objc4/runtime/NSObject-internal.h` → same file | Retain `magic_t` and page data; remove ObjC ownership annotations. Pointer packing is disabled so all C++ pointer bits survive. |
| `objc4/runtime/objc-private.h` → `runtime/StripedMap.h`, `runtime/NativeSupport.h` | Retain 64 cache-aligned stripes and Apple's pointer hash, plus supporting pointer/hash utilities. Remove runtime-wide lock-debug helpers; use public `os_unfair_lock` in the side table. |
| `objc4/runtime/llvm-DenseMap*.h` | Reuse the imported implementation directly. Only change: conditionally include `NativeSupport.h` instead of `objc-private.h` when building `CPP_ARC_RUNTIME`. Other build modes are unchanged. |
| `runtime/arc.cpp` | Simplified SideTable-only lifetime path with `{count, destroy}` metadata; no isa/tagged-pointer/Swift/message-send logic, packed count flags, overflow-to-side-table path, or weak table. Adapt Apple's retain-before-store-before-release order. |

`patches/cpp-arc-pool.patch` shows the class-level changes relative to Apple's original pool (support definitions and exported entry points live outside that class). `patches/objc4-818.2-runtime.patch` includes all changes in imported runtime files relative to the official release. The small public-SDK support header supplies fatal logging and standard pthread operations; private runtime facilities are not linked.

The side table uses complete `void*` keys rather than disguised ObjC-object keys. Its metadata is never eligible for DenseMap's zero-value purging; entries are removed explicitly at final release. Tables intentionally have process lifetime so callbacks during thread/global teardown can still release objects.

The plan conversation contains conflicting autorelease suggestions. This implementation preserves Apple's actual ownership rule: autorelease schedules a release without adding a reference. A producer transfers one owned reference to the pool, and an escaping caller retains its own reference. The return-value functions are plain unoptimized calls. This is a runtime API, not a modified compiler or a promise that ordinary pointer assignments are automatically managed.

## Product names

The C++ product is `libcpparc.a`. The retained reference products are `libcpparc-reference.dylib` and `libcpparc-trampolines.dylib`, with `@rpath` install names and `/usr/local/lib` as their optional install directory. Apple's install-only `libobjc.dylib` alias phase is removed. The post-link markgc path uses the renamed reference product, and the reference runtime loads its renamed trampoline library via `@loader_path`. Source filenames and upstream historical comments retain their original names.

## Edge-case hardening

The 64-stripe layout and Apple page algorithms remain intact. Metadata now also tracks pending pool releases. `cpp_autorelease` reserves an existing owned reference under the stripe lock, and the pool calls an internal scheduled-release helper to consume that reservation. Explicit release cannot consume a reserved reference. This is a deliberate checked extension of Apple's compiler-enforced ownership contract, not an extra retain.

Public pool tokens are monotonically allocated opaque handles tracked by a per-thread stack, separate from Apple's internal page-slot/placeholder tokens. This prevents stale-token reuse and rejects wrong-thread, double, out-of-order, or reentrant ancestor pops before dereferencing user input. The registry uses pthread TLS and stays available during reentrant thread-exit draining. Apple's TLS destructor and the registry cleanup cooperate regardless of destructor order; the page callback uses the raw internal pop path. The pool extraction patch records these small integration hooks.
