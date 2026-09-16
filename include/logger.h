#ifndef NADIR_LOGGER_H
#define NADIR_LOGGER_H

typedef enum {
    NONE,
    INFO,
    DEBUG,
    WARN,
    FINEST
} LoggingLevel;

typedef struct Logger {
    const char* file_name;
    LoggingLevel logging_level;
} Logger;

Logger* init_logger(const char* logger_file_name);
void log_message(Logger* logger, const char* action, const char* message);

#endif