// lexer.cpp -- turns Eymenium source text into a token stream.
//
// Indentation handling: like Python, leading whitespace is significant.
// We track an indent stack; increases emit INDENT, decreases emit one
// DEDENT per level popped. Only spaces are supported for indentation
// (tabs are rejected with a clear error) to avoid the tabs-vs-spaces
// ambiguity entirely.
//
// Multi-line statements: unlike Python, we do NOT track bracket depth to
// suppress newlines inside (), [], {}. Instead a trailing backslash `\`
// explicitly continues a logical line onto the next physical line. This
// is a deliberate simplification -- documented in the README.

#include "eymenium.hpp"
#include <cctype>
#include <stdexcept>

namespace {

struct Lexer {
    const std::string& src;
    const std::string& filename;
    size_t pos = 0;
    int line = 1;
    std::vector<int> indentStack{0};
    std::vector<Token> out;
    bool atLineStart = true;

    Lexer(const std::string& s, const std::string& fn) : src(s), filename(fn) {}

    char peek(int off = 0) { size_t p = pos + off; return p < src.size() ? src[p] : '\0'; }
    char advance() { return pos < src.size() ? src[pos++] : '\0'; }

    void err(const std::string& msg) {
        throw EymeniumFatal(filename + ":" + std::to_string(line) + ": lex error: " + msg);
    }

    void push(TokKind k, const std::string& text = "") {
        Token t; t.kind = k; t.text = text; t.line = line;
        out.push_back(t);
    }

    void handleIndentation() {
        int col = 0;
        while (true) {
            char c = peek();
            if (c == ' ') { col++; pos++; }
            else if (c == '\t') { err("tabs are not supported for indentation; use spaces"); }
            else break;
        }
        // blank line or comment-only line: don't touch indent stack
        if (peek() == '\n' || peek() == '\0' || peek() == '#') return;

        if (col > indentStack.back()) {
            indentStack.push_back(col);
            push(TokKind::Indent);
        } else {
            while (col < indentStack.back()) {
                indentStack.pop_back();
                push(TokKind::Dedent);
            }
            if (col != indentStack.back()) err("inconsistent indentation");
        }
    }

    void lexString(char quote) {
        std::string s;
        int startLine = line;
        advance(); // consume opening quote
        while (true) {
            char c = peek();
            if (c == '\0' || c == '\n') { line = startLine; err("unterminated string literal"); }
            if (c == quote) { advance(); break; }
            if (c == '\\') {
                advance();
                char e = advance();
                switch (e) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case '\\': s += '\\'; break;
                    case '\'': s += '\''; break;
                    case '"': s += '"'; break;
                    case '0': s += '\0'; break;
                    default: s += e; break;
                }
            } else {
                s += c;
                advance();
            }
        }
        Token t; t.kind = TokKind::String; t.text = s; t.line = startLine;
        out.push_back(t);
    }

    void lexNumber() {
        int startLine = line;
        std::string s;
        bool isFloat = false;
        while (isdigit((unsigned char)peek())) { s += advance(); }
        if (peek() == '.' && isdigit((unsigned char)peek(1))) {
            isFloat = true;
            s += advance();
            while (isdigit((unsigned char)peek())) s += advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            size_t save = pos; std::string exp; exp += advance();
            if (peek() == '+' || peek() == '-') exp += advance();
            if (isdigit((unsigned char)peek())) {
                isFloat = true;
                while (isdigit((unsigned char)peek())) exp += advance();
                s += exp;
            } else {
                pos = save; // not actually an exponent, back off
            }
        }
        Token t; t.kind = TokKind::Number; t.line = startLine; t.isFloat = isFloat;
        if (isFloat) t.fval = std::stod(s); else t.ival = std::stoll(s);
        t.text = s;
        out.push_back(t);
    }

    void run() {
        while (true) {
            if (atLineStart) {
                handleIndentation();
                atLineStart = false;
            }
            char c = peek();
            if (c == '\0') break;
            if (c == '#') { while (peek() != '\n' && peek() != '\0') advance(); continue; }
            if (c == '\n') {
                // avoid duplicate NEWLINEs for blank lines; tag the token with
                // the line that's ending, THEN advance the line counter
                if (!out.empty() && out.back().kind != TokKind::Newline &&
                    out.back().kind != TokKind::Indent && out.back().kind != TokKind::Dedent) {
                    push(TokKind::Newline);
                }
                advance(); line++;
                atLineStart = true;
                continue;
            }
            if (c == '\\' && peek(1) == '\n') { advance(); advance(); line++; continue; } // line continuation
            if (c == ' ' || c == '\r') { advance(); continue; }
            if (c == '"' || c == '\'') { lexString(c); continue; }
            if (isdigit((unsigned char)c)) { lexNumber(); continue; }
            if (isalpha((unsigned char)c) || c == '_') {
                std::string s; int startLine = line;
                while (isalnum((unsigned char)peek()) || peek() == '_') s += advance();
                Token t; t.kind = TokKind::Ident; t.text = s; t.line = startLine;
                out.push_back(t);
                continue;
            }
            // operators / punctuation (longest match first)
            static const char* two[] = {"==","!=","<=",">=","**","//","+=","-=","*=","/=","%=", nullptr};
            std::string two_s; two_s += c; two_s += peek(1);
            bool matched = false;
            for (int i = 0; two[i]; i++) {
                if (two_s == two[i]) { push(TokKind::Op, two_s); advance(); advance(); matched = true; break; }
            }
            if (matched) continue;
            static const std::string singles = "+-*/%()[]{}:,.<>=";
            if (singles.find(c) != std::string::npos) {
                push(TokKind::Op, std::string(1, c));
                advance();
                continue;
            }
            err(std::string("unexpected character '") + c + "'");
        }
        // final newline + dedents to close out the file cleanly
        if (!out.empty() && out.back().kind != TokKind::Newline) push(TokKind::Newline);
        while (indentStack.size() > 1) { indentStack.pop_back(); push(TokKind::Dedent); }
        push(TokKind::End);
    }
};

} // namespace

std::vector<Token> lex(const std::string& source, const std::string& filename) {
    Lexer lx(source, filename);
    lx.run();
    return lx.out;
}
