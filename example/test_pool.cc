#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include "XmdMemory.h"

struct alignas(64) Aligned { int value = 42; };
struct Base { virtual ~Base() = default; };
struct Derived : Base { char bytes[4096]; };
struct Throws { Throws() { throw std::runtime_error("expected"); } };

int main()
{
    auto small = makePoolUnique<int>(17);
    auto aligned = makePoolUnique<Aligned>();
    PoolUniquePtr<Base> polymorphic(makePoolUnique<Derived>().release());
    bool threw = false;
    try { auto unused = makePoolUnique<Throws>(); }
    catch (const std::runtime_error &) { threw = true; }
    PoolByteVector bytes(32);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = char(i);
    bytes.resize(100000);
    bool preserved = true;
    for (int i = 0; i < 32; ++i) preserved = preserved && bytes[i] == char(i);
    const bool ok = *small == 17 && aligned->value == 42 &&
        reinterpret_cast<uintptr_t>(aligned.get()) % 64 == 0 && threw && preserved;
    std::printf("pool sized/overaligned/polymorphic/exception/byte storage: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
