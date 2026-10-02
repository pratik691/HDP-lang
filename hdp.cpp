// ─────────────────────────────────────────────────────────────────────────────
//  HDP (High Definition Programming): a tiny language simpler than Python,
//  implemented in C++17.
//
//  Build:   g++ -std=c++17 -O2 -o hdp hdp.cpp
//           (Windows/MSVC:  cl /std:c++17 /EHsc /O2 hdp.cpp)
//  Run:     hdp program.hdp       run a file
//           hdp                    interactive prompt
//
//  How it works:  source text → Lexer (tokens) → Parser (syntax tree)
//                 → tree-walking interpreter (each node runs itself).
// ─────────────────────────────────────────────────────────────────────────────
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

// ═════════════════════════════ Errors ═══════════════════════════════════════
struct HdpError : runtime_error {
    int line;
    HdpError(const string& msg, int l) : runtime_error(msg), line(l) {}
};

// ═════════════════════════════ Values ═══════════════════════════════════════
// Only five kinds of data: number, text, list, true/false, nothing (+ functions).
struct Env;
struct Stmt;
struct Function;
struct NativeFn;
struct Value;
using List = vector<Value>;
using StmtP = shared_ptr<Stmt>;
using Block = vector<StmtP>;

struct Value {
    enum Type { NOTHING, NUM, BOOL, TEXT, LIST, FN, NATIVE };
    Type type = NOTHING;
    double n = 0;
    bool b = false;
    string s;
    shared_ptr<List> list;
    shared_ptr<Function> fn;
    shared_ptr<NativeFn> native;

    static Value Nothing() { return Value(); }
    static Value Num(double v) { Value x; x.type = NUM; x.n = v; return x; }
    static Value Bool(bool v) { Value x; x.type = BOOL; x.b = v; return x; }
    static Value Text(string v) { Value x; x.type = TEXT; x.s = move(v); return x; }
    static Value Lst(shared_ptr<List> v) { Value x; x.type = LIST; x.list = move(v); return x; }
};

struct Function {
    string name;
    vector<string> params;
    shared_ptr<Block> body;
    shared_ptr<Env> closure;
};

struct NativeFn {
    string name;
    int minArgs, maxArgs;
    function<Value(vector<Value>&, int)> call;
};

string typeName(const Value& v) {
    switch (v.type) {
        case Value::NOTHING: return "nothing";
        case Value::NUM: return "number";
        case Value::BOOL: return "true/false";
        case Value::TEXT: return "text";
        case Value::LIST: return "list";
        default: return "function";
    }
}

string fmtNum(double d) {
    if (std::isfinite(d) && d == std::floor(d) && std::fabs(d) < 1e15) return to_string((long long)d);
    ostringstream o;
    o << setprecision(10) << d;
    return o.str();
}

string toText(const Value& v, bool quoted = false) {
    switch (v.type) {
        case Value::NOTHING: return "nothing";
        case Value::NUM: return fmtNum(v.n);
        case Value::BOOL: return v.b ? "true" : "false";
        case Value::TEXT: return quoted ? "\"" + v.s + "\"" : v.s;
        case Value::LIST: {
            string r = "[";
            for (size_t k = 0; k < v.list->size(); k++) {
                if (k) r += ", ";
                r += toText((*v.list)[k], true);
            }
            return r + "]";
        }
        case Value::FN: return "<function " + v.fn->name + ">";
        case Value::NATIVE: return "<built-in " + v.native->name + ">";
    }
    return "";
}

bool truthy(const Value& v) {
    switch (v.type) {
        case Value::NOTHING: return false;
        case Value::NUM: return v.n != 0;
        case Value::BOOL: return v.b;
        case Value::TEXT: return !v.s.empty();
        case Value::LIST: return !v.list->empty();
        default: return true;
    }
}

bool equal(const Value& a, const Value& b) {
    if (a.type != b.type) return false;
    switch (a.type) {
        case Value::NOTHING: return true;
        case Value::NUM: return a.n == b.n;
        case Value::BOOL: return a.b == b.b;
        case Value::TEXT: return a.s == b.s;
        case Value::LIST:
            if (a.list->size() != b.list->size()) return false;
            for (size_t k = 0; k < a.list->size(); k++)
                if (!equal((*a.list)[k], (*b.list)[k])) return false;
            return true;
        case Value::FN: return a.fn == b.fn;
        case Value::NATIVE: return a.native == b.native;
    }
    return false;
}

// Text that looks like a number becomes a number (used by ask and number()).
bool looksLikeNumber(const string& t, double& out) {
    size_t a = t.find_first_not_of(" \t\r\n");
    if (a == string::npos) return false;
    size_t z = t.find_last_not_of(" \t\r\n");
    string s = t.substr(a, z - a + 1);
    try {
        size_t pos = 0;
        out = stod(s, &pos);
        return pos == s.size();
    } catch (...) {
        return false;
    }
}

// ═════════════════════════════ Variables ════════════════════════════════════
struct Env {
    unordered_map<string, Value> vars;
    shared_ptr<Env> parent;
    bool builtIn = false;
    explicit Env(shared_ptr<Env> p = nullptr) : parent(move(p)) {}

    Value* find(const string& name) {
        for (Env* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
    // x = 5 : update x wherever it already lives, otherwise create it here.
    void set(const string& name, const Value& v, int line) {
        for (Env* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) {
                if (e->builtIn) throw HdpError("'" + name + "' is a built-in name — please pick a different name", line);
                it->second = v;
                return;
            }
        }
        vars[name] = v;
    }
};

// ═════════════════════════════ Syntax tree ══════════════════════════════════
enum class Flow { Normal, Stop, Skip, Return };
Value g_ret;

struct Expr {
    int line;
    explicit Expr(int l) : line(l) {}
    virtual ~Expr() = default;
    virtual Value eval(const shared_ptr<Env>& env) = 0;
};
using ExprP = shared_ptr<Expr>;

struct Stmt {
    int line;
    explicit Stmt(int l) : line(l) {}
    virtual ~Stmt() = default;
    virtual Flow exec(const shared_ptr<Env>& env) = 0;
};

Value callValue(const Value& callee, vector<Value>& args, int line);

Flow runBlock(const Block& b, const shared_ptr<Env>& env) {
    for (auto& s : b) {
        Flow f = s->exec(env);
        if (f != Flow::Normal) return f;
    }
    return Flow::Normal;
}

double needNum(const Value& v, int line, const string& what) {
    if (v.type != Value::NUM) throw HdpError(what + " needs a number, but got " + typeName(v) + " (" + toText(v, true) + ")", line);
    return v.n;
}

// list[0] is the first item, list[-1] is the last.
size_t realIndex(const Value& idx, size_t size, int line) {
    double d = needNum(idx, line, "a position");
    if (d != std::floor(d)) throw HdpError("a position must be a whole number, not " + fmtNum(d), line);
    long long i = (long long)d;
    if (i < 0) i += (long long)size;
    if (i < 0 || (size_t)i >= size)
        throw HdpError("position " + fmtNum(d) + " doesn't exist (there are only " + to_string(size) + " items)", line);
    return (size_t)i;
}

Value binaryOp(const string& op, const Value& a, const Value& b, int line) {
    if (op == "==") return Value::Bool(equal(a, b));
    if (op == "!=") return Value::Bool(!equal(a, b));
    if (op == "in") {
        if (b.type == Value::LIST) {
            for (auto& x : *b.list) if (equal(x, a)) return Value::Bool(true);
            return Value::Bool(false);
        }
        if (b.type == Value::TEXT) return Value::Bool(b.s.find(toText(a)) != string::npos);
        throw HdpError("'in' needs a list or text on the right, not " + typeName(b), line);
    }
    if (op == "+") {
        if (a.type == Value::TEXT || b.type == Value::TEXT) return Value::Text(toText(a) + toText(b));  // "Age: " + 20
        if (a.type == Value::LIST && b.type == Value::LIST) {
            auto r = make_shared<List>(*a.list);
            r->insert(r->end(), b.list->begin(), b.list->end());
            return Value::Lst(r);
        }
    }
    if (op == "*" && a.type == Value::TEXT && b.type == Value::NUM) {   // "-" * 10
        string r;
        for (int k = 0; k < (int)b.n; k++) r += a.s;
        return Value::Text(r);
    }
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        int c;
        if (a.type == Value::NUM && b.type == Value::NUM) c = (a.n < b.n) ? -1 : (a.n > b.n);
        else if (a.type == Value::TEXT && b.type == Value::TEXT) { c = a.s.compare(b.s); c = (c < 0) ? -1 : (c > 0); }
        else throw HdpError("can't compare " + typeName(a) + " with " + typeName(b), line);
        if (op == "<") return Value::Bool(c < 0);
        if (op == ">") return Value::Bool(c > 0);
        if (op == "<=") return Value::Bool(c <= 0);
        return Value::Bool(c >= 0);
    }
    if (a.type != Value::NUM || b.type != Value::NUM)
        throw HdpError("can't do " + toText(a, true) + " " + op + " " + toText(b, true) +
                        " (" + typeName(a) + " " + op + " " + typeName(b) + ")", line);
    if (op == "+") return Value::Num(a.n + b.n);
    if (op == "-") return Value::Num(a.n - b.n);
    if (op == "*") return Value::Num(a.n * b.n);
    if (op == "/") {
        if (b.n == 0) throw HdpError("can't divide by zero", line);
        return Value::Num(a.n / b.n);   // 7 / 2 is 3.5 — no int/float surprises
    }
    if (op == "%") {
        if (b.n == 0) throw HdpError("can't divide by zero", line);
        return Value::Num(std::fmod(a.n, b.n));
    }
    if (op == "^") return Value::Num(std::pow(a.n, b.n));
    throw HdpError("unknown operator " + op, line);
}

// ─── expressions ───
struct Literal : Expr {
    Value v;
    Literal(Value x, int l) : Expr(l), v(move(x)) {}
    Value eval(const shared_ptr<Env>&) override { return v; }
};

struct Var : Expr {
    string name;
    Var(string n, int l) : Expr(l), name(move(n)) {}
    Value eval(const shared_ptr<Env>& env) override {
        Value* v = env->find(name);
        if (!v) throw HdpError("I don't know what '" + name + "' is — did you set it first?", line);
        return *v;
    }
};

struct ListLit : Expr {
    vector<ExprP> items;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        auto l = make_shared<List>();
        for (auto& e : items) l->push_back(e->eval(env));
        return Value::Lst(l);
    }
};

struct Unary : Expr {
    string op;
    ExprP rhs;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        Value v = rhs->eval(env);
        if (op == "not") return Value::Bool(!truthy(v));
        return Value::Num(-needNum(v, line, "'-'"));
    }
};

struct Binary : Expr {
    string op;
    ExprP l, r;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        Value a = l->eval(env);
        Value b = r->eval(env);
        return binaryOp(op, a, b, line);
    }
};

struct Logic : Expr {
    bool isAnd = true;
    ExprP l, r;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        bool left = truthy(l->eval(env));
        if (isAnd && !left) return Value::Bool(false);
        if (!isAnd && left) return Value::Bool(true);
        return Value::Bool(truthy(r->eval(env)));
    }
};

struct Index : Expr {
    ExprP obj, idx;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        Value o = obj->eval(env);
        Value k = idx->eval(env);
        if (o.type == Value::LIST) return (*o.list)[realIndex(k, o.list->size(), line)];
        if (o.type == Value::TEXT) return Value::Text(string(1, o.s[realIndex(k, o.s.size(), line)]));
        throw HdpError("can't use [ ] on " + typeName(o), line);
    }
};

struct Call : Expr {
    ExprP callee;
    vector<ExprP> args;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        Value f = callee->eval(env);
        vector<Value> av;
        for (auto& a : args) av.push_back(a->eval(env));
        return callValue(f, av, line);
    }
};

struct Ask : Expr {   // name = ask "What's your name? "
    ExprP prompt;
    using Expr::Expr;
    Value eval(const shared_ptr<Env>& env) override {
        if (prompt) cout << toText(prompt->eval(env)) << flush;
        string s;
        if (!getline(cin, s)) throw HdpError("ask: there's no more input to read", line);
        if (!s.empty() && s.back() == '\r') s.pop_back();
        double d;
        if (looksLikeNumber(s, d)) return Value::Num(d);   // typing 42 gives the number 42
        return Value::Text(s);
    }
};

// ─── statements ───
struct ExprStmt : Stmt {
    ExprP e;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override { e->eval(env); return Flow::Normal; }
};

struct Say : Stmt {
    vector<ExprP> parts;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        for (size_t k = 0; k < parts.size(); k++) {
            if (k) cout << ' ';
            cout << toText(parts[k]->eval(env));
        }
        cout << '\n' << flush;
        return Flow::Normal;
    }
};

struct Assign : Stmt {   // x = 5   x += 1   list[0] = 9
    ExprP target, value;
    string op;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        Value v = value->eval(env);
        if (auto var = dynamic_cast<Var*>(target.get())) {
            if (op != "=") v = binaryOp(op.substr(0, 1), var->eval(env), v, line);
            env->set(var->name, v, line);
            return Flow::Normal;
        }
        auto ix = static_cast<Index*>(target.get());
        Value o = ix->obj->eval(env);
        Value k = ix->idx->eval(env);
        if (o.type == Value::TEXT) throw HdpError("text can't be changed letter by letter — build a new text instead", line);
        if (o.type != Value::LIST) throw HdpError("can't use [ ] on " + typeName(o), line);
        Value& slot = (*o.list)[realIndex(k, o.list->size(), line)];
        slot = (op == "=") ? v : binaryOp(op.substr(0, 1), slot, v, line);
        return Flow::Normal;
    }
};

struct If : Stmt {
    vector<pair<ExprP, Block>> branches;   // if / else if ...
    Block elseBody;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        for (auto& [cond, body] : branches)
            if (truthy(cond->eval(env))) return runBlock(body, env);
        return runBlock(elseBody, env);
    }
};

// Shared loop logic: stop = break, skip = continue.
#define LOOP_BODY(body)                              \
    {                                                \
        Flow f = runBlock(body, env);                \
        if (f == Flow::Stop) break;                  \
        if (f == Flow::Return) return f;             \
    }

struct While : Stmt {
    ExprP cond;
    Block body;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        while (truthy(cond->eval(env))) LOOP_BODY(body)
        return Flow::Normal;
    }
};

struct Repeat : Stmt {   // repeat 5 times
    ExprP count;
    Block body;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        double n = needNum(count->eval(env), line, "repeat");
        for (long long k = 0; k < (long long)n; k++) LOOP_BODY(body)
        return Flow::Normal;
    }
};

struct ForRange : Stmt {   // for i from 1 to 10 [step 2]   (counts down by itself if needed)
    string var;
    ExprP from, to, step;
    Block body;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        double a = needNum(from->eval(env), line, "for ... from");
        double b = needNum(to->eval(env), line, "for ... to");
        double s = step ? needNum(step->eval(env), line, "step") : (a <= b ? 1 : -1);
        if (s == 0) throw HdpError("step can't be 0", line);
        for (double i = a; s > 0 ? i <= b + 1e-9 : i >= b - 1e-9; i += s) {
            env->set(var, Value::Num(i), line);
            LOOP_BODY(body)
        }
        return Flow::Normal;
    }
};

struct ForEach : Stmt {   // for fruit in fruits
    string var;
    ExprP iter;
    Block body;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        Value c = iter->eval(env);
        List items;
        if (c.type == Value::LIST) items = *c.list;   // copy, so changing the list inside the loop is safe
        else if (c.type == Value::TEXT) for (char ch : c.s) items.push_back(Value::Text(string(1, ch)));
        else if (c.type == Value::NUM) for (long long k = 1; k <= (long long)c.n; k++) items.push_back(Value::Num((double)k));
        else throw HdpError("can't loop over " + typeName(c), line);
        for (auto& item : items) {
            env->set(var, item, line);
            LOOP_BODY(body)
        }
        return Flow::Normal;
    }
};

struct FuncDef : Stmt {
    shared_ptr<Function> proto;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        auto f = make_shared<Function>(*proto);
        f->closure = env;
        Value v;
        v.type = Value::FN;
        v.fn = f;
        env->set(f->name, v, line);
        return Flow::Normal;
    }
};

struct Return : Stmt {
    ExprP e;
    using Stmt::Stmt;
    Flow exec(const shared_ptr<Env>& env) override {
        g_ret = e ? e->eval(env) : Value::Nothing();
        return Flow::Return;
    }
};

struct Jump : Stmt {   // stop / skip
    Flow kind;
    Jump(Flow k, int l) : Stmt(l), kind(k) {}
    Flow exec(const shared_ptr<Env>&) override { return kind; }
};

// ═══════════════════════════ Calling functions ══════════════════════════════
int g_depth = 0;

Value callValue(const Value& callee, vector<Value>& args, int line) {
    if (callee.type == Value::NATIVE) {
        NativeFn& n = *callee.native;
        if ((int)args.size() < n.minArgs || (n.maxArgs >= 0 && (int)args.size() > n.maxArgs)) {
            string want = n.minArgs == n.maxArgs ? to_string(n.minArgs)
                        : n.maxArgs < 0 ? "at least " + to_string(n.minArgs)
                        : to_string(n.minArgs) + " to " + to_string(n.maxArgs);
            throw HdpError(n.name + "() needs " + want + " value(s), but got " + to_string(args.size()), line);
        }
        return n.call(args, line);
    }
    if (callee.type != Value::FN) throw HdpError(toText(callee, true) + " is " + typeName(callee) + ", not a function", line);
    Function& f = *callee.fn;
    if (args.size() != f.params.size())
        throw HdpError(f.name + "() needs " + to_string(f.params.size()) + " value(s), but got " + to_string(args.size()), line);
    if (g_depth >= 1000) throw HdpError("too much recursion — " + f.name + "() keeps calling itself (over 1000 deep)", line);
    struct Guard { Guard() { ++g_depth; } ~Guard() { --g_depth; } } guard;

    auto env = make_shared<Env>(f.closure);
    for (size_t k = 0; k < args.size(); k++) env->vars[f.params[k]] = args[k];
    if (runBlock(*f.body, env) == Flow::Return) return g_ret;
    return Value::Nothing();
}

// ═══════════════════════════════ Lexer ══════════════════════════════════════
enum class TT { Num, Text, Name, Word, Op, Newline, End };
struct Token {
    TT type;
    string text;
    double num;
    int line;
};

const unordered_set<string> KEYWORDS = {
    "say", "ask", "if", "else", "while", "repeat", "times", "for", "from", "to", "step", "in",
    "function", "return", "stop", "skip", "end", "and", "or", "not", "is", "mod",
    "true", "false", "yes", "no", "nothing"};

vector<Token> lex(const string& src) {
    vector<Token> out;
    size_t i = 0, n = src.size();
    int line = 1, depth = 0;   // newlines inside ( ) and [ ] are ignored
    auto push = [&](TT type, const string& text, double num = 0) { out.push_back(Token{type, text, num, line}); };
    auto newline = [&]() {
        if (depth == 0 && !out.empty() && out.back().type != TT::Newline) push(TT::Newline, "end of line");
    };
    static const vector<string> two = {"==", "!=", "<=", ">=", "+=", "-=", "*=", "/="};
    while (i < n) {
        char c = src[i];
        if (c == '\n') { newline(); line++; i++; continue; }
        if (c == ';') { newline(); i++; continue; }
        if (isspace((unsigned char)c)) { i++; continue; }
        if (c == '#' || (c == '/' && i + 1 < n && src[i + 1] == '/')) {   // comments
            while (i < n && src[i] != '\n') i++;
            continue;
        }
        if (isdigit((unsigned char)c) || (c == '.' && i + 1 < n && isdigit((unsigned char)src[i + 1]))) {
            size_t st = i;
            while (i < n && (isdigit((unsigned char)src[i]) || src[i] == '.' || src[i] == '_')) i++;
            string t;
            for (size_t k = st; k < i; k++) if (src[k] != '_') t += src[k];   // 1_000_000 is allowed
            push(TT::Num, t, stod(t));
            continue;
        }
        if (c == '"' || c == '\'') {
            char q = c;
            int start = line;
            string s;
            i++;
            while (i < n && src[i] != q) {
                if (src[i] == '\n') throw HdpError("this text is missing its closing " + string(1, q), start);
                if (src[i] == '\\' && i + 1 < n) {
                    char e = src[i + 1];
                    s += (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
                    i += 2;
                } else {
                    s += src[i++];
                }
            }
            if (i >= n) throw HdpError("this text is missing its closing " + string(1, q), start);
            i++;
            push(TT::Text, s);
            continue;
        }
        if (isalpha((unsigned char)c) || c == '_') {
            size_t st = i;
            while (i < n && (isalnum((unsigned char)src[i]) || src[i] == '_')) i++;
            string w = src.substr(st, i - st);
            push(KEYWORDS.count(w) ? TT::Word : TT::Name, w);
            continue;
        }
        if (i + 1 < n && find(two.begin(), two.end(), src.substr(i, 2)) != two.end()) {
            push(TT::Op, src.substr(i, 2));
            i += 2;
            continue;
        }
        if (string("+-*/%^<>=()[],").find(c) != string::npos) {
            if (c == '(' || c == '[') depth++;
            if ((c == ')' || c == ']') && depth > 0) depth--;
            push(TT::Op, string(1, c));
            i++;
            continue;
        }
        throw HdpError(string("I don't understand the symbol '") + c + "'", line);
    }
    newline();
    out.push_back(Token{TT::End, "end of file", 0, line});
    return out;
}

// ═══════════════════════════════ Parser ═════════════════════════════════════
struct Parser {
    vector<Token> t;
    size_t p = 0;
    int loops = 0, functions = 0;
    explicit Parser(vector<Token> toks) : t(move(toks)) {}

    const Token& peek(size_t k = 0) const { return t[min(p + k, t.size() - 1)]; }
    bool is(const string& s, size_t k = 0) const {
        const Token& tk = peek(k);
        return (tk.type == TT::Op || tk.type == TT::Word) && tk.text == s;
    }
    bool match(const string& s) {
        if (is(s)) { p++; return true; }
        return false;
    }
    static string show(const Token& tk) {
        if (tk.type == TT::Newline || tk.type == TT::End) return tk.text;
        if (tk.type == TT::Text) return "\"" + tk.text + "\"";
        return "'" + tk.text + "'";
    }
    [[noreturn]] void fail(const string& msg) const { throw HdpError(msg, peek().line); }
    void expect(const string& s, const string& why) {
        if (!match(s)) fail("expected '" + s + "' " + why + ", but found " + show(peek()));
    }
    string name(const string& why) {
        if (peek().type != TT::Name) {
            if (peek().type == TT::Word) fail("'" + peek().text + "' is a special word in HDP and can't be used as a name");
            fail("expected a name " + why + ", but found " + show(peek()));
        }
        return t[p++].text;
    }
    void skipNewlines() { while (peek().type == TT::Newline) p++; }
    void endOfLine() {
        if (peek().type == TT::End) return;
        if (peek().type != TT::Newline) fail("expected the line to end here, but found " + show(peek()));
        skipNewlines();
    }

    Block program() {
        Block b;
        skipNewlines();
        while (peek().type != TT::End) b.push_back(statement());
        return b;
    }

    // Reads lines until one of the stop-words ('end', 'else').
    Block block(const string& opener, int openLine) {
        Block b;
        skipNewlines();
        while (!is("end") && !is("else")) {
            if (peek().type == TT::End)
                throw HdpError("the '" + opener + "' on line " + to_string(openLine) + " is missing its 'end'", peek().line);
            b.push_back(statement());
        }
        return b;
    }
    void closeBlock(const string& opener, int openLine) {
        if (is("else")) fail("'else' only goes with 'if' (this block was opened by '" + opener + "' on line " + to_string(openLine) + ")");
        expect("end", "to close the '" + opener + "' on line " + to_string(openLine));
        endOfLine();
    }
    Block loopBlock(const string& opener, int line) {
        loops++;
        Block b = block(opener, line);
        loops--;
        closeBlock(opener, line);
        return b;
    }

    StmtP statement() {
        int line = peek().line;

        if (match("say")) {
            auto s = make_shared<Say>(line);
            if (peek().type != TT::Newline && peek().type != TT::End) {
                do s->parts.push_back(expression());
                while (match(","));
            }
            endOfLine();
            return s;
        }
        if (match("if")) {
            auto s = make_shared<If>(line);
            ExprP cond = expression();
            endOfLine();
            s->branches.push_back({cond, block("if", line)});
            while (match("else")) {
                if (match("if")) {
                    ExprP c = expression();
                    endOfLine();
                    s->branches.push_back({c, block("if", line)});
                } else {
                    endOfLine();
                    s->elseBody = block("if", line);
                    if (is("else")) fail("there's already an 'else' — another 'else' can't come after it");
                    break;
                }
            }
            expect("end", "to close the 'if' on line " + to_string(line));
            endOfLine();
            return s;
        }
        if (match("while")) {
            auto s = make_shared<While>(line);
            s->cond = expression();
            endOfLine();
            s->body = loopBlock("while", line);
            return s;
        }
        if (match("repeat")) {
            auto s = make_shared<Repeat>(line);
            s->count = expression();
            match("times");
            endOfLine();
            s->body = loopBlock("repeat", line);
            return s;
        }
        if (match("for")) {
            string var = name("after 'for'");
            if (match("from")) {
                auto s = make_shared<ForRange>(line);
                s->var = var;
                s->from = expression();
                expect("to", "in 'for " + var + " from ... to ...'");
                s->to = expression();
                if (match("step")) s->step = expression();
                endOfLine();
                s->body = loopBlock("for", line);
                return s;
            }
            if (match("in")) {
                auto s = make_shared<ForEach>(line);
                s->var = var;
                s->iter = expression();
                endOfLine();
                s->body = loopBlock("for", line);
                return s;
            }
            fail("write either 'for " + var + " from 1 to 10' or 'for " + var + " in my_list'");
        }
        if (match("function")) {
            auto f = make_shared<Function>();
            f->name = name("after 'function'");
            expect("(", "after the function name (even if it's empty, like greet())");
            if (!is(")")) {
                do f->params.push_back(name("for an input"));
                while (match(","));
            }
            expect(")", "after the inputs");
            endOfLine();
            int savedLoops = loops;
            loops = 0;
            functions++;
            f->body = make_shared<Block>(block("function", line));
            functions--;
            loops = savedLoops;
            closeBlock("function", line);
            auto s = make_shared<FuncDef>(line);
            s->proto = f;
            return s;
        }
        if (match("return")) {
            auto s = make_shared<Return>(line);
            if (peek().type != TT::Newline && peek().type != TT::End) s->e = expression();
            endOfLine();
            return s;
        }
        if (is("stop") || is("skip")) {
            bool stop = t[p++].text == "stop";
            if (loops == 0) throw HdpError("'" + string(stop ? "stop" : "skip") + "' only works inside a loop", line);
            endOfLine();
            return make_shared<Jump>(stop ? Flow::Stop : Flow::Skip, line);
        }
        if (is("end")) fail("this 'end' doesn't close anything");
        if (is("else")) fail("this 'else' doesn't belong to any 'if'");

        // assignment?   x = ...    x += ...    list[2] = ...
        if (isAssignment()) {
            auto s = make_shared<Assign>(line);
            s->target = postfix();
            s->op = t[p++].text;
            s->value = expression();
            endOfLine();
            return s;
        }
        auto s = make_shared<ExprStmt>(line);
        s->e = expression();
        if (!dynamic_cast<Call*>(s->e.get()) && !dynamic_cast<Ask*>(s->e.get()))
            throw HdpError("this line calculates something but doesn't use it — did you mean 'say ...'?", line);
        endOfLine();
        return s;
    }

    bool isAssignment() const {
        if (peek().type != TT::Name) return false;
        size_t j = p + 1;
        while (j < t.size() && t[j].type == TT::Op && t[j].text == "[") {   // skip over [ ... ]
            int d = 0;
            for (; j < t.size(); j++) {
                if (t[j].type == TT::Op && t[j].text == "[") d++;
                if (t[j].type == TT::Op && t[j].text == "]" && --d == 0) break;
            }
            j++;
        }
        if (j >= t.size() || t[j].type != TT::Op) return false;
        const string& op = t[j].text;
        return op == "=" || op == "+=" || op == "-=" || op == "*=" || op == "/=";
    }

    // ─── expressions (lowest precedence first) ───
    ExprP expression() { return logicOr(); }

    ExprP logicOr() {
        ExprP e = logicAnd();
        while (is("or")) {
            auto node = make_shared<Logic>(peek().line);
            p++;
            node->isAnd = false;
            node->l = e;
            node->r = logicAnd();
            e = node;
        }
        return e;
    }
    ExprP logicAnd() {
        ExprP e = logicNot();
        while (is("and")) {
            auto node = make_shared<Logic>(peek().line);
            p++;
            node->l = e;
            node->r = logicNot();
            e = node;
        }
        return e;
    }
    ExprP logicNot() {
        if (is("not")) {
            auto u = make_shared<Unary>(peek().line);
            p++;
            u->op = "not";
            u->rhs = logicNot();
            return u;
        }
        return comparison();
    }
    ExprP makeBinary(const string& op, ExprP l, ExprP r, int line) {
        auto b = make_shared<Binary>(line);
        b->op = op;
        b->l = l;
        b->r = r;
        return b;
    }
    ExprP comparison() {
        ExprP e = sum();
        while (true) {
            int line = peek().line;
            string op;
            if (is("==") || is("=")) op = "==";            // in a condition, = and == both mean "equals"
            else if (is("!=")) op = "!=";
            else if (is("<") || is(">") || is("<=") || is(">=")) op = peek().text;
            else if (is("in")) op = "in";
            else if (is("is")) {                         // x is 5    x is not 5
                p++;
                bool neg = match("not");
                e = makeBinary(neg ? "!=" : "==", e, sum(), line);
                continue;
            } else return e;
            p++;
            e = makeBinary(op, e, sum(), line);
        }
    }
    ExprP sum() {
        ExprP e = product();
        while (is("+") || is("-")) {
            int line = peek().line;
            string op = t[p++].text;
            e = makeBinary(op, e, product(), line);
        }
        return e;
    }
    ExprP product() {
        ExprP e = unary();
        while (is("*") || is("/") || is("%") || is("mod")) {
            int line = peek().line;
            string op = t[p++].text;
            if (op == "mod") op = "%";
            e = makeBinary(op, e, unary(), line);
        }
        return e;
    }
    ExprP unary() {
        if (is("-")) {
            auto u = make_shared<Unary>(peek().line);
            p++;
            u->op = "-";
            u->rhs = unary();
            return u;
        }
        return power();
    }
    ExprP power() {
        ExprP e = postfix();
        if (is("^")) {
            int line = peek().line;
            p++;
            return makeBinary("^", e, unary(), line);   // 2 ^ 3 = 8
        }
        return e;
    }
    ExprP postfix() {
        ExprP e = primary();
        while (true) {
            int line = peek().line;
            if (match("(")) {
                auto c = make_shared<Call>(line);
                c->callee = e;
                if (!is(")")) {
                    do c->args.push_back(expression());
                    while (match(","));
                }
                expect(")", "to finish the function call");
                e = c;
            } else if (match("[")) {
                auto ix = make_shared<Index>(line);
                ix->obj = e;
                ix->idx = expression();
                expect("]", "after the position");
                e = ix;
            } else {
                return e;
            }
        }
    }
    ExprP primary() {
        Token tk = peek();
        int line = tk.line;
        if (tk.type == TT::Num) { p++; return make_shared<Literal>(Value::Num(tk.num), line); }
        if (tk.type == TT::Text) { p++; return make_shared<Literal>(Value::Text(tk.text), line); }
        if (tk.type == TT::Name) { p++; return make_shared<Var>(tk.text, line); }
        if (match("true") || match("yes")) return make_shared<Literal>(Value::Bool(true), line);
        if (match("false") || match("no")) return make_shared<Literal>(Value::Bool(false), line);
        if (match("nothing")) return make_shared<Literal>(Value::Nothing(), line);
        if (match("ask")) {
            auto a = make_shared<Ask>(line);
            if (peek().type == TT::Text || peek().type == TT::Name || is("(")) a->prompt = postfix();
            return a;
        }
        if (match("(")) {
            ExprP e = expression();
            expect(")", "to close the '('");
            return e;
        }
        if (match("[")) {
            auto l = make_shared<ListLit>(line);
            while (!is("]")) {
                l->items.push_back(expression());
                if (!match(",")) break;
            }
            expect("]", "to close the list");
            return l;
        }
        fail("didn't expect " + show(tk) + " here");
    }
};

// ═══════════════════════════ Built-in functions ═════════════════════════════
shared_ptr<Env> makeBuiltins() {
    auto g = make_shared<Env>();
    g->builtIn = true;
    using A = vector<Value>&;
    auto def = [&](const string& name, int minA, int maxA, function<Value(vector<Value>&, int)> fn) {
        auto nf = make_shared<NativeFn>();
        nf->name = name;
        nf->minArgs = minA;
        nf->maxArgs = maxA;
        nf->call = move(fn);
        Value v;
        v.type = Value::NATIVE;
        v.native = nf;
        g->vars[name] = v;
    };
    auto needList = [](const Value& v, int line, const string& fn) -> List& {
        if (v.type != Value::LIST) throw HdpError(fn + "() needs a list, but got " + typeName(v), line);
        return *v.list;
    };
    auto needText = [](const Value& v, int line, const string& fn) -> const string& {
        if (v.type != Value::TEXT) throw HdpError(fn + "() needs text, but got " + typeName(v), line);
        return v.s;
    };

    // ── lists & text ──
    auto sizeFn = [](A a, int line) {
        if (a[0].type == Value::LIST) return Value::Num((double)a[0].list->size());
        if (a[0].type == Value::TEXT) return Value::Num((double)a[0].s.size());
        throw HdpError("size() needs a list or text, but got " + typeName(a[0]), line);
    };
    def("size", 1, 1, sizeFn);
    def("length", 1, 1, sizeFn);
    def("add", 2, 2, [needList](A a, int l) { needList(a[0], l, "add").push_back(a[1]); return Value::Nothing(); });
    def("insert", 3, 3, [needList](A a, int l) {
        List& v = needList(a[0], l, "insert");
        size_t i = (a[1].type == Value::NUM && a[1].n == (double)v.size()) ? v.size() : realIndex(a[1], v.size(), l);
        v.insert(v.begin() + (long)i, a[2]);
        return Value::Nothing();
    });
    def("remove", 1, 2, [needList](A a, int l) {   // remove(list) removes the last; remove(list, 0) the first
        List& v = needList(a[0], l, "remove");
        if (v.empty()) throw HdpError("remove(): the list is already empty", l);
        size_t i = a.size() == 2 ? realIndex(a[1], v.size(), l) : v.size() - 1;
        Value x = v[i];
        v.erase(v.begin() + (long)i);
        return x;
    });
    def("sort", 1, 1, [needList](A a, int l) {   // gives back a sorted copy
        List v = needList(a[0], l, "sort");
        sort(v.begin(), v.end(), [l](const Value& x, const Value& y) { return binaryOp("<", x, y, l).b; });
        return Value::Lst(make_shared<List>(v));
    });
    def("reverse", 1, 1, [](A a, int l) {
        if (a[0].type == Value::TEXT) { string s = a[0].s; reverse(s.begin(), s.end()); return Value::Text(s); }
        if (a[0].type != Value::LIST) throw HdpError("reverse() needs a list or text", l);
        List v = *a[0].list;
        reverse(v.begin(), v.end());
        return Value::Lst(make_shared<List>(v));
    });
    def("sum", 1, 1, [needList](A a, int l) {
        double s = 0;
        for (auto& x : needList(a[0], l, "sum")) s += needNum(x, l, "sum()");
        return Value::Num(s);
    });
    def("average", 1, 1, [needList](A a, int l) {
        List& v = needList(a[0], l, "average");
        if (v.empty()) throw HdpError("average() of an empty list", l);
        double s = 0;
        for (auto& x : v) s += needNum(x, l, "average()");
        return Value::Num(s / (double)v.size());
    });
    auto extreme = [&](const string& nm, bool wantMin) {
        def(nm, 1, -1, [nm, wantMin](A a, int l) {   // min(3, 7)  or  min(my_list)
            List v = (a.size() == 1 && a[0].type == Value::LIST) ? *a[0].list : a;
            if (v.empty()) throw HdpError(nm + "() of nothing", l);
            Value best = v[0];
            for (auto& x : v)
                if (binaryOp(wantMin ? "<" : ">", x, best, l).b) best = x;
            return best;
        });
    };
    extreme("min", true);
    extreme("max", false);
    def("upper", 1, 1, [needText](A a, int l) { string s = needText(a[0], l, "upper"); for (auto& c : s) c = (char)toupper((unsigned char)c); return Value::Text(s); });
    def("lower", 1, 1, [needText](A a, int l) { string s = needText(a[0], l, "lower"); for (auto& c : s) c = (char)tolower((unsigned char)c); return Value::Text(s); });
    def("trim", 1, 1, [needText](A a, int l) {
        const string& s = needText(a[0], l, "trim");
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == string::npos) return Value::Text("");
        return Value::Text(s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1));
    });
    def("split", 1, 2, [needText](A a, int l) {   // split("a,b,c", ",")  — default splits on spaces
        const string& s = needText(a[0], l, "split");
        auto out = make_shared<List>();
        if (a.size() == 1) {
            istringstream in(s);
            string w;
            while (in >> w) out->push_back(Value::Text(w));
            return Value::Lst(out);
        }
        const string& sep = needText(a[1], l, "split");
        if (sep.empty()) { for (char c : s) out->push_back(Value::Text(string(1, c))); return Value::Lst(out); }
        size_t start = 0, pos;
        while ((pos = s.find(sep, start)) != string::npos) { out->push_back(Value::Text(s.substr(start, pos - start))); start = pos + sep.size(); }
        out->push_back(Value::Text(s.substr(start)));
        return Value::Lst(out);
    });
    def("join", 1, 2, [needList, needText](A a, int l) {   // join(["a","b"], ", ")
        string sep = a.size() == 2 ? needText(a[1], l, "join") : " ";
        string r;
        List& v = needList(a[0], l, "join");
        for (size_t k = 0; k < v.size(); k++) { if (k) r += sep; r += toText(v[k]); }
        return Value::Text(r);
    });
    def("replace", 3, 3, [needText](A a, int l) {
        string s = needText(a[0], l, "replace");
        const string& from = needText(a[1], l, "replace");
        const string& to = needText(a[2], l, "replace");
        if (from.empty()) return Value::Text(s);
        for (size_t pos = 0; (pos = s.find(from, pos)) != string::npos; pos += to.size()) s.replace(pos, from.size(), to);
        return Value::Text(s);
    });
    // ── conversions ──
    def("number", 1, 1, [](A a, int l) {
        if (a[0].type == Value::NUM) return a[0];
        double d;
        if (a[0].type == Value::TEXT && looksLikeNumber(a[0].s, d)) return Value::Num(d);
        throw HdpError(toText(a[0], true) + " can't be turned into a number", l);
    });
    def("text", 1, 1, [](A a, int) { return Value::Text(toText(a[0])); });
    def("type", 1, 1, [](A a, int) { return Value::Text(typeName(a[0])); });
    // ── maths ──
    def("round", 1, 2, [](A a, int l) {   // round(3.14159, 2) = 3.14
        double x = needNum(a[0], l, "round()");
        double places = a.size() == 2 ? needNum(a[1], l, "round()") : 0;
        double m = std::pow(10.0, places);
        return Value::Num(std::round(x * m) / m);
    });
    def("floor", 1, 1, [](A a, int l) { return Value::Num(std::floor(needNum(a[0], l, "floor()"))); });
    def("ceil", 1, 1, [](A a, int l) { return Value::Num(std::ceil(needNum(a[0], l, "ceil()"))); });
    def("abs", 1, 1, [](A a, int l) { return Value::Num(std::fabs(needNum(a[0], l, "abs()"))); });
    def("sqrt", 1, 1, [](A a, int l) {
        double x = needNum(a[0], l, "sqrt()");
        if (x < 0) throw HdpError("sqrt() of a negative number", l);
        return Value::Num(std::sqrt(x));
    });
    static mt19937 rng((unsigned)time(nullptr));
    def("random", 0, 2, [](A a, int l) {   // random() 0..1,  random(6) 1..6,  random(5, 10) 5..10
        if (a.empty()) return Value::Num(uniform_real_distribution<double>(0, 1)(rng));
        long long lo = a.size() == 2 ? (long long)needNum(a[0], l, "random()") : 1;
        long long hi = (long long)needNum(a.back(), l, "random()");
        if (lo > hi) swap(lo, hi);
        return Value::Num((double)uniform_int_distribution<long long>(lo, hi)(rng));
    });
    g->vars["pi"] = Value::Num(3.14159265358979323846);
    return g;
}

// ═══════════════════════════════ Running ════════════════════════════════════
void run(const string& src, const shared_ptr<Env>& globals) {
    Parser parser(lex(src));
    Block program = parser.program();
    // Functions can be used before the line where they're written.
    for (auto& s : program)
        if (dynamic_cast<FuncDef*>(s.get())) s->exec(globals);
    for (auto& s : program) {
        if (dynamic_cast<FuncDef*>(s.get())) continue;
        if (s->exec(globals) == Flow::Return) break;
    }
}

void repl(const shared_ptr<Env>& globals) {
    cout << "HDP (High Definition Programming) — simpler than Python. Type 'exit' to quit.\n";
    static const unordered_set<string> openers = {"if", "while", "repeat", "for", "function"};
    string buffer, line;
    int open = 0;
    while (true) {
        cout << (buffer.empty() ? ">>> " : "... ") << flush;
        if (!getline(cin, line)) break;
        if (buffer.empty() && (line == "exit" || line == "quit")) break;
        buffer += line + "\n";
        istringstream words(line);
        string first;
        words >> first;
        if (openers.count(first)) open++;
        if (first == "end") open--;
        if (open > 0) continue;   // wait for the matching 'end'
        open = 0;
        string src = buffer;
        buffer.clear();
        try {
            Parser parser(lex(src));
            // A bare value like  2 + 3  just gets shown.
            size_t save = parser.p;
            bool shown = false;
            ExprP bare;
            if (!parser.isAssignment() && parser.peek().type != TT::End) {
                try {
                    ExprP e = parser.expression();
                    parser.skipNewlines();
                    if (parser.peek().type == TT::End) bare = e;
                } catch (const HdpError&) {
                }
            }
            if (bare) {
                Value v = bare->eval(globals);
                if (v.type != Value::NOTHING) cout << toText(v, true) << "\n";
                shown = true;
            }
            if (!shown) {
                parser.p = save;
                Block prog = parser.program();
                for (auto& s : prog) s->exec(globals);
            }
        } catch (const HdpError& e) {
            cout << "Oops: " << e.what() << "\n";
        } catch (const exception& e) {
            cout << "Oops: " << e.what() << "\n";
        }
    }
}

int main(int argc, char** argv) {
    auto globals = make_shared<Env>(makeBuiltins());
    if (argc < 2) {
        repl(globals);
        return 0;
    }
    ifstream in(argv[1]);
    if (!in) {
        cerr << "hdp: can't open the file '" << argv[1] << "'\n";
        return 1;
    }
    stringstream ss;
    ss << in.rdbuf();
    try {
        run(ss.str(), globals);
        return 0;
    } catch (const HdpError& e) {
        cout.flush();
        cerr << "\nOops! Line " << e.line << ": " << e.what() << "\n";
    } catch (const exception& e) {
        cout.flush();
        cerr << "\nOops! " << e.what() << "\n";
    }
    return 1;
}
