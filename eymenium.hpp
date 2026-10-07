// eymenium.hpp -- shared declarations for the Eymenium native interpreter.
//
// Design notes (for the reader coming from a compiler background):
//
// * AST nodes use one "universal" Node struct with a Kind tag rather than a
//   class hierarchy + visitor pattern. This trades some type safety for a
//   much smaller amount of boilerplate, which matters for a hand-rolled
//   interpreter of this size. Interpretation is a big switch over Kind.
//
// * The lexer does NOT have a token kind per keyword. Keywords are just
//   IDENT tokens whose text happens to match a reserved word; the parser
//   checks token text directly. This keeps the token enum tiny and avoids
//   a giant keyword-to-tokenkind table.
//
// * Values are a tagged variant (std::variant). Compound types (list, dict,
//   instance, function) are stored behind shared_ptr so that assignment
//   copies a *reference*, matching Python/Eymenium's "everything is an
//   object reference" semantics -- mutating a list through one variable is
//   visible through an alias, exactly like the language you're used to.
//
// * Control flow (return/break/continue/throw) is implemented with C++
//   exceptions internal to the interpreter. This is a completely standard
//   technique for tree-walking interpreters and keeps eval() from needing
//   an explicit "signal" return channel threaded through every call.

#pragma once
#include <string>
#include <vector>
#include <memory>
#include <variant>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <stdexcept>

// ============================================================================
// Lexer
// ============================================================================

enum class TokKind { Ident, Number, String, Op, Newline, Indent, Dedent, End };

struct Token {
    TokKind kind;
    std::string text;      // raw text: identifier name, operator symbol, or unescaped string content
    bool isFloat = false;  // for Number tokens
    long long ival = 0;
    double fval = 0.0;
    int line = 0;
};

std::vector<Token> lex(const std::string& source, const std::string& filename);

// ============================================================================
// AST
// ============================================================================

enum class NodeKind {
    Program, Block,
    NumLit, StrLit, BoolLit, NullLit, Ident,
    ListLit, DictLit,
    BinOp, UnaryOp,
    Assign,
    Index, Attr, Call,
    If, While, For,
    FuncDef, Lambda, Return, Break, Continue, Pass,
    ClassDef,
    Try, Throw,
    Import, Global, Nonlocal, Del, Assert,
    ListComp,
    ExprStmt
};

struct Node;
using NodePtr = std::unique_ptr<Node>;

struct CatchClause {
    bool hasType = false;
    std::string typeName;
    bool hasBind = false;
    std::string bindName;
    NodePtr body;
};

struct Node {
    NodeKind kind;
    int line = 0;

    // generic payload fields, meaning depends on `kind` (see comments at each
    // construction site in parser.cpp for exactly which fields are used)
    std::string str;                       // identifier text / operator text / string literal / attr name / class name / import name
    bool isFloat = false;
    long long ival = 0;
    double fval = 0.0;
    bool bval = false;

    std::vector<NodePtr> children;         // general-purpose child list
    std::vector<std::string> names;        // GLOBAL/NONLOCAL name lists
    std::vector<std::string> paramNames;   // FuncDef/Lambda parameter names
    std::vector<NodePtr> paramDefaults;    // parallel to paramNames; nullptr = no default
    std::vector<CatchClause> catches;      // Try node's catch clauses
    NodePtr finallyBlock;                  // Try node's optional finally block (or null)

    Node(NodeKind k, int ln) : kind(k), line(ln) {}
};

NodePtr parseProgram(std::vector<Token>& tokens, const std::string& filename);

// ============================================================================
// Runtime values
// ============================================================================

struct Environment;
struct FunctionValue;
struct ClassValue;
struct InstanceValue;
struct Value;

using EnvPtr = std::shared_ptr<Environment>;
using ListVal = std::shared_ptr<std::vector<Value>>;
using DictVal = std::shared_ptr<std::unordered_map<std::string, Value>>; // string-keyed dict (documented simplification)
using FuncVal = std::shared_ptr<FunctionValue>;
using ClassVal = std::shared_ptr<ClassValue>;
using InstVal = std::shared_ptr<InstanceValue>;
using NativeFn = std::function<Value(std::vector<Value>&)>;

struct RangeVal {
    long long start, stop, step;
};

struct Value {
    std::variant<
        std::monostate,   // null
        bool,
        long long,
        double,
        std::string,
        ListVal,
        DictVal,
        FuncVal,
        ClassVal,
        InstVal,
        std::shared_ptr<NativeFn>,
        RangeVal
    > v;

    Value() : v(std::monostate{}) {}
    Value(std::monostate) : v(std::monostate{}) {}
    Value(bool b) : v(b) {}
    Value(long long i) : v(i) {}
    Value(double d) : v(d) {}
    Value(const std::string& s) : v(s) {}
    Value(ListVal l) : v(l) {}
    Value(DictVal d) : v(d) {}
    Value(FuncVal f) : v(f) {}
    Value(ClassVal c) : v(c) {}
    Value(InstVal i) : v(i) {}
    Value(std::shared_ptr<NativeFn> n) : v(n) {}
    Value(RangeVal r) : v(r) {}
};

struct Environment : std::enable_shared_from_this<Environment> {
    std::unordered_map<std::string, Value> vars;
    EnvPtr parent;
    // names declared `glob`/`nloc` in THIS function scope (see interpreter.cpp
    // assignTo() -- plain assignment defaults to local-only, Python-style,
    // and these sets are what let glob/nloc redirect a write elsewhere)
    std::unordered_set<std::string> globalDecls;
    std::unordered_set<std::string> nonlocalDecls;
    explicit Environment(EnvPtr p = nullptr) : parent(p) {}

    bool getLocal(const std::string& name, Value& out) {
        auto it = vars.find(name);
        if (it == vars.end()) return false;
        out = it->second;
        return true;
    }
    // walks up the closure chain
    bool get(const std::string& name, Value& out) {
        Environment* e = this;
        while (e) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) { out = it->second; return true; }
            e = e->parent.get();
        }
        return false;
    }
    void define(const std::string& name, const Value& val) { vars[name] = val; }
    // assigns to the nearest enclosing scope that already defines `name`;
    // if none does, defines it locally (this is how a bare `x = 1` behaves)
    void assign(const std::string& name, const Value& val) {
        Environment* e = this;
        while (e) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) { it->second = val; return; }
            e = e->parent.get();
        }
        vars[name] = val;
    }
};

struct FunctionValue {
    std::string name;
    std::vector<std::string> paramNames;
    std::vector<Node*> paramDefaults;  // raw pointers into the owning AST (AST outlives all execution)
    Node* body = nullptr;              // Block node (statement body) OR expr node (lambda)
    bool isLambda = false;
    EnvPtr closure;
};

struct ClassValue {
    std::string name;
    ClassVal parent;  // nullptr if no base class
    std::unordered_map<std::string, FuncVal> methods;
    std::unordered_map<std::string, Value> classVars;

    bool findMethod(const std::string& n, FuncVal& out) {
        ClassValue* c = this;
        while (c) {
            auto it = c->methods.find(n);
            if (it != c->methods.end()) { out = it->second; return true; }
            c = c->parent.get();
        }
        return false;
    }
    bool isOrInherits(const std::string& n) {
        ClassValue* c = this;
        while (c) {
            if (c->name == n) return true;
            c = c->parent.get();
        }
        return false;
    }
};

struct InstanceValue {
    ClassVal cls;
    std::unordered_map<std::string, Value> fields;
};

// thrown for `throw`, and for internal runtime errors (division by zero,
// undefined name, wrong arg count, ...) which are represented as instances
// of pre-registered built-in "error classes" -- see interpreter.cpp
struct EymeniumThrow { Value payload; };

struct ReturnSignal { Value value; };
struct BreakSignal {};
struct ContinueSignal {};

// raised for things that should stop the whole program (parse errors,
// uncaught throws reaching top level, etc.)
struct EymeniumFatal : std::runtime_error {
    explicit EymeniumFatal(const std::string& msg) : std::runtime_error(msg) {}
};

class Interpreter {
public:
    Interpreter();
    void run(Node* program, const std::string& filename);
    void runRepl();

private:
    EnvPtr globals;
    ClassVal errType(const std::string& name);
    std::unordered_map<std::string, ClassVal> builtinErrorClasses;

    Value eval(Node* n, EnvPtr env);
    void exec(Node* n, EnvPtr env);
    void execBlock(Node* block, EnvPtr env);

    Value callFunction(FuncVal fn, std::vector<Value>& args, InstVal boundSelf);
    Value callValue(Value callee, std::vector<Value>& args, int line);

    Value getAttr(Value obj, const std::string& name, int line);
    void setAttr(Value obj, const std::string& name, const Value& val, int line);
    Value indexGet(Value obj, Value idx, int line);
    void indexSet(Value obj, Value idx, const Value& val, int line);
    Value binaryOpDispatch(const std::string& op, Value l, Value r, int line);

    void assignTo(Node* target, const Value& val, EnvPtr env);

    [[noreturn]] void throwError(const std::string& type, const std::string& msg, int line);

    void registerBuiltins();
    void loadModule(const std::string& moduleName, EnvPtr env, int line);
    std::vector<std::string> loadedModules;
    std::string baseDir;

    friend std::string valueToString(const Value& v, bool repr);
};

std::string valueToString(const Value& v, bool repr = false);
std::string valueKind(const Value& v);
bool valuesEqual(const Value& a, const Value& b);
bool isTruthy(const Value& v);
