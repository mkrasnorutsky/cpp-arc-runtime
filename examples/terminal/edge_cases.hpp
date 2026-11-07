#pragma once
#include <cpp_arc/arc.hpp>
#include "StripedMap.h" // Only used to construct a deterministic same-stripe collision.
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <limits.h>
#include <mach-o/dyld.h>
#include <future>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
extern char** environ;

namespace edge_cases {
inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
struct MyChainNode {
    MyChainNode* next = nullptr;
    int* destroyed;
    explicit MyChainNode(int& count) : destroyed(&count) {}
    ~MyChainNode() { ++*destroyed; cpp_release(next); }
};
inline void sameStripeDestructors() {
    int destroyed = 0;
    // Pigeonhole principle: 129 objects among 64 stripes gives 3 in one stripe.
    struct Marker {};
    cpp_arc_runtime::StripedMap<Marker> stripes;
    std::vector<MyChainNode*> objects;
    std::array<MyChainNode*, 3> chain{};
    for (int i = 0; i < 129; ++i) objects.push_back(cpp_arc::make<MyChainNode>(destroyed));
    for (auto* candidate : objects) {
        size_t count = 0;
        for (auto* object : objects) {
            if (&stripes[candidate] == &stripes[object]) {
                chain[count++] = object;
                if (count == chain.size()) break;
            }
        }
        if (count == chain.size()) break;
    }
    require(chain[2] != nullptr, "could not construct stripe collision");
    for (auto* object : objects)
        if (object != chain[0] && object != chain[1] && object != chain[2]) cpp_release(object);
    destroyed = 0;
    // Transfer existing +1 references into A -> B -> C (no extra retain).
    chain[0]->next = chain[1]; chain[1]->next = chain[2];
    cpp_release(chain[0]);
    require(destroyed == 3, "reentrant destruction did not destroy A, B, C");
    std::cout << "    PASS: A -> B -> C destructors release objects in the SAME stripe without deadlock.\n";
}
struct MyLeaf {
    std::atomic<int>* destroyed;
    explicit MyLeaf(std::atomic<int>& count) : destroyed(&count) {}
    ~MyLeaf() { ++*destroyed; }
};
struct MyDrainObject {
    std::atomic<int>* destroyed;
    explicit MyDrainObject(std::atomic<int>& count) : destroyed(&count) {}
    ~MyDrainObject() {
        // Force new pool pages while the old page is being drained.
        for (int i = 0; i < 5000; ++i) cpp_autorelease(cpp_arc::make<MyLeaf>(*destroyed));
        { cpp_arc::autorelease_pool inner; cpp_autorelease(cpp_arc::make<MyLeaf>(*destroyed)); }
        ++*destroyed;
    }
};
inline void mutatePoolWhileDraining() {
    std::atomic<int> destroyed{0};
    { cpp_arc::autorelease_pool pool; cpp_autorelease(cpp_arc::make<MyDrainObject>(destroyed)); }
    require(destroyed == 5002, "pool lost objects appended during pop");
    destroyed = 0;
    std::thread worker([&] { cpp_autorelease(cpp_arc::make<MyDrainObject>(destroyed)); });
    worker.join();
    require(destroyed == 5002, "thread-exit pool lost reentrant autoreleases");
    std::cout << "    PASS: destructor appends 5,000 objects and nests a pool during pop and thread exit.\n";
}
inline void stripedConcurrency() {
    std::atomic<int> destroyed{0};
    auto* shared = cpp_arc::make<MyLeaf>(destroyed);
    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) workers.emplace_back([&] {
        for (int i = 0; i < 10000; ++i) { cpp_retain(shared); cpp_release(shared); }
        for (int i = 0; i < 500; ++i) {
            auto* separate = cpp_arc::make<MyLeaf>(destroyed);
            cpp_release(separate);
        }
    });
    for (auto& worker : workers) worker.join();
    require(cpp_retainCount(shared) == 1 && destroyed == 4000, "concurrent counts were corrupted");
    cpp_release(shared);
    require(destroyed == 4001, "last shared reference was not released");
    std::cout << "    PASS: 8 threads balance shared references and 4,000 separate objects across 64 stripes.\n";
}
inline void exceptionUnwinding() {
    std::atomic<int> destroyed{0};
    try {
        cpp_arc::autorelease_pool outer;
        cpp_autorelease(cpp_arc::make<MyLeaf>(destroyed));
        cpp_arc::autorelease_pool inner;
        cpp_autorelease(cpp_arc::make<MyLeaf>(destroyed));
        throw std::runtime_error("intentional");
    } catch (const std::runtime_error&) {}
    require(destroyed == 2, "nested pools leaked during exception unwinding");
    std::cout << "    PASS: exception unwinding pops both nested RAII pools.\n";
}
inline void crossThreadHandoff() {
    std::atomic<int> destroyed{0};
    std::promise<MyLeaf*> pointer;
    std::promise<void> poolDrained;
    auto transferred = pointer.get_future();
    auto drained = poolDrained.get_future();
    std::thread producer([&] {
        {
            cpp_arc::autorelease_pool pool;
            auto* object = cpp_arc::make<MyLeaf>(destroyed);
            cpp_autorelease(object);
            cpp_retain(object); // Reserve the consumer's ownership BEFORE publishing.
            pointer.set_value(object);
        }
        poolDrained.set_value();
    });
    auto* object = transferred.get();
    drained.get(); // Deterministically use it only after the producer pool is gone.
    require(destroyed == 0 && cpp_retainCount(object) == 1, "handoff lost consumer ownership");
    cpp_release(object);
    producer.join();
    require(destroyed == 1, "consumer final release failed");
    std::cout << "    PASS: retained handoff survives the producer's pool; consumer performs final release.\n";
}
// Misuse scenarios run in fresh processes, so expected aborts cannot corrupt
// the demo's tables/pools. Never demonstrate a use-after-free in the parent.
inline int dispatchMisuse(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "--cpparc-misuse") return -1;
    rlimit limit{0, 0}; setrlimit(RLIMIT_CORE, &limit);
    alarm(10); // A regression that deadlocks is a test failure, not an endless run.
    std::string mode = argv[2];
    std::atomic<int> destroyed{0};
    if (mode == "early-release" || mode == "extra-autorelease") {
        cpp_arc::autorelease_pool pool;
        auto* object = cpp_arc::make<MyLeaf>(destroyed);
        cpp_autorelease(object);
        if (mode == "early-release") cpp_release(object);
        else cpp_autorelease(object);
    } else if (mode == "double-pop" || mode == "stale-token") {
        void* token = cpp_autoreleasePoolPush();
        cpp_autoreleasePoolPop(token);
        if (mode == "stale-token") (void)cpp_autoreleasePoolPush(); // Page/placeholder reuse.
        cpp_autoreleasePoolPop(token);
    } else if (mode == "stale-used-token") {
        void* token = cpp_autoreleasePoolPush();
        cpp_autorelease(cpp_arc::make<MyLeaf>(destroyed));
        cpp_autoreleasePoolPop(token);
        (void)cpp_autoreleasePoolPush();
        cpp_autorelease(cpp_arc::make<MyLeaf>(destroyed));
        cpp_autoreleasePoolPop(token);
    } else if (mode == "reentrant-exit-pop") {
        std::thread worker([&] {
            void* token = cpp_autoreleasePoolPush();
            struct BadPop { void* token; explicit BadPop(void* t) : token(t) {} ~BadPop() { cpp_autoreleasePoolPop(token); } };
            cpp_autorelease(cpp_arc::make<BadPop>(token));
        });
        worker.join();
    } else if (mode == "reentrant-pop") {
        void* outer = cpp_autoreleasePoolPush();
        void* inner = cpp_autoreleasePoolPush();
        struct BadPop { void* token; explicit BadPop(void* t) : token(t) {} ~BadPop() { cpp_autoreleasePoolPop(token); } };
        cpp_autorelease(cpp_arc::make<BadPop>(outer));
        cpp_autoreleasePoolPop(inner);
    } else if (mode == "null-token") {
        cpp_autoreleasePoolPop(nullptr);
    } else if (mode == "missing-retain-handoff") {
        MyLeaf* object = nullptr;
        std::thread producer([&] {
            cpp_arc::autorelease_pool pool;
            object = cpp_arc::make<MyLeaf>(destroyed);
            cpp_autorelease(object);
        });
        producer.join();
        cpp_retain(object); // Library call diagnoses it; never dereference it.
    } else if (mode == "wrong-thread") {
        void* token = cpp_autoreleasePoolPush();
        std::thread other([=] { cpp_autoreleasePoolPop(token); }); other.join();
    } else if (mode == "out-of-order") {
        void* outer = cpp_autoreleasePoolPush(); (void)cpp_autoreleasePoolPush();
        cpp_autoreleasePoolPop(outer);
    } else if (mode == "invalid-token") {
        cpp_autoreleasePoolPop(reinterpret_cast<void*>(uintptr_t(0x12345)));
    } else if (mode == "unregistered-autorelease") {
        int value = 0; cpp_autorelease(&value);
    } else return 3;
    return 2; // Reaching here means the expected guard did not reject misuse.
}
inline void expectRejected(const char* mode, const char* expected) {
    uint32_t size = PATH_MAX;
    std::vector<char> executable(size);
    if (_NSGetExecutablePath(executable.data(), &size)) {
        executable.resize(size);
        require(_NSGetExecutablePath(executable.data(), &size) == 0, "executable lookup failed");
    }
    int output[2]; require(pipe(output) == 0, "pipe failed");
    posix_spawn_file_actions_t actions;
    require(posix_spawn_file_actions_init(&actions) == 0, "spawn actions failed");
    require(posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO) == 0, "stderr redirect failed");
    require(posix_spawn_file_actions_addclose(&actions, output[0]) == 0, "spawn close failed");
    require(posix_spawn_file_actions_addclose(&actions, output[1]) == 0, "spawn close failed");
    char* arguments[]{executable.data(), const_cast<char*>("--cpparc-misuse"), const_cast<char*>(mode), nullptr};
    pid_t child;
    int result = posix_spawn(&child, executable.data(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions); close(output[1]);
    if (result) { close(output[0]); throw std::runtime_error("could not spawn misuse scenario"); }
    std::string diagnostic; char buffer[1024]; ssize_t count;
    while ((count = read(output[0], buffer, sizeof(buffer))) != 0) {
        if (count > 0) diagnostic.append(buffer, static_cast<size_t>(count));
        else if (errno != EINTR) break;
    }
    close(output[0]);
    int status = 0; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    // Darwin abort() may fall back to a trap while pthread TLS is being torn
    // down. Accept that signal only for this scenario, with the exact diagnostic.
    bool fatalSignal = WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT ||
        (std::string(mode) == "reentrant-exit-pop" && WTERMSIG(status) == SIGILL));
    if (waited != child || !fatalSignal || diagnostic.find(expected) == std::string::npos)
        throw std::runtime_error(std::string("misuse check failed: ") + mode + ": " + diagnostic);

    require(diagnostic.find(expected) != std::string::npos, "misuse failed without the expected runtime diagnostic");
    std::cout << "    PASS: rejected " << mode << " before accessing freed objects or invalid pool memory.\n";
}
inline void ownershipAndTokenGuards() {
    // A legitimate explicit release is safe when backed by an EXTRA retained +1.
    std::atomic<int> destroyed{0};
    {
        cpp_arc::autorelease_pool pool;
        auto* object = cpp_arc::make<MyLeaf>(destroyed);
        cpp_autorelease(object); cpp_retain(object); cpp_release(object);
        require(destroyed == 0 && cpp_retainCount(object) == 1, "pool ownership was consumed early");
    }
    require(destroyed == 1, "balanced scheduled reference leaked");
    expectRejected("early-release", "ownership already transferred");
    expectRejected("extra-autorelease", "untransferred owned reference");
    expectRejected("unregistered-autorelease", "autorelease of unregistered object");
    expectRejected("missing-retain-handoff", "retain of unregistered object");
    for (auto* mode : {"double-pop", "stale-token", "stale-used-token", "wrong-thread", "out-of-order", "reentrant-pop", "reentrant-exit-pop", "invalid-token", "null-token"})
        expectRejected(mode, "pool token");
}
inline void runAll() {
    std::cout << "\n6. Edge cases: same-stripe destructor reentrancy\n"; sameStripeDestructors();
    std::cout << "\n7. Edge cases: mutation during pool drain\n"; mutatePoolWhileDraining();
    std::cout << "\n8. Edge cases: striped-table concurrency\n"; stripedConcurrency();
    std::cout << "\n9. Edge cases: unbalanced ownership and invalid pool pops\n"; ownershipAndTokenGuards();
    std::cout << "\n10. Edge cases: exception unwinding\n"; exceptionUnwinding();
    std::cout << "\n11. Edge cases: cross-thread ownership transfer\n"; crossThreadHandoff();
}
}
