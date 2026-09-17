#include "logger.h"
#include <stdio.h>
#include <ast.h>
#include "sobject.h"
#include "token.h"
#include "value.h"
#include "eval.h"

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

    FILE* file = fopen(logger->file_name, "a+");

    if (!file) {
        return;
    }
    char date_buf[64];
    format_timestamp(time(NULL), date_buf, sizeof(date_buf));

    fprintf(file, "%s|%s: %s\n", val_string(date_buf).as.string_val, action, message);
}

void log_node(Logger* logger, const char* node_type, ASTNode* node) {
    if (!logger || !node) {
        return;
    }
    
    FILE* file = fopen(logger->file_name, "a+");
    if (!file) {
        return;
    }

    char date_buf[64];
    format_timestamp(time(NULL), date_buf, sizeof(date_buf));

    switch (node->type) {
        case NODE_CLASS_DECL:
            fprintf(file, "%s|CLASS_DECL: class %s", 
                val_string(date_buf).as.string_val, node->as.class_decl.name);
            if (node->as.class_decl.parent_class) {
                fprintf(file, " extends %s", node->as.class_decl.parent_class);
            }
            fprintf(file, "\n");
            break;

        case NODE_METHOD_DECL:
            fprintf(file, "%s|METHOD_DECL: %s %s%s(%d params, static=%d)\n",
                val_string(date_buf).as.string_val, node->as.method_decl.return_type,
                node->as.method_decl.name, node->as.method_decl.is_static ? "static " : "",
                node->as.method_decl.param_count, node->as.method_decl.is_static);
            break;

        case NODE_BLOCK:
            fprintf(file, "%s|BLOCK_START: %d statements inside\n",
                val_string(date_buf).as.string_val, node->as.block.statements.count);
            break;

        case NODE_VAR_DECL:
            fprintf(file, "%s|VARIABLE_DECL: var %s = %s (%s)\n",
                val_string(date_buf).as.string_val, node->as.var_decl.var_name,
                node->as.var_decl.init ? "exist" : "new instance",
                node->as.var_decl.type_name);
            break;

        case NODE_ASSIGN:
            fprintf(file, "%s|ASSIGNMENT: %s = type=%d\n",
                val_string(date_buf).as.string_val, node->as.assign.target->as.identifier.name,
                node->as.assign.op);
            break;

        case NODE_RETURN:
            fprintf(file, "%s|RETURN: returning value type=%d\n",
                val_string(date_buf).as.string_val, node->as.return_stmt.value->type);
            break;

        case NODE_IF:
            fprintf(file, "%s|IF_STATEMENT: condition type=%s\n",
                val_string(date_buf).as.string_val, get_expr_type_name(node->as.if_stmt.cond->type));
            break;

        case NODE_WHILE:
            fprintf(file, "%s|WHILE_LOOP: iterating while condition==true\n");
            break;

        case NODE_FOR:
            fprintf(file, "%s|FOR_LOOP: for-each iteration\n");
            break;

        case NODE_CALL:
            fprintf(file, "%s|METHOD_INVOCATION: %s(%d args)\n",
                val_string(date_buf).as.string_val,
                node->as.call.method_name, node->as.call.args.count);
            break;

        case NODE_SOQL:
            fprintf(file, "%s|SOQL_QUERY: executing query on %s\n",
                val_string(date_buf).as.string_val, node->as.soql.from_object);
            break;

        case NODE_DML:
            fprintf(file, "%s|DML: %s %d records\n",
                val_string(date_buf).as.string_val,node->as.dml.operation, node->as.dml.target->as.literal.val.as.string_val);
            break;

        case NODE_BINARY_OP:
            fprintf(file, "%s|BINARY_OP: %s %s %s",
                val_string(date_buf).as.string_val,
                get_expr_type_name(node->as.binary.left),
                get_token_name(node->as.binary.op),
                get_expr_type_name(node->as.binary.right));
            break;

        case NODE_UNARY_OP:
            fprintf(file, "%s|UNARY_OP: %s\n",
                val_string(date_buf).as.string_val,
                get_token_name(node->as.unary.op));
            break;

        case NODE_IDENTIFIER:
            fprintf(file, "%s|IDENTIFIER: %s\n",
                val_string(date_buf).as.string_val, node->as.identifier.name);
            break;

        case NODE_LITERAL:
            ValueType val_type = node->as.literal.val.type;
            switch (val_type) {
                case VAL_NULL: {
                    fprintf(file, "%s|NULL_LITERAL\n",
                        val_string(date_buf).as.string_val);
                    break;
                }
                case VAL_INT: {
                    fprintf(file, "%s|INT_LITERAL: \"%d\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.int_val);
                    break;
                };
                case VAL_DOUBLE: {
                    fprintf(file, "%s|DOUBLE_LITERAL: \"%d\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.double_val);
                    break;
                };
                case VAL_BOOL: {
                    fprintf(file, "%s|BOOL_LITERAL: \"%s\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.bool_val ? "TRUE" : "FALSE");
                    break;
                };
                case VAL_STRING: {
                    fprintf(file, "%s|STRING_LITERAL: \"%s\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.string_val);
                    break;
                };
                case VAL_LIST: {
                    fprintf(file, "%s|LIST_LITERAL: \"%d Items\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.list_val->count);
                    break;
                };
                case VAL_MAP: {
                    fprintf(file, "%s|MAP_LITERAL: \"%d Entries\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.map_val->count);
                    break;
                };
                case VAL_SOBJECT: {
                    fprintf(file, "%s|SOBJECT_LITERAL: \"%s\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.sobject_val->type_name);
                    break;
                };
                case VAL_INSTANCE: {
                    fprintf(file, "%s|INSTANCE_LITERAL: \"%s\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.instance_val->klass->name);
                    break;
                };
                case VAL_NATIVE_FN: {
                    fprintf(file, "%s|NATIVE_FN_LITERAL: \"%s\"\n",
                    val_string(date_buf).as.string_val, node->as.literal.val.as.string_val);
                    break;
                };
            
                default:
                    break;
            }
            break;
        case NODE_NEW:
            fprintf(file, "%s|NEW_INSTANCE: new %s()\n",
                val_string(date_buf).as.string_val, node->as.new_expr.type_name);
            break;

        default:
            fprintf(file, "%s|%s processed\n", 
                val_string(date_buf).as.string_val, get_node_type_name(node->type));
    }

    fclose(file);
}

const char* get_expr_type_name(enum NodeType type) {
    switch (type) {
        case NODE_BINARY_OP: return "BINARY";
        case NODE_UNARY_OP: return "UNARY";
        case NODE_IF: return "IF";
        case NODE_WHILE: return "WHILE";
        case NODE_FOR: return "FOR";
        case NODE_IDENTIFIER: return "IDENTIFIER";
        default: return "UNKNOWN";
    }
}

const char* get_token_name(TokenType type) {
    switch (type) {
        case TOKEN_AND: return "&&";
        case TOKEN_OR: return "||";
        case TOKEN_NOT: return "!";
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_STAR: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_PERCENT: return "%";
        case TOKEN_PLUS_PLUS: return "++";
        case TOKEN_MINUS_MINUS: return "--";
        case TOKEN_ASSIGN: return "=";
        case TOKEN_PLUS_ASSIGN: return "+=";
        case TOKEN_MINUS_ASSIGN: return "-=";
        case TOKEN_STAR_ASSIGN: return "*=";
        case TOKEN_SLASH_ASSIGN: return "/=";
        case TOKEN_EQUAL: return "==";
        case TOKEN_NOT_EQUAL: return "!= or <>";
        case TOKEN_EXACT_EQUAL: return "===";
        case TOKEN_EXACT_NOT_EQUAL: return "!==";
        case TOKEN_LT: return "<";
        case TOKEN_GT: return ">";
        case TOKEN_LE: return "<=";
        case TOKEN_GE: return ">=";
        case TOKEN_QUESTION: return "?";
        case TOKEN_COLON: return ":";
        case TOKEN_SAFE_DOT: return "?.";
        case TOKEN_NULL_COALESCE: return "??";
        case TOKEN_ARROW: return "=>";
        case TOKEN_AT: return "@";
        case TOKEN_DOT: return ".";
        case TOKEN_COMMA: return ",";
        case TOKEN_SEMICOLON: return ";";
        case TOKEN_LPAREN: return "(";
        case TOKEN_RPAREN: return ")";
        case TOKEN_LBRACE: return "{";
        case TOKEN_RBRACE: return "}";
        case TOKEN_LBRACKET: return "[";
        case TOKEN_RBRACKET: return "]";
        default: return "OP";
    }
}

const char* get_node_type_name(enum NodeType type) {
    switch (type) {
        case NODE_CLASS_DECL: return "CLASS_DECL";
        case NODE_METHOD_DECL: return "METHOD_DECL";
        case NODE_BLOCK: return "BLOCK";
        case NODE_VAR_DECL: return "VAR_DECL";
        case NODE_RETURN: return "RETURN";
        case NODE_IF: return "IF";
        case NODE_WHILE: return "WHILE";
        case NODE_FOR: return "FOR";
        case NODE_IDENTIFIER: return "IDENTIFIER";
        case NODE_NEW: return "NEW_INSTANTIATION";
        case NODE_PROGRAM: return "PROGRAM";
        case NODE_LITERAL: return "LITERAL";
        case NODE_ASSIGN: return "ASSIGN";
        case NODE_BINARY_OP: return "BINARY_OP";
        case NODE_UNARY_OP: return "UNARY_OP";
        case NODE_TERNARY: return "TERNARY";
        case NODE_DO_WHILE: return "DO_WHILE";
        case NODE_FOR_EACH: return "FOR_EACH";
        case NODE_SWITCH: return "SWITCH";
        case NODE_TRY_CATCH: return "TRY_CATCH";
        case NODE_CALL: return "CALL";
        case NODE_MEMBER_ACCESS: return "MEMBER_ACCESS";
        case NODE_TRIGGER: return "TRIGGER";
        case NODE_DML: return "DML";
        case NODE_SOQL: return "SOQL";
        default: return "UNKNOWN";
    }
}