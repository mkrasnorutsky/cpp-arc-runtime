#pragma once
#include "arc.h"
#include <memory>
#include <utility>

namespace cpp_arc {
// A pool always pops during scope exit, including exception unwinding.
class autorelease_pool final {
    void* token_ = cpp_autoreleasePoolPush();
public:
    autorelease_pool() noexcept = default;
    ~autorelease_pool() { cpp_autoreleasePoolPop(token_); }
    autorelease_pool(const autorelease_pool&) = delete;
    autorelease_pool& operator=(const autorelease_pool&) = delete;
    autorelease_pool(autorelease_pool&&) = delete;
    autorelease_pool& operator=(autorelease_pool&&) = delete;
};
// Optional allocation helper; the runtime itself has no layout requirements.
// Compiler-generated code can use new + cpp_register_object directly.
template<class T, class... Args> T* make(Args&&... args) {
    auto object = std::make_unique<T>(std::forward<Args>(args)...);
    cpp_register_object(object.get(), [](void* p) { delete static_cast<T*>(p); });
    return object.release();
}
}
