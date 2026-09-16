#include "logger.h"
#include <stdio.h>
#include <ast.h>
#include "sobject.h"

Logger* init_logger(const char* logger_file_name) {
    Logger* logger = malloc(sizeof(Logger));

    logger->file_name = logger_file_name;
    logger->logging_level = DEBUG;

    return logger;
}

void log_message(Logger* logger, const char* action, const char* message) {
    if (!logger) {
        return;
    }
    Logger l = *logger; 

    FILE* file = fopen(logger->file_name, "a+");

    if (!file) {
        return;
    }
    char date_buf[64];
    format_timestamp(time(NULL), date_buf, sizeof(date_buf));

    fprintf(file, "%s|%s: %s", val_string(date_buf).as.string_val, action, message);
}