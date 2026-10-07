// interpreter.cpp -- evaluates the AST produced by parser.cpp.
//
// Scoping model: only function calls, lambda calls, and class-body
// execution introduce a new Environment. if/while/for/try do NOT create
// new scopes (matching how Python's own if/while/for work). Plain
// assignment (`x = 1`) is LOCAL-ONLY by default -- it writes into the
// current function's own scope, creating the name there if needed, even
// if an outer scope already has a variable with that name. `glob`/`nloc`
// override this per-name, exactly mirroring Python's `global`/`nonlocal`.
// Reads always search the full closure chain, unaffected by any of this.

#include "eymenium.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <cstdlib>

// ============================================================================
// Value helpers
// ============================================================================

static std::string formatDouble(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d > 0 ? "inf" : "-inf";
    std::ostringstream ss;
    ss << d;
    std::string s = ss.str();
    // ensure it always reads as a float (e.g. "3" -> "3.0"), like Python
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos && s.find("nan") == std::string::npos) {
        s += ".0";
    }
    return s;
}

std::string valueKind(const Value& val) {
    return std::visit([](auto&& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "null";
        else if constexpr (std::is_same_v<T, bool>) return "bool";
        else if constexpr (std::is_same_v<T, long long>) return "int";
        else if constexpr (std::is_same_v<T, double>) return "float";
        else if constexpr (std::is_same_v<T, std::string>) return "string";
        else if constexpr (std::is_same_v<T, ListVal>) return "list";
        else if constexpr (std::is_same_v<T, DictVal>) return "dict";
        else if constexpr (std::is_same_v<T, FuncVal>) return "function";
        else if constexpr (std::is_same_v<T, ClassVal>) return "class";
        else if constexpr (std::is_same_v<T, InstVal>) return "instance";
        else if constexpr (std::is_same_v<T, std::shared_ptr<NativeFn>>) return "function";
        else if constexpr (std::is_same_v<T, RangeVal>) return "range";
        else return "unknown";
    }, val.v);
}

std::string valueToString(const Value& val, bool repr) {
    return std::visit([&](auto&& x) -> std::string {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return "null";
        else if constexpr (std::is_same_v<T, bool>) return x ? "yes" : "no";
        else if constexpr (std::is_same_v<T, long long>) return std::to_string(x);
        else if constexpr (std::is_same_v<T, double>) return formatDouble(x);
        else if constexpr (std::is_same_v<T, std::string>) {
            if (!repr) return x;
            std::string out = "\"";
            for (char c : x) { if (c == '"' || c == '\\') out += '\\'; out += c; }
            out += "\"";
            return out;
        }
        else if constexpr (std::is_same_v<T, ListVal>) {
            std::string out = "[";
            for (size_t i = 0; i < x->size(); i++) { if (i) out += ", "; out += valueToString((*x)[i], true); }
            out += "]";
            return out;
        }
        else if constexpr (std::is_same_v<T, DictVal>) {
            std::string out = "{";
            bool first = true;
            for (auto& [k, v] : *x) { if (!first) out += ", "; first = false; out += "\"" + k + "\": " + valueToString(v, true); }
            out += "}";
            return out;
        }
        else if constexpr (std::is_same_v<T, FuncVal>) return "<function " + x->name + ">";
        else if constexpr (std::is_same_v<T, ClassVal>) return "<blueprint " + x->name + ">";
        else if constexpr (std::is_same_v<T, InstVal>) return "<" + x->cls->name + " instance>";
        else if constexpr (std::is_same_v<T, std::shared_ptr<NativeFn>>) return "<native function>";
        else if constexpr (std::is_same_v<T, RangeVal>) return "<range>";
        else return "?";
    }, val.v);
}

bool isTruthy(const Value& val) {
    return std::visit([](auto&& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return false;
        else if constexpr (std::is_same_v<T, bool>) return x;
        else if constexpr (std::is_same_v<T, long long>) return x != 0;
        else if constexpr (std::is_same_v<T, double>) return x != 0.0;
        else if constexpr (std::is_same_v<T, std::string>) return !x.empty();
        else if constexpr (std::is_same_v<T, ListVal>) return !x->empty();
        else if constexpr (std::is_same_v<T, DictVal>) return !x->empty();
        else return true;
    }, val.v);
}

static bool isNumeric(const Value& v) { return std::holds_alternative<long long>(v.v) || std::holds_alternative<double>(v.v); }
static double toDouble(const Value& v) {
    if (std::holds_alternative<long long>(v.v)) return (double)std::get<long long>(v.v);
    return std::get<double>(v.v);
}

bool valuesEqual(const Value& a, const Value& b) {
    if (isNumeric(a) && isNumeric(b)) {
        if (std::holds_alternative<long long>(a.v) && std::holds_alternative<long long>(b.v))
            return std::get<long long>(a.v) == std::get<long long>(b.v);
        return toDouble(a) == toDouble(b);
    }
    if (a.v.index() != b.v.index()) return false;
    return std::visit([&](auto&& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, std::monostate>) return true;
        else if constexpr (std::is_same_v<T, bool>) return x == std::get<bool>(b.v);
        else if constexpr (std::is_same_v<T, std::string>) return x == std::get<std::string>(b.v);
        else if constexpr (std::is_same_v<T, ListVal>) {
            auto& other = std::get<ListVal>(b.v);
            if (x->size() != other->size()) return false;
            for (size_t i = 0; i < x->size(); i++) if (!valuesEqual((*x)[i], (*other)[i])) return false;
            return true;
        }
        else if constexpr (std::is_same_v<T, DictVal>) {
            auto& other = std::get<DictVal>(b.v);
            if (x->size() != other->size()) return false;
            for (auto& [k, v] : *x) {
                auto it = other->find(k);
                if (it == other->end() || !valuesEqual(v, it->second)) return false;
            }
            return true;
        }
        else return false; // functions/classes/instances: identity only (see 'same')
    }, a.v);
}

static bool identicalValues(const Value& a, const Value& b) {
    if (a.v.index() != b.v.index()) return false;
    return std::visit([&](auto&& x) -> bool {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<T, ListVal>) return x == std::get<ListVal>(b.v);
        else if constexpr (std::is_same_v<T, DictVal>) return x == std::get<DictVal>(b.v);
        else if constexpr (std::is_same_v<T, FuncVal>) return x == std::get<FuncVal>(b.v);
        else if constexpr (std::is_same_v<T, ClassVal>) return x == std::get<ClassVal>(b.v);
        else if constexpr (std::is_same_v<T, InstVal>) return x == std::get<InstVal>(b.v);
        else return valuesEqual(Value(x), b); // primitives: identity == equality
    }, a.v);
}

// ============================================================================
// Interpreter
// ============================================================================

Interpreter::Interpreter() {
    globals = std::make_shared<Environment>();
    registerBuiltins();
}

ClassVal Interpreter::errType(const std::string& name) {
    auto it = builtinErrorClasses.find(name);
    if (it != builtinErrorClasses.end()) return it->second;
    auto c = std::make_shared<ClassValue>();
    c->name = name;
    builtinErrorClasses[name] = c;
    return c;
}

void Interpreter::throwError(const std::string& type, const std::string& msg, int line) {
    auto inst = std::make_shared<InstanceValue>();
    inst->cls = errType(type);
    inst->fields["msg"] = Value(msg);
    inst->fields["line"] = Value((long long)line);
    throw EymeniumThrow{ Value(inst) };
}

// ---------------- iteration ----------------

static std::vector<Value> materializeIterable(Value v, Interpreter* interp, int line, std::function<void(const std::string&, const std::string&, int)> err) {
    std::vector<Value> items;
    if (std::holds_alternative<ListVal>(v.v)) {
        auto& l = std::get<ListVal>(v.v);
        items.assign(l->begin(), l->end());
    } else if (std::holds_alternative<DictVal>(v.v)) {
        for (auto& [k, val] : *std::get<DictVal>(v.v)) items.push_back(Value(k));
    } else if (std::holds_alternative<std::string>(v.v)) {
        for (char c : std::get<std::string>(v.v)) items.push_back(Value(std::string(1, c)));
    } else if (std::holds_alternative<RangeVal>(v.v)) {
        RangeVal r = std::get<RangeVal>(v.v);
        if (r.step > 0) for (long long i = r.start; i < r.stop; i += r.step) items.push_back(Value(i));
        else if (r.step < 0) for (long long i = r.start; i > r.stop; i += r.step) items.push_back(Value(i));
    } else {
        err("TypeMismatchError", "cannot loop over a " + valueKind(v), line);
    }
    return items;
}

// ============================================================================
// eval() -- expressions
// ============================================================================

Value Interpreter::eval(Node* n, EnvPtr env) {
    switch (n->kind) {
        case NodeKind::NumLit:
            return n->isFloat ? Value(n->fval) : Value(n->ival);
        case NodeKind::StrLit:
            return Value(n->str);
        case NodeKind::BoolLit:
            return Value(n->bval);
        case NodeKind::NullLit:
            return Value();
        case NodeKind::Ident: {
            Value out;
            if (env->get(n->str, out)) return out;
            throwError("NameError", "name '" + n->str + "' is not defined", n->line);
        }
        case NodeKind::ListLit: {
            auto lst = std::make_shared<std::vector<Value>>();
            for (auto& c : n->children) lst->push_back(eval(c.get(), env));
            return Value(lst);
        }
        case NodeKind::DictLit: {
            auto d = std::make_shared<std::unordered_map<std::string, Value>>();
            for (size_t i = 0; i + 1 < n->children.size(); i += 2) {
                Value k = eval(n->children[i].get(), env);
                Value v = eval(n->children[i + 1].get(), env);
                (*d)[valueToString(k, false)] = v;
            }
            return Value(d);
        }
        case NodeKind::ListComp: {
            Value iterable = eval(n->children[0].get(), env);
            auto items = materializeIterable(iterable, this, n->line,
                [this](const std::string& t, const std::string& m, int l) { throwError(t, m, l); });
            auto result = std::make_shared<std::vector<Value>>();
            for (auto& item : items) {
                env->define(n->str, item);
                if (n->children[2] && !isTruthy(eval(n->children[2].get(), env))) continue;
                result->push_back(eval(n->children[1].get(), env));
            }
            return Value(result);
        }
        case NodeKind::Lambda: {
            auto fn = std::make_shared<FunctionValue>();
            fn->name = "<lambda>";
            fn->paramNames = n->paramNames;
            fn->isLambda = true;
            fn->body = n->children[0].get();
            fn->closure = env;
            return Value(fn);
        }
        case NodeKind::UnaryOp: {
            if (n->str == "deny") return Value(!isTruthy(eval(n->children[0].get(), env)));
            Value operand = eval(n->children[0].get(), env);
            if (std::holds_alternative<long long>(operand.v)) return Value(-std::get<long long>(operand.v));
            if (std::holds_alternative<double>(operand.v)) return Value(-std::get<double>(operand.v));
            throwError("TypeMismatchError", "cannot negate a " + valueKind(operand), n->line);
        }
        case NodeKind::BinOp: {
            if (n->str == "also") {
                Value l = eval(n->children[0].get(), env);
                if (!isTruthy(l)) return l;
                return eval(n->children[1].get(), env);
            }
            if (n->str == "either") {
                Value l = eval(n->children[0].get(), env);
                if (isTruthy(l)) return l;
                return eval(n->children[1].get(), env);
            }
            Value l = eval(n->children[0].get(), env);
            Value r = eval(n->children[1].get(), env);
            return binaryOpDispatch(n->str, l, r, n->line);
        }
        case NodeKind::Index: {
            Value obj = eval(n->children[0].get(), env);
            Value idx = eval(n->children[1].get(), env);
            return indexGet(obj, idx, n->line);
        }
        case NodeKind::Attr: {
            Value obj = eval(n->children[0].get(), env);
            return getAttr(obj, n->str, n->line);
        }
        case NodeKind::Call: {
            Node* calleeNode = n->children[0].get();
            std::vector<Value> args;
            for (size_t i = 1; i < n->children.size(); i++) args.push_back(eval(n->children[i].get(), env));
            if (calleeNode->kind == NodeKind::Attr) {
                Value obj = eval(calleeNode->children[0].get(), env);
                const std::string& name = calleeNode->str;
                if (std::holds_alternative<InstVal>(obj.v)) {
                    auto inst = std::get<InstVal>(obj.v);
                    auto fit = inst->fields.find(name);
                    if (fit != inst->fields.end()) return callValue(fit->second, args, n->line);
                    FuncVal m;
                    if (inst->cls->findMethod(name, m)) return callFunction(m, args, inst);
                    throwError("AttributeError", "'" + inst->cls->name + "' has no attribute '" + name + "'", n->line);
                }
                if (std::holds_alternative<ClassVal>(obj.v)) {
                    auto cls = std::get<ClassVal>(obj.v);
                    auto vit = cls->classVars.find(name);
                    if (vit != cls->classVars.end()) return callValue(vit->second, args, n->line);
                    FuncVal m;
                    if (cls->findMethod(name, m)) return callFunction(m, args, nullptr);
                    throwError("AttributeError", "class '" + cls->name + "' has no attribute '" + name + "'", n->line);
                }
                throwError("TypeMismatchError", "cannot call an attribute on a " + valueKind(obj), n->line);
            }
            Value callee = eval(calleeNode, env);
            return callValue(callee, args, n->line);
        }
        default:
            throw EymeniumFatal("internal error: eval() got a statement node kind");
    }
}

// ============================================================================
// binary operators
// ============================================================================

Value Interpreter::binaryOpDispatch(const std::string& op, Value l, Value r, int line) {
    if (op == "==") return Value(valuesEqual(l, r));
    if (op == "!=") return Value(!valuesEqual(l, r));
    if (op == "same") return Value(identicalValues(l, r));

    if (op == "within") {
        if (std::holds_alternative<ListVal>(r.v)) {
            for (auto& e : *std::get<ListVal>(r.v)) if (valuesEqual(e, l)) return Value(true);
            return Value(false);
        }
        if (std::holds_alternative<DictVal>(r.v)) {
            auto& d = std::get<DictVal>(r.v);
            return Value(d->find(valueToString(l, false)) != d->end());
        }
        if (std::holds_alternative<std::string>(r.v) && std::holds_alternative<std::string>(l.v)) {
            return Value(std::get<std::string>(r.v).find(std::get<std::string>(l.v)) != std::string::npos);
        }
        throwError("TypeMismatchError", "cannot test membership in a " + valueKind(r), line);
    }

    if (op == "+" && (std::holds_alternative<std::string>(l.v) || std::holds_alternative<std::string>(r.v))) {
        // string concatenation with auto-stringify -- a deliberate Eymenium
        // ergonomic choice, unlike Python's stricter str()-required rule
        return Value(valueToString(l, false) + valueToString(r, false));
    }
    if (op == "+" && std::holds_alternative<ListVal>(l.v) && std::holds_alternative<ListVal>(r.v)) {
        auto out = std::make_shared<std::vector<Value>>(*std::get<ListVal>(l.v));
        auto& rl = std::get<ListVal>(r.v);
        out->insert(out->end(), rl->begin(), rl->end());
        return Value(out);
    }

    if (!isNumeric(l) || !isNumeric(r)) {
        if (op == "<" || op == "<=" || op == ">" || op == ">=") {
            if (std::holds_alternative<std::string>(l.v) && std::holds_alternative<std::string>(r.v)) {
                const auto& a = std::get<std::string>(l.v); const auto& b = std::get<std::string>(r.v);
                if (op == "<") return Value(a < b);
                if (op == "<=") return Value(a <= b);
                if (op == ">") return Value(a > b);
                return Value(a >= b);
            }
        }
        throwError("TypeMismatchError", "unsupported operand types for '" + op + "': " + valueKind(l) + " and " + valueKind(r), line);
    }

    bool bothInt = std::holds_alternative<long long>(l.v) && std::holds_alternative<long long>(r.v);
    if (op == "/") {
        double rv = toDouble(r);
        if (rv == 0.0) throwError("DivisionError", "division by zero", line);
        return Value(toDouble(l) / rv);
    }
    if (op == "//") {
        if (bothInt) {
            long long b = std::get<long long>(r.v);
            if (b == 0) throwError("DivisionError", "division by zero", line);
            long long a = std::get<long long>(l.v);
            long long q = a / b;
            if ((a % b != 0) && ((a < 0) != (b < 0))) q--; // floor toward -inf like Python
            return Value(q);
        }
        double b = toDouble(r);
        if (b == 0.0) throwError("DivisionError", "division by zero", line);
        return Value(std::floor(toDouble(l) / b));
    }
    if (op == "%") {
        if (bothInt) {
            long long b = std::get<long long>(r.v);
            if (b == 0) throwError("DivisionError", "modulo by zero", line);
            long long a = std::get<long long>(l.v);
            long long m = a % b;
            if (m != 0 && ((m < 0) != (b < 0))) m += b; // Python-style modulo sign
            return Value(m);
        }
        double b = toDouble(r);
        double m = std::fmod(toDouble(l), b);
        if (m != 0.0 && ((m < 0) != (b < 0))) m += b;
        return Value(m);
    }
    if (op == "**") {
        if (bothInt && std::get<long long>(r.v) >= 0) {
            long long base = std::get<long long>(l.v), exp = std::get<long long>(r.v), res = 1;
            for (long long i = 0; i < exp; i++) res *= base;
            return Value(res);
        }
        return Value(std::pow(toDouble(l), toDouble(r)));
    }
    if (op == "+") return bothInt ? Value(std::get<long long>(l.v) + std::get<long long>(r.v)) : Value(toDouble(l) + toDouble(r));
    if (op == "-") return bothInt ? Value(std::get<long long>(l.v) - std::get<long long>(r.v)) : Value(toDouble(l) - toDouble(r));
    if (op == "*") return bothInt ? Value(std::get<long long>(l.v) * std::get<long long>(r.v)) : Value(toDouble(l) * toDouble(r));
    if (op == "<") return bothInt ? Value(std::get<long long>(l.v) < std::get<long long>(r.v)) : Value(toDouble(l) < toDouble(r));
    if (op == "<=") return bothInt ? Value(std::get<long long>(l.v) <= std::get<long long>(r.v)) : Value(toDouble(l) <= toDouble(r));
    if (op == ">") return bothInt ? Value(std::get<long long>(l.v) > std::get<long long>(r.v)) : Value(toDouble(l) > toDouble(r));
    if (op == ">=") return bothInt ? Value(std::get<long long>(l.v) >= std::get<long long>(r.v)) : Value(toDouble(l) >= toDouble(r));

    throw EymeniumFatal("internal error: unhandled operator '" + op + "'");
}

// ============================================================================
// attribute / index access
// ============================================================================

Value Interpreter::getAttr(Value obj, const std::string& name, int line) {
    if (std::holds_alternative<InstVal>(obj.v)) {
        auto inst = std::get<InstVal>(obj.v);
        auto fit = inst->fields.find(name);
        if (fit != inst->fields.end()) return fit->second;
        FuncVal m;
        if (inst->cls->findMethod(name, m)) {
            auto self = inst;
            NativeFn nf = [this, m, self](std::vector<Value>& args) -> Value { return callFunction(m, args, self); };
            return Value(std::make_shared<NativeFn>(nf));
        }
        throwError("AttributeError", "'" + inst->cls->name + "' has no attribute '" + name + "'", line);
    }
    if (std::holds_alternative<ClassVal>(obj.v)) {
        auto cls = std::get<ClassVal>(obj.v);
        auto vit = cls->classVars.find(name);
        if (vit != cls->classVars.end()) return vit->second;
        FuncVal m;
        if (cls->findMethod(name, m)) {
            NativeFn nf = [this, m](std::vector<Value>& args) -> Value { return callFunction(m, args, nullptr); };
            return Value(std::make_shared<NativeFn>(nf));
        }
        throwError("AttributeError", "class '" + cls->name + "' has no attribute '" + name + "'", line);
    }
    throwError("TypeMismatchError", "cannot access attribute '" + name + "' on a " + valueKind(obj), line);
}

void Interpreter::setAttr(Value obj, const std::string& name, const Value& val, int line) {
    if (std::holds_alternative<InstVal>(obj.v)) {
        std::get<InstVal>(obj.v)->fields[name] = val;
        return;
    }
    throwError("TypeMismatchError", "cannot set attribute '" + name + "' on a " + valueKind(obj), line);
}

static long long asIndex(Value v, Interpreter* interp, int line, std::function<void(const std::string&, const std::string&, int)> err) {
    if (std::holds_alternative<long long>(v.v)) return std::get<long long>(v.v);
    err("TypeMismatchError", "index must be an int, got " + valueKind(v), line);
    return 0;
}

Value Interpreter::indexGet(Value obj, Value idx, int line) {
    auto errFn = [this](const std::string& t, const std::string& m, int l) { throwError(t, m, l); };
    if (std::holds_alternative<ListVal>(obj.v)) {
        auto& l = std::get<ListVal>(obj.v);
        long long i = asIndex(idx, this, line, errFn);
        if (i < 0) i += (long long)l->size();
        if (i < 0 || i >= (long long)l->size()) throwError("IndexError", "list index out of range", line);
        return (*l)[i];
    }
    if (std::holds_alternative<DictVal>(obj.v)) {
        auto& d = std::get<DictVal>(obj.v);
        std::string k = valueToString(idx, false);
        auto it = d->find(k);
        if (it == d->end()) throwError("KeyError", "key '" + k + "' not found", line);
        return it->second;
    }
    if (std::holds_alternative<std::string>(obj.v)) {
        auto& s = std::get<std::string>(obj.v);
        long long i = asIndex(idx, this, line, errFn);
        if (i < 0) i += (long long)s.size();
        if (i < 0 || i >= (long long)s.size()) throwError("IndexError", "string index out of range", line);
        return Value(std::string(1, s[i]));
    }
    throwError("TypeMismatchError", "cannot index into a " + valueKind(obj), line);
}

void Interpreter::indexSet(Value obj, Value idx, const Value& val, int line) {
    auto errFn = [this](const std::string& t, const std::string& m, int l) { throwError(t, m, l); };
    if (std::holds_alternative<ListVal>(obj.v)) {
        auto& l = std::get<ListVal>(obj.v);
        long long i = asIndex(idx, this, line, errFn);
        if (i < 0) i += (long long)l->size();
        if (i < 0 || i >= (long long)l->size()) throwError("IndexError", "list index out of range", line);
        (*l)[i] = val;
        return;
    }
    if (std::holds_alternative<DictVal>(obj.v)) {
        (*std::get<DictVal>(obj.v))[valueToString(idx, false)] = val;
        return;
    }
    if (std::holds_alternative<std::string>(obj.v)) throwError("TypeMismatchError", "strings are immutable", line);
    throwError("TypeMismatchError", "cannot assign into a " + valueKind(obj), line);
}

// ============================================================================
// calling
// ============================================================================

Value Interpreter::callFunction(FuncVal fn, std::vector<Value>& args, InstVal boundSelf) {
    auto callEnv = std::make_shared<Environment>(fn->closure);
    size_t pi = 0;
    if (boundSelf) {
        if (fn->paramNames.empty()) throwError("ArgumentError", "method '" + fn->name + "' declares no 'self' parameter", 0);
        callEnv->define(fn->paramNames[0], Value(boundSelf));
        pi = 1;
    }
    size_t provided = args.size();
    size_t maxParams = fn->paramNames.size() - pi;
    if (provided > maxParams) throwError("ArgumentError", "too many arguments to '" + fn->name + "' (expected at most " + std::to_string(maxParams) + ", got " + std::to_string(provided) + ")", 0);
    for (size_t i = pi; i < fn->paramNames.size(); i++) {
        size_t argi = i - pi;
        if (argi < provided) {
            callEnv->define(fn->paramNames[i], args[argi]);
        } else if (i < fn->paramDefaults.size() && fn->paramDefaults[i]) {
            callEnv->define(fn->paramNames[i], eval(fn->paramDefaults[i], callEnv));
        } else {
            throwError("ArgumentError", "missing required argument '" + fn->paramNames[i] + "' in call to '" + fn->name + "'", 0);
        }
    }
    if (fn->isLambda) return eval(fn->body, callEnv);
    try {
        execBlock(fn->body, callEnv);
    } catch (ReturnSignal& r) {
        return r.value;
    }
    return Value();
}

Value Interpreter::callValue(Value callee, std::vector<Value>& args, int line) {
    if (std::holds_alternative<FuncVal>(callee.v)) return callFunction(std::get<FuncVal>(callee.v), args, nullptr);
    if (std::holds_alternative<std::shared_ptr<NativeFn>>(callee.v)) return (*std::get<std::shared_ptr<NativeFn>>(callee.v))(args);
    if (std::holds_alternative<ClassVal>(callee.v)) {
        auto cls = std::get<ClassVal>(callee.v);
        auto inst = std::make_shared<InstanceValue>();
        inst->cls = cls;
        FuncVal initFn;
        if (cls->findMethod("init", initFn)) callFunction(initFn, args, inst);
        return Value(inst);
    }
    throwError("TypeMismatchError", "a " + valueKind(callee) + " is not callable", line);
}

// ============================================================================
// assignment targets
// ============================================================================

void Interpreter::assignTo(Node* target, const Value& val, EnvPtr env) {
    if (target->kind == NodeKind::Ident) {
        const std::string& name = target->str;
        if (env->globalDecls.count(name)) { globals->define(name, val); return; }
        if (env->nonlocalDecls.count(name)) {
            Environment* e = env->parent.get();
            while (e) { auto it = e->vars.find(name); if (it != e->vars.end()) { it->second = val; return; } e = e->parent.get(); }
            throwError("NameError", "no binding for nonlocal '" + name + "'", target->line);
        }
        env->define(name, val); // local-only default, Python-style
        return;
    }
    if (target->kind == NodeKind::Index) {
        Value obj = eval(target->children[0].get(), env);
        Value idx = eval(target->children[1].get(), env);
        indexSet(obj, idx, val, target->line);
        return;
    }
    if (target->kind == NodeKind::Attr) {
        Value obj = eval(target->children[0].get(), env);
        setAttr(obj, target->str, val, target->line);
        return;
    }
    throwError("SyntaxError", "invalid assignment target", target->line);
}

// ============================================================================
// exec() -- statements
// ============================================================================

void Interpreter::execBlock(Node* block, EnvPtr env) {
    for (auto& stmt : block->children) exec(stmt.get(), env);
}

void Interpreter::exec(Node* n, EnvPtr env) {
    switch (n->kind) {
        case NodeKind::Block:
        case NodeKind::Program:
            execBlock(n, env);
            return;
        case NodeKind::ExprStmt:
            eval(n->children[0].get(), env);
            return;
        case NodeKind::Assign: {
            Value rhs = eval(n->children[1].get(), env);
            if (!n->str.empty()) {
                Value cur = eval(n->children[0].get(), env);
                rhs = binaryOpDispatch(n->str, cur, rhs, n->line);
            }
            assignTo(n->children[0].get(), rhs, env);
            return;
        }
        case NodeKind::If: {
            if (isTruthy(eval(n->children[0].get(), env))) exec(n->children[1].get(), env);
            else if (n->children[2]) exec(n->children[2].get(), env);
            return;
        }
        case NodeKind::While: {
            while (isTruthy(eval(n->children[0].get(), env))) {
                try { execBlock(n->children[1].get(), env); }
                catch (BreakSignal&) { break; }
                catch (ContinueSignal&) { continue; }
            }
            return;
        }
        case NodeKind::For: {
            Value iterable = eval(n->children[0].get(), env);
            auto items = materializeIterable(iterable, this, n->line,
                [this](const std::string& t, const std::string& m, int l) { throwError(t, m, l); });
            for (auto& item : items) {
                env->assign(n->str, item);
                try { execBlock(n->children[1].get(), env); }
                catch (BreakSignal&) { break; }
                catch (ContinueSignal&) { continue; }
            }
            return;
        }
        case NodeKind::FuncDef: {
            auto fn = std::make_shared<FunctionValue>();
            fn->name = n->str;
            fn->paramNames = n->paramNames;
            for (auto& d : n->paramDefaults) fn->paramDefaults.push_back(d.get());
            fn->body = n->children[0].get();
            fn->isLambda = false;
            fn->closure = env;
            env->define(n->str, Value(fn));
            return;
        }
        case NodeKind::ClassDef: {
            ClassVal parentCls;
            if (!n->names.empty()) {
                Value pv;
                if (!env->get(n->names[0], pv) || !std::holds_alternative<ClassVal>(pv.v))
                    throwError("NameError", "base blueprint '" + n->names[0] + "' is not defined", n->line);
                parentCls = std::get<ClassVal>(pv.v);
            }
            auto bodyEnv = std::make_shared<Environment>(env);
            execBlock(n->children[0].get(), bodyEnv);
            auto cls = std::make_shared<ClassValue>();
            cls->name = n->str;
            cls->parent = parentCls;
            for (auto& [name, val] : bodyEnv->vars) {
                if (std::holds_alternative<FuncVal>(val.v)) cls->methods[name] = std::get<FuncVal>(val.v);
                else cls->classVars[name] = val;
            }
            env->define(n->str, Value(cls));
            return;
        }
        case NodeKind::Return: {
            Value v = n->children.empty() ? Value() : eval(n->children[0].get(), env);
            throw ReturnSignal{v};
        }
        case NodeKind::Break: throw BreakSignal{};
        case NodeKind::Continue: throw ContinueSignal{};
        case NodeKind::Pass: return;
        case NodeKind::Throw: {
            Value payload = eval(n->children[0].get(), env);
            throw EymeniumThrow{payload};
        }
        case NodeKind::Try: {
            bool finallyRan = false;
            try {
                try {
                    execBlock(n->children[0].get(), env);
                } catch (EymeniumThrow& t) {
                    bool handled = false;
                    for (auto& cc : n->catches) {
                        bool matches;
                        if (!cc.hasType) matches = true;
                        else if (std::holds_alternative<InstVal>(t.payload.v)) matches = std::get<InstVal>(t.payload.v)->cls->isOrInherits(cc.typeName);
                        else matches = (valueKind(t.payload) == cc.typeName);
                        if (matches) {
                            if (cc.hasBind) env->define(cc.bindName, t.payload);
                            execBlock(cc.body.get(), env);
                            handled = true;
                            break;
                        }
                    }
                    if (!handled) throw;
                }
            } catch (...) {
                if (n->finallyBlock) { finallyRan = true; execBlock(n->finallyBlock.get(), env); }
                throw;
            }
            if (!finallyRan && n->finallyBlock) execBlock(n->finallyBlock.get(), env);
            return;
        }
        case NodeKind::Import:
            loadModule(n->str, env, n->line);
            return;
        case NodeKind::Global:
            for (auto& nm : n->names) env->globalDecls.insert(nm);
            return;
        case NodeKind::Nonlocal:
            for (auto& nm : n->names) env->nonlocalDecls.insert(nm);
            return;
        case NodeKind::Del: {
            Node* t = n->children[0].get();
            if (t->kind == NodeKind::Ident) {
                Environment* e = env.get();
                while (e) { auto it = e->vars.find(t->str); if (it != e->vars.end()) { e->vars.erase(it); return; } e = e->parent.get(); }
                throwError("NameError", "name '" + t->str + "' is not defined", t->line);
            }
            if (t->kind == NodeKind::Index) {
                Value obj = eval(t->children[0].get(), env);
                Value idx = eval(t->children[1].get(), env);
                if (std::holds_alternative<ListVal>(obj.v)) {
                    auto& l = std::get<ListVal>(obj.v);
                    long long i = std::get<long long>(idx.v);
                    if (i < 0) i += (long long)l->size();
                    if (i < 0 || i >= (long long)l->size()) throwError("IndexError", "list index out of range", t->line);
                    l->erase(l->begin() + i);
                    return;
                }
                if (std::holds_alternative<DictVal>(obj.v)) {
                    auto& d = std::get<DictVal>(obj.v);
                    std::string k = valueToString(idx, false);
                    if (!d->erase(k)) throwError("KeyError", "key '" + k + "' not found", t->line);
                    return;
                }
                throwError("TypeMismatchError", "cannot remove an index from a " + valueKind(obj), t->line);
            }
            if (t->kind == NodeKind::Attr) {
                Value obj = eval(t->children[0].get(), env);
                if (std::holds_alternative<InstVal>(obj.v)) {
                    auto& f = std::get<InstVal>(obj.v)->fields;
                    if (!f.erase(t->str)) throwError("AttributeError", "no attribute '" + t->str + "' to remove", t->line);
                    return;
                }
            }
            throwError("SyntaxError", "invalid 'remove' target", t->line);
        }
        case NodeKind::Assert: {
            if (!isTruthy(eval(n->children[0].get(), env))) {
                std::string msg = n->children.size() > 1 ? valueToString(eval(n->children[1].get(), env), false) : "assertion failed";
                throwError("AssertionError", msg, n->line);
            }
            return;
        }
        default:
            throw EymeniumFatal("internal error: exec() got an expression node kind");
    }
}

// ============================================================================
// modules
// ============================================================================

void Interpreter::loadModule(const std::string& name, EnvPtr env, int line) {
    for (auto& m : loadedModules) if (m == name) return; // already loaded, no-op
    std::string path = baseDir.empty() ? (name + ".eym") : (baseDir + "/" + name + ".eym");
    std::ifstream f(path);
    if (!f) throwError("ImportError", "module '" + name + "' not found (looked for " + path + ")", line);
    std::stringstream buf; buf << f.rdbuf();
    auto tokens = lex(buf.str(), path);
    auto program = parseProgram(tokens, path);
    execBlock(program.get(), env);
    loadedModules.push_back(name);
    // note: the parsed AST for the module is intentionally leaked for the
    // process lifetime -- function/class values created while executing it
    // hold raw Node* pointers into this tree, so it must outlive them.
    program.release();
}

// ============================================================================
// top-level run / repl
// ============================================================================

void Interpreter::run(Node* program, const std::string& filename) {
    size_t slash = filename.find_last_of("/\\");
    baseDir = (slash == std::string::npos) ? "" : filename.substr(0, slash);
    try {
        execBlock(program, globals);
    } catch (EymeniumThrow& t) {
        std::string msg = "uncaught error";
        std::string type = "Error";
        if (std::holds_alternative<InstVal>(t.payload.v)) {
            auto inst = std::get<InstVal>(t.payload.v);
            type = inst->cls->name;
            auto it = inst->fields.find("msg");
            if (it != inst->fields.end()) msg = valueToString(it->second, false);
        } else {
            msg = valueToString(t.payload, false);
        }
        std::cerr << filename << ": uncaught " << type << ": " << msg << std::endl;
        std::exit(1);
    } catch (ReturnSignal&) {
        std::cerr << filename << ": 'back' used outside a function" << std::endl;
        std::exit(1);
    } catch (BreakSignal&) {
        std::cerr << filename << ": 'stop' used outside a loop" << std::endl;
        std::exit(1);
    } catch (ContinueSignal&) {
        std::cerr << filename << ": 'skip' used outside a loop" << std::endl;
        std::exit(1);
    }
}

void Interpreter::runRepl() {
    std::cout << "Eymenium Native REPL (.eym) -- type 'exit' to quit, blank line ends a block\n";
    baseDir = ".";
    std::string buf;
    while (true) {
        std::cout << (buf.empty() ? "eym> " : "...> ");
        std::string line;
        if (!std::getline(std::cin, line)) { std::cout << std::endl; break; }
        if (buf.empty() && (line == "exit" || line == "quit")) break;
        if (line.empty() && !buf.empty()) {
            try {
                auto tokens = lex(buf, "<repl>");
                auto program = parseProgram(tokens, "<repl>");
                Node* raw = program.get();
                try {
                    execBlock(raw, globals);
                } catch (EymeniumThrow& t) {
                    std::string msg = valueToString(t.payload, false);
                    if (std::holds_alternative<InstVal>(t.payload.v)) {
                        auto inst = std::get<InstVal>(t.payload.v);
                        auto it = inst->fields.find("msg");
                        std::cerr << inst->cls->name << ": " << (it != inst->fields.end() ? valueToString(it->second, false) : msg) << std::endl;
                    } else {
                        std::cerr << "error: " << msg << std::endl;
                    }
                }
                program.release(); // functions defined in the REPL may still reference this AST
            } catch (EymeniumFatal& e) {
                std::cerr << e.what() << std::endl;
            }
            buf.clear();
            continue;
        }
        if (!line.empty()) buf += line + "\n";
        if (buf.empty()) continue;
    }
}
