#pragma once

// Core objects and Pool* containers use KamaMemoryPool by default.
// MUDUO_USE_MEMORY_POOL=0 switches these wrappers to the standard allocator for A/B tests.
// 池内部的元数据仍使用系统 operator new，因此这里不替换全局 new/delete。

#include <kama/Allocator.h>
#include <kama/MemoryPool.h>

#include <functional>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <utility>
#include <type_traits>
#include <vector>

#ifndef MUDUO_USE_MEMORY_POOL
#define MUDUO_USE_MEMORY_POOL 1
#endif

// Byte storage is overwritten by recv/memcpy before it becomes readable. Keep
// Kama allocation, but avoid vector value-initializing unused buffer capacity.
template <typename T>
class UninitializedPoolAllocator
    : public
#if MUDUO_USE_MEMORY_POOL
      Kama_memoryPool::Allocator<T>
#else
      std::allocator<T>
#endif
{
public:
    using value_type = T;
    template <typename U> struct rebind { using other = UninitializedPoolAllocator<U>; };
    UninitializedPoolAllocator() noexcept = default;
    template <typename U>
    UninitializedPoolAllocator(const UninitializedPoolAllocator<U> &) noexcept {}
    template <typename U> void construct(U *p) { ::new (static_cast<void *>(p)) U; }
    template <typename U, typename... Args> void construct(U *p, Args &&...args)
    { ::new (static_cast<void *>(p)) U(std::forward<Args>(args)...); }
};

using PoolByteVector = std::vector<char, UninitializedPoolAllocator<char>>;

struct PoolDeleter
{
    template <typename T>
    void operator()(T *pointer) const noexcept
    {
#if MUDUO_USE_MEMORY_POOL
        if (!pointer) return;
        pointer->~T();
        // A polymorphic base pointer may own a larger derived allocation.
        if (!std::is_polymorphic<T>::value && alignof(T) <= Kama_memoryPool::kAlignment)
            Kama_memoryPool::MemoryPool::deallocate(pointer, sizeof(T));
        else
            Kama_memoryPool::MemoryPool::deallocate(pointer);
#else
        delete pointer;
#endif
    }
};

template <typename T>
using PoolUniquePtr = std::unique_ptr<T, PoolDeleter>;

template <typename T>
#if MUDUO_USE_MEMORY_POOL
using PoolVector = std::vector<T, Kama_memoryPool::Allocator<T>>;
#else
using PoolVector = std::vector<T>;
#endif

using PoolString = std::basic_string<char,
                                     std::char_traits<char>,
#if MUDUO_USE_MEMORY_POOL
                                     Kama_memoryPool::Allocator<char>>;
#else
                                     std::allocator<char>>;
#endif

template <typename Key, typename T>
using PoolUnorderedMap = std::unordered_map<
    Key,
    T,
    std::hash<Key>,
    std::equal_to<Key>,
#if MUDUO_USE_MEMORY_POOL
    Kama_memoryPool::Allocator<std::pair<const Key, T>>>;
#else
    std::allocator<std::pair<const Key, T>>>;
#endif

template <typename T, typename... Args>
PoolUniquePtr<T> makePoolUnique(Args &&...args)
{
#if MUDUO_USE_MEMORY_POOL
    T *object = Kama_memoryPool::MemoryPool::newElement<T>(std::forward<Args>(args)...);
#else
    T *object = new T(std::forward<Args>(args)...);
#endif
    if (object == nullptr)
    {
        throw std::bad_alloc();
    }
    return PoolUniquePtr<T>(object);
}

template <typename T, typename... Args>
std::shared_ptr<T> makePoolShared(Args &&...args)
{
#if MUDUO_USE_MEMORY_POOL
    return std::allocate_shared<T>(Kama_memoryPool::Allocator<T>(),
                                   std::forward<Args>(args)...);
#else
    return std::make_shared<T>(std::forward<Args>(args)...);
#endif
}
