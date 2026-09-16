// James Ezra Seitenschlag
// 11.09.2026
//
// nadir runtime thingy

#ifndef NADIR_EVAL_H
#define NADIR_EVAL_H

#include "ast.h"
#include "env.h"
#include "sobject.h"
#include "logger.h"

// method on an apex class
typedef struct ApexMethod {
    char* name;
    char* return_type;
    ParamDecl* params;
    int param_count;
    ASTNode* body;
    bool is_static;
} ApexMethod;

// class def in runtime
typedef struct ApexClassDef {
    char* name;
    char* parent_class;
    ApexMethod* methods;
    int method_count;
    int method_capacity;
} ApexClassDef;

// class instance
typedef struct ApexInstance {
    ApexClassDef* klass;
    Environment* fields;
} ApexInstance;

// governor limits tracking thing
typedef struct GovernorLimits {
    int soql_queries;       // 100 max sync
    int limit_soql_queries;
    int dml_statements;     // 150 max
    int limit_dml_statements;
    int dml_rows;           // 10k rows
    int limit_dml_rows;
    int cpu_time_ms;
    int limit_cpu_time_ms;
} GovernorLimits;

// interpreter main state
typedef struct Interpreter {
    Environment* global_env;
    ApexClassDef* classes;
    int class_count;
    int class_capacity;
    ASTNode** ast_roots;
    int ast_root_count;
    int ast_root_capacity;
    bool return_flag;
    Value return_val;
    GovernorLimits limits;
    Logger* logger;
} Interpreter;

Interpreter* interpreter_new(void);
void interpreter_free(Interpreter* interp);
Value interpreter_eval(Interpreter* interp, ASTNode* node, Environment* env);
Value interpreter_run(Interpreter* interp, ASTNode* program);
void interpreter_register_class(Interpreter* interp, ASTNode* class_decl);
ApexClassDef* find_class(Interpreter* interp, const char* name);
ApexMethod* find_method(Interpreter* interp, ApexClassDef* klass, const char* name);

#endif
