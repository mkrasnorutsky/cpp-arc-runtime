#include <cpp_arc/arc.hpp>
#include "edge_cases.hpp"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

// Ordinary C++ class: no ARC base class, isa field, or embedded reference count.
class MyClass {
    std::string name_;
public:
    inline static int liveObjects = 0;
    explicit MyClass(std::string name) : name_(std::move(name)) {
        ++liveObjects;
        std::cout << "  + MyClass(\"" << name_ << "\") constructed\n";
    }
    ~MyClass() {
        std::cout << "  - MyClass(\"" << name_ << "\") destroyed\n";
        --liveObjects;
    }
    void sayHello() const { std::cout << "    Hello from " << name_ << "!\n"; }
};

// The registered callback must destroy AND deallocate the complete allocation.
void destroyMyClass(void* object) {
    delete static_cast<MyClass*>(object);
}

void showCount(const char* explanation, void* object) {
    // Call only while we own a reference or a scheduled pool release keeps it alive.
    std::cout << "    " << explanation << ": count = " << cpp_retainCount(object) << '\n';
}

void referenceCountingDemo() {
    std::cout << "\n1. Register, retain, release\n";
    auto* first = new MyClass("manual registration");
    cpp_register_object(first, destroyMyClass); // Initial ownership: +1.
    showCount("after registration", first);

    auto* second = static_cast<MyClass*>(cpp_retain(first)); // Another owner: +1.
    second->sayHello();
    showCount("after retaining a second pointer", first);
    cpp_release(second);
    second = nullptr;
    showCount("after releasing the second owner", first);
    std::cout << "    Releasing the final owner calls destroyMyClass:\n";
    cpp_release(first);
    first = nullptr; // Never read a released allocation.
}

// A second custom class demonstrates ownership of another ARC-managed object.
class MyOwner {
    // Use a genuine void* slot for the C ABI, avoiding a T** -> void** cast.
    void* child_ = nullptr;
public:
    explicit MyOwner(MyClass* child) {
        std::cout << "  + MyOwner retains its child using cpp_storeStrong\n";
        cpp_storeStrong(&child_, child);
    }
    ~MyOwner() {
        std::cout << "  - MyOwner clears its strong child\n";
        cpp_storeStrong(&child_, nullptr);
    }
    void sayHello() const { static_cast<MyClass*>(child_)->sayHello(); }
};

void strongAssignmentDemo() {
    std::cout << "\n2. Strong assignment and a custom owning class\n";
    auto* child = cpp_arc::make<MyClass>("owned child"); // new + registration helper.
    auto* owner = cpp_arc::make<MyOwner>(child);
    showCount("local owner plus MyOwner's strong field", child);
    cpp_release(child); // MyOwner keeps the child alive.
    owner->sayHello();
    std::cout << "    Releasing MyOwner also releases its child (outside the table lock):\n";
    cpp_release(owner);

    void* slot = nullptr;
    auto* value = cpp_arc::make<MyClass>("strong slot");
    cpp_storeStrong(&slot, value);
    cpp_release(value); // Transfer effective ownership to slot.
    cpp_storeStrong(&slot, slot); // Safe self-assignment.
    showCount("self-assignment keeps one owner", slot);
    cpp_storeStrong(&slot, nullptr); // Final release.
}

void nestedPoolsDemo() {
    std::cout << "\n3. Explicit nested autorelease pools\n";
    void* outer = cpp_autoreleasePoolPush();
    auto* first = cpp_arc::make<MyClass>("outer pool");
    cpp_autorelease(first); // Schedule the existing +1; do not release first again.
    showCount("autorelease schedules a release without retaining", first);
    void* inner = cpp_autoreleasePoolPush();
    cpp_autorelease(cpp_arc::make<MyClass>("inner pool, first"));
    cpp_autorelease(cpp_arc::make<MyClass>("inner pool, second"));
    std::cout << "    Popping inner pool destroys its objects in reverse order:\n";
    cpp_autoreleasePoolPop(inner);
    first->sayHello(); // Outer pool's release is still pending.
    std::cout << "    Popping outer pool:\n";
    cpp_autoreleasePoolPop(outer);
}

MyClass* createMyClass() {
    auto* object = cpp_arc::make<MyClass>("factory return");
    // Transfer the producer's +1 to the pool, instead of releasing it at return.
    return static_cast<MyClass*>(cpp_autoreleaseReturnValue(object));
}

void returnedValueDemo() {
    std::cout << "\n4. A returned object that outlives its pool\n";
    MyClass* survivor = nullptr;
    {
        cpp_arc::autorelease_pool pool;
        survivor = static_cast<MyClass*>(cpp_retainAutoreleasedReturnValue(createMyClass()));
        showCount("one caller reference plus one scheduled release", survivor);
        std::cout << "    Leaving the pool releases only the producer's reference.\n";
    }
    survivor->sayHello();
    showCount("caller still owns the returned object", survivor);
    cpp_release(survivor);
}

void exceptionDemo() {
    std::cout << "\n5. RAII pool cleanup during exception unwinding\n";
    try {
        cpp_arc::autorelease_pool pool;
        cpp_autorelease(cpp_arc::make<MyClass>("exception scope"));
        std::cout << "    Throwing an exception; the pool will pop before catch.\n";
        throw std::runtime_error("demo exception");
    } catch (const std::runtime_error& error) {
        std::cout << "    Caught: " << error.what() << '\n';
    }
}

int main(int argc, char** argv) {
    int misuse = edge_cases::dispatchMisuse(argc, argv);
    if (misuse >= 0) return misuse;
    std::cout << "CppARC: C++ memory-management demo\n"
                 "Runtime calls are explicit here; ordinary pointer assignments do not invoke ARC.\n";
    referenceCountingDemo();
    strongAssignmentDemo();
    nestedPoolsDemo();
    returnedValueDemo();
    exceptionDemo();
    edge_cases::runAll();
    std::cout << "\nLive MyClass objects: " << MyClass::liveObjects << '\n';
    if (MyClass::liveObjects != 0) {
        std::cerr << "Demo failed: an object was not destroyed.\n";
        return 1;
    }
    std::cout << "Demo complete: every MyClass object was destroyed.\n";
    return 0;
}
