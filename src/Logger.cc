#include <ctime>

#include "Logger.h"

// 获取日志唯一的实例对象 单例
Logger &Logger::instance()
{
    static Logger logger;
    return logger;
}

// 设置日志级别
void Logger::setLogLevel(int level)
{
    logLevel_ = level;
}

// 写日志 [级别] time : msg
void Logger::log(int level, const char *msg)
{
    const char *pre = "[INFO]";
    switch (level)
    {
    case INFO:
        pre = "[INFO]";
        break;
    case ERROR:
        pre = "[ERROR]";
        break;
    case FATAL:
        pre = "[FATAL]";
        break;
    case DEBUG:
        pre = "[DEBUG]";
        break;
    default:
        break;
    }

    time_t now = time(nullptr);
    struct tm tm_time;
#if defined(_WIN32)
    localtime_s(&tm_time, &now);
#else
    localtime_r(&now, &tm_time);
#endif
    // 不用 std::string，也不用 endl，避免每次日志都分配内存并强制刷出。
    fprintf(stdout, "%s%4d/%02d/%02d %02d:%02d:%02d : %s\n",
            pre,
            tm_time.tm_year + 1900,
            tm_time.tm_mon + 1,
            tm_time.tm_mday,
            tm_time.tm_hour,
            tm_time.tm_min,
            tm_time.tm_sec,
            msg != nullptr ? msg : "");
    fflush(stdout);
}
