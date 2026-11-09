#pragma once

#include <cstdio>
#include <cstdlib>

#include "noncopyable.h"

// LOG_INFO("%s %d", arg1, arg2)
#define LOG_INFO(logmsgFormat, ...)                                      \
    do                                                                   \
    {                                                                    \
        char buf[1024];                                                  \
        snprintf(buf, sizeof(buf), logmsgFormat, ##__VA_ARGS__);         \
        Logger::instance().log(INFO, buf);                               \
    } while (0)

#define LOG_ERROR(logmsgFormat, ...)                                     \
    do                                                                   \
    {                                                                    \
        char buf[1024];                                                  \
        snprintf(buf, sizeof(buf), logmsgFormat, ##__VA_ARGS__);         \
        Logger::instance().log(ERROR, buf);                              \
    } while (0)

#define LOG_FATAL(logmsgFormat, ...)                                     \
    do                                                                   \
    {                                                                    \
        char buf[1024];                                                  \
        snprintf(buf, sizeof(buf), logmsgFormat, ##__VA_ARGS__);         \
        Logger::instance().log(FATAL, buf);                              \
        exit(-1);                                                        \
    } while (0)

#ifdef MUDEBUG
#define LOG_DEBUG(logmsgFormat, ...)                                     \
    do                                                                   \
    {                                                                    \
        char buf[1024];                                                  \
        snprintf(buf, sizeof(buf), logmsgFormat, ##__VA_ARGS__);         \
        Logger::instance().log(DEBUG, buf);                              \
    } while (0)
#else
#define LOG_DEBUG(logmsgFormat, ...)
#endif

// 定义日志的级别 INFO ERROR FATAL DEBUG
enum LogLevel
{
    INFO,  // 普通信息
    ERROR, // 错误信息
    FATAL, // core dump信息
    DEBUG, // 调试信息
};

// 输出一个日志类

class Logger : noncopyable
{
public:
    // 获取日志唯一的实例对象 单例
    static Logger &instance();
    // 设置日志级别
    void setLogLevel(int level);
    // 写日志。level 由调用方传入，避免多线程先改共享级别再打印。
    void log(int level, const char *msg);

private:
    int logLevel_{INFO};
};
