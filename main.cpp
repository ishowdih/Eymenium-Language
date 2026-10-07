// main.cpp -- CLI entry point for the native Eymenium interpreter.

#include "eymenium.hpp"
#include <iostream>
#include <fstream>
#include <sstream>

int main(int argc, char** argv) {
    if (argc < 2) {
        Interpreter interp;
        interp.runRepl();
        return 0;
    }

    std::string path = argv[1];
    std::ifstream f(path);
    if (!f) {
        std::cerr << "eym: no such file: " << path << std::endl;
        return 1;
    }
    std::stringstream buf;
    buf << f.rdbuf();
    std::string source = buf.str();

    try {
        auto tokens = lex(source, path);
        auto program = parseProgram(tokens, path);
        Interpreter interp;
        interp.run(program.get(), path);
    } catch (EymeniumFatal& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
    return 0;
}
