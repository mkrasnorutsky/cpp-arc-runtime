#include <cpp_arc/arc.hpp>
#include "../examples/terminal/edge_cases.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>
#include <vector>
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
extern "C" int c_api_smoke(void);
struct Object {
    std::atomic<int>* deaths;
    explicit Object(std::atomic<int>& count) : deaths(&count) {}
    ~Object() { ++*deaths; }
};
static Object* create(std::atomic<int>& count) {
    return static_cast<Object*>(cpp_autoreleaseReturnValue(cpp_arc::make<Object>(count)));
}
static void basics() {
    CHECK(c_api_smoke() == 0);
    std::atomic<int> deaths{0};
    auto* object = cpp_arc::make<Object>(deaths);
    CHECK(cpp_retainCount(object) == 1);
    CHECK(cpp_retain(object) == object);
    CHECK(cpp_retainCount(object) == 2);
    cpp_release(object);
    void* slot = nullptr;
    cpp_storeStrong(&slot, object);
    cpp_storeStrong(&slot, object); // self assignment must not change count
    CHECK(cpp_retainCount(object) == 2);
    cpp_release(object);
    cpp_storeStrong(&slot, nullptr);
    CHECK(slot == nullptr && deaths == 1);
    // Callback owns destruction and deallocation, including over-aligned types.
    struct alignas(256) Aligned : Object { using Object::Object; };
    auto* aligned = cpp_arc::make<Aligned>(deaths);
    CHECK(reinterpret_cast<uintptr_t>(aligned) % 256 == 0);
    cpp_release(aligned);
    CHECK(deaths == 2);
}
static void pools() {
    std::atomic<int> deaths{0};
    {
        cpp_arc::autorelease_pool outer;
        auto* object = create(deaths);
        CHECK(cpp_retainCount(object) == 1); // autorelease must NOT retain
        {
            cpp_arc::autorelease_pool inner;
            cpp_autorelease(cpp_arc::make<Object>(deaths));
        }
        CHECK(deaths == 1);
        cpp_retainAutoreleasedReturnValue(object);
        CHECK(cpp_retainCount(object) == 2);
        cpp_release(object);
    }
    CHECK(deaths == 2);
    Object* survivor;
    {
        cpp_arc::autorelease_pool pool;
        survivor = create(deaths);
        cpp_retain(survivor);
    }
    CHECK(deaths == 2 && cpp_retainCount(survivor) == 1);
    cpp_release(survivor);
    CHECK(deaths == 3);
    try {
        cpp_arc::autorelease_pool pool;
        cpp_autorelease(cpp_arc::make<Object>(deaths));
        throw std::runtime_error("unwind");
    } catch (const std::runtime_error&) {}
    CHECK(deaths == 4);
    // Empty placeholder, nested placeholder, and allocation after empty pop.
    for (int i = 0; i < 100; ++i) {
        cpp_arc::autorelease_pool a;
        cpp_arc::autorelease_pool b;
    }
    // Repeat scheduling is legal if backed by matching ownership references.
    {
        cpp_arc::autorelease_pool pool;
        auto* object = cpp_arc::make<Object>(deaths);
        cpp_retain(object);
        cpp_autorelease(object);
        cpp_autorelease(object);
    }
    CHECK(deaths == 5);
}
struct Ordered {
    int id;
    std::vector<int>* order;
    Ordered(int value, std::vector<int>* output) : id(value), order(output) {}
    ~Ordered() { order->push_back(id); }
};
static void manyPages() {
    std::vector<int> order;
    order.reserve(30000);
    for (int round = 0; round < 3; ++round) {
        order.clear();
        cpp_arc::autorelease_pool pool;
        for (int i = 0; i < 10000; ++i)
            cpp_autorelease(cpp_arc::make<Ordered>(i, &order));
    }
    CHECK(order.size() == 10000);
    for (int i = 0; i < 10000; ++i) CHECK(order[i] == 9999 - i);
}
struct Reentrant {
    std::atomic<int>* deaths;
    ~Reentrant() {
        // Exercise registration/release and autorelease from a drain callback.
        cpp_release(cpp_arc::make<Object>(*deaths));
        cpp_autorelease(cpp_arc::make<Object>(*deaths));
        { cpp_arc::autorelease_pool nested; cpp_autorelease(cpp_arc::make<Object>(*deaths)); }
        ++*deaths;
    }
};
static void reentrant() {
    std::atomic<int> deaths{0};
    {
        cpp_arc::autorelease_pool pool;
        auto* value = new Reentrant{&deaths};
        cpp_register_object(value, [](void* p) { delete static_cast<Reentrant*>(p); });
        cpp_autorelease(value);
    }
    CHECK(deaths == 4);
    deaths = 0;
    std::thread worker([&] {
        auto* value = new Reentrant{&deaths};
        cpp_register_object(value, [](void* p) { delete static_cast<Reentrant*>(p); });
        cpp_autorelease(value);
    });
    worker.join();
    CHECK(deaths == 4);
}
static void threads() {
    std::atomic<int> deaths{0};
    auto* shared = cpp_arc::make<Object>(deaths);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) workers.emplace_back([&] {
        for (int i = 0; i < 20000; ++i) { cpp_retain(shared); cpp_release(shared); }
        for (int i = 0; i < 1000; ++i) {
            cpp_arc::autorelease_pool pool;
            cpp_autorelease(cpp_arc::make<Object>(deaths));
        }
        // No explicit pool: Apple's implicit root drains at thread exit.
        cpp_autorelease(cpp_arc::make<Object>(deaths));
    });
    for (auto& worker : workers) worker.join();
    CHECK(cpp_retainCount(shared) == 1);
    CHECK(deaths == 8008);
    cpp_release(shared);
    CHECK(deaths == 8009);
    // Exactly one last release may invoke the callback under contention.
    deaths = 0;
    shared = cpp_arc::make<Object>(deaths);
    for (int i = 1; i < 8; ++i) cpp_retain(shared);
    workers.clear();
    for (int i = 0; i < 8; ++i) workers.emplace_back([shared] { cpp_release(shared); });
    for (auto& worker : workers) worker.join();
    CHECK(deaths == 1);
}
int main(int argc, char** argv) {
    int misuse = edge_cases::dispatchMisuse(argc, argv);
    if (misuse >= 0) return misuse;
    basics(); pools(); manyPages(); reentrant(); threads();
    edge_cases::runAll();
    std::puts("All C++ ARC runtime tests passed");
}
