#include "LangLexer.h"
#include "LangParser.h"
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>
#include <deque>
#include <fstream>
#include <filesystem>
#include "antlr4-runtime.h"
#include "AST.h"
#include "ASTBuilder.h"

using namespace antlr4;
using namespace mysym;

using Memory = std::map<std::string, std::string>;


class State {
  public:
    Memory memory;
    std::vector<std::string> pc;
    std::string result;
    std::deque<Statement*> next_statements;
    int statement_id;

    State(
        Memory memory, std::vector<std::string> pc, std::string result, std::deque<Statement*> next_statements, int statement_id = 0
    ): memory(memory), pc(pc), result(result), next_statements(next_statements), statement_id(statement_id) {};

    std::string to_string() {
        std::string pc_all = "";
        if (pc.size()) {
            pc_all += pc[0];
        }
        if (pc.size() > 1) {
            pc_all = "(" + pc[0] + ")";
        }
        for (int i = 1; i < pc.size(); ++i) {
            pc_all += " & (" + pc[i] + ")";
        }
        std::string memory_str = "";
        for (auto it = memory.begin(); it != memory.cend(); it++) {       
            memory_str += "\t" + it->first + " = " + it->second + "\n";
        }
        return (
            "{\n"
            + memory_str
            + "\tpc = " + pc_all + "\n\t"
            + "result = " + result + "\n"
            + "}"
        );
    }
};


std::string process_expression(Memory *memory, Expression *expression_ptr) {
    if (auto* var_ref = dynamic_cast<VarRef*>(expression_ptr)) {
        return (*memory)[var_ref->identifier];
    } else if (auto* int_constant = dynamic_cast<IntConstant*>(expression_ptr)) {
        return std::to_string(int_constant->value);
    } else if (auto* bool_constant = dynamic_cast<BoolConstant*>(expression_ptr)) {
        return std::to_string(bool_constant->value);
    } else if (auto* un_op = dynamic_cast<UnOp*>(expression_ptr)) {
        return "-(" + process_expression(memory, (un_op->subExpr).get()) + ")";
    } else if (auto* bin_op = dynamic_cast<BinOp*>(expression_ptr)) {
        std::map<BinOpKind, std::string> bin_op_kind_to_str{
            {BO_Add, "+"}, {BO_Sub, "-"}, {BO_Lt, "<"}, {BO_Gt, ">"}, {BO_LAnd, "&"}, {BO_LOr, "|"}
        };
        return (
            process_expression(memory, bin_op->lhs.get())
            + " " + bin_op_kind_to_str[bin_op->kind] + " "
            + process_expression(memory, bin_op->rhs.get())
        );
    } else {
        throw std::invalid_argument("Incorrect expression!");
    }
}

std::vector<State> process_step(State state, Statement *ptr) {
    if (state.next_statements.size()) {
        state.next_statements.pop_front();
    } else {
        state.statement_id += 1;
    }
    if (auto* assignment = dynamic_cast<Assignment*>(ptr)) {
        state.memory[assignment->var] = "(" + process_expression(&state.memory, assignment->value.get()) + ")";
        return {state};
    }
    else if (auto* if_stmt = dynamic_cast<IfStmt*>(ptr)) {
        Memory memory_copy;
        memory_copy.insert(state.memory.begin(), state.memory.end());
        State state_copy = State(memory_copy, state.pc, state.result, state.next_statements, state.statement_id);
        std::string condition_str = process_expression(&state.memory, if_stmt->condition.get());
        state.pc.push_back(condition_str);
        for (auto it = if_stmt->thenBlock.rbegin(); it != if_stmt->thenBlock.rend(); ++it) {
            state.next_statements.push_front((*it).get());
        }
        state_copy.pc.push_back("!(" + condition_str + ")");
        for (auto it = if_stmt->elseBlock.rbegin(); it != if_stmt->elseBlock.rend(); ++it) {
            state_copy.next_statements.push_front((*it).get());
        }
        return {state, state_copy};
    }
    else {
        throw std::invalid_argument("Incorrect statement type!");
    }
}


std::vector<State> runner(Function *func) {
    Memory memory = Memory();
    for (Parameter p: func->parameters) {
        memory[p.name] = "'" + p.name + "'";
    }
    std::vector<Statement*> statements_ptr({});
    for (auto it: func->body) {
        statements_ptr.push_back(it.get());
    }
    Expression *return_expression_ptr = func->returnValue.get();

    State start_state = State(memory, std::vector<std::string>(), std::string(), std::deque<Statement*>());
    std::vector<State> final_states({});
    std::deque<State> curr_states({start_state});

    while (curr_states.size()) {
        State state = curr_states.front();
        curr_states.pop_front();
        Statement *statement_ptr;
        if (state.next_statements.size()) {
            statement_ptr = state.next_statements.front();
        } else if (state.statement_id < statements_ptr.size()) {
            statement_ptr = statements_ptr[state.statement_id];
        } else {
            final_states.push_back(state);
            continue;
        }
        std::vector<State> new_states = process_step(state, statement_ptr);
        for (auto it = new_states.rbegin(); it != new_states.rend(); ++it) {
            curr_states.push_front(*it);
        }
    }
    for (int i = 0; i < final_states.size(); ++i) {
        State state = final_states[i];
        final_states[i].result = process_expression(&state.memory, return_expression_ptr);
    }
    return final_states;
}


int main(int argc, const char **argv) {
    if (argc <= 2) {
        std::cerr << "Not enough arguments. Please enter input and output file paths\n";
        exit(1);
    }
    std::filesystem::path path(argv[1]);
    {
        std::ifstream istream(path);
        ANTLRInputStream input(istream);
        LangLexer lexer(&input);
        CommonTokenStream tokens(&lexer);
        LangParser parser(&tokens);

        tree::ParseTree *tree = parser.function();

        if (lexer.getNumberOfSyntaxErrors() > 0 || parser.getNumberOfSyntaxErrors() > 0) {
            std::cerr << "fatal: encountered syntax errors\n";
            exit(1);
        }

        tree::ParseTreeWalker walker;

        std::shared_ptr<IASTBuilder> builder = IASTBuilder::create();
        std::shared_ptr<Function> ast;

        tree::ParseTreeWalker::DEFAULT.walk(builder.get(), tree);
        ast = builder->getFunction();

        Function *func = ast.get();
        std::vector<State> final_states = runner(func);

        std::ofstream f_out;
        f_out.open(argv[2]);
        if (f_out.is_open()) {
            for (State state: final_states) {
                f_out << state.to_string() << "\n";
            }
        }
        f_out.close();
    }
}
