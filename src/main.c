// James Ezra Seitenschlag
// 11.09.2026
//
// nadir runtime thingy

#include "common.h"
#include "lexer.h"
#include "parser.h"
#include "eval.h"
#include "metadata.h"
#include "nadir_daemon.h"
# include "logger.h"

static Value run_source(const char* source, Interpreter* interp, bool is_repl) {
    Lexer lexer;
    lexer_init(&lexer, source);

    Parser parser;
    parser_init(&parser, &lexer);

    ASTNode* program = parser_parse(&parser);
    Value res = val_null();
    if (!parser.had_error && program) {
        res = interpreter_run(interp, program);
        if (is_repl && res.type != VAL_NULL) {
            val_print(res);
            printf("\n");
        }
    } else if (program) {
        ast_node_free(program);
    }
    return res;
}

static void run_file(const char* path) {
    char* source = read_file(path);
    if (!source) return;

    Interpreter* interp = interpreter_new();
    run_source(source, interp, false);
    interpreter_free(interp);
    free(source);
}

static int count_char(const char* str, char c) {
    int cnt = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] == c) cnt++;
    }
    return cnt;
}

static void start_repl(void) {
    printf("nadir apex runtime\n");
    printf("type exit to quit\n\n");

    Interpreter* interp = interpreter_new();
    char buffer[8192] = {0};
    char line[1024];

    int open_braces = 0;
    int open_parens = 0;
    int open_brackets = 0;

    while (1) {
        if (open_braces > 0 || open_parens > 0 || open_brackets > 0) {
            printf("... ");
        } else {
            printf(">>> ");
        }
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        // trim whitespace
        char* trimmed = line;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        int len = (int)strlen(trimmed);
        while (len > 0 && (trimmed[len - 1] == '\n' || trimmed[len - 1] == '\r' || trimmed[len - 1] == ' ' || trimmed[len - 1] == ';')) {
            trimmed[--len] = '\0';
        }

        if (open_braces == 0 && open_parens == 0 && open_brackets == 0) {
            if (STRNCASECMP(trimmed, "exit", 4) == 0 || STRNCASECMP(trimmed, "quit", 4) == 0) {
                break;
            }
            if (STRNCASECMP(trimmed, "clear", 5) == 0 || STRNCASECMP(trimmed, "cls", 3) == 0) {
                #ifdef _WIN32
                system("cls");
                #else
                system("clear");
                #endif
                continue;
            }
            if (STRNCASECMP(trimmed, "help", 4) == 0) {
                printf("commands: exit, clear, help, import(<path>), init([path])\n\n");
                continue;
            }
            if (STRNCASECMP(trimmed, "init", 4) == 0 || STRNCASECMP(trimmed, "project_init", 12) == 0) {
                const char* cmd_name = STRNCASECMP(trimmed, "init", 4) == 0 ? "init" : "project_init";
                int clen = (int)strlen(cmd_name);
                char* p = trimmed + clen;
                while (*p == ' ' || *p == '\t' || *p == '(') p++;
                if (*p == '\'' || *p == '"') p++;
                char path_buf[512] = {0};
                int plen = 0;
                while (*p && *p != '\'' && *p != '"' && *p != ')' && *p != ';' && plen < 510) {
                    path_buf[plen++] = *p++;
                }
                while (plen > 0 && (path_buf[plen - 1] == ' ' || path_buf[plen - 1] == '\t')) {
                    path_buf[--plen] = '\0';
                }
                path_buf[plen] = '\0';
                const char* target_path = plen > 0 ? path_buf : ".";
                nadir_project_init(target_path, interp);
                continue;
            }
            if (STRNCASECMP(trimmed, "import", 6) == 0) {
                char* p = trimmed + 6;
                while (*p == ' ' || *p == '\t' || *p == '(') p++;
                if (*p == '\'' || *p == '"') p++;
                char path_buf[512] = {0};
                int plen = 0;
                while (*p && *p != '\'' && *p != '"' && *p != ')' && *p != ';' && plen < 510) {
                    path_buf[plen++] = *p++;
                }
                while (plen > 0 && (path_buf[plen - 1] == ' ' || path_buf[plen - 1] == '\t')) {
                    path_buf[--plen] = '\0';
                }
                path_buf[plen] = '\0';
                if (plen > 0) {
                    char* file_src = read_file(path_buf);
                    if (file_src) {
                        run_source(file_src, interp, false);
                        free(file_src);
                        printf("[imported] %s\n", path_buf);
                    } else {
                        fprintf(stderr, "Error: Could not import file \"%s\"\n", path_buf);
                    }
                } else {
                    fprintf(stderr, "Usage: import(path/to/class.cls)\n");
                }
                continue;
            }
        }

        strcat(buffer, line);
        // multiline balance tracking
        open_braces += count_char(line, '{') - count_char(line, '}');
        open_parens += count_char(line, '(') - count_char(line, ')');
        open_brackets += count_char(line, '[') - count_char(line, ']');

        if (open_braces <= 0 && open_parens <= 0 && open_brackets <= 0) {
            open_braces = 0;
            open_parens = 0;
            open_brackets = 0;

            if (strlen(buffer) > 0) {
                run_source(buffer, interp, true);
                buffer[0] = '\0';
            }
        }
    }

    printf("Exiting Nadir REPL.\n");
    interpreter_free(interp);
}

int main(int argc, char* argv[]) {
    const char* log_file_name = "debug.log";
    if (argc > 1) {
        Interpreter* interp = interpreter_new();
        interp->logger = init_logger(log_file_name);
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--init") == 0 || strcmp(argv[i], "init") == 0) {
                const char* p = (i + 1 < argc) ? argv[++i] : ".";
                nadir_project_init(p, interp);
            } else if (strcmp(argv[i], "--daemon") == 0 || strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "daemon") == 0) {
                int port = 8042;
                if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                    port = atoi(argv[++i]);
                }
                int ret = nadir_daemon_start(port, ".", interp);
                interpreter_free(interp);
                return ret;
            } else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
                run_source(argv[++i], interp, false);
            } else if (strcmp(argv[i], "-") == 0) {
                // Read entire stdin and execute as batch script
                size_t cap = 65536;
                size_t len = 0;
                char* buf = (char*)malloc(cap);
                if (buf) {
                    size_t n;
                    while ((n = fread(buf + len, 1, cap - len - 1, stdin)) > 0) {
                        len += n;
                        if (len + 1024 >= cap) {
                            cap *= 2;
                            buf = (char*)realloc(buf, cap);
                            if (!buf) break;
                        }
                    }
                    if (buf) {
                        buf[len] = '\0';
                        run_source(buf, interp, false);
                        free(buf);
                    }
                }
            } else {
                char* source = read_file(argv[i]);
                if (source) {
                    run_source(source, interp, false);
                    free(source);
                }
            }
        }
        interpreter_free(interp);
    } else {
        start_repl();
    }
    return 0;
}