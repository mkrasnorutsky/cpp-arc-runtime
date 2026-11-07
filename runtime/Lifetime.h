#pragma once
namespace cpp_arc_runtime {
// Pool ownership is reserved under the same stripe lock as the total count.
void reserveAutorelease(void* object) noexcept;
void releaseAutoreleased(void* object) noexcept;
}
