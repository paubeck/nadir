#ifndef NADIR_LEXER_H
#define NADIR_LEXER_H

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
static void log_message(Logger* logger, const char* action, const char* message);

#endif