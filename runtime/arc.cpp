/*
 * Copyright (c) 2010-2012 Apple Inc. All rights reserved.
 * 
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
// SideTable and storeStrong adapted from objc4-818.2 NSObject.mm.
#include <cpp_arc/arc.h>
#include "StripedMap.h"
#include "Lifetime.h"
#include <os/lock.h>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <mutex>
#include "llvm-DenseMap.h"

namespace {
[[noreturn]] void invalid(const char* message) noexcept {
    std::fprintf(stderr, "cpp-arc-runtime: %s\n", message);
    std::abort();
}
struct Lock {
    os_unfair_lock value = OS_UNFAIR_LOCK_INIT;
    void lock() noexcept { os_unfair_lock_lock(&value); }
    void unlock() noexcept { os_unfair_lock_unlock(&value); }
};
struct Metadata { size_t count; cpp_destructor_t destroy; size_t pending = 0; };
// Metadata entries must not be purged as though they were Apple's zero RC bits.
struct MetadataInfo { static bool isPurgeable(const Metadata&) { return false; } };
using RefcountMap = objc::DenseMap<void*, Metadata, MetadataInfo>;
struct SideTable { Lock slock; RefcountMap refcnts; };
auto& sideTables() {
    // Process lifetime: destructors and TLS pool drains may use this at shutdown.
    static auto* tables = new cpp_arc_runtime::StripedMap<SideTable>;
    return *tables;
}
}
extern "C" void cpp_register_object(void* object, cpp_destructor_t destroy) noexcept {
    if (!object) return;
    if (!destroy) invalid("null destruction callback");
    auto& table = sideTables()[object];
    std::lock_guard<Lock> lock(table.slock);
    if (!table.refcnts.insert(std::make_pair(object, Metadata{1, destroy})).second)
        invalid("duplicate registration");
}
extern "C" void* cpp_retain(void* object) noexcept {
    if (!object) return nullptr;
    auto& table = sideTables()[object];
    std::lock_guard<Lock> lock(table.slock);
    auto it = table.refcnts.find(object);
    if (it == table.refcnts.end()) invalid("retain of unregistered object");
    if (it->second.count == std::numeric_limits<size_t>::max()) invalid("retain count overflow");
    ++it->second.count;
    return object;
}
namespace {
void release(void* object, bool scheduled) noexcept {
    if (!object) return;
    cpp_destructor_t destroy = nullptr;
    auto& table = sideTables()[object];
    {
        std::lock_guard<Lock> lock(table.slock);
        auto it = table.refcnts.find(object);
        if (it == table.refcnts.end()) invalid("release of unregistered object");
        if (scheduled) {
            if (!it->second.pending) invalid("pool release without scheduled ownership");
            --it->second.pending;
        } else if (it->second.count == it->second.pending) {
            invalid("release consumes ownership already transferred to an autorelease pool");
        }
        if (--it->second.count == 0) {
            destroy = it->second.destroy;
            table.refcnts.erase(it);
        }
    }
    if (destroy) destroy(object);
}
} // namespace
extern "C" void cpp_release(void* object) noexcept { release(object, false); }
void cpp_arc_runtime::releaseAutoreleased(void* object) noexcept { release(object, true); }
void cpp_arc_runtime::reserveAutorelease(void* object) noexcept {
    auto& table = sideTables()[object];
    std::lock_guard<Lock> lock(table.slock);
    auto it = table.refcnts.find(object);
    if (it == table.refcnts.end()) invalid("autorelease of unregistered object");
    if (it->second.pending == it->second.count)
        invalid("autorelease without an untransferred owned reference");
    ++it->second.pending;
}
extern "C" void cpp_storeStrong(void** location, void* object) noexcept {
    if (!location) invalid("null strong slot");
    void* previous = *location;
    if (previous == object) return;
    cpp_retain(object);
    *location = object;
    cpp_release(previous);
}
extern "C" size_t cpp_retainCount(void* object) noexcept {
    if (!object) return 0;
    auto& table = sideTables()[object];
    std::lock_guard<Lock> lock(table.slock);
    auto it = table.refcnts.find(object);
    if (it == table.refcnts.end()) invalid("count of unregistered object");
    return it->second.count;
}
extern "C" void* cpp_autoreleaseReturnValue(void* object) noexcept {
    return cpp_autorelease(object);
}
extern "C" void* cpp_retainAutoreleasedReturnValue(void* object) noexcept {
    return cpp_retain(object);
}
