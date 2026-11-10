#pragma once

#include <string>
#include <cstring>
#include <stddef.h>
#include <algorithm>
#include <limits>
#include <stdexcept>

#include "XmdMemory.h"

// 网络库底层的缓冲区类型定义
class Buffer
{
public:
    static const size_t kCheapPrepend = 8;//初始预留的prependabel空间大小
    static const size_t kInitialSize = 1024;

    explicit Buffer(size_t initalSize = kInitialSize)
        : buffer_(allocationSize(initalSize))
        , readerIndex_(kCheapPrepend)
        , writerIndex_(kCheapPrepend)
    {
    }

    size_t readableBytes() const { return writerIndex_ - readerIndex_; }
    size_t writableBytes() const { return buffer_.size() - writerIndex_; }
    size_t prependableBytes() const { return readerIndex_; }
    size_t storageBytes() const { return buffer_.size(); }

    // 返回缓冲区中可读数据的起始地址
    const char *peek() const { return begin() + readerIndex_; }
    void retrieve(size_t len)
    {
        if (len < readableBytes())
        {
            readerIndex_ += len; // 说明应用只读取了可读缓冲区数据的一部分，就是len长度 还剩下readerIndex+=len到writerIndex_的数据未读
        }
        else // len == readableBytes()
        {
            retrieveAll();
        }
    }
    void retrieveAll()
    {
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    // 把onMessage函数上报的Buffer数据 转成string类型的数据返回
    std::string retrieveAllAsString() { return retrieveAsString(readableBytes()); }
    std::string retrieveAsString(size_t len)
    {
        std::string result(peek(), len);
        retrieve(len); // 上面一句把缓冲区中可读的数据已经读取出来 这里肯定要对缓冲区进行复位操作
        return result;
    }

    void reserve(size_t len)
    {
        ensureWritableBytes(len);
    }

    // buffer_.size - writerIndex_
    void ensureWritableBytes(size_t len)
    {
        if (len > std::numeric_limits<size_t>::max() - readableBytes() - kCheapPrepend)
            throw std::length_error("Buffer capacity overflow");
        if (writableBytes() < len)
        {
            makeSpace(len); // 扩容
        }
    }

    // 把[data, data+len]内存上的数据添加到writable缓冲区当中
    void append(const char *data, size_t len)
    {
        ensureWritableBytes(len);
        if (len > 0)
        {
            memcpy(beginWrite(), data, len);
        }
        writerIndex_ += len;
    }
    char *beginWrite() { return begin() + writerIndex_; }
    const char *beginWrite() const { return begin() + writerIndex_; }

    // 在可读数据前写入（用于长度头等），要求 prependableBytes() >= len
    void prepend(const void *data, size_t len)
    {
        if (len > prependableBytes())
        {
            return;
        }
        readerIndex_ -= len;
        const char *d = static_cast<const char *>(data);
        memcpy(begin() + readerIndex_, d, len);
    }

    // 从fd上读取数据
    ssize_t readFd(int fd, int *saveErrno);
    // 通过fd发送数据
    ssize_t writeFd(int fd, int *saveErrno);

private:
    // Round the complete allocation, including prepend space, to powers of two.
    // Keep a payload that fits (including prepend) within the 512KiB pool
    // boundary instead of drifting past it through non-aligned doubling.
    static size_t allocationSize(size_t payload)
    {
        if (payload > std::numeric_limits<size_t>::max() - kCheapPrepend)
            throw std::length_error("Buffer capacity overflow");
        const size_t need = payload + kCheapPrepend;
        size_t size = 64;
        while (size < need)
        {
            if (size > std::numeric_limits<size_t>::max() / 2) return need;
            size *= 2;
        }
        return size;
    }
    // vector底层数组首元素的地址 也就是数组的起始地址
    char *begin() { return &*buffer_.begin(); }
    const char *begin() const { return &*buffer_.begin(); }

    void makeSpace(size_t len)
    {
        /**
         * | kCheapPrepend |xxx| reader | writer |                     // xxx标示reader中已读的部分
         * | kCheapPrepend | reader ｜          len          |
         **/
        if (buffer_.size() - readableBytes() - kCheapPrepend < len)
        {
            // 传输场景下按倍增扩容，减少大文件推流时反复 resize。
            const size_t readable = readableBytes();
            if (len > std::numeric_limits<size_t>::max() - readable)
                throw std::length_error("Buffer capacity overflow");
            PoolByteVector grown(allocationSize(readable + len));
            // Only live bytes need copying; vector::resize copied consumed data
            // too, and zero-filled the new tail on every growth.
            std::memcpy(grown.data() + kCheapPrepend, peek(), readable);
            buffer_.swap(grown);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = kCheapPrepend + readable;
        }
        else // 这里说明 len <= xxx + writer 把reader搬到从xxx开始 使得xxx后面是一段连续空间
        {
            size_t readable = readableBytes(); // readable = reader的长度
            // 源和目的可能重叠，用 memmove 搬到 kCheapPrepend，腾出连续可写空间
            memmove(begin() + kCheapPrepend,
                    begin() + readerIndex_,
                    readable);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
        }
    }

    PoolByteVector buffer_;
    size_t readerIndex_;
    size_t writerIndex_;
};
