#include <stdbool.h>
#include <vslc.h>

/**Generate table of strings in a rodata section. */
void generate_stringtable(void);
/**Declare global variables in a bss section */
void generate_global_variables(size_t n_globals, symbol_t **global_list);
/**Declare global variables in a bss section */
void generate_functions(symbol_t **main, size_t n_globals, symbol_t **global_list);
/**Generate function entry code
 * @param function symbol table entry of function */
void generate_function(symbol_t *function);
/**Generate code for a node in the AST, to be called recursively from
 * generate_function
 * @param node root node of current code block */
static void generate_node(node_t *node, symbol_t *function, unsigned int *stack_alignment, bool *returned);
/**Initializes program (already implemented) */
void generate_main(symbol_t *first);

#define FUNC_PREFIX "_func_"

#define MIN(a, b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a < _b ? _a : _b; })

#define MAX(a, b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
     _a > _b ? _a : _b; })

static const char *PARAMETER_REGISTERS[6] = {
    "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9"};

void generate_program(void) {
    /* TODO: Emit assembly instructions for functions, function calls,
     * print statements and expressions.
     * The provided function 'generate_main' creates a program entry point
     * for the function symbol it is given as argument.
     */

    symbol_t *main;

    generate_stringtable();

    size_t n_globals = tlhash_size(global_names);
    symbol_t **global_list = malloc(sizeof(symbol_t *) * n_globals);
    tlhash_values(global_names, (void **)global_list);

    generate_global_variables(n_globals, global_list);
    generate_functions(&main, n_globals, global_list);

    generate_main(main);

    free(global_list);
}

void generate_stringtable(void) {
    /* These can be used to emit numbers, strings and a run-time
     * error msg. from main
     */
    puts(".section .rodata");
    puts(".newline:\n\t.asciz \"\\n\"");
    puts(".intout:\n\t.asciz \"\%ld \"");
    puts(".strout:\n\t.asciz \"\%s \"");
    puts(".errout:\n\t.asciz \"Wrong number of arguments\"");

    for (size_t i = 0; i < stringc; i++) {
        printf(".STR%d:\n\t.asciz %s\n", i, string_list[i]);
    }
}

void generate_global_variables(size_t n_globals, symbol_t **global_list) {
    puts(".section .bss");
    puts(".align 8");

    symbol_t *sym;
    for (size_t i = 0; i < n_globals; i++) {
        sym = global_list[i];
        if (sym->type != SYM_GLOBAL_VAR) {
            continue;
        }

        printf(".%s:\n", sym->name);
    }
}

void generate_functions(symbol_t **main, size_t n_globals, symbol_t **global_list) {
    *main = NULL;
    bool main_lock = false;

    puts(".section .text");

    symbol_t *sym;
    for (size_t i = 0; i < n_globals; i++) {
        sym = global_list[i];
        if (sym->type != SYM_FUNCTION) {
            continue;
        }

        bool is_main = !strncmp("main", sym->name, 6);
        if (is_main || !main_lock && (*main == NULL || (*main)->seq > sym->seq)) {
            *main = sym;
            main_lock = is_main;
        }

        generate_function(sym);
    }
}

unsigned int __allocate_aligned_stack(size_t slots, unsigned int *stack_alignment) {
    *stack_alignment += slots * 8;

    unsigned int offset = 0;
    if ((*stack_alignment) % 16 != 0) {
        offset = 16 - ((*stack_alignment) % 16);
        *stack_alignment += offset;
    }

    if (offset == 0 && slots == 0) {
        return 0;
    }

    printf("\t# Stack allocation for %u slots with alignment of %u\n", slots, offset);
    printf("\tsubq $%d, %%rsp\n", slots * 8 + offset);
    return offset;
}

void __allocate_stack(size_t slots, unsigned int *stack_alignment) {
    if (slots == 0) {
        return;
    }

    *stack_alignment += slots * 8;
    printf("\t# Stack allocation for %u slots (Stack offset %u)\n", slots, (*stack_alignment) % 16);
    printf("\tsubq $%d, %%rsp\n", slots * 8);
}

unsigned int __align_stack(unsigned int *stack_alignment) {
    // Stack is already aligned
    if ((*stack_alignment) % 16 == 0) {
        return 0;
    }

    unsigned int offset = 16 - ((*stack_alignment) % 16);
    *stack_alignment += offset;
    printf("\t# Stack with alignment of %u\n", offset);
    printf("\tsubq $%d, %%rsp\n", offset);
    return offset;
}

void __unalign_stack(unsigned int alignment, unsigned int *stack_alignment) {
    if (alignment != 0) {
        printf("\t# Return stack to state before alignment");
        printf("\taddq $%d, %rsp\n", alignment);
        *stack_alignment -= alignment;
    }
}

void __move_reg_to_slot(const char *reg, int slot) {
    printf("\tmovq %s, %d(%%rbp)\n", reg, (slot + 1) * -8);
}

void __move_slot_to_reg(const char *reg, int slot) {
    printf("\tmovq %d(%%rbp), %s\n", (slot + 1) * -8, reg);
}

void __move_reg_to_global(const char *reg, char *global) {
    printf("\tmovq %s, .%s\n", reg, global);
}

void __move_global_to_reg(const char *reg, char *global) {
    printf("\tmovq .%s, %s\n", global, reg);
}

size_t __get_variable_count(symbol_t *function) {
    size_t n_locals = tlhash_size(function->locals);
    return n_locals - function->nparms;
}

int __get_slot(symbol_t *function, symbol_t *sym) {
    if (sym->type == SYM_PARAMETER) {
        return MIN(5, function->nparms - 1) - sym->seq;
    }

    return sym->seq + MIN(6, function->nparms);
}

void __print_slots(symbol_t *function) {
    size_t n_locals = tlhash_size(function->locals);
    symbol_t **local_list = malloc(sizeof(symbol_t *) * n_locals);
    tlhash_values(function->locals, (void **)local_list);

    printf("Stack slots for function \"%s\"\n", function->name);

    symbol_t *node;
    for (size_t i = 0; i < n_locals; i++) {
        node = local_list[i];
        if (node->type == SYM_PARAMETER) {
            printf("PARAM");
        } else {
            printf("VAR");
        }

        printf(": Seq %d - Slot %d\n", node->seq, __get_slot(function, node));
    }

    free(local_list);
}

void generate_function(symbol_t *function) {
    printf(".globl %s%s\n", FUNC_PREFIX, function->name);
    printf("%s%s:\n", FUNC_PREFIX, function->name);
    // Initialize stack frame
    puts("\tpushq %rbp");
    puts("\tmovq %rsp, %rbp");

    // The amount of parameters that are not yet on the stack
    size_t paramc = MIN(6, function->nparms);

    // At this stage the stack is aligned, since we've got
    // return address and rbp pushed.
    unsigned int stack_alignment = 0;
    bool returned = false;
    __allocate_stack(paramc + __get_variable_count(function), &stack_alignment);

    //__print_slots(function);

    // Move this in right to left order so that parameter 0
    // is at the top of the stack. This also means that our
    // parameters will be in order on the stack, with 0 at
    // the top.
    for (int param = 0; param < paramc; param++) {
        __move_reg_to_slot(PARAMETER_REGISTERS[paramc - param - 1], param);
    }

    // All parameters are now on the stack

    generate_node(function->node, function, &stack_alignment, &returned);

    // This means there was no return statement
    if (!returned) {
        puts("# Generated return statement");
        puts("\tmovq $0, %rax");
        puts("\tleave");
        puts("\rret");
    }
}

void __write_param_accessor(size_t param, char *str, size_t nchars) {
    memset(str, 0, nchars);  // Reset buffer

    if (param < 6) {
        strcpy(str, PARAMETER_REGISTERS[param]);
        return;
    }

    snprintf(str, nchars, "%d(%%rsp)", (param - 6) * 8);
}

void __call_function(node_t *node, symbol_t *calling_function, unsigned int *stack_alignment) {
    if (node->n_children != 2) {
        fprintf(stderr, "Invalid function call\n");
        exit(EXIT_FAILURE);
    }

    node_t *identifier = node->children[0];
    symbol_t *func = identifier->entry;

    node_t *argument_list = node->children[1];

    int args_provided = argument_list == NULL ? 0 : argument_list->n_children;
    if (args_provided != func->nparms) {
        fprintf(stderr, "Wrong number of arguments for call to %s in %s\n", func->name, calling_function->name);
        exit(EXIT_FAILURE);
    }

    unsigned int required_stack_space = MAX(6, func->nparms) - 6;
    unsigned int alignment = __allocate_aligned_stack(required_stack_space, stack_alignment);

    char access_buffer[32];
    node_t *arg;
    for (size_t param = 0; param < func->nparms; param++) {
        arg = argument_list->children[param];
        // Illegal to return so we can leave it as NULL
        generate_node(arg, calling_function, stack_alignment, NULL);

        __write_param_accessor(param, access_buffer, 32);
        printf("\tmovq %%rax, %s\n", access_buffer);
    }

    printf("\tcall %s%s\n", FUNC_PREFIX, func->name);
    __unalign_stack(alignment, stack_alignment);
}

void __generate_expression(node_t *node, symbol_t *function, unsigned int *stack_alignment) {
    // For all calls here we disallow the return statement so we can pass a null pointer

    char *op = node->data;
    if (op == NULL) {
        // This means that we have either
        // 1. Identifier
        // 2. Constant value
        // 3. Function call

        // This is then a function call
        if (node->n_children == 2) {
            // This will put the result in %rax which is what we want
            __call_function(node, function, stack_alignment);
            return;
        }

        // We have support for generating these in generate_node so we
        // simply delegate it
        generate_node(node->children[0], function, stack_alignment, NULL);
        return;
    }

    node_t *c1 = node->children[0];

    // Unary operators
    if (node->n_children == 1) {
        generate_node(c1, function, stack_alignment, NULL);

        switch (*op) {
            case '-':
                puts("\tneg %rax");
                break;
            case '~':
                puts("\tnot %rax");
                break;
        }
        return;
    }

    node_t *c2 = node->children[1];

    generate_node(c2, function, stack_alignment, NULL);
    puts("\tpushq %rax");
    *stack_alignment += 8;

    generate_node(c1, function, stack_alignment, NULL);

    puts("\tpopq %r8");  // Random caller-saved register
    *stack_alignment -= 8;

    // Now have lh side in rax and rh side in r8

    switch (*op) {
        case '|':
            puts("\tor %r8, %rax");
            break;
        case '^':
            puts("\txor %r8, %rax");
            break;
        case '&':
            puts("\tand %r8, %rax");
            break;
        case '+':
            puts("\taddq %r8, %rax");
            break;
        case '-':
            puts("\tsubq %r8, %rax");
            break;
        case '*':
            puts("\timulq %r8");
            break;
        case '/':
            puts("\tidivq %r8");
            break;
    }
}

void __access_variable(const char *reg, symbol_t *sym, symbol_t *function) {
    switch (sym->type) {
        case SYM_GLOBAL_VAR:
            __move_global_to_reg(reg, sym->name);
            break;
        case SYM_LOCAL_VAR:
        case SYM_PARAMETER:
            __move_slot_to_reg(reg, __get_slot(function, sym));
            break;
        default:
            fprintf(stderr, "Unsupported symbol type for identifier data \"%s\"\n", sym->name);
            exit(EXIT_FAILURE);
    }
}

void __write_variable(const char *reg, symbol_t *sym, symbol_t *function) {
    switch (sym->type) {
        case SYM_GLOBAL_VAR:
            __move_reg_to_global(reg, sym->name);
            break;
        case SYM_LOCAL_VAR:
        case SYM_PARAMETER:
            __move_reg_to_slot(reg, __get_slot(function, sym));
            break;
        default:
            fprintf(stderr, "Unsupported symbol type for identifier data \"%s\"\n", sym->name);
            exit(EXIT_FAILURE);
    }
}

void generate_node(node_t *node, symbol_t *function, unsigned int *stack_alignment, bool *returned) {
    node_t *identifier;
    node_t *expression;
    switch (node->type) {
        case EXPRESSION:
            __generate_expression(node, function, stack_alignment);
            return;
        case IDENTIFIER_DATA:
            // This assumes the case where we want to access the value in the
            // referenced variable. Assignments are handled separately.
            __access_variable("%rax", node->entry, function);
            return;
        case NUMBER_DATA:
            int64_t value = *((int64_t *)node->data);
            printf("\tmovq $%ld, %%rax\n", value);
            return;
        case ASSIGNMENT_STATEMENT:
            identifier = node->children[0];
            expression = node->children[1];

            // This will find whatever expression we need and put it in %rax
            generate_node(expression, function, stack_alignment, returned);
            __write_variable("%rax", identifier->entry, function);
            return;
        case ADD_STATEMENT:
        case SUBTRACT_STATEMENT:
        case DIVIDE_STATEMENT:
        case MULTIPLY_STATEMENT:
            identifier = node->children[0];
            expression = node->children[1];

            generate_node(expression, function, stack_alignment, returned);
            puts("\tmovq %rax, %r8");
            __access_variable("%rax", identifier->entry, function);

            switch (node->type) {
                case ADD_STATEMENT:
                    puts("\taddq %r8, %rax");
                    break;
                case SUBTRACT_STATEMENT:
                    puts("\tsubq %r8, %rax");
                    break;
                case DIVIDE_STATEMENT:
                    puts("\tidivq %r8");
                    break;
                case MULTIPLY_STATEMENT:
                    puts("\timulq %r8");
                    break;
            }

            __write_variable("%rax", identifier->entry, function);
            return;
        case PRINT_STATEMENT:
            node_t *item;
            unsigned int alignment;
            for (size_t i = 0; i < node->n_children; i++) {
                item = node->children[i];

                switch (item->type) {
                    case STRING_DATA:
                        printf("\tmovq $.strout, %%rdi\n");
                        printf("\tmovq $.STR%ld, %%rsi\n", *((size_t *)item->data));

                        alignment = __align_stack(stack_alignment);
                        puts("\tcall printf");
                        __unalign_stack(alignment, stack_alignment);
                        break;
                    case IDENTIFIER_DATA:
                        printf("\tmovq $.intout, %%rdi\n");
                        __access_variable("%rsi", item->entry, function);

                        alignment = __align_stack(stack_alignment);
                        puts("\tcall printf");
                        __unalign_stack(alignment, stack_alignment);
                        break;
                    case EXPRESSION:
                        generate_node(item, function, stack_alignment, returned);
                        printf("\tmovq $.intout, %%rdi\n");
                        puts("\tmovq %rax, %rsi");

                        alignment = __align_stack(stack_alignment);
                        puts("\tcall printf");
                        __unalign_stack(alignment, stack_alignment);
                        break;
                }
            }

            // New line
            printf("\tmovq $.newline, %%rdi\n");
            puts("\tcall printf");

            return;
        case RETURN_STATEMENT:
            if (returned == NULL) {
                fprintf(stderr, "Return in illegal position inside %s\n", function->name);
                exit(EXIT_FAILURE);
            }

            *returned = true;
            generate_node(node->children[0], function, stack_alignment, returned);
            puts("\tleave");
            puts("\tret");
            return;
    }

    // A return statement has been called, we don't need to continue here
    if (returned != NULL && *returned) {
        return;
    }

    node_t *child;
    for (size_t i = 0; i < node->n_children; i++) {
        child = node->children[i];
        if (child->type == DECLARATION) {
            continue;
        }

        generate_node(child, function, stack_alignment, returned);
    }
}

/**Generates the main function with argument parsing and calling of our
 * main function (first, if no function is named main)
 * @param first Symbol table entry of our main function */
void generate_main(symbol_t *first) {
    puts(".globl main");
    puts(".section .text");
    puts("main:");

    puts("\tpushq   %rbp");
    puts("\tmovq    %rsp, %rbp");

    unsigned int stack_alignment = 0;

    printf("\tsubq\t$1,%%rdi\n");
    printf("\tcmpq\t$%zu,%%rdi\n", first->nparms);
    printf("\tjne\tABORT\n");
    printf("\tcmpq\t$0,%%rdi\n");
    printf("\tjz\tSKIP_ARGS\n");

    printf("\tmovq\t%%rdi,%%rcx\n");
    printf("\taddq $%zu, %%rsi\n", 8 * first->nparms);
    printf("PARSE_ARGV:\n");
    printf("\tpushq %%rcx\n");
    printf("\tpushq %%rsi\n");

    printf("\tmovq\t(%%rsi),%%rdi\n");
    printf("\tmovq\t$0,%%rsi\n");
    printf("\tmovq\t$10,%%rdx\n");
    printf("\tcall\tstrtol\n");

    /*  Now a new argument is an integer in rax */

    printf("\tpopq %%rsi\n");
    printf("\tpopq %%rcx\n");
    printf("\tpushq %%rax\n");

    printf("\tsubq $8, %%rsi\n");
    printf("\tloop PARSE_ARGV\n");

    /* Now the arguments are in order on stack */
    for (int arg = 0; arg < MIN(6, first->nparms); arg++)
        printf("\tpopq\t%s\n", PARAMETER_REGISTERS[arg]);

    stack_alignment += (MAX(6, first->nparms) - 6) * 8;

    printf("SKIP_ARGS:\n");

    unsigned int alignment = __align_stack(&stack_alignment);
    printf("\tcall %s%s\n", FUNC_PREFIX, first->name);
    __unalign_stack(alignment, stack_alignment);

    printf("\tjmp\tEND\n");
    printf("ABORT:\n");
    printf("\tmovq\t$.errout, %%rdi\n");
    printf("\tcall puts\n");

    printf("END:\n");
    puts("\tmovq    %rax, %rdi");
    puts("\tcall    exit");
}
