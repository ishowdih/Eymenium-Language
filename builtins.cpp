// builtins.cpp -- native functions pre-bound into the global scope.
//
// These are NOT keywords (see the design note in eym's docs): they're
// ordinary names bound to a callable before user code runs, exactly like
// `out`/`getuser`/`cmd` in the earlier Python-hosted prototype -- except
// here `cmd` genuinely needs no host runtime to piggyback on, since this
// whole interpreter is already native code.

#include "eymenium.hpp"
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <sstream>

static Value V() { return Value(); }

static double numArg(std::vector<Value>& args, size_t i, const std::string& fname, Interpreter* /*unused*/) {
    if (i >= args.size()) throw EymeniumThrow{}; // guarded by arity checks below; unreachable in practice
    if (std::holds_alternative<long long>(args[i].v)) return (double)std::get<long long>(args[i].v);
    if (std::holds_alternative<double>(args[i].v)) return std::get<double>(args[i].v);
    throw std::runtime_error(fname + ": expected a number");
}

void Interpreter::registerBuiltins() {
    auto def = [&](const std::string& name, NativeFn fn) {
        globals->define(name, Value(std::make_shared<NativeFn>(fn)));
    };

    def("out", [](std::vector<Value>& args) -> Value {
        for (size_t i = 0; i < args.size(); i++) {
            if (i) std::cout << " ";
            std::cout << valueToString(args[i], false);
        }
        std::cout << "\n";
        return V();
    });

    def("getuser", [](std::vector<Value>& args) -> Value {
        if (!args.empty()) std::cout << valueToString(args[0], false) << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) throw EymeniumThrow{}; // caller sees a generic error; acceptable for EOF-on-input edge case
        return Value(line);
    });

    def("cmd", [this](std::vector<Value>& args) -> Value {
        if (args.empty() || !std::holds_alternative<std::string>(args[0].v)) throwError("ArgumentError", "cmd(command) expects a string", 0);
        int rc = std::system(std::get<std::string>(args[0].v).c_str());
        return Value((long long)rc);
    });

    def("sizeof", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "sizeof(x) needs an argument", 0);
        auto& a = args[0];
        if (std::holds_alternative<std::string>(a.v)) return Value((long long)std::get<std::string>(a.v).size());
        if (std::holds_alternative<ListVal>(a.v)) return Value((long long)std::get<ListVal>(a.v)->size());
        if (std::holds_alternative<DictVal>(a.v)) return Value((long long)std::get<DictVal>(a.v)->size());
        throwError("TypeMismatchError", "sizeof() doesn't apply to a " + valueKind(a), 0);
    });

    def("kind", [](std::vector<Value>& args) -> Value {
        return Value(args.empty() ? std::string("null") : valueKind(args[0]));
    });

    def("grab", [this](std::vector<Value>& args) -> Value {
        if (args.size() < 2) throwError("ArgumentError", "grab(obj, name) needs 2 arguments", 0);
        std::string name = valueToString(args[1], false);
        try {
            return getAttr(args[0], name, 0);
        } catch (EymeniumThrow&) {
            if (args.size() >= 3) return args[2];
            throw;
        }
    });

    def("put", [this](std::vector<Value>& args) -> Value {
        if (args.size() < 3) throwError("ArgumentError", "put(obj, name, value) needs 3 arguments", 0);
        setAttr(args[0], valueToString(args[1], false), args[2], 0);
        return V();
    });

    def("has", [this](std::vector<Value>& args) -> Value {
        if (args.size() < 2) throwError("ArgumentError", "has(obj, name) needs 2 arguments", 0);
        try { getAttr(args[0], valueToString(args[1], false), 0); return Value(true); }
        catch (EymeniumThrow&) { return Value(false); }
    });

    def("nums", [this](std::vector<Value>& args) -> Value {
        long long start = 0, stop = 0, step = 1;
        auto asInt = [&](Value& v) -> long long {
            if (std::holds_alternative<long long>(v.v)) return std::get<long long>(v.v);
            throwError("ArgumentError", "nums() arguments must be ints", 0);
        };
        if (args.size() == 1) { stop = asInt(args[0]); }
        else if (args.size() == 2) { start = asInt(args[0]); stop = asInt(args[1]); }
        else if (args.size() >= 3) { start = asInt(args[0]); stop = asInt(args[1]); step = asInt(args[2]); }
        else throwError("ArgumentError", "nums() needs 1 to 3 arguments", 0);
        if (step == 0) throwError("ArgumentError", "nums() step cannot be zero", 0);
        return Value(RangeVal{start, stop, step});
    });

    // ---- list / dict helpers (no dot-method syntax on builtins in v1, so
    // these are free functions -- see README "design choices") ----

    def("append", [this](std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !std::holds_alternative<ListVal>(args[0].v)) throwError("ArgumentError", "append(list, value) expects a list", 0);
        std::get<ListVal>(args[0].v)->push_back(args[1]);
        return V();
    });
    def("pop", [this](std::vector<Value>& args) -> Value {
        if (args.empty() || !std::holds_alternative<ListVal>(args[0].v)) throwError("ArgumentError", "pop(list) expects a list", 0);
        auto& l = std::get<ListVal>(args[0].v);
        if (l->empty()) throwError("IndexError", "pop from an empty list", 0);
        Value v = l->back();
        l->pop_back();
        return v;
    });
    def("keys", [this](std::vector<Value>& args) -> Value {
        if (args.empty() || !std::holds_alternative<DictVal>(args[0].v)) throwError("ArgumentError", "keys(dict) expects a dict", 0);
        auto out = std::make_shared<std::vector<Value>>();
        for (auto& [k, v] : *std::get<DictVal>(args[0].v)) out->push_back(Value(k));
        return Value(out);
    });
    def("values", [this](std::vector<Value>& args) -> Value {
        if (args.empty() || !std::holds_alternative<DictVal>(args[0].v)) throwError("ArgumentError", "values(dict) expects a dict", 0);
        auto out = std::make_shared<std::vector<Value>>();
        for (auto& [k, v] : *std::get<DictVal>(args[0].v)) out->push_back(v);
        return Value(out);
    });
    def("joinstr", [this](std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !std::holds_alternative<std::string>(args[0].v) || !std::holds_alternative<ListVal>(args[1].v))
            throwError("ArgumentError", "joinstr(sep, list) expects a string and a list", 0);
        std::string sep = std::get<std::string>(args[0].v);
        auto& l = std::get<ListVal>(args[1].v);
        std::string out;
        for (size_t i = 0; i < l->size(); i++) { if (i) out += sep; out += valueToString((*l)[i], false); }
        return Value(out);
    });
    def("splitstr", [this](std::vector<Value>& args) -> Value {
        if (args.size() < 2 || !std::holds_alternative<std::string>(args[0].v) || !std::holds_alternative<std::string>(args[1].v))
            throwError("ArgumentError", "splitstr(string, sep) expects two strings", 0);
        std::string s = std::get<std::string>(args[0].v), sep = std::get<std::string>(args[1].v);
        auto out = std::make_shared<std::vector<Value>>();
        if (sep.empty()) { out->push_back(Value(s)); return Value(out); }
        size_t pos = 0, found;
        while ((found = s.find(sep, pos)) != std::string::npos) {
            out->push_back(Value(s.substr(pos, found - pos)));
            pos = found + sep.size();
        }
        out->push_back(Value(s.substr(pos)));
        return Value(out);
    });

    // ---- math / conversion (no import needed, same philosophy as `cmd`) ----

    def("sqrt", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "sqrt(x) needs an argument", 0);
        return Value(std::sqrt(numArg(args, 0, "sqrt", this)));
    });
    def("floor", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "floor(x) needs an argument", 0);
        return Value((long long)std::floor(numArg(args, 0, "floor", this)));
    });
    def("ceil", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "ceil(x) needs an argument", 0);
        return Value((long long)std::ceil(numArg(args, 0, "ceil", this)));
    });
    def("absval", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "absval(x) needs an argument", 0);
        auto& a = args[0];
        if (std::holds_alternative<long long>(a.v)) return Value(std::llabs(std::get<long long>(a.v)));
        return Value(std::fabs(numArg(args, 0, "absval", this)));
    });
    def("roundval", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "roundval(x) needs an argument", 0);
        return Value((long long)std::llround(numArg(args, 0, "roundval", this)));
    });
    def("intval", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "intval(x) needs an argument", 0);
        auto& a = args[0];
        if (std::holds_alternative<long long>(a.v)) return a;
        if (std::holds_alternative<double>(a.v)) return Value((long long)std::get<double>(a.v));
        if (std::holds_alternative<bool>(a.v)) return Value((long long)(std::get<bool>(a.v) ? 1 : 0));
        if (std::holds_alternative<std::string>(a.v)) {
            try { return Value((long long)std::stoll(std::get<std::string>(a.v))); }
            catch (...) { throwError("ValueError", "cannot convert '" + std::get<std::string>(a.v) + "' to an int", 0); }
        }
        throwError("TypeMismatchError", "cannot convert a " + valueKind(a) + " to an int", 0);
    });
    def("floatval", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "floatval(x) needs an argument", 0);
        auto& a = args[0];
        if (std::holds_alternative<double>(a.v)) return a;
        if (std::holds_alternative<long long>(a.v)) return Value((double)std::get<long long>(a.v));
        if (std::holds_alternative<std::string>(a.v)) {
            try { return Value(std::stod(std::get<std::string>(a.v))); }
            catch (...) { throwError("ValueError", "cannot convert '" + std::get<std::string>(a.v) + "' to a float", 0); }
        }
        throwError("TypeMismatchError", "cannot convert a " + valueKind(a) + " to a float", 0);
    });
    def("strval", [](std::vector<Value>& args) -> Value {
        return Value(args.empty() ? std::string("null") : valueToString(args[0], false));
    });
    def("classname", [this](std::vector<Value>& args) -> Value {
        if (args.empty()) throwError("ArgumentError", "classname(x) needs an argument", 0);
        if (std::holds_alternative<InstVal>(args[0].v)) return Value(std::get<InstVal>(args[0].v)->cls->name);
        if (std::holds_alternative<ClassVal>(args[0].v)) return Value(std::get<ClassVal>(args[0].v)->name);
        throwError("TypeMismatchError", "classname() expects an instance or blueprint", 0);
    });
}
