/*
 * ============================================================== *
 *  OpenGlider                                                    *
 *  Copyright (c) 2026 Alexander Blohmè <alexander.blohme@gmail.com> *
 *  SPDX-License-Identifier: LGPL-3.0-or-later                    *
 *  Licensed under the GNU Lesser General Public License,         *
 *  version 3 or later. See COPYING.LESSER and COPYING.           *
 * ============================================================== *
 *
 *  Date: 2026-09-11
 *  File: ScriptParser.cpp
 *
 *  Desc:
 *  Implementation of ScriptParser (combat execution, rotation
 *  scripting, and class behaviors).
 *
 *
 */
#include "og/combat/script/ScriptParser.h"

#include <cctype>
#include <charconv>
#include <format>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace og::script
{

std::string Diagnostic::toString(const std::string& file) const
{
    const std::string prefix = file.empty() ? std::string() : file + ":";
    return std::format("{}{}:{}: {}: {}", prefix, loc.line, loc.column, warning ? "warning" : "error", message);
}

std::string valueToString(const Value& v)
{
    struct Visitor
    {
        std::string operator()(std::monostate) const
        {
            return "nil";
        }

        std::string operator()(bool b) const
        {
            return b ? "true" : "false";
        }

        std::string operator()(double d) const
        {
            if (d == static_cast<double>(static_cast<long long>(d)))
            {
                return std::to_string(static_cast<long long>(d));
            }

            return std::format("{}", d);
        }

        std::string operator()(const std::string& s) const
        {
            return "\"" + s + "\"";
        }

        std::string operator()(const ObjectRef& o) const
        {
            return o.name;
        }

        std::string operator()(const FunctionRef& f) const
        {
            return f.object.empty() ? f.name + "()" : f.object + "." + f.name + "()";
        }
    };
    return std::visit(Visitor{}, v);
}

namespace
{

enum class Tok
{
    End,
    Ident,
    Number,
    String,
    LBrace,
    RBrace,
    LParen,
    RParen,
    Comma,
    Dot,
    Assign,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Bang,
    AndAnd,
    OrOr,
};

struct Token
{
    Tok type = Tok::End;
    std::string text;
    double number = 0.0;
    SourceLoc loc;
    std::size_t begin = 0, end = 0; // byte offsets in the source
};

struct LexError : std::runtime_error
{
    SourceLoc loc;
    LexError(const std::string& m, SourceLoc l) : std::runtime_error(m), loc(l)
    {
    }
};

class Lexer
{
public:
    explicit Lexer(std::string_view src) : src_(src)
    {
    }

    std::vector<Token> run(std::vector<Diagnostic>& errors)
    {
        std::vector<Token> out;

        while (true)
        {
            const std::size_t before = pos_;

            try
            {
                Token t = next();
                const bool end = t.type == Tok::End;
                out.push_back(std::move(t));

                if (end)
                {
                    break;
                }
            }
            catch (const LexError& e)
            {
                errors.push_back({e.loc, e.what()});

                if (pos_ == before && pos_ < src_.size())
                {
                    advance();
                }
            }
        }

        return out;
    }

private:
    char peek(std::size_t off = 0) const
    {
        return pos_ + off < src_.size() ? src_[pos_ + off] : '\0';
    }

    char advance()
    {
        const char c = src_[pos_++];

        if (c == '\n')
        {
            ++line_;
            col_ = 1;
        }
        else
        {
            ++col_;
        }

        return c;
    }

    void skipTrivia()
    {
        while (pos_ < src_.size())
        {
            const char c = peek();

            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            {
                advance();
            }
            else if (c == '#' || (c == '/' && peek(1) == '/'))
            {
                while (pos_ < src_.size() && peek() != '\n')
                {
                    advance();
                }
            }
            else
            {
                break;
            }
        }
    }

    Token make(Tok type, SourceLoc loc, std::string text = {})
    {
        Token t;
        t.type = type;
        t.loc = loc;
        t.text = std::move(text);
        t.begin = start_;
        t.end = pos_;
        return t;
    }

    Token next()
    {
        skipTrivia();
        start_ = pos_;
        const SourceLoc loc{line_, col_};

        if (pos_ >= src_.size())
        {
            return make(Tok::End, loc);
        }

        const char c = peek();

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_')
        {
            std::string id;

            while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
            {
                id += advance();
            }

            return make(Tok::Ident, loc, std::move(id));
        }

        if (std::isdigit(static_cast<unsigned char>(c)))
        {
            std::string num;

            while (std::isdigit(static_cast<unsigned char>(peek())))
            {
                num += advance();
            }

            if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))
            {
                num += advance();

                while (std::isdigit(static_cast<unsigned char>(peek())))
                {
                    num += advance();
                }
            }

            Token t = make(Tok::Number, loc, num);
            std::from_chars(num.data(), num.data() + num.size(), t.number);
            return t;
        }

        if (c == '"' || c == '\'')
        {
            const char quote = advance();
            std::string s;

            while (true)
            {
                if (pos_ >= src_.size() || peek() == '\n')
                {
                    throw LexError("unterminated string", loc);
                }

                const char ch = advance();

                if (ch == quote)
                {
                    break;
                }

                if (ch == '\\')
                {
                    if (pos_ >= src_.size())
                    {
                        throw LexError("unterminated string", loc);
                    }

                    const char e = advance();

                    switch (e)
                    {
                    case 'n':
                        s += '\n';
                        break;
                    case 't':
                        s += '\t';
                        break;
                    default:
                        s += e;
                        break;
                    }
                }
                else
                {
                    s += ch;
                }
            }

            return make(Tok::String, loc, std::move(s));
        }

        advance();

        switch (c)
        {
        case '{':
            return make(Tok::LBrace, loc, "{");
        case '}':
            return make(Tok::RBrace, loc, "}");
        case '(':
            return make(Tok::LParen, loc, "(");
        case ')':
            return make(Tok::RParen, loc, ")");
        case ',':
            return make(Tok::Comma, loc, ",");
        case '.':
            return make(Tok::Dot, loc, ".");
        case '+':
            return make(Tok::Plus, loc, "+");
        case '-':
            return make(Tok::Minus, loc, "-");
        case '*':
            return make(Tok::Star, loc, "*");
        case '/':
            return make(Tok::Slash, loc, "/");
        case '%':
            return make(Tok::Percent, loc, "%");
        case '=':
            if (peek() == '=')
            {
                advance();
                return make(Tok::Eq, loc, "==");
            }

            return make(Tok::Assign, loc, "=");
        case '!':
            if (peek() == '=')
            {
                advance();
                return make(Tok::Ne, loc, "!=");
            }

            return make(Tok::Bang, loc, "!");
        case '<':
            if (peek() == '=')
            {
                advance();
                return make(Tok::Le, loc, "<=");
            }

            return make(Tok::Lt, loc, "<");
        case '>':
            if (peek() == '=')
            {
                advance();
                return make(Tok::Ge, loc, ">=");
            }

            return make(Tok::Gt, loc, ">");
        case '&':
            if (peek() == '&')
            {
                advance();
                return make(Tok::AndAnd, loc, "&&");
            }

            break;
        case '|':
            if (peek() == '|')
            {
                advance();
                return make(Tok::OrOr, loc, "||");
            }

            break;
        default:
            break;
        }

        throw LexError(std::format("unexpected character '{}'", c), loc);
    }

    std::string_view src_;
    std::size_t pos_ = 0;
    std::size_t start_ = 0; // where the token being lexed began
    std::size_t line_ = 1;
    std::size_t col_ = 1;
};

struct ParseError : std::runtime_error
{
    SourceLoc loc;
    ParseError(const std::string& m, SourceLoc l) : std::runtime_error(m), loc(l)
    {
    }
};

class Parser
{
public:
    Parser(std::string_view source, std::vector<Token> tokens, std::vector<Diagnostic>& errors) : source_(source), toks_(std::move(tokens)), errors_(errors)
    {
    }

    Program run()
    {
        Program prog;

        while (!at(Tok::End))
        {
            try
            {
                parseItem(prog);
            }
            catch (const ParseError& e)
            {
                errors_.push_back({e.loc, e.what()});
                recoverToLineAfter(e.loc.line);

                if (at(Tok::RBrace))
                {
                    take(); // stray '}' at top level
                }
            }
        }

        return prog;
    }

private:
    const Token& cur() const
    {
        return toks_[pos_];
    }

    bool at(Tok t) const
    {
        return cur().type == t;
    }

    bool atKeyword(std::string_view kw) const
    {
        return at(Tok::Ident) && cur().text == kw;
    }

    const Token& take()
    {
        const Token& t = toks_[pos_];

        if (t.type != Tok::End)
        {
            ++pos_;
        }

        return t;
    }

    const Token& expect(Tok type, std::string_view what)
    {
        if (!at(type))
        {
            throw ParseError(std::format("expected {} but found '{}'", what, describe(cur())), cur().loc);
        }

        return take();
    }

    static std::string describe(const Token& t)
    {
        return t.type == Tok::End ? "end of file" : t.text;
    }

    static bool isReserved(std::string_view s)
    {
        return s == "if" || s == "else" || s == "and" || s == "or" || s == "not" || s == "true" || s == "false" || s == "class" || s == "settings";
    }

    void recoverToLineAfter(std::size_t line)
    {
        while (!at(Tok::End) && cur().loc.line <= line && !at(Tok::RBrace))
        {
            take();
        }
    }

    void parseItem(Program& prog)
    {
        if (atKeyword("class"))
        {
            take();
            prog.className = expect(Tok::String, "class name string").text;
            return;
        }

        if (atKeyword("settings"))
        {
            take();
            expect(Tok::LBrace, "'{'");

            while (!at(Tok::RBrace) && !at(Tok::End))
            {
                try
                {
                    Setting s;
                    s.loc = cur().loc;
                    s.name = expect(Tok::Ident, "setting name").text;
                    expect(Tok::Assign, "'='");
                    s.value = parseExpr();
                    prog.settings.push_back(std::move(s));
                }
                catch (const ParseError& e)
                {
                    errors_.push_back({e.loc, e.what()});
                    recoverToLineAfter(e.loc.line);
                }
            }

            expect(Tok::RBrace, "'}'");
            return;
        }

        if (at(Tok::Ident) && !isReserved(cur().text))
        {
            Block block;
            block.loc = cur().loc;
            block.name = take().text;
            expect(Tok::LBrace, "'{' after block name");
            parseRules(block.rules);
            expect(Tok::RBrace, "'}' to close block '" + block.name + "'");
            prog.blocks.push_back(std::move(block));
            return;
        }

        throw ParseError(std::format("expected 'class', 'settings' or a block name, found '{}'", describe(cur())), cur().loc);
    }

    // The rules up to the closing '}', the ones inside `if <condition> { ... }`
    // with that condition too - nested as deep as written.
    void parseRules(std::vector<Rule>& rules)
    {
        while (!at(Tok::RBrace) && !at(Tok::End))
        {
            try
            {
                if (atKeyword("else"))
                {
                    throw ParseError("'else' without an 'if' block right before it", cur().loc);
                }

                // First on its line: an 'if' after an action is that rule's condition.
                if (atKeyword("if") && (pos_ == 0 || toks_[pos_ - 1].loc.line != cur().loc.line))
                {
                    parseIfChain(rules);
                    continue;
                }

                rules.push_back(parseRule());
            }
            catch (const ParseError& e)
            {
                errors_.push_back({e.loc, e.what()});
                recoverToLineAfter(e.loc.line);
            }
        }
    }

    // `if c1 { ... } else if c2 { ... } else { ... }`: each part's rules
    // under its own condition and the negation of every one before it.
    void parseIfChain(std::vector<Rule>& rules)
    {
        std::vector<std::pair<ExprPtr, std::string>> before; // the conditions so far
        bool last = false;

        while (!last)
        {
            const SourceLoc loc = cur().loc;
            ExprPtr condition;
            std::string text;

            if (atKeyword("if"))
            {
                take();
                const std::size_t begin = cur().begin;
                condition = parseExpr();
                text = std::string(source_.substr(begin, toks_[pos_ - 1].end - begin));
            }
            else
            {
                last = true; // a plain else ends the chain
            }

            expect(Tok::LBrace, last ? "'{' after 'else'" : "'{' after the 'if' condition");
            // Not any of the ones before, and its own.
            ExprPtr guard;
            std::vector<std::string> texts;

            for (const auto& [previous, previousText] : before)
            {
                auto negated = node(Expr::Kind::Unary, previous->loc);
                negated->op = Op::Not;
                negated->lhs = clone(*previous);
                guard = guard ? binary(Op::And, std::move(guard), std::move(negated), loc) : std::move(negated);
                texts.push_back("not (" + previousText + ")");
            }

            if (condition)
            {
                guard = guard ? binary(Op::And, std::move(guard), clone(*condition), loc) : clone(*condition);
                texts.push_back(text);
                before.emplace_back(std::move(condition), text);
            }

            const std::size_t guardsBefore = guards_.size();

            if (guard)
            {
                guards_.emplace_back(std::move(guard), conjoin(texts));
            }

            parseRules(rules);
            guards_.resize(guardsBefore);

            if (!at(Tok::RBrace))
            {
                throw ParseError("expected '}' to close the block begun on line " + std::to_string(loc.line), cur().loc);
            }

            take();

            if (!atKeyword("else"))
            {
                break;
            }

            take(); // else, and then `if` or `{`
        }
    }

    static ExprPtr clone(const Expr& e)
    {
        auto c = std::make_unique<Expr>();
        c->kind = e.kind;
        c->loc = e.loc;
        c->literal = e.literal;
        c->name = e.name;
        c->op = e.op;

        if (e.lhs)
        {
            c->lhs = clone(*e.lhs);
        }

        if (e.rhs)
        {
            c->rhs = clone(*e.rhs);
        }

        for (const auto& a : e.args)
        {
            c->args.push_back(clone(*a));
        }

        return c;
    }

    // In the log: "a and b", each in brackets where it has an 'or' of its own.
    static std::string conjoin(const std::vector<std::string>& parts)
    {
        std::string out;

        for (const auto& p : parts)
        {
            if (!out.empty())
            {
                out += " and ";
            }

            out += parts.size() > 1 && p.find(" or ") != std::string::npos ? "(" + p + ")" : p;
        }

        return out;
    }

    Rule parseRule()
    {
        Rule r;
        r.loc = cur().loc;

        if (!at(Tok::Ident) || isReserved(cur().text))
        {
            throw ParseError(std::format("expected action name, found '{}'", describe(cur())), cur().loc);
        }

        r.action = take().text;

        while (at(Tok::Dot))
        {
            take();
            r.action += '.';
            r.action += expect(Tok::Ident, "identifier after '.' in action name").text;
        }

        // player.WalkBackward(10): how far.
        if (at(Tok::LParen))
        {
            take();
            r.argument = parseExpr();
            expect(Tok::RParen, "')' after the argument");
        }

        if (atKeyword("if"))
        {
            take();
            const std::size_t begin = cur().begin;
            r.condition = parseExpr();
            r.conditionText = std::string(source_.substr(begin, toks_[pos_ - 1].end - begin));
        }

        // Inside `if` blocks: their conditions first, then the rule's own.
        if (!guards_.empty())
        {
            std::vector<std::string> texts;
            ExprPtr all;

            for (const auto& [guard, text] : guards_)
            {
                all = all ? binary(Op::And, std::move(all), clone(*guard), guard->loc) : clone(*guard);
                texts.push_back(text);
            }

            if (r.condition)
            {
                all = binary(Op::And, std::move(all), std::move(r.condition), r.loc);
                texts.push_back(r.conditionText);
            }

            r.condition = std::move(all);
            r.conditionText = conjoin(texts);
        }

        return r;
    }

    ExprPtr node(Expr::Kind kind, SourceLoc loc)
    {
        auto e = std::make_unique<Expr>();
        e->kind = kind;
        e->loc = loc;
        return e;
    }

    ExprPtr binary(Op op, ExprPtr l, ExprPtr r, SourceLoc loc)
    {
        auto e = node(Expr::Kind::Binary, loc);
        e->op = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }

    ExprPtr parseExpr()
    {
        return parseOr();
    }

    ExprPtr parseOr()
    {
        ExprPtr l = parseAnd();

        while (atKeyword("or") || at(Tok::OrOr))
        {
            const SourceLoc loc = take().loc;
            l = binary(Op::Or, std::move(l), parseAnd(), loc);
        }

        return l;
    }

    ExprPtr parseAnd()
    {
        ExprPtr l = parseNot();

        while (atKeyword("and") || at(Tok::AndAnd))
        {
            const SourceLoc loc = take().loc;
            l = binary(Op::And, std::move(l), parseNot(), loc);
        }

        return l;
    }

    ExprPtr parseNot()
    {
        if (atKeyword("not") || at(Tok::Bang))
        {
            const SourceLoc loc = take().loc;
            auto e = node(Expr::Kind::Unary, loc);
            e->op = Op::Not;
            e->lhs = parseNot();
            return e;
        }

        return parseComparison();
    }

    ExprPtr parseComparison()
    {
        ExprPtr l = parseAdditive();
        Op op;

        switch (cur().type)
        {
        case Tok::Eq:
            op = Op::Eq;
            break;
        case Tok::Ne:
            op = Op::Ne;
            break;
        case Tok::Lt:
            op = Op::Lt;
            break;
        case Tok::Le:
            op = Op::Le;
            break;
        case Tok::Gt:
            op = Op::Gt;
            break;
        case Tok::Ge:
            op = Op::Ge;
            break;
        case Tok::Assign:
            throw ParseError("'=' is assignment; use '==' to compare", cur().loc);
        default:
            return l;
        }

        const SourceLoc loc = take().loc;
        return binary(op, std::move(l), parseAdditive(), loc);
    }

    ExprPtr parseAdditive()
    {
        ExprPtr l = parseMultiplicative();

        while (at(Tok::Plus) || at(Tok::Minus))
        {
            const Op op = at(Tok::Plus) ? Op::Add : Op::Sub;
            const SourceLoc loc = take().loc;
            l = binary(op, std::move(l), parseMultiplicative(), loc);
        }

        return l;
    }

    ExprPtr parseMultiplicative()
    {
        ExprPtr l = parseUnary();

        while (at(Tok::Star) || at(Tok::Slash) || at(Tok::Percent))
        {
            const Op op = at(Tok::Star) ? Op::Mul : at(Tok::Slash) ? Op::Div : Op::Mod;
            const SourceLoc loc = take().loc;
            l = binary(op, std::move(l), parseUnary(), loc);
        }

        return l;
    }

    ExprPtr parseUnary()
    {
        if (at(Tok::Minus))
        {
            const SourceLoc loc = take().loc;
            auto e = node(Expr::Kind::Unary, loc);
            e->op = Op::Neg;
            e->lhs = parseUnary();
            return e;
        }

        return parsePostfix();
    }

    ExprPtr parsePostfix()
    {
        ExprPtr e = parsePrimary();

        while (true)
        {
            if (at(Tok::Dot))
            {
                const SourceLoc loc = take().loc;
                auto m = node(Expr::Kind::Member, loc);
                m->name = expect(Tok::Ident, "member name after '.'").text;
                m->lhs = std::move(e);
                e = std::move(m);
            }
            else if (at(Tok::LParen))
            {
                const SourceLoc loc = take().loc;
                auto c = node(Expr::Kind::Call, loc);
                c->lhs = std::move(e);

                if (!at(Tok::RParen))
                {
                    c->args.push_back(parseExpr());

                    while (at(Tok::Comma))
                    {
                        take();
                        c->args.push_back(parseExpr());
                    }
                }

                expect(Tok::RParen, "')'");
                e = std::move(c);
            }
            else
            {
                return e;
            }
        }
    }

    ExprPtr parsePrimary()
    {
        const Token& t = cur();

        switch (t.type)
        {
        case Tok::Number:
        {
            auto e = node(Expr::Kind::Literal, t.loc);
            e->literal = t.number;
            take();
            return e;
        }

        case Tok::String:
        {
            auto e = node(Expr::Kind::Literal, t.loc);
            e->literal = t.text;
            take();
            return e;
        }

        case Tok::LParen:
        {
            take();
            ExprPtr e = parseExpr();
            expect(Tok::RParen, "')'");
            return e;
        }

        case Tok::Ident:
        {
            if (t.text == "true" || t.text == "false")
            {
                auto e = node(Expr::Kind::Literal, t.loc);
                e->literal = t.text == "true";
                take();
                return e;
            }

            if (isReserved(t.text))
            {
                break;
            }

            auto e = node(Expr::Kind::Name, t.loc);
            e->name = t.text;
            take();
            return e;
        }

        default:
            break;
        }

        throw ParseError(std::format("expected expression, found '{}'", describe(t)), t.loc);
    }

    std::string_view source_;
    std::vector<Token> toks_;
    std::size_t pos_ = 0;
    std::vector<Diagnostic>& errors_;
    // The conditions of the `if` blocks the parser is inside, outermost first.
    std::vector<std::pair<ExprPtr, std::string>> guards_;
};

} // namespace

ParseResult parse(std::string_view source)
{
    ParseResult result;
    auto tokens = Lexer(source).run(result.errors);
    Parser parser(source, std::move(tokens), result.errors);
    result.program = parser.run();
    return result;
}

} // namespace og::script
