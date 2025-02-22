#include "rewriter/Minitest.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "core/Context.h"
#include "core/Names.h"
#include "core/core.h"
#include "core/errors/rewriter.h"
#include "rewriter/rewriter.h"

using namespace std;

namespace sorbet::rewriter {

namespace {
unique_ptr<ast::Expression> addSigVoid(unique_ptr<ast::Expression> expr) {
    return ast::MK::InsSeq1(expr->loc, ast::MK::SigVoid(expr->loc, ast::MK::Hash0(expr->loc)), std::move(expr));
}
} // namespace

unique_ptr<ast::Expression> recurse(core::MutableContext ctx, unique_ptr<ast::Expression> body);

unique_ptr<ast::Expression> prepareBody(core::MutableContext ctx, unique_ptr<ast::Expression> body) {
    body = recurse(ctx, std::move(body));

    auto bodySeq = ast::cast_tree<ast::InsSeq>(body.get());
    if (bodySeq) {
        for (auto &exp : bodySeq->stats) {
            exp = recurse(ctx, std::move(exp));
        }

        bodySeq->expr = recurse(ctx, std::move(bodySeq->expr));
    }
    return body;
}

string to_s(core::Context ctx, unique_ptr<ast::Expression> &arg) {
    auto argLit = ast::cast_tree<ast::Literal>(arg.get());
    string argString;
    if (argLit != nullptr) {
        if (argLit->isString(ctx)) {
            return argLit->asString(ctx).show(ctx);
        } else if (argLit->isSymbol(ctx)) {
            return argLit->asSymbol(ctx).show(ctx);
        }
    }
    auto argConstant = ast::cast_tree<ast::UnresolvedConstantLit>(arg.get());
    if (argConstant != nullptr) {
        return argConstant->cnst.show(ctx);
    }
    return arg->toString(ctx);
}

unique_ptr<ast::Expression> runSingle(core::MutableContext ctx, ast::Send *send) {
    if (send->block == nullptr) {
        return nullptr;
    }

    if (!send->recv->isSelfReference()) {
        return nullptr;
    }

    if (send->args.empty() && (send->fun == core::Names::before() || send->fun == core::Names::after())) {
        auto name = send->fun == core::Names::after() ? core::Names::afterAngles() : core::Names::initialize();
        return addSigVoid(ast::MK::Method0(send->loc, send->loc, name, prepareBody(ctx, std::move(send->block->body)),
                                           ast::MethodDef::RewriterSynthesized));
    }

    if (send->args.size() != 1) {
        return nullptr;
    }
    auto &arg = send->args[0];
    auto argString = to_s(ctx, arg);

    if (send->fun == core::Names::describe()) {
        ast::ClassDef::ANCESTORS_store ancestors;
        ancestors.emplace_back(ast::MK::Self(arg->loc));
        ast::ClassDef::RHS_store rhs;
        rhs.emplace_back(prepareBody(ctx, std::move(send->block->body)));
        auto name = ast::MK::UnresolvedConstant(arg->loc, ast::MK::EmptyTree(),
                                                ctx.state.enterNameConstant("<class_" + argString + ">"));
        return ast::MK::Class(send->loc, send->loc, std::move(name), std::move(ancestors), std::move(rhs),
                              ast::ClassDefKind::Class);
    } else if (send->fun == core::Names::it()) {
        auto name = ctx.state.enterNameUTF8("<test_" + argString + ">");
        return addSigVoid(ast::MK::Method0(send->loc, send->loc, std::move(name),
                                           prepareBody(ctx, std::move(send->block->body)),
                                           ast::MethodDef::RewriterSynthesized));
    }

    return nullptr;
}

unique_ptr<ast::Expression> recurse(core::MutableContext ctx, unique_ptr<ast::Expression> body) {
    auto bodySend = ast::cast_tree<ast::Send>(body.get());
    if (bodySend) {
        auto change = runSingle(ctx, bodySend);
        if (change) {
            return change;
        }
    }
    return body;
}

vector<unique_ptr<ast::Expression>> Minitest::run(core::MutableContext ctx, ast::Send *send) {
    vector<unique_ptr<ast::Expression>> stats;
    if (ctx.state.runningUnderAutogen) {
        return stats;
    }

    auto exp = runSingle(ctx, send);
    if (exp != nullptr) {
        stats.emplace_back(std::move(exp));
    }
    return stats;
}

}; // namespace sorbet::rewriter
