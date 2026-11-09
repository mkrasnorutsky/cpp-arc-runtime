#ifndef CPP_ARC_ARC_H
#define CPP_ARC_ARC_H
#include <stddef.h>
#ifdef __cplusplus
#define CPP_ARC_NOEXCEPT noexcept
extern "C" {
#else
#define CPP_ARC_NOEXCEPT
#endif

typedef void (*cpp_destructor_t)(void*);
/* Register a complete allocation with one owned reference. Callback must both
 * destroy and deallocate it, must not throw, and runs outside the table lock.
 * Null objects are ignored. Duplicate registration and invalid operations abort. */
void cpp_register_object(void* object, cpp_destructor_t destroy) CPP_ARC_NOEXCEPT;
void* cpp_retain(void* object) CPP_ARC_NOEXCEPT;
void cpp_release(void* object) CPP_ARC_NOEXCEPT;
/* location must be initialized; callers synchronize shared strong slots. */
void cpp_storeStrong(void** location, void* object) CPP_ARC_NOEXCEPT;
/* Schedule one release, WITHOUT retaining. Transfers an existing +1 to pool.
 * That reference is reserved: an explicit release needs another owned +1.
 * Overscheduling and consuming reserved ownership abort before destruction. */
void cpp_initStrong(void **location, void *object) CPP_ARC_NOEXCEPT;
void cpp_destroyStrong(void **location) CPP_ARC_NOEXCEPT;
void cpp_initStrongOwned(void **location, void *object) CPP_ARC_NOEXCEPT;
void cpp_storeStrongOwned(void **location, void *object) CPP_ARC_NOEXCEPT;
void* cpp_autorelease(void* object) CPP_ARC_NOEXCEPT;
/* Tokens are opaque, thread-affine, single-use handles. Pop strictly LIFO.
 * Invalid/stale/foreign tokens and popping an ancestor during draining abort. */
void* cpp_autoreleasePoolPush(void) CPP_ARC_NOEXCEPT;
void cpp_autoreleasePoolPop(void* token) CPP_ARC_NOEXCEPT;
/* Unoptimized ABI equivalents; no TLS/return-address handshake. */
void* cpp_autoreleaseReturnValue(void* object) CPP_ARC_NOEXCEPT;
void* cpp_retainAutoreleasedReturnValue(void* object) CPP_ARC_NOEXCEPT;
/* Snapshot for diagnostics only; caller must already own a reference. */
size_t cpp_retainCount(void* object) CPP_ARC_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#undef CPP_ARC_NOEXCEPT
#endif
