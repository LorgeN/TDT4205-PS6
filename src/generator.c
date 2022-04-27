#include <stdbool.h>
#include <vslc.h>

// Struct containing data for the current target/goal/objective
// of the compiler
struct compilation_target_t {
    // The node we are working on
    node_t *node;
    // The function the node is contained within
    symbol_t *function;
    // The current stack alignment, i.e. how many bytes we've pushed
    // on the stack at this point
    unsigned int *stack_alignment;
    // If a return statement has been added
    bool *returned;
    // The target destination of the value of this node, such as a
    // register or memory address
    char *target_destination;
    // Some number we use to mangle labels to make them unique
    unsigned int *label_mangle_index;
    // Jump label for NULL_STATEMENT
    char *surrounding_loop_label;
};

/**Generate table of strings in a rodata section. */
static void generate_stringtable(void);
/**Declare global variables in a bss section */
static void generate_global_variables(size_t n_globals, symbol_t **global_list);
/**Declare global variables in a bss section */
static void generate_functions(symbol_t **main, size_t n_globals, symbol_t **global_list);
/**Generate function entry code
 * @param function symbol table entry of function */
static void generate_function(symbol_t *function);
static void generate_node(struct compilation_target_t target);
/**Initializes program (already implemented) */
static void generate_main(symbol_t *first);

// Prefix for all functions that are compiled
#define FUNC_PREFIX "_func_"
#define LABEL_MAX_SIZE 128

// Macros that avoid evaluating twice
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
        printf(".STR%lu:\n\t.asciz %s\n", i, string_list[i]);
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

static unsigned int allocate_aligned_stack(size_t slots, unsigned int *stack_alignment) {
    *stack_alignment += slots * 8;

    unsigned int offset = 0;
    if ((*stack_alignment) % 16 != 0) {
        offset = 16 - ((*stack_alignment) % 16);
        *stack_alignment += offset;
    }

    if (offset == 0 && slots == 0) {
        return 0;
    }

    printf("\tsubq $%lu, %%rsp\n", slots * 8 + offset);
    return offset;
}

static void allocate_stack(size_t slots, unsigned int *stack_alignment) {
    if (slots == 0) {
        return;
    }

    *stack_alignment += slots * 8;
    printf("\tsubq $%lu, %%rsp\n", slots * 8);
}

static unsigned int align_stack(unsigned int *stack_alignment) {
    // Stack is already aligned
    if ((*stack_alignment) % 16 == 0) {
        return 0;
    }

    unsigned int offset = 16 - ((*stack_alignment) % 16);
    *stack_alignment += offset;
    printf("\tsubq $%d, %%rsp\n", offset);
    return offset;
}

static void unalign_stack(unsigned int alignment, unsigned int *stack_alignment) {
    if (alignment != 0) {
        printf("\taddq $%d, %%rsp\n", alignment);
        *stack_alignment -= alignment;
    }
}

static void make_label(char *buf, size_t maxlen, char *prefix, struct compilation_target_t target) {
    snprintf(buf, maxlen, "._%s_%s%u", target.function->name, prefix, *target.label_mangle_index);
}

static void label_here(char *buf) {
    printf("%s:\n", buf);
}

static void move_reg_to_slot(const char *reg, int slot) {
    printf("\tmovq %s, %d(%%rbp)\n", reg, (slot + 1) * -8);
}

static void move_slot_to_reg(const char *reg, int slot) {
    printf("\tmovq %d(%%rbp), %s\n", (slot + 1) * -8, reg);
}

static void move_reg_to_global(const char *reg, char *global) {
    printf("\tmovq %s, .%s\n", reg, global);
}

static void move_global_to_reg(const char *reg, char *global) {
    printf("\tmovq .%s, %s\n", global, reg);
}

static size_t get_variable_count(symbol_t *function) {
    return tlhash_size(function->locals) - function->nparms;
}

static int get_slot(symbol_t *function, symbol_t *sym) {
    if (sym->type == SYM_PARAMETER) {
        return MIN(5, function->nparms - 1) - sym->seq;
    }

    return sym->seq + MIN(6, function->nparms);
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
    unsigned int mangle_index = 0;
    bool returned = false;
    allocate_stack(paramc + get_variable_count(function), &stack_alignment);

    // Move this in right to left order so that parameter 0
    // is at the top of the stack. This also means that our
    // parameters will be in order on the stack, with 0 at
    // the top.
    for (int param = 0; param < paramc; param++) {
        move_reg_to_slot(PARAMETER_REGISTERS[paramc - param - 1], param);
    }

    // All parameters are now on the stack

    struct compilation_target_t target = {
        .function = function,
        .node = function->node,
        .stack_alignment = &stack_alignment,
        .target_destination = "%rax",
        .returned = &returned,
        .label_mangle_index = &mangle_index,
        .surrounding_loop_label = NULL};

    generate_node(target);

    // This means there was no return statement
    if (!returned) {
        puts("\t# Automatically generated return statement");
        puts("\tmovq $0, %rax");
        puts("\tleave");
        puts("\tret");
    }
}

static void write_param_accessor(size_t param, char *str, size_t nchars) {
    memset(str, 0, nchars);  // Reset buffer

    if (param < 6) {
        strcpy(str, PARAMETER_REGISTERS[param]);
        return;
    }

    snprintf(str, nchars, "%lu(%%rsp)", (param - 6) * 8);
}

static void call_function(struct compilation_target_t target) {
    if (target.node->n_children != 2) {
        fprintf(stderr, "Invalid function call\n");
        exit(EXIT_FAILURE);
    }

    node_t *identifier = target.node->children[0];
    symbol_t *func = identifier->entry;

    node_t *argument_list = target.node->children[1];

    int args_provided = argument_list == NULL ? 0 : argument_list->n_children;
    if (args_provided != func->nparms) {
        fprintf(stderr, "Wrong number of arguments for call to %s in %s\n", func->name, target.function->name);
        exit(EXIT_FAILURE);
    }

    /*
    This compiler does not utilize the caller-saved registers in a way
    that requires us to save them here, that is to say that any value
    that isn't immediately used is pushed to the stack anyway (this mainly
    applies to expressions)
    */

    unsigned int required_stack_space = MAX(6, func->nparms) - 6;
    unsigned int alignment = allocate_aligned_stack(required_stack_space, target.stack_alignment);

    char access_buffer[32] = {0};
    node_t *arg;
    for (size_t param = 0; param < func->nparms; param++) {
        arg = argument_list->children[param];

        write_param_accessor(param, access_buffer, 32);
        struct compilation_target_t child_target = {
            .function = target.function,
            .node = arg,
            .stack_alignment = target.stack_alignment,
            .returned = NULL,
            .target_destination = access_buffer,
            .label_mangle_index = target.label_mangle_index,
            .surrounding_loop_label = target.surrounding_loop_label};

        generate_node(child_target);
    }

    printf("\tcall %s%s\n", FUNC_PREFIX, func->name);
    unalign_stack(alignment, target.stack_alignment);
}

static void generate_expression(struct compilation_target_t target) {
    char *op = target.node->data;
    if (op == NULL) {
        // This means that we have either
        // 1. Identifier
        // 2. Constant value
        // 3. Function call

        // This is then a function call
        if (target.node->n_children == 2) {
            // This will put the result in %rax
            call_function(target);

            // Move if different
            if (strcmp("%rax", target.target_destination)) {
                printf("\tmovq %%rax, %s\n", target.target_destination);
            }
            return;
        }

        // We have support for generating these in generate_node so we
        // simply delegate it
        target.node = target.node->children[0];
        generate_node(target);
        return;
    }

    node_t *c1 = target.node->children[0];

    // Unary operators
    if (target.node->n_children == 1) {
        target.node = c1;
        generate_node(target);

        switch (*op) {
            case '-':
                printf("\tnegq %s\n", target.target_destination);
                break;
            case '~':
                printf("\tnotq %s\n", target.target_destination);
                break;
        }
        return;
    }

    node_t *c2 = target.node->children[1];

    // For all calls here we disallow the return statement so we can pass a null pointer
    struct compilation_target_t child_target = {
        .node = c2,
        .function = target.function,
        .stack_alignment = target.stack_alignment,
        .returned = NULL,
        .target_destination = "%rax",
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    generate_node(child_target);
    *target.stack_alignment += 8;
    puts("\tpushq %rax");  // Store temporary value

    child_target.node = c1;

    generate_node(child_target);
    *target.stack_alignment -= 8;
    puts("\tpopq %r10");  // Retrieve previously calculated value

    // Now have lh side in rax and rh side in r8

    switch (*op) {
        case '|':
            puts("\torq %r10, %rax");
            break;
        case '^':
            puts("\txorq %r10, %rax");
            break;
        case '&':
            puts("\tandq %r10, %rax");
            break;
        case '+':
            puts("\taddq %r10, %rax");
            break;
        case '-':
            puts("\tsubq %r10, %rax");
            break;
        case '*':
            puts("\timulq %r10");
            break;
        case '/':
            // Extends sign so that it is rdx:rax. This is required by idivq
            puts("\tcqto");
            puts("\tidivq %r10");
            break;
    }

    // A lot of the above ops do support giving them a memory address directly,
    // but to keep the compiler a bit simpler (because some of them don't),
    // we're doing it in a separate instruction
    if (strncmp("%rax", target.target_destination, 4)) {
        printf("\tmovq %%rax, %s\n", target.target_destination);
    }
}

static void generate_conditional(struct compilation_target_t target) {
    node_t *relation = target.node;

    char *type = (char *)relation->data;
    node_t *lh_expr = relation->children[0];
    node_t *rh_expr = relation->children[1];

    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = NULL,
        .stack_alignment = target.stack_alignment,
        .target_destination = "%rax",
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    child_target.node = lh_expr;
    generate_node(child_target);
    *target.stack_alignment += 8;
    puts("\tpushq %rax");

    child_target.target_destination = "%r11";
    child_target.node = rh_expr;
    generate_node(child_target);
    *target.stack_alignment -= 8;
    puts("\tpopq %r10");
    puts("\tcmp %r11, %r10");
}

static void access_variable(const char *reg, symbol_t *sym, symbol_t *function) {
    switch (sym->type) {
        case SYM_GLOBAL_VAR:
            move_global_to_reg(reg, sym->name);
            break;
        case SYM_LOCAL_VAR:
        case SYM_PARAMETER:
            move_slot_to_reg(reg, get_slot(function, sym));
            break;
        default:
            fprintf(stderr, "Unsupported symbol type for identifier data \"%s\"\n", sym->name);
            exit(EXIT_FAILURE);
    }
}

static void write_variable(const char *reg, symbol_t *sym, symbol_t *function) {
    switch (sym->type) {
        case SYM_GLOBAL_VAR:
            move_reg_to_global(reg, sym->name);
            break;
        case SYM_LOCAL_VAR:
        case SYM_PARAMETER:
            move_reg_to_slot(reg, get_slot(function, sym));
            break;
        default:
            fprintf(stderr, "Unsupported symbol type for identifier data \"%s\"\n", sym->name);
            exit(EXIT_FAILURE);
    }
}

static void write_variable_accessor(char *buf, size_t bufsize, symbol_t *sym, symbol_t *function) {
    switch (sym->type) {
        case SYM_GLOBAL_VAR:
            snprintf(buf, bufsize, ".%s", sym->name);
            break;
        case SYM_LOCAL_VAR:
        case SYM_PARAMETER:
            snprintf(buf, bufsize, "%d(%%rbp)", (get_slot(function, sym) + 1) * -8);
            break;
        default:
            fprintf(stderr, "Unsupported symbol type for identifier data \"%s\"\n", sym->name);
            exit(EXIT_FAILURE);
    }
}

static void skip_jump_by_relation(char relation, char *label) {
    switch (relation) {
        case '=':
            printf("\tjne %s\n", label);
            break;
        case '>':
            printf("\tjng %s\n", label);
            break;
        case '<':
            printf("\tjnl %s\n", label);
            break;
        default:
            fprintf(stderr, "Unknown relation operator %c\n", relation);
            break;
    }
}

static void generate_if_statement(struct compilation_target_t target) {
    node_t *relation;

    char first_skip_label[LABEL_MAX_SIZE] = {0};
    char control_end_buffer[LABEL_MAX_SIZE] = {0};
    bool local_return = false;

    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = &local_return,
        .stack_alignment = target.stack_alignment,
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    relation = target.node->children[0];
    child_target.node = relation;
    generate_conditional(child_target);

    bool has_else = target.node->n_children == 3;
    make_label(first_skip_label, LABEL_MAX_SIZE, has_else ? "ELSE" : "ENDIF", child_target);

    skip_jump_by_relation(*((char *)relation->data), first_skip_label);

    child_target.node = target.node->children[1];
    generate_node(child_target);

    if (has_else) {
        child_target.node = target.node->children[2];
        make_label(control_end_buffer, LABEL_MAX_SIZE, "ENDIF", child_target);
        printf("\tjmp %s\n", control_end_buffer);
    }

    label_here(first_skip_label);

    if (has_else) {
        generate_node(child_target);
        label_here(control_end_buffer);
    }

    // Increase for next use so that each control structure has its own "ID"
    (*target.label_mangle_index)++;
}

static void generate_while_statement(struct compilation_target_t target) {
    char check_label[LABEL_MAX_SIZE] = {0};
    char end_label[LABEL_MAX_SIZE] = {0};
    bool local_return = false;

    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = &local_return,
        .stack_alignment = target.stack_alignment,
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    make_label(check_label, LABEL_MAX_SIZE, "WCHECK", child_target);
    make_label(end_label, LABEL_MAX_SIZE, "WEND", child_target);
    label_here(check_label);

    node_t *relation = target.node->children[0];
    node_t *body = target.node->children[1];

    child_target.node = relation;
    generate_conditional(child_target);
    skip_jump_by_relation(*((char *)relation->data), end_label);

    child_target.surrounding_loop_label = check_label;
    child_target.node = body;
    generate_node(child_target);
    printf("\tjmp %s\n", check_label);
    label_here(end_label);

    // Increase for next use so that each control structure has its own "ID"
    (*target.label_mangle_index)++;
}

static void generate_assignment(struct compilation_target_t target) {
    node_t *var = target.node->children[0];
    node_t *value = target.node->children[1];

    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = target.returned,
        .stack_alignment = target.stack_alignment,
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    char variable_accessor[64] = {0};

    if (target.node->type == ASSIGNMENT_STATEMENT) {
        write_variable_accessor(variable_accessor, 64, var->entry, target.function);
        child_target.target_destination = "%rax";
        child_target.node = value;

        generate_node(child_target);
        printf("\tmovq %rax, %s\n", variable_accessor);
        return;
    }

    child_target.target_destination = "%r10";
    child_target.node = value;

    // This will find whatever expression we need and put it in %r10
    generate_node(child_target);
    access_variable("%rax", var->entry, target.function);

    switch (target.node->type) {
        case ADD_STATEMENT:
            puts("\taddq %r10, %rax");
            break;
        case SUBTRACT_STATEMENT:
            puts("\tsubq %r10, %rax");
            break;
        case DIVIDE_STATEMENT:
            // Extends sign so that it is rdx:rax. This is required by idivq
            puts("\tcqto");
            puts("\tidivq %r10");
            break;
        case MULTIPLY_STATEMENT:
            puts("\timulq %r10");
            break;
    }

    write_variable("%rax", var->entry, target.function);
}

static void genereate_number_data(struct compilation_target_t target) {
    int64_t value = *((int64_t *)target.node->data);
    printf("\tmovq $%ld, %s\n", value, target.target_destination);
}

static void generate_print_statement(struct compilation_target_t target) {
    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = target.returned,
        .stack_alignment = target.stack_alignment,
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    node_t *item;
    unsigned int alignment;

    for (size_t i = 0; i < target.node->n_children; i++) {
        item = target.node->children[i];

        switch (item->type) {
            case STRING_DATA:
                printf("\tmovq $.strout, %%rdi\n");
                printf("\tmovq $.STR%ld, %%rsi\n", *((size_t *)item->data));
                break;
            case IDENTIFIER_DATA:
                printf("\tmovq $.intout, %%rdi\n");
                access_variable("%rsi", item->entry, target.function);
                break;
            case EXPRESSION:
                child_target.node = item;
                child_target.target_destination = "%rsi";

                generate_node(child_target);
                printf("\tmovq $.intout, %%rdi\n");
                break;
        }

        // We align at every print call because expressions etc.
        // may push variables on the stack, causing an alignment
        // for this as a whole to not work real well
        alignment = align_stack(target.stack_alignment);
        puts("\tcall printf");
        unalign_stack(alignment, target.stack_alignment);
    }

    // New line
    printf("\tmovq $.newline, %%rdi\n");
    alignment = align_stack(target.stack_alignment);
    puts("\tcall printf");
    unalign_stack(alignment, target.stack_alignment);
}

static void generate_return_statement(struct compilation_target_t target) {
    if (target.returned == NULL) {
        fprintf(stderr, "Return in illegal position inside %s\n", target.function->name);
        exit(EXIT_FAILURE);
    }

    *target.returned = true;

    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = target.returned,
        .stack_alignment = target.stack_alignment,
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label,
        .node = target.node->children[0],
        .target_destination = "%rax"};

    generate_node(child_target);
    puts("\tleave");
    puts("\tret");
}

void generate_node(struct compilation_target_t target) {
    switch (target.node->type) {
        case IF_STATEMENT:
            generate_if_statement(target);
            return;
        case WHILE_STATEMENT:
            generate_while_statement(target);
            return;
        case NULL_STATEMENT:
            if (target.surrounding_loop_label == NULL) {
                fprintf(stderr, "Continue in illegal position inside %s\n", target.function->name);
                exit(EXIT_FAILURE);
            }

            printf("\tjmp %s\n", target.surrounding_loop_label);
            return;
        case EXPRESSION:
            generate_expression(target);
            return;
        case IDENTIFIER_DATA:
            // This assumes the case where we want to access the value in the
            // referenced variable. Assignments are handled separately.
            access_variable(target.target_destination, target.node->entry, target.function);
            return;
        case NUMBER_DATA:
            genereate_number_data(target);
            return;
        case ASSIGNMENT_STATEMENT:
        case ADD_STATEMENT:
        case SUBTRACT_STATEMENT:
        case DIVIDE_STATEMENT:
        case MULTIPLY_STATEMENT:
            generate_assignment(target);
            return;
        case PRINT_STATEMENT:
            generate_print_statement(target);
            return;
        case RETURN_STATEMENT:
            generate_return_statement(target);
            return;
    }

    // A return statement has been called, we don't need to continue here
    if (target.returned != NULL && *target.returned) {
        return;
    }

    struct compilation_target_t child_target = {
        .function = target.function,
        .returned = target.returned,
        .stack_alignment = target.stack_alignment,
        .label_mangle_index = target.label_mangle_index,
        .surrounding_loop_label = target.surrounding_loop_label};

    node_t *child;
    for (size_t i = 0; i < target.node->n_children; i++) {
        child = target.node->children[i];
        if (child->type == DECLARATION) {
            continue;
        }

        child_target.node = child;
        child_target.target_destination = target.target_destination;

        generate_node(child_target);
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

    unsigned int alignment = align_stack(&stack_alignment);
    printf("\tcall %s%s\n", FUNC_PREFIX, first->name);
    unalign_stack(alignment, &stack_alignment);

    printf("\tjmp\tEND\n");
    printf("ABORT:\n");
    printf("\tmovq\t$.errout, %%rdi\n");
    printf("\tcall puts\n");

    printf("END:\n");
    puts("\tmovq    %rax, %rdi");
    puts("\tcall    exit");
}
