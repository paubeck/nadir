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
void log_node(Logger* logger, const char* node_type, ASTNode* node);
const char* get_expr_type_name(enum ASTNodeType type);
const char* get_token_name(TokenType type);
const char* get_node_type_name(enum ASTNodeType type);

#endif