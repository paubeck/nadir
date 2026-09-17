// James Ezra Seitenschlag
// 11.09.2026
//
// nadir runtime thingy

#include "eval_internal.h"
#include "logger.h"

Interpreter* interpreter_new(void) {
    Interpreter* interp = (Interpreter*)malloc(sizeof(Interpreter));
    if (!interp) {
        fprintf(stderr, "FATAL: failed to allocate interpreter state\n");
        abort();
    }
    interp->global_env = env_new(NULL);
    interp->classes = NULL;
    interp->class_count = 0;
    interp->class_capacity = 0;
    interp->ast_roots = NULL;
    interp->ast_root_count = 0;
    interp->ast_root_capacity = 0;
    interp->return_flag = false;
    interp->return_val = val_null();

    // governor limits quotas
    interp->limits.soql_queries = 0;
    interp->limits.limit_soql_queries = 100;
    interp->limits.dml_statements = 0;
    interp->limits.limit_dml_statements = 150;
    interp->limits.dml_rows = 0;
    interp->limits.limit_dml_rows = 10000;
    interp->limits.cpu_time_ms = 5;
    interp->limits.limit_cpu_time_ms = 10000;

    return interp;
}

void interpreter_free(Interpreter* interp) {
    if (!interp) return;
    env_free(interp->global_env);
    if (interp->classes) {
        for (int i = 0; i < interp->class_count; i++) {
            if (interp->classes[i].name) free(interp->classes[i].name);
            if (interp->classes[i].parent_class) free(interp->classes[i].parent_class);
            for (int m = 0; m < interp->classes[i].method_count; m++) {
                if (interp->classes[i].methods[m].name) free(interp->classes[i].methods[m].name);
                if (interp->classes[i].methods[m].return_type) free(interp->classes[i].methods[m].return_type);
            }
            if (interp->classes[i].methods) free(interp->classes[i].methods);
        }
        free(interp->classes);
    }
    if (interp->ast_roots) {
        for (int i = 0; i < interp->ast_root_count; i++) {
            ast_node_free(interp->ast_roots[i]);
        }
        free(interp->ast_roots);
    }
    free(interp);
}

void interpreter_register_class(Interpreter* interp, ASTNode* class_decl) {
    if (class_decl->type != NODE_CLASS_DECL) return;
    if (interp->class_count + 1 > interp->class_capacity) {
        interp->class_capacity = interp->class_capacity < 8 ? 8 : interp->class_capacity * 2;
        interp->classes = (ApexClassDef*)realloc(interp->classes, sizeof(ApexClassDef) * interp->class_capacity);
    }
    ApexClassDef* def = &interp->classes[interp->class_count++];
    def->name = duplicate_string(class_decl->as.class_decl.name);
    def->parent_class = duplicate_string(class_decl->as.class_decl.parent_class);
    def->methods = NULL;
    def->method_count = 0;
    def->method_capacity = 0;

    for (int i = 0; i < class_decl->as.class_decl.members.count; i++) {
        ASTNode* member = class_decl->as.class_decl.members.nodes[i];
        if (member->type == NODE_CLASS_DECL) {
            interpreter_register_class(interp, member);
        } else if (member->type == NODE_METHOD_DECL) {
            if (def->method_count + 1 > def->method_capacity) {
                def->method_capacity = def->method_capacity < 8 ? 8 : def->method_capacity * 2;
                def->methods = (ApexMethod*)realloc(def->methods, sizeof(ApexMethod) * def->method_capacity);
            }
            ApexMethod* m = &def->methods[def->method_count++];
            m->name = duplicate_string(member->as.method_decl.name);
            m->return_type = duplicate_string(member->as.method_decl.return_type);
            m->params = member->as.method_decl.params;
            m->param_count = member->as.method_decl.param_count;
            m->body = member->as.method_decl.body;
            m->is_static = member->as.method_decl.is_static;
        }
    }
}

ApexClassDef* find_class(Interpreter* interp, const char* name) {
    if (!interp || !name) return NULL;
    for (int i = 0; i < interp->class_count; i++) {
        if (string_equal_case(interp->classes[i].name, name)) {
            return &interp->classes[i];
        }
    }

    char path[512];
    snprintf(path, sizeof(path), "%s.cls", name);
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(path, sizeof(path), "%s.apex", name);
        f = fopen(path, "rb");
    }
    if (f) {
        fseek(f, 0L, SEEK_END);
        size_t sz = ftell(f);
        rewind(f);
        char* src = (char*)malloc(sz + 1);
        if (src) {
            fread(src, sizeof(char), sz, f);
            src[sz] = '\0';
            fclose(f);
            Lexer l;
            lexer_init(&l, src);
            Parser p;
            parser_init(&p, &l);
            ASTNode* prog = parser_parse(&p);
            if (prog && prog->type == NODE_PROGRAM) {
                for (int i = 0; i < prog->as.program.statements.count; i++) {
                    if (prog->as.program.statements.nodes[i]->type == NODE_CLASS_DECL) {
                        interpreter_register_class(interp, prog->as.program.statements.nodes[i]);
                    }
                }
            }
            free(src);
        } else {
            fclose(f);
        }
        for (int i = 0; i < interp->class_count; i++) {
            if (string_equal_case(interp->classes[i].name, name)) {
                return &interp->classes[i];
            }
        }
    }

    return NULL;
}

ApexMethod* find_method(Interpreter* interp, ApexClassDef* klass, const char* name) {
    if (!klass) return NULL;
    for (int i = 0; i < klass->method_count; i++) {
        if (string_equal_case(klass->methods[i].name, name)) {
            return &klass->methods[i];
        }
    }
    if (klass->parent_class && interp) {
        ApexClassDef* parent = find_class(interp, klass->parent_class);
        if (parent) {
            return find_method(interp, parent, name);
        }
    }
    return NULL;
}

Value interpreter_eval(Interpreter* interp, ASTNode* node, Environment* env) {
    if (!interp || !node || interp->return_flag) return val_null();
    log_node(interp->logger, node->type, node);

    switch (node->type) {
        case NODE_PROGRAM: {
            Value last = val_null();
            for (int i = 0; i < node->as.program.statements.count; i++) {
                if (node->as.program.statements.nodes[i]->type == NODE_CLASS_DECL) {
                    interpreter_register_class(interp, node->as.program.statements.nodes[i]);
                }
            }
            for (int i = 0; i < node->as.program.statements.count; i++) {
                if (node->as.program.statements.nodes[i]->type != NODE_CLASS_DECL) {
                    last = interpreter_eval(interp, node->as.program.statements.nodes[i], env);
                    if (interp->return_flag) break;
                }
            }
            return last;
        }

        case NODE_LITERAL:
            return node->as.literal.val;

        case NODE_IDENTIFIER: {
            const char* name = node->as.identifier.name;
            uint32_t hash = node->as.identifier.hash;
            if (hash == 0 && name) hash = nadr_hash_str(name);
            Value v;
            if (env_get_prehashed(env, name, hash, &v)) {
                return v;
            }
            if (string_equal_case(name, "system") || string_equal_case(name, "math") || string_equal_case(name, "string") ||
                string_equal_case(name, "database") || string_equal_case(name, "schema") || string_equal_case(name, "test") ||
                string_equal_case(name, "limits") || string_equal_case(name, "userinfo") || string_equal_case(name, "trigger") ||
                string_equal_case(name, "json") || string_equal_case(name, "blob") || string_equal_case(name, "crypto") ||
                find_class(interp, name) != NULL) {
                return val_string(name);
            }
            fprintf(stderr, "[Line %d] RuntimeError: Undefined identifier '%s'\n", node->line, name);
            return val_null();
        }

        case NODE_VAR_DECL: {
            Value init_val = val_null();
            if (node->as.var_decl.init) {
                init_val = interpreter_eval(interp, node->as.var_decl.init, env);
            }
            uint32_t hash = node->as.var_decl.hash;
            if (hash == 0 && node->as.var_decl.var_name) hash = nadr_hash_str(node->as.var_decl.var_name);
            env_define_prehashed(env, node->as.var_decl.var_name, hash, init_val);
            return init_val;
        }

        case NODE_ASSIGN:
            return eval_assign_op(interp, node, env);

        case NODE_BINARY_OP:
            return eval_binary_op(interp, node, env);

        case NODE_UNARY_OP:
            return eval_unary_op(interp, node, env);

        case NODE_TERNARY: {
            Value cond = interpreter_eval(interp, node->as.ternary.cond, env);
            if (val_is_truthy(cond)) {
                return interpreter_eval(interp, node->as.ternary.then_expr, env);
            } else {
                return interpreter_eval(interp, node->as.ternary.else_expr, env);
            }
        }

        case NODE_BLOCK: {
            Environment* block_env = env_new(env);
            Value res = val_null();
            for (int i = 0; i < node->as.block.statements.count; i++) {
                res = interpreter_eval(interp, node->as.block.statements.nodes[i], block_env);
                if (interp->return_flag) break;
            }
            env_free(block_env);
            return res;
        }

        case NODE_IF: {
            Value cond = interpreter_eval(interp, node->as.if_stmt.cond, env);
            if (val_is_truthy(cond)) {
                return interpreter_eval(interp, node->as.if_stmt.then_b, env);
            } else if (node->as.if_stmt.else_b) {
                return interpreter_eval(interp, node->as.if_stmt.else_b, env);
            }
            return val_null();
        }

        case NODE_WHILE: {
            Value last = val_null();
            while (val_is_truthy(interpreter_eval(interp, node->as.while_stmt.cond, env))) {
                last = interpreter_eval(interp, node->as.while_stmt.body, env);
                if (interp->return_flag) break;
            }
            return last;
        }

        case NODE_DO_WHILE: {
            Value last = val_null();
            do {
                last = interpreter_eval(interp, node->as.do_while.body, env);
                if (interp->return_flag) break;
            } while (val_is_truthy(interpreter_eval(interp, node->as.do_while.cond, env)));
            return last;
        }

        case NODE_FOR: {
            Environment* for_env = env_new(env);
            if (node->as.for_stmt.init) {
                interpreter_eval(interp, node->as.for_stmt.init, for_env);
            }
            Value last = val_null();
            while (true) {
                if (node->as.for_stmt.cond) {
                    Value c = interpreter_eval(interp, node->as.for_stmt.cond, for_env);
                    if (!val_is_truthy(c)) break;
                }
                last = interpreter_eval(interp, node->as.for_stmt.body, for_env);
                if (interp->return_flag) break;
                if (node->as.for_stmt.update) {
                    interpreter_eval(interp, node->as.for_stmt.update, for_env);
                }
            }
            env_free(for_env);
            return last;
        }

        case NODE_FOR_EACH: {
            Value coll = interpreter_eval(interp, node->as.for_each.collection, env);
            if (coll.type != VAL_LIST || !coll.as.list_val) return val_null();
            Value last = val_null();
            ValueArray* arr = coll.as.list_val;
            Environment* loop_env = env_new(env);
            const char* item_name = node->as.for_each.item_name;
            uint32_t item_hash = node->as.for_each.item_hash;
            if (item_hash == 0 && item_name) item_hash = nadr_hash_str(item_name);
            for (int i = 0; i < arr->count; i++) {
                env_define_prehashed(loop_env, item_name, item_hash, arr->items[i]);
                last = interpreter_eval(interp, node->as.for_each.body, loop_env);
                if (interp->return_flag) break;
            }
            env_free(loop_env);
            return last;
        }

        case NODE_SWITCH: {
            Value target = interpreter_eval(interp, node->as.switch_stmt.target, env);
            ASTNode* default_case_body = NULL;
            for (int i = 0; i < node->as.switch_stmt.case_count; i++) {
                SwitchCase* sc = &node->as.switch_stmt.cases[i];
                if (!sc->match_val) {
                    default_case_body = sc->body;
                    continue;
                }
                Value match_v = interpreter_eval(interp, sc->match_val, env);
                if (val_equals(target, match_v)) {
                    return interpreter_eval(interp, sc->body, env);
                }
            }
            if (default_case_body) {
                return interpreter_eval(interp, default_case_body, env);
            }
            return val_null();
        }

        case NODE_TRY_CATCH: {
            Value res = interpreter_eval(interp, node->as.try_catch.try_block, env);
            if (node->as.try_catch.finally_block) {
                interpreter_eval(interp, node->as.try_catch.finally_block, env);
            }
            return res;
        }

        case NODE_RETURN: {
            Value ret = val_null();
            if (node->as.return_stmt.value) {
                ret = interpreter_eval(interp, node->as.return_stmt.value, env);
            }
            interp->return_flag = true;
            interp->return_val = ret;
            return ret;
        }

        case NODE_NEW: {
            const char* tname = node->as.new_expr.type_name;
            if (STRNCASECMP(tname, "List", 4) == 0) {
                Value list_v = val_list();
                for (int i = 0; i < node->as.new_expr.list_init.count; i++) {
                    Value item = interpreter_eval(interp, node->as.new_expr.list_init.nodes[i], env);
                    val_list_add(&list_v, item);
                }
                return list_v;
            }
            if (STRNCASECMP(tname, "Map", 3) == 0) {
                Value map_v = val_map();
                for (int i = 0; i < node->as.new_expr.map_keys.count; i++) {
                    Value k = interpreter_eval(interp, node->as.new_expr.map_keys.nodes[i], env);
                    Value v = interpreter_eval(interp, node->as.new_expr.map_vals.nodes[i], env);
                    val_map_put(&map_v, k, v);
                }
                return map_v;
            }
            ApexClassDef* klass = find_class(interp, tname);
            if (klass) {
                ApexInstance* inst = (ApexInstance*)malloc(sizeof(ApexInstance));
                inst->klass = klass;
                inst->fields = env_new(interp->global_env);

                ApexMethod* ctor = find_method(interp, klass, klass->name);
                if (ctor && ctor->body) {
                    Environment* ctor_env = env_new(inst->fields);
                    env_define(ctor_env, "this", val_instance(inst));
                    for (int p = 0; p < ctor->param_count && p < node->as.new_expr.args.count; p++) {
                        Value arg_val = interpreter_eval(interp, node->as.new_expr.args.nodes[p], env);
                        env_define(ctor_env, ctor->params[p].param_name, arg_val);
                    }
                    interpreter_eval(interp, ctor->body, ctor_env);
                    env_free(ctor_env);
                }
                return val_instance(inst);
            }
            // sobject constructor
            SObject* sobj = sobject_new(tname);
            for (int i = 0; i < node->as.new_expr.args.count; i++) {
                ASTNode* arg = node->as.new_expr.args.nodes[i];
                if (arg->type == NODE_ASSIGN && arg->as.assign.target->type == NODE_IDENTIFIER) {
                    const char* fname = arg->as.assign.target->as.identifier.name;
                    Value fval = interpreter_eval(interp, arg->as.assign.value, env);
                    sobject_put(sobj, fname, fval);
                }
            }
            return val_sobject(sobj);
        }

        case NODE_MEMBER_ACCESS: {
            Value target = interpreter_eval(interp, node->as.member_access.target, env);
            const char* mem = node->as.member_access.member_name;
            if (target.type == VAL_NULL && node->as.member_access.safe_nav) {
                return val_null();
            }
            if (target.type == VAL_SOBJECT && target.as.sobject_val) {
                return sobject_get(target.as.sobject_val, mem);
            }
            if (target.type == VAL_INSTANCE && target.as.instance_val) {
                Value fv;
                if (env_get(target.as.instance_val->fields, mem, &fv)) {
                    return fv;
                }
            }
            return val_null();
        }

        case NODE_CALL:
            return eval_method_or_call(interp, node, env);

        case NODE_TRIGGER:
            if (node->as.trigger.body) {
                return interpreter_eval(interp, node->as.trigger.body, env);
            }
            return val_null();

        case NODE_DML: {
            interp->limits.dml_statements++;
            Value target = interpreter_eval(interp, node->as.dml.target, env);
            const char* op = node->as.dml.operation;
            log_message(interp->logger, "DML_OPERATION", op);
            if (target.type == VAL_SOBJECT && target.as.sobject_val) {
                interp->limits.dml_rows++;
                if (string_equal_case(op, "insert") || string_equal_case(op, "upsert")) return mock_db_insert(target.as.sobject_val);
                if (string_equal_case(op, "update")) return mock_db_update(target.as.sobject_val);
                if (string_equal_case(op, "delete")) return mock_db_delete(target.as.sobject_val);
                if (string_equal_case(op, "undelete") || string_equal_case(op, "merge")) return target;
            } else if (target.type == VAL_LIST && target.as.list_val) {
                // Bulk DML: gather the SObjects and hand the whole statement to
                // the storage layer once, so the transaction timestamp, the id
                // sequence and the table capacity growth are all amortised over
                // the batch instead of repeating per row.
                ValueArray* arr = target.as.list_val;
                SObject** batch = (SObject**)malloc(sizeof(SObject*) * (size_t)(arr->count > 0 ? arr->count : 1));
                if (!batch) abort();
                int n = 0;
                for (int i = 0; i < arr->count; i++) {
                    Value item = arr->items[i];
                    if (item.type == VAL_SOBJECT && item.as.sobject_val) {
                        batch[n++] = item.as.sobject_val;
                    }
                }
                interp->limits.dml_rows += n;
                if (n > 0) {
                    mock_db_dml_bulk(op, batch, n);
                }
                free(batch);
                return target;
            }
            return val_null();
        }

        case NODE_SOQL: {
            interp->limits.soql_queries++;
            Value wval = val_null();
            if (node->as.soql.where_val) {
                wval = interpreter_eval(interp, node->as.soql.where_val, env);
            }
            return mock_db_query(
                node->as.soql.from_object,
                (const char**)node->as.soql.fields,
                node->as.soql.field_count,
                node->as.soql.where_field,
                node->as.soql.where_op,
                wval,
                node->as.soql.limit
            );
        }

        default:
            return val_null();
    }
}

Value interpreter_run(Interpreter* interp, ASTNode* program) {
    if (!interp || !program) return val_null();
    interp->return_flag = false;
    if (interp->ast_root_count + 1 > interp->ast_root_capacity) {
        interp->ast_root_capacity = interp->ast_root_capacity < 8 ? 8 : interp->ast_root_capacity * 2;
        interp->ast_roots = (ASTNode**)realloc(interp->ast_roots, sizeof(ASTNode*) * interp->ast_root_capacity);
    }
    interp->ast_roots[interp->ast_root_count++] = program;
    return interpreter_eval(interp, program, interp->global_env);
}
