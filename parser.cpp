// parser.cpp -- recursive-descent parser for Eymenium.
//
// Grammar summary (low to high precedence for expressions):
//   lambda:     fn ( params ) : expr
//   or:         and (either and)*
//   and:        not (also not)*
//   not:        deny not | comparison
//   comparison: additive ((==|!=|<|<=|>|>=|within|same) additive)*
//   additive:   term ((+|-) term)*
//   term:       unary ((*|/|%|//) unary)*
//   unary:      -unary | power
//   power:      postfix (** unary)?
//   postfix:    primary (call | index | attr)*
//   primary:    NUMBER | STRING | yes | no | null | IDENT | (expr)
//               | [ list-or-comprehension ] | { dict } | fn(...) : expr
//
// Statements are parsed at the top level of parseStatement(); assignment is
// handled as a statement (parse an expr, then check for `=`/aug-assign)
// rather than as a low-precedence expression operator, mirroring how
// Python itself treats assignment as a statement, not an expression.

#include "eymenium.hpp"
#include <stdexcept>

namespace {

struct Parser {
    std::vector<Token>& toks;
    size_t pos = 0;
    std::string filename;

    Parser(std::vector<Token>& t, const std::string& fn) : toks(t), filename(fn) {}

    Token& cur() { return toks[pos]; }
    Token& peekAt(int off) { size_t p = pos + off; return p < toks.size() ? toks[p] : toks.back(); }
    bool isEnd() { return cur().kind == TokKind::End; }

    [[noreturn]] void err(const std::string& msg) {
        throw EymeniumFatal(filename + ":" + std::to_string(cur().line) + ": parse error: " + msg +
                             " (near '" + cur().text + "')");
    }

    bool isKw(const std::string& kw) { return cur().kind == TokKind::Ident && cur().text == kw; }
    bool isOp(const std::string& op) { return cur().kind == TokKind::Op && cur().text == op; }

    Token advance() { Token t = cur(); if (pos + 1 < toks.size()) pos++; return t; }

    void expectOp(const std::string& op) {
        if (!isOp(op)) err("expected '" + op + "'");
        advance();
    }
    void expectKw(const std::string& kw) {
        if (!isKw(kw)) err("expected '" + kw + "'");
        advance();
    }
    std::string expectIdent() {
        if (cur().kind != TokKind::Ident) err("expected identifier");
        return advance().text;
    }
    void expectNewline() {
        if (cur().kind == TokKind::Newline) { advance(); return; }
        if (cur().kind == TokKind::End) return;
        err("expected end of line");
    }
    void skipNewlines() { while (cur().kind == TokKind::Newline) advance(); }

    // reserved words that cannot be used as plain identifiers in expression position
    bool isReservedWord(const std::string& s) {
        static const std::vector<std::string> kws = {
            "exp","chk","orchk","nah","loop","spin","back","stop","skip","nop",
            "blueprint","yes","no","null","also","either","deny","within","same",
            "attempt","catch","atlast","throw","called","fn","add","glob","nloc",
            "remove","ensure"
        };
        for (auto& k : kws) if (k == s) return true;
        return false;
    }

    NodePtr makeNode(NodeKind k) { return std::make_unique<Node>(k, cur().line); }

    // ---------------- top level / blocks ----------------

    NodePtr parseProgram() {
        auto prog = std::make_unique<Node>(NodeKind::Program, 1);
        skipNewlines();
        while (!isEnd()) {
            prog->children.push_back(parseStatement());
            skipNewlines();
        }
        return prog;
    }

    NodePtr parseBlock() {
        skipNewlines();
        if (cur().kind != TokKind::Indent) err("expected an indented block");
        advance(); // Indent
        auto blk = makeNode(NodeKind::Block);
        skipNewlines();
        while (cur().kind != TokKind::Dedent) {
            if (isEnd()) err("unexpected end of file inside block");
            blk->children.push_back(parseStatement());
            skipNewlines();
        }
        advance(); // Dedent
        return blk;
    }

    // ---------------- statements ----------------

    NodePtr parseStatement() {
        if (isKw("chk")) return parseIf();
        if (isKw("spin")) return parseWhile();
        if (isKw("loop")) return parseFor();
        if (isKw("exp")) return parseFuncDef();
        if (isKw("blueprint")) return parseClassDef();
        if (isKw("attempt")) return parseTry();
        if (isKw("back")) return parseReturn();
        if (isKw("stop")) { auto n = makeNode(NodeKind::Break); advance(); expectNewline(); return n; }
        if (isKw("skip")) { auto n = makeNode(NodeKind::Continue); advance(); expectNewline(); return n; }
        if (isKw("nop")) { auto n = makeNode(NodeKind::Pass); advance(); expectNewline(); return n; }
        if (isKw("throw")) return parseThrow();
        if (isKw("add")) return parseImport();
        if (isKw("glob")) return parseNameList(NodeKind::Global);
        if (isKw("nloc")) return parseNameList(NodeKind::Nonlocal);
        if (isKw("remove")) return parseDel();
        if (isKw("ensure")) return parseAssert();
        return parseExprOrAssignStatement();
    }

    NodePtr parseIf() {
        auto n = makeNode(NodeKind::If);
        expectKw("chk");
        n->children.push_back(parseExpr());
        expectOp(":");
        n->children.push_back(parseBlock());
        if (isKw("orchk")) {
            // desugar elif into a nested If in the else slot
            auto elifNode = makeNode(NodeKind::If);
            expectKw("orchk");
            elifNode->children.push_back(parseExpr());
            expectOp(":");
            elifNode->children.push_back(parseBlock());
            NodePtr rest = finishIfChain();
            elifNode->children.push_back(std::move(rest));
            n->children.push_back(std::move(elifNode));
        } else if (isKw("nah")) {
            advance();
            expectOp(":");
            n->children.push_back(parseBlock());
        } else {
            n->children.push_back(nullptr);
        }
        return n;
    }
    // handles further orchk/nah chained after the first orchk was already consumed by parseIf
    NodePtr finishIfChain() {
        if (isKw("orchk")) {
            auto elifNode = makeNode(NodeKind::If);
            expectKw("orchk");
            elifNode->children.push_back(parseExpr());
            expectOp(":");
            elifNode->children.push_back(parseBlock());
            elifNode->children.push_back(finishIfChain());
            return elifNode;
        } else if (isKw("nah")) {
            advance();
            expectOp(":");
            return parseBlock();
        }
        return nullptr;
    }

    NodePtr parseWhile() {
        auto n = makeNode(NodeKind::While);
        expectKw("spin");
        n->children.push_back(parseExpr());
        expectOp(":");
        n->children.push_back(parseBlock());
        return n;
    }

    NodePtr parseFor() {
        auto n = makeNode(NodeKind::For);
        expectKw("loop");
        n->str = expectIdent();
        expectKw("within");
        n->children.push_back(parseExpr());
        expectOp(":");
        n->children.push_back(parseBlock());
        return n;
    }

    void parseParamList(std::vector<std::string>& names, std::vector<NodePtr>* defaults) {
        expectOp("(");
        while (!isOp(")")) {
            std::string pname = expectIdent();
            names.push_back(pname);
            if (defaults) {
                if (isOp("=")) { advance(); defaults->push_back(parseExpr()); }
                else defaults->push_back(nullptr);
            }
            if (isOp(",")) advance(); else break;
        }
        expectOp(")");
    }

    NodePtr parseFuncDef() {
        auto n = makeNode(NodeKind::FuncDef);
        expectKw("exp");
        n->str = expectIdent();
        parseParamList(n->paramNames, &n->paramDefaults);
        expectOp(":");
        n->children.push_back(parseBlock());
        return n;
    }

    NodePtr parseClassDef() {
        auto n = makeNode(NodeKind::ClassDef);
        expectKw("blueprint");
        n->str = expectIdent();
        if (isOp("(")) {
            advance();
            if (!isOp(")")) n->names.push_back(expectIdent());
            expectOp(")");
        }
        expectOp(":");
        n->children.push_back(parseBlock());
        return n;
    }

    NodePtr parseReturn() {
        auto n = makeNode(NodeKind::Return);
        expectKw("back");
        if (cur().kind != TokKind::Newline && !isEnd()) n->children.push_back(parseExpr());
        expectNewline();
        return n;
    }

    NodePtr parseThrow() {
        auto n = makeNode(NodeKind::Throw);
        expectKw("throw");
        n->children.push_back(parseExpr());
        expectNewline();
        return n;
    }

    NodePtr parseImport() {
        auto n = makeNode(NodeKind::Import);
        expectKw("add");
        n->str = expectIdent();
        expectNewline();
        return n;
    }

    NodePtr parseNameList(NodeKind k) {
        auto n = makeNode(k);
        advance(); // glob/nloc
        n->names.push_back(expectIdent());
        while (isOp(",")) { advance(); n->names.push_back(expectIdent()); }
        expectNewline();
        return n;
    }

    NodePtr parseDel() {
        auto n = makeNode(NodeKind::Del);
        expectKw("remove");
        n->children.push_back(parsePostfix());
        expectNewline();
        return n;
    }

    NodePtr parseAssert() {
        auto n = makeNode(NodeKind::Assert);
        expectKw("ensure");
        n->children.push_back(parseExpr());
        if (isOp(",")) { advance(); n->children.push_back(parseExpr()); }
        expectNewline();
        return n;
    }

    NodePtr parseTry() {
        auto n = makeNode(NodeKind::Try);
        expectKw("attempt");
        expectOp(":");
        n->children.push_back(parseBlock());
        while (isKw("catch")) {
            advance();
            CatchClause cc;
            if (isKw("called")) {
                advance();
                cc.hasBind = true;
                cc.bindName = expectIdent();
            } else if (cur().kind == TokKind::Ident && !isOp(":")) {
                cc.hasType = true;
                cc.typeName = expectIdent();
                if (isKw("called")) { advance(); cc.hasBind = true; cc.bindName = expectIdent(); }
            }
            expectOp(":");
            cc.body = parseBlock();
            n->catches.push_back(std::move(cc));
        }
        if (n->catches.empty() && !isKw("atlast")) err("'attempt' needs at least one 'catch' or an 'atlast'");
        if (isKw("atlast")) {
            advance();
            expectOp(":");
            n->finallyBlock = parseBlock();
        }
        return n;
    }

    NodePtr parseExprOrAssignStatement() {
        NodePtr lhs = parseExpr();
        static const std::vector<std::string> augs = {"+=","-=","*=","/=","%="};
        if (isOp("=")) {
            advance();
            auto n = makeNode(NodeKind::Assign);
            n->str = "";
            n->children.push_back(std::move(lhs));
            n->children.push_back(parseExpr());
            expectNewline();
            return n;
        }
        for (auto& a : augs) {
            if (isOp(a)) {
                advance();
                auto n = makeNode(NodeKind::Assign);
                n->str = a.substr(0, a.size() - 1); // strip trailing '='
                n->children.push_back(std::move(lhs));
                n->children.push_back(parseExpr());
                expectNewline();
                return n;
            }
        }
        auto n = makeNode(NodeKind::ExprStmt);
        n->children.push_back(std::move(lhs));
        expectNewline();
        return n;
    }

    // ---------------- expressions ----------------

    NodePtr parseExpr() { return parseLambdaOrOr(); }

    NodePtr parseLambdaOrOr() {
        if (isKw("fn")) {
            auto n = makeNode(NodeKind::Lambda);
            advance();
            parseParamList(n->paramNames, nullptr);
            expectOp(":");
            n->children.push_back(parseExpr());
            return n;
        }
        return parseOr();
    }

    NodePtr parseOr() {
        NodePtr left = parseAnd();
        while (isKw("either")) {
            auto n = makeNode(NodeKind::BinOp);
            n->str = "either";
            advance();
            n->children.push_back(std::move(left));
            n->children.push_back(parseAnd());
            left = std::move(n);
        }
        return left;
    }
    NodePtr parseAnd() {
        NodePtr left = parseNot();
        while (isKw("also")) {
            auto n = makeNode(NodeKind::BinOp);
            n->str = "also";
            advance();
            n->children.push_back(std::move(left));
            n->children.push_back(parseNot());
            left = std::move(n);
        }
        return left;
    }
    NodePtr parseNot() {
        if (isKw("deny")) {
            auto n = makeNode(NodeKind::UnaryOp);
            n->str = "deny";
            advance();
            n->children.push_back(parseNot());
            return n;
        }
        return parseComparison();
    }
    NodePtr parseComparison() {
        NodePtr left = parseAdditive();
        static const std::vector<std::string> ops = {"==","!=","<=",">=","<",">"};
        while (true) {
            bool matched = false;
            for (auto& o : ops) {
                if (isOp(o)) {
                    auto n = makeNode(NodeKind::BinOp);
                    n->str = o;
                    advance();
                    n->children.push_back(std::move(left));
                    n->children.push_back(parseAdditive());
                    left = std::move(n);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                if (isKw("within") || isKw("same")) {
                    auto n = makeNode(NodeKind::BinOp);
                    n->str = cur().text;
                    advance();
                    n->children.push_back(std::move(left));
                    n->children.push_back(parseAdditive());
                    left = std::move(n);
                    continue;
                }
                break;
            }
        }
        return left;
    }
    NodePtr parseAdditive() {
        NodePtr left = parseTerm();
        while (isOp("+") || isOp("-")) {
            auto n = makeNode(NodeKind::BinOp);
            n->str = cur().text;
            advance();
            n->children.push_back(std::move(left));
            n->children.push_back(parseTerm());
            left = std::move(n);
        }
        return left;
    }
    NodePtr parseTerm() {
        NodePtr left = parseUnary();
        while (isOp("*") || isOp("/") || isOp("%") || isOp("//")) {
            auto n = makeNode(NodeKind::BinOp);
            n->str = cur().text;
            advance();
            n->children.push_back(std::move(left));
            n->children.push_back(parseUnary());
            left = std::move(n);
        }
        return left;
    }
    NodePtr parseUnary() {
        if (isOp("-")) {
            auto n = makeNode(NodeKind::UnaryOp);
            n->str = "-";
            advance();
            n->children.push_back(parseUnary());
            return n;
        }
        return parsePower();
    }
    NodePtr parsePower() {
        NodePtr left = parsePostfix();
        if (isOp("**")) {
            auto n = makeNode(NodeKind::BinOp);
            n->str = "**";
            advance();
            n->children.push_back(std::move(left));
            n->children.push_back(parseUnary()); // right-assoc, allows unary on rhs
            return n;
        }
        return left;
    }

    NodePtr parsePostfix() {
        NodePtr expr = parsePrimary();
        while (true) {
            if (isOp(".")) {
                advance();
                auto n = makeNode(NodeKind::Attr);
                n->str = expectIdent();
                n->children.push_back(std::move(expr));
                expr = std::move(n);
            } else if (isOp("[")) {
                advance();
                auto n = makeNode(NodeKind::Index);
                n->children.push_back(std::move(expr));
                n->children.push_back(parseExpr());
                expectOp("]");
                expr = std::move(n);
            } else if (isOp("(")) {
                advance();
                auto n = makeNode(NodeKind::Call);
                n->children.push_back(std::move(expr));
                while (!isOp(")")) {
                    n->children.push_back(parseExpr());
                    if (isOp(",")) advance(); else break;
                }
                expectOp(")");
                expr = std::move(n);
            } else break;
        }
        return expr;
    }

    NodePtr parsePrimary() {
        if (cur().kind == TokKind::Number) {
            auto n = makeNode(NodeKind::NumLit);
            n->isFloat = cur().isFloat;
            n->ival = cur().ival;
            n->fval = cur().fval;
            advance();
            return n;
        }
        if (cur().kind == TokKind::String) {
            auto n = makeNode(NodeKind::StrLit);
            n->str = cur().text;
            advance();
            return n;
        }
        if (isKw("yes") || isKw("no")) {
            auto n = makeNode(NodeKind::BoolLit);
            n->bval = isKw("yes");
            advance();
            return n;
        }
        if (isKw("null")) { auto n = makeNode(NodeKind::NullLit); advance(); return n; }
        if (isOp("(")) {
            advance();
            NodePtr inner = parseExpr();
            expectOp(")");
            return inner;
        }
        if (isOp("[")) return parseListLitOrComp();
        if (isOp("{")) return parseDictLit();
        if (cur().kind == TokKind::Ident) {
            if (isReservedWord(cur().text)) err("unexpected keyword '" + cur().text + "' in expression");
            auto n = makeNode(NodeKind::Ident);
            n->str = advance().text;
            return n;
        }
        err("unexpected token in expression");
    }

    NodePtr parseListLitOrComp() {
        int ln = cur().line;
        expectOp("[");
        if (isOp("]")) { advance(); auto n = std::make_unique<Node>(NodeKind::ListLit, ln); return n; }
        NodePtr first = parseExpr();
        if (isKw("loop")) {
            advance();
            auto n = std::make_unique<Node>(NodeKind::ListComp, ln);
            n->str = expectIdent();
            expectKw("within");
            n->children.push_back(parseExpr());       // [0] iterable
            n->children.push_back(std::move(first));   // [1] result expr
            if (isKw("chk")) {
                advance();
                n->children.push_back(parseExpr());     // [2] condition
            } else {
                n->children.push_back(nullptr);
            }
            expectOp("]");
            return n;
        }
        auto n = std::make_unique<Node>(NodeKind::ListLit, ln);
        n->children.push_back(std::move(first));
        while (isOp(",")) {
            advance();
            if (isOp("]")) break; // trailing comma
            n->children.push_back(parseExpr());
        }
        expectOp("]");
        return n;
    }

    NodePtr parseDictLit() {
        auto n = makeNode(NodeKind::DictLit);
        expectOp("{");
        while (!isOp("}")) {
            n->children.push_back(parseExpr()); // key
            expectOp(":");
            n->children.push_back(parseExpr()); // value
            if (isOp(",")) advance(); else break;
        }
        expectOp("}");
        return n;
    }
};

} // namespace

NodePtr parseProgram(std::vector<Token>& tokens, const std::string& filename) {
    Parser p(tokens, filename);
    return p.parseProgram();
}
