// James Ezra Seitenschlag
// 11.09.2026
//
// nadir runtime thingy

#include "eval_internal.h"
#include "logger.h"
#include <ctype.h>

Value eval_method_or_call(Interpreter* interp, ASTNode* node, Environment* env) {
    log_message(interp->logger, "METHOD_INVOCATION", node->as.call.method_name);
    const char* method_name = node->as.call.method_name;
    ASTNode* callee_node = node->as.call.callee;

    // Fast path: if callee is a local variable in env, resolve it directly!
    Value target = val_null();
    bool is_local_var = false;
    if (callee_node && callee_node->type == NODE_IDENTIFIER) {
        const char* vname = callee_node->as.identifier.name;
        uint32_t vhash = callee_node->as.identifier.hash;
        if (vhash == 0 && vname) vhash = nadr_hash_str(vname);
        if (env_get_prehashed(env, vname, vhash, &target)) {
            is_local_var = true;
        }
    }

    if (!is_local_var) {
        // Check system / standard library namespace builtins first
        bool handled = false;
        Value sys_res = eval_system_builtins(interp, node, env, &handled);
        if (handled) {
            return sys_res;
        }

        // Direct or recursive function call without explicit receiver
        if (!callee_node) {
            for (int c = 0; c < interp->class_count; c++) {
                ApexMethod* m = find_method(interp, &interp->classes[c], method_name);
                if (m && m->body) {
                    Environment* menv = env_new(interp->global_env);
                    for (int p = 0; p < m->param_count && p < node->as.call.args.count; p++) {
                        Value av = interpreter_eval(interp, node->as.call.args.nodes[p], env);
                        env_define(menv, m->params[p].param_name, av);
                    }
                    interpreter_eval(interp, m->body, menv);
                    env_free(menv);
                    if (interp->return_flag) {
                        interp->return_flag = false;
                        return interp->return_val;
                    }
                    return val_null();
                }
            }
        }

        // Static class method call (only if not a local/enclosing variable)
        if (callee_node && callee_node->type == NODE_IDENTIFIER) {
            ApexClassDef* klass = find_class(interp, callee_node->as.identifier.name);
            if (klass) {
                ApexMethod* m = find_method(interp, klass, method_name);
                if (m && m->body) {
                    Environment* menv = env_new(interp->global_env);
                    for (int p = 0; p < m->param_count && p < node->as.call.args.count; p++) {
                        Value av = interpreter_eval(interp, node->as.call.args.nodes[p], env);
                        env_define(menv, m->params[p].param_name, av);
                    }
                    interpreter_eval(interp, m->body, menv);
                    env_free(menv);
                    if (interp->return_flag) {
                        interp->return_flag = false;
                        return interp->return_val;
                    }
                    return val_null();
                }
            }
        }
    }

    // Instance or collection method calls
    if (callee_node) {
        if (!is_local_var) {
            target = interpreter_eval(interp, callee_node, env);
        }
        if (target.type == VAL_NULL && node->as.call.safe_nav) return val_null();

        // String methods
        if (target.type == VAL_STRING && target.as.string_val) {
            if (string_equal_case(method_name, "length")) {
                return val_int((int64_t)strlen(target.as.string_val));
            }
            if (string_equal_case(method_name, "tolowercase")) {
                char* dup = duplicate_string(target.as.string_val);
                for (char* p = dup; *p; p++) *p = (char)tolower((unsigned char)*p);
                Value res = val_string(dup);
                free(dup);
                return res;
            }
            if (string_equal_case(method_name, "touppercase")) {
                char* dup = duplicate_string(target.as.string_val);
                for (char* p = dup; *p; p++) *p = (char)toupper((unsigned char)*p);
                Value res = val_string(dup);
                free(dup);
                return res;
            }
            if (string_equal_case(method_name, "contains") && node->as.call.args.count >= 1) {
                Value sub = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                if (sub.type == VAL_STRING && sub.as.string_val) {
                    return val_bool(strstr(target.as.string_val, sub.as.string_val) != NULL);
                }
                return val_bool(false);
            }
        }

        // List methods
        if (target.type == VAL_LIST && target.as.list_val) {
            if (string_equal_case(method_name, "add") && node->as.call.args.count >= 1) {
                Value it = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                val_list_add(&target, it);
                return val_null();
            }
            if (string_equal_case(method_name, "get") && node->as.call.args.count >= 1) {
                Value idx = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                return val_list_get(&target, (int)idx.as.int_val);
            }
            if (string_equal_case(method_name, "size")) {
                return val_int(val_list_size(&target));
            }
            if (string_equal_case(method_name, "isempty")) {
                return val_bool(val_list_size(&target) == 0);
            }
            if (string_equal_case(method_name, "clear")) {
                target.as.list_val->count = 0;
                return val_null();
            }
            if (string_equal_case(method_name, "contains") && node->as.call.args.count >= 1) {
                Value it = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                for (int k = 0; k < target.as.list_val->count; k++) {
                    if (val_equals(target.as.list_val->items[k], it)) return val_bool(true);
                }
                return val_bool(false);
            }
        }

        // Map methods
        if (target.type == VAL_MAP && target.as.map_val) {
            if (string_equal_case(method_name, "put") && node->as.call.args.count >= 2) {
                Value k = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                Value v = interpreter_eval(interp, node->as.call.args.nodes[1], env);
                val_map_put(&target, k, v);
                return val_null();
            }
            if (string_equal_case(method_name, "get") && node->as.call.args.count >= 1) {
                Value k = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                return val_map_get(&target, k);
            }
            if (string_equal_case(method_name, "size")) {
                return val_int(val_map_size(&target));
            }
            if (string_equal_case(method_name, "containskey") && node->as.call.args.count >= 1) {
                Value k = interpreter_eval(interp, node->as.call.args.nodes[0], env);
                Value found = val_map_get(&target, k);
                return val_bool(found.type != VAL_NULL);
            }
        }

        // Apex class instance methods
        if (target.type == VAL_INSTANCE && target.as.instance_val) {
            ApexInstance* inst = target.as.instance_val;
            ApexMethod* m = find_method(interp, inst->klass, method_name);
            if (m && m->body) {
                log_message(interp->logger, "INSTANCE_METHOD_CALL", method_name);
                Environment* menv = env_new(inst->fields);
                env_define(menv, "this", target);
                for (int p = 0; p < m->param_count && p < node->as.call.args.count; p++) {
                    Value av = interpreter_eval(interp, node->as.call.args.nodes[p], env);
                    env_define(menv, m->params[p].param_name, av);
                }
                interpreter_eval(interp, m->body, menv);
                env_free(menv);
                if (interp->return_flag) {
                    interp->return_flag = false;
                    return interp->return_val;
                }
                return val_null();
            }
        }
    } else {
        if (string_equal_case(method_name, "super")) {
            return val_null();
        }
        // Call on implicit 'this'
        Value this_val;
        if (env_get(env, "this", &this_val) && this_val.type == VAL_INSTANCE) {
            ApexInstance* inst = this_val.as.instance_val;
            ApexMethod* m = find_method(interp, inst->klass, method_name);
            if (m && m->body) {
                log_message(interp->logger, "IMPLICIT_METHOD_CALL", m->name);
                Environment* menv = env_new(inst->fields);
                env_define(menv, "this", this_val);
                for (int p = 0; p < m->param_count && p < node->as.call.args.count; p++) {
                    Value av = interpreter_eval(interp, node->as.call.args.nodes[p], env);
                    env_define(menv, m->params[p].param_name, av);
                }
                interpreter_eval(interp, m->body, menv);
                env_free(menv);
                if (interp->return_flag) {
                    interp->return_flag = false;
                    return interp->return_val;
                }
                return val_null();
            }
        }
    }

    return val_null();
}
