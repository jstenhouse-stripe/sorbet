#include "ast/ArgParsing.h"
#include "ast/Helpers.h"
#include "cfg/builder/builder.h"
#include "common/typecase.h"
#include "core/Names.h"
#include "core/errors/cfg.h"
#include "core/errors/internal.h"

using namespace std;

namespace sorbet::cfg {

void conditionalJump(BasicBlock *from, core::LocalVariable cond, BasicBlock *thenb, BasicBlock *elseb, CFG &inWhat,
                     core::Loc loc) {
    thenb->flags |= CFG::WAS_JUMP_DESTINATION;
    elseb->flags |= CFG::WAS_JUMP_DESTINATION;
    if (from != inWhat.deadBlock()) {
        ENFORCE(!from->bexit.isCondSet(), "condition for block already set");
        ENFORCE(from->bexit.thenb == nullptr, "thenb already set");
        ENFORCE(from->bexit.elseb == nullptr, "elseb already set");
        from->bexit.cond = cond;
        from->bexit.thenb = thenb;
        from->bexit.elseb = elseb;
        from->bexit.loc = loc;
        thenb->backEdges.emplace_back(from);
        elseb->backEdges.emplace_back(from);
    }
}

void unconditionalJump(BasicBlock *from, BasicBlock *to, CFG &inWhat, core::Loc loc) {
    to->flags |= CFG::WAS_JUMP_DESTINATION;
    if (from != inWhat.deadBlock()) {
        ENFORCE(!from->bexit.isCondSet(), "condition for block already set");
        ENFORCE(from->bexit.thenb == nullptr, "thenb already set");
        ENFORCE(from->bexit.elseb == nullptr, "elseb already set");
        from->bexit.cond = core::LocalVariable::noVariable();
        from->bexit.elseb = to;
        from->bexit.thenb = to;
        from->bexit.loc = loc;
        to->backEdges.emplace_back(from);
    }
}

void jumpToDead(BasicBlock *from, CFG &inWhat, core::Loc loc) {
    auto *db = inWhat.deadBlock();
    if (from != db) {
        ENFORCE(!from->bexit.isCondSet(), "condition for block already set");
        ENFORCE(from->bexit.thenb == nullptr, "thenb already set");
        ENFORCE(from->bexit.elseb == nullptr, "elseb already set");
        from->bexit.cond = core::LocalVariable::noVariable();
        from->bexit.elseb = db;
        from->bexit.thenb = db;
        from->bexit.loc = loc;
        db->backEdges.emplace_back(from);
    }
}

core::LocalVariable global2Local(CFGContext cctx, core::SymbolRef what) {
    // Note: this will add an empty local to aliases if 'what' is not there
    core::LocalVariable &alias = cctx.aliases[what];
    if (!alias.exists()) {
        const core::SymbolData data = what.data(cctx.ctx);
        alias = cctx.newTemporary(data->name);
    }
    return alias;
}

core::LocalVariable unresolvedIdent2Local(CFGContext cctx, ast::UnresolvedIdent *id) {
    core::SymbolRef klass;

    switch (id->kind) {
        case ast::UnresolvedIdent::Class:
            klass = cctx.ctx.owner.data(cctx.ctx)->enclosingClass(cctx.ctx);
            while (klass.data(cctx.ctx)->attachedClass(cctx.ctx).exists()) {
                klass = klass.data(cctx.ctx)->attachedClass(cctx.ctx);
            }
            break;
        case ast::UnresolvedIdent::Instance:
            ENFORCE(cctx.ctx.owner.data(cctx.ctx)->isMethod());
            klass = cctx.ctx.owner.data(cctx.ctx)->owner;
            break;
        default:
            // These should have been removed in the namer
            Exception::notImplemented();
    }
    ENFORCE(klass.data(cctx.ctx)->isClassOrModule());

    auto sym = klass.data(cctx.ctx)->findMemberTransitive(cctx.ctx, id->name);
    if (!sym.exists()) {
        auto fnd = cctx.discoveredUndeclaredFields.find(id->name);
        if (fnd == cctx.discoveredUndeclaredFields.end()) {
            if (auto e = cctx.ctx.state.beginError(id->loc, core::errors::CFG::UndeclaredVariable)) {
                e.setHeader("Use of undeclared variable `{}`", id->name.show(cctx.ctx));
            }
            auto ret = cctx.newTemporary(id->name);
            cctx.discoveredUndeclaredFields[id->name] = ret;
            return ret;
        }
        return fnd->second;
    } else {
        return global2Local(cctx, sym);
    }
}

void CFGBuilder::synthesizeExpr(BasicBlock *bb, core::LocalVariable var, core::Loc loc, unique_ptr<Instruction> inst) {
    auto &inserted = bb->exprs.emplace_back(var, loc, move(inst));
    inserted.value->isSynthetic = true;
}

/** Convert `what` into a cfg, by starting to evaluate it in `current` inside method defined by `inWhat`.
 * store result of evaluation into `target`. Returns basic block in which evaluation should proceed.
 */
BasicBlock *CFGBuilder::walk(CFGContext cctx, ast::Expression *what, BasicBlock *current) {
    /** Try to pay additional attention not to duplicate any part of tree.
     * Though this may lead to more effictient and a better CFG if it was to be actually compiled into code
     * This will lead to duplicate typechecking and may lead to exponential explosion of typechecking time
     * for some code snippets. */
    ENFORCE(!current->bexit.isCondSet() || current == cctx.inWhat.deadBlock(),
            "current block has already been finalized!");

    try {
        BasicBlock *ret = nullptr;
        typecase(
            what,
            [&](ast::While *a) {
                auto headerBlock = cctx.inWhat.freshBlock(cctx.loops + 1, cctx.rubyBlockId);
                // breakNotCalledBlock is only entered if break is not called in
                // the loop body
                auto breakNotCalledBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                auto continueBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                unconditionalJump(current, headerBlock, cctx.inWhat, a->loc);

                core::LocalVariable condSym = cctx.newTemporary(core::Names::whileTemp());
                auto headerEnd = walk(cctx.withTarget(condSym).withLoopScope(headerBlock, continueBlock), a->cond.get(),
                                      headerBlock);
                auto bodyBlock = cctx.inWhat.freshBlock(cctx.loops + 1, cctx.rubyBlockId);
                conditionalJump(headerEnd, condSym, bodyBlock, breakNotCalledBlock, cctx.inWhat, a->cond->loc);
                // finishHeader
                core::LocalVariable bodySym = cctx.newTemporary(core::Names::statTemp());

                auto body = walk(cctx.withTarget(bodySym)
                                     .withLoopScope(headerBlock, continueBlock)
                                     .withBlockBreakTarget(cctx.target),
                                 a->body.get(), bodyBlock);
                unconditionalJump(body, headerBlock, cctx.inWhat, a->loc);

                synthesizeExpr(breakNotCalledBlock, cctx.target, a->loc, make_unique<Literal>(core::Types::nilClass()));
                unconditionalJump(breakNotCalledBlock, continueBlock, cctx.inWhat, a->loc);
                ret = continueBlock;

                /*
                 * This code:
                 *
                 *     a = while cond; break b; end
                 *
                 * generates this CFG:
                 *
                 *   ┌──▶ Loop Header ──────┐
                 *   │      │               │
                 *   │      │               ▼
                 *   │      ▼        breakNotCalledBlock
                 *   └─ Loop Body         a = nil
                 *          │               │
                 *        a = b             │
                 *          │               │
                 *          ▼               │
                 *    continueBlock ◀──────-┘
                 *
                 */
            },
            [&](ast::Return *a) {
                core::LocalVariable retSym = cctx.newTemporary(core::Names::returnTemp());
                auto cont = walk(cctx.withTarget(retSym), a->expr.get(), current);
                cont->exprs.emplace_back(cctx.target, a->loc, make_unique<Return>(retSym)); // dead assign.
                jumpToDead(cont, cctx.inWhat, a->loc);
                ret = cctx.inWhat.deadBlock();
            },
            [&](ast::If *a) {
                core::LocalVariable ifSym = cctx.newTemporary(core::Names::ifTemp());
                ENFORCE(ifSym.exists(), "ifSym does not exist");
                auto cont = walk(cctx.withTarget(ifSym), a->cond.get(), current);
                auto thenBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                auto elseBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                conditionalJump(cont, ifSym, thenBlock, elseBlock, cctx.inWhat, a->cond->loc);

                auto thenEnd = walk(cctx, a->thenp.get(), thenBlock);
                auto elseEnd = walk(cctx, a->elsep.get(), elseBlock);
                if (thenEnd != cctx.inWhat.deadBlock() || elseEnd != cctx.inWhat.deadBlock()) {
                    if (thenEnd == cctx.inWhat.deadBlock()) {
                        ret = elseEnd;
                    } else if (thenEnd == cctx.inWhat.deadBlock()) {
                        ret = thenEnd;
                    } else {
                        ret = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                        unconditionalJump(thenEnd, ret, cctx.inWhat, a->loc);
                        unconditionalJump(elseEnd, ret, cctx.inWhat, a->loc);
                    }
                } else {
                    ret = cctx.inWhat.deadBlock();
                }
            },
            [&](ast::Literal *a) {
                current->exprs.emplace_back(cctx.target, a->loc, make_unique<Literal>(a->value));
                ret = current;
            },
            [&](ast::UnresolvedIdent *id) {
                core::LocalVariable loc = unresolvedIdent2Local(cctx, id);
                ENFORCE(loc.exists());
                current->exprs.emplace_back(cctx.target, id->loc, make_unique<Ident>(loc));

                ret = current;
            },
            [&](ast::UnresolvedConstantLit *a) { Exception::raise("Should have been eliminated by namer/resolver"); },
            [&](ast::Field *a) {
                current->exprs.emplace_back(cctx.target, a->loc, make_unique<Ident>(global2Local(cctx, a->symbol)));
                ret = current;
            },
            [&](ast::ConstantLit *a) {
                if (a->symbol == core::Symbols::StubModule()) {
                    current->exprs.emplace_back(cctx.target, a->loc, make_unique<Alias>(core::Symbols::untyped()));
                } else {
                    current->exprs.emplace_back(cctx.target, a->loc, make_unique<Alias>(a->symbol));
                }

                if (a->original) {
                    if (auto nested = ast::cast_tree<ast::ConstantLit>(a->original->scope.get())) {
                        core::LocalVariable deadSym = cctx.newTemporary(core::Names::keepForIde());
                        current = walk(cctx.withTarget(deadSym), nested, current);
                    }
                }

                ret = current;
            },
            [&](ast::Local *a) {
                current->exprs.emplace_back(cctx.target, a->loc, make_unique<Ident>(a->localVariable));
                ret = current;
            },
            [&](ast::Assign *a) {
                core::LocalVariable lhs;
                if (auto lhsIdent = ast::cast_tree<ast::ConstantLit>(a->lhs.get())) {
                    lhs = global2Local(cctx, lhsIdent->symbol);
                } else if (auto field = ast::cast_tree<ast::Field>(a->lhs.get())) {
                    lhs = global2Local(cctx, field->symbol);
                } else if (auto lhsLocal = ast::cast_tree<ast::Local>(a->lhs.get())) {
                    lhs = lhsLocal->localVariable;
                } else if (auto ident = ast::cast_tree<ast::UnresolvedIdent>(a->lhs.get())) {
                    lhs = unresolvedIdent2Local(cctx, ident);
                    ENFORCE(lhs.exists());
                } else {
                    Exception::raise("should never be reached");
                }

                auto rhsCont = walk(cctx.withTarget(lhs), a->rhs.get(), current);
                rhsCont->exprs.emplace_back(cctx.target, a->loc, make_unique<Ident>(lhs));
                ret = rhsCont;
            },
            [&](ast::InsSeq *a) {
                for (auto &exp : a->stats) {
                    core::LocalVariable temp = cctx.newTemporary(core::Names::statTemp());
                    current = walk(cctx.withTarget(temp), exp.get(), current);
                }
                ret = walk(cctx, a->expr.get(), current);
            },
            [&](ast::Send *s) {
                core::LocalVariable recv;

                if (s->fun == core::Names::absurd()) {
                    if (auto cnst = ast::cast_tree<ast::ConstantLit>(s->recv.get())) {
                        if (cnst->symbol == core::Symbols::T()) {
                            if (s->args.size() != 1) {
                                if (auto e = cctx.ctx.state.beginError(s->loc, core::errors::CFG::MalformedTAbsurd)) {
                                    e.setHeader("`{}` expects exactly one argument but got `{}`", "T.absurd",
                                                s->args.size());
                                }
                                ret = current;
                                return;
                            }

                            if (ast::isa_tree<ast::Send>(s->args[0].get())) {
                                // Providing a send is the most common way T.absurd is misused
                                if (auto e = cctx.ctx.state.beginError(s->loc, core::errors::CFG::MalformedTAbsurd)) {
                                    e.setHeader("`{}` expects to be called on a variable, not a method call",
                                                "T.absurd", s->args.size());
                                }
                                ret = current;
                                return;
                            }

                            auto temp = cctx.newTemporary(core::Names::statTemp());
                            current = walk(cctx.withTarget(temp), s->args[0].get(), current);
                            current->exprs.emplace_back(cctx.target, s->loc, make_unique<TAbsurd>(temp));
                            ret = current;
                            return;
                        }
                    }
                }

                recv = cctx.newTemporary(core::Names::statTemp());
                current = walk(cctx.withTarget(recv), s->recv.get(), current);

                InlinedVector<core::LocalVariable, 2> args;
                InlinedVector<core::Loc, 2> argLocs;
                for (auto &exp : s->args) {
                    core::LocalVariable temp;
                    temp = cctx.newTemporary(core::Names::statTemp());
                    current = walk(cctx.withTarget(temp), exp.get(), current);

                    args.emplace_back(temp);
                    argLocs.emplace_back(exp->loc);
                }

                if (s->block != nullptr) {
                    auto newRubyBlockId = ++cctx.inWhat.maxRubyBlockId;
                    vector<ast::ParsedArg> blockArgs = ast::ArgParsing::parseArgs(cctx.ctx, s->block->args);
                    vector<core::ArgInfo::ArgFlags> argFlags;
                    for (auto &e : blockArgs) {
                        auto &target = argFlags.emplace_back();
                        target.isKeyword = e.keyword;
                        target.isRepeated = e.repeated;
                        target.isDefault = e.default_ != nullptr;
                        target.isShadow = e.shadow;
                    }
                    auto link = make_shared<core::SendAndBlockLink>(s->fun, move(argFlags), newRubyBlockId);
                    auto send = make_unique<Send>(recv, s->fun, s->recv->loc, args, argLocs, s->isPrivateOk(), link);
                    core::LocalVariable sendTemp = cctx.newTemporary(core::Names::blockPreCallTemp());
                    auto solveConstraint = make_unique<SolveConstraint>(link, sendTemp);
                    current->exprs.emplace_back(sendTemp, s->loc, move(send));
                    core::LocalVariable restoreSelf = cctx.newTemporary(core::Names::selfRestore());
                    synthesizeExpr(current, restoreSelf, core::Loc::none(),
                                   make_unique<Ident>(core::LocalVariable::selfVariable()));

                    auto headerBlock = cctx.inWhat.freshBlock(cctx.loops + 1, newRubyBlockId);
                    // solveConstraintBlock is only entered if break is not called
                    // in the block body.
                    auto solveConstraintBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                    auto postBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                    auto bodyBlock = cctx.inWhat.freshBlock(cctx.loops + 1, newRubyBlockId);

                    core::LocalVariable argTemp = cctx.newTemporary(core::Names::blkArg());
                    core::LocalVariable idxTmp = cctx.newTemporary(core::Names::blkArg());
                    bodyBlock->exprs.emplace_back(core::LocalVariable::selfVariable(), s->loc,
                                                  make_unique<LoadSelf>(link, core::LocalVariable::selfVariable()));
                    bodyBlock->exprs.emplace_back(argTemp, s->block->loc, make_unique<LoadYieldParams>(link));

                    for (int i = 0; i < blockArgs.size(); ++i) {
                        auto &arg = blockArgs[i];
                        core::LocalVariable argLoc = arg.local;

                        if (arg.repeated) {
                            if (i != 0) {
                                // Mixing positional and rest args in blocks is
                                // not currently supported; drop in an untyped.
                                bodyBlock->exprs.emplace_back(argLoc, arg.loc,
                                                              make_unique<Alias>(core::Symbols::untyped()));
                            } else {
                                bodyBlock->exprs.emplace_back(argLoc, arg.loc, make_unique<Ident>(argTemp));
                            }
                            continue;
                        }

                        // Inserting a statement that does not directly map to any source text. Make its loc
                        // 0-length so LSP ignores it in queries.
                        core::Loc zeroLengthLoc = arg.loc.copyWithZeroLength();
                        bodyBlock->exprs.emplace_back(
                            idxTmp, zeroLengthLoc,
                            make_unique<Literal>(core::make_type<core::LiteralType>(int64_t(i))));
                        InlinedVector<core::LocalVariable, 2> idxVec{idxTmp};
                        InlinedVector<core::Loc, 2> locs{zeroLengthLoc};
                        auto isPrivateOk = false;
                        bodyBlock->exprs.emplace_back(argLoc, arg.loc,
                                                      make_unique<Send>(argTemp, core::Names::squareBrackets(),
                                                                        s->block->loc, idxVec, locs, isPrivateOk));
                    }

                    conditionalJump(headerBlock, core::LocalVariable::blockCall(), bodyBlock, solveConstraintBlock,
                                    cctx.inWhat, s->loc);

                    unconditionalJump(current, headerBlock, cctx.inWhat, s->loc);

                    core::LocalVariable blockrv = cctx.newTemporary(core::Names::blockReturnTemp());
                    auto blockLast = walk(cctx.withTarget(blockrv)
                                              .withBlockBreakTarget(cctx.target)
                                              .withLoopScope(headerBlock, postBlock, true)
                                              .withSendAndBlockLink(link)
                                              .withRubyBlockId(newRubyBlockId),
                                          s->block->body.get(), bodyBlock);
                    if (blockLast != cctx.inWhat.deadBlock()) {
                        core::LocalVariable dead = cctx.newTemporary(core::Names::blockReturnTemp());
                        synthesizeExpr(blockLast, dead, s->block->loc, make_unique<BlockReturn>(link, blockrv));
                    }

                    unconditionalJump(blockLast, headerBlock, cctx.inWhat, s->loc);
                    unconditionalJump(solveConstraintBlock, postBlock, cctx.inWhat, s->loc);

                    solveConstraintBlock->exprs.emplace_back(cctx.target, s->loc, move(solveConstraint));
                    current = postBlock;
                    synthesizeExpr(current, core::LocalVariable::selfVariable(), s->loc,
                                   make_unique<Ident>(restoreSelf));

                    /*
                     * This code:
                     *
                     *     a = while cond; break b; end
                     *
                     * generates this CFG:
                     *
                     *   ┌──▶ headerBlock ──────┐
                     *   │      │               │
                     *   │      │               │
                     *   │      ▼               │
                     *   └─ Block Body          ▼
                     *          │    a = solveConstraintBlock
                     *        a = b             │
                     *          │               │
                     *          ▼               │
                     *      Post Block ◀───────-┘
                     *
                     */
                } else {
                    current->exprs.emplace_back(
                        cctx.target, s->loc,
                        make_unique<Send>(recv, s->fun, s->recv->loc, args, argLocs, s->isPrivateOk()));
                }

                ret = current;
            },

            [&](ast::Block *a) { Exception::raise("should never encounter a bare Block"); },

            [&](ast::Next *a) {
                core::LocalVariable exprSym = cctx.newTemporary(core::Names::nextTemp());
                auto afterNext = walk(cctx.withTarget(exprSym), a->expr.get(), current);
                if (afterNext != cctx.inWhat.deadBlock() && cctx.isInsideRubyBlock) {
                    core::LocalVariable dead = cctx.newTemporary(core::Names::nextTemp());
                    ENFORCE(cctx.link.get() != nullptr);
                    afterNext->exprs.emplace_back(dead, a->loc, make_unique<BlockReturn>(cctx.link, exprSym));
                }

                if (cctx.nextScope == nullptr) {
                    if (auto e = cctx.ctx.state.beginError(a->loc, core::errors::CFG::NoNextScope)) {
                        e.setHeader("No `{}` block around `{}`", "do", "next");
                    }
                    // I guess just keep going into deadcode?
                    unconditionalJump(afterNext, cctx.inWhat.deadBlock(), cctx.inWhat, a->loc);
                } else {
                    unconditionalJump(afterNext, cctx.nextScope, cctx.inWhat, a->loc);
                }

                ret = cctx.inWhat.deadBlock();
            },

            [&](ast::Break *a) {
                core::LocalVariable exprSym = cctx.newTemporary(core::Names::returnTemp());
                auto afterBreak = walk(cctx.withTarget(exprSym), a->expr.get(), current);

                // Here, since cctx.blockBreakTarget refers to something outside of the block,
                // it will show up on the pinned variables list (with type of NilClass).
                // Then, since we are assigning to it at a higher loop level, we throw a
                // "changing type in loop" error.

                // To get around this, we first assign to a
                // temporary blockBreakAssign variable, and then assign blockBreakAssign to
                // cctx.blockBreakTarget. This allows us to silence this error, if the RHS is
                // a variable of type "blockBreakAssign". You can find the silencing code in
                // infer/environment.cc, if you search for "== core::Names::blockBreakAssign()".

                // This is a temporary hack until we change how pining works to handle this case.
                auto blockBreakAssign = cctx.newTemporary(core::Names::blockBreakAssign());
                afterBreak->exprs.emplace_back(blockBreakAssign, a->loc, make_unique<Ident>(exprSym));
                afterBreak->exprs.emplace_back(cctx.blockBreakTarget, a->loc, make_unique<Ident>(blockBreakAssign));

                if (cctx.breakScope == nullptr) {
                    if (auto e = cctx.ctx.state.beginError(a->loc, core::errors::CFG::NoNextScope)) {
                        e.setHeader("No `{}` block around `{}`", "do", "break");
                    }
                    // I guess just keep going into deadcode?
                    unconditionalJump(afterBreak, cctx.inWhat.deadBlock(), cctx.inWhat, a->loc);
                } else {
                    unconditionalJump(afterBreak, cctx.breakScope, cctx.inWhat, a->loc);
                }
                ret = cctx.inWhat.deadBlock();
            },

            [&](ast::Retry *a) {
                if (cctx.rescueScope == nullptr) {
                    if (auto e = cctx.ctx.state.beginError(a->loc, core::errors::CFG::NoNextScope)) {
                        e.setHeader("No `{}` block around `{}`", "begin", "retry");
                    }
                    // I guess just keep going into deadcode?
                    unconditionalJump(current, cctx.inWhat.deadBlock(), cctx.inWhat, a->loc);
                } else {
                    unconditionalJump(current, cctx.rescueScope, cctx.inWhat, a->loc);
                }
                ret = cctx.inWhat.deadBlock();
            },

            [&](ast::Rescue *a) {
                auto rescueStartBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                unconditionalJump(current, rescueStartBlock, cctx.inWhat, a->loc);
                cctx.rescueScope = rescueStartBlock;

                // We have a simplified view of the control flow here but in
                // practise it has been reasonable on our codebase.
                // We don't model that each expression in the `body` or `else` could
                // throw, instead we model only never running anything in the
                // body, or running the whole thing. To do this we  have a magic
                // Unanalyzable variable at the top of the body using
                // `rescueStartTemp` and one at the end of the else using
                // `rescueEndTemp` which can jump into the rescue handlers.
                auto rescueHandlersBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                auto bodyBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                auto rescueStartTemp = cctx.newTemporary(core::Names::rescueStartTemp());
                synthesizeExpr(rescueStartBlock, rescueStartTemp, what->loc, make_unique<Unanalyzable>());
                conditionalJump(rescueStartBlock, rescueStartTemp, rescueHandlersBlock, bodyBlock, cctx.inWhat, a->loc);

                // cctx.loops += 1; // should formally be here but this makes us report a lot of false errors
                bodyBlock = walk(cctx, a->body.get(), bodyBlock);
                auto elseBody = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                unconditionalJump(bodyBlock, elseBody, cctx.inWhat, a->loc);

                elseBody = walk(cctx, a->else_.get(), elseBody);
                auto ensureBody = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);

                auto shouldEnsureBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                unconditionalJump(elseBody, shouldEnsureBlock, cctx.inWhat, a->loc);
                auto rescueEndTemp = cctx.newTemporary(core::Names::rescueEndTemp());
                synthesizeExpr(shouldEnsureBlock, rescueEndTemp, what->loc, make_unique<Unanalyzable>());
                conditionalJump(shouldEnsureBlock, rescueEndTemp, rescueHandlersBlock, ensureBody, cctx.inWhat, a->loc);

                for (auto &rescueCase : a->rescueCases) {
                    auto caseBody = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                    auto &exceptions = rescueCase->exceptions;
                    auto added = false;
                    auto *local = ast::cast_tree<ast::Local>(rescueCase->var.get());
                    ENFORCE(local != nullptr, "rescue case var not a local?");
                    rescueHandlersBlock->exprs.emplace_back(local->localVariable, rescueCase->var->loc,
                                                            make_unique<Unanalyzable>());

                    if (exceptions.empty()) {
                        // rescue without a class catches StandardError
                        exceptions.emplace_back(
                            ast::MK::Constant(rescueCase->var->loc, core::Symbols::StandardError()));
                        added = true;
                    }
                    for (auto &ex : exceptions) {
                        auto loc = ex->loc;
                        auto exceptionClass = cctx.newTemporary(core::Names::exceptionClassTemp());
                        rescueHandlersBlock = walk(cctx.withTarget(exceptionClass), ex.get(), rescueHandlersBlock);

                        auto isaCheck = cctx.newTemporary(core::Names::isaCheckTemp());
                        InlinedVector<core::LocalVariable, 2> args;
                        InlinedVector<core::Loc, 2> argLocs = {loc};
                        args.emplace_back(exceptionClass);

                        auto isPrivateOk = false;
                        rescueHandlersBlock->exprs.emplace_back(isaCheck, loc,
                                                                make_unique<Send>(local->localVariable,
                                                                                  core::Names::is_a_p(), loc, args,
                                                                                  argLocs, isPrivateOk));

                        auto otherHandlerBlock = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                        conditionalJump(rescueHandlersBlock, isaCheck, caseBody, otherHandlerBlock, cctx.inWhat, loc);
                        rescueHandlersBlock = otherHandlerBlock;
                    }
                    if (added) {
                        exceptions.pop_back();
                    }

                    caseBody = walk(cctx, rescueCase->body.get(), caseBody);
                    unconditionalJump(caseBody, ensureBody, cctx.inWhat, a->loc);
                }

                // This magic local remembers if none of the `rescue`s match,
                // and if so, after the ensure runs, we should jump to dead
                // since in Ruby the exception would propagate up the statck.
                auto gotoDeadTemp = cctx.newTemporary(core::Names::gotoDeadTemp());
                synthesizeExpr(rescueHandlersBlock, gotoDeadTemp, a->loc,
                               make_unique<Literal>(core::make_type<core::LiteralType>(true)));
                unconditionalJump(rescueHandlersBlock, ensureBody, cctx.inWhat, a->loc);

                auto throwAway = cctx.newTemporary(core::Names::throwAwayTemp());
                ensureBody = walk(cctx.withTarget(throwAway), a->ensure.get(), ensureBody);
                ret = cctx.inWhat.freshBlock(cctx.loops, cctx.rubyBlockId);
                conditionalJump(ensureBody, gotoDeadTemp, cctx.inWhat.deadBlock(), ret, cctx.inWhat, a->loc);
            },

            [&](ast::Hash *h) {
                InlinedVector<core::LocalVariable, 2> vars;
                InlinedVector<core::Loc, 2> locs;
                for (int i = 0; i < h->keys.size(); i++) {
                    core::LocalVariable keyTmp = cctx.newTemporary(core::Names::hashTemp());
                    core::LocalVariable valTmp = cctx.newTemporary(core::Names::hashTemp());
                    current = walk(cctx.withTarget(keyTmp), h->keys[i].get(), current);
                    current = walk(cctx.withTarget(valTmp), h->values[i].get(), current);
                    vars.emplace_back(keyTmp);
                    vars.emplace_back(valTmp);
                    locs.emplace_back(h->keys[i]->loc);
                    locs.emplace_back(h->values[i]->loc);
                }
                core::LocalVariable magic = cctx.newTemporary(core::Names::magic());
                synthesizeExpr(current, magic, core::Loc::none(), make_unique<Alias>(core::Symbols::Magic()));

                auto isPrivateOk = false;
                current->exprs.emplace_back(
                    cctx.target, h->loc,
                    make_unique<Send>(magic, core::Names::buildHash(), h->loc, vars, locs, isPrivateOk));
                ret = current;
            },

            [&](ast::Array *a) {
                InlinedVector<core::LocalVariable, 2> vars;
                InlinedVector<core::Loc, 2> locs;
                for (auto &elem : a->elems) {
                    core::LocalVariable tmp = cctx.newTemporary(core::Names::arrayTemp());
                    current = walk(cctx.withTarget(tmp), elem.get(), current);
                    vars.emplace_back(tmp);
                    locs.emplace_back(a->loc);
                }
                core::LocalVariable magic = cctx.newTemporary(core::Names::magic());
                synthesizeExpr(current, magic, core::Loc::none(), make_unique<Alias>(core::Symbols::Magic()));
                auto isPrivateOk = false;
                current->exprs.emplace_back(
                    cctx.target, a->loc,
                    make_unique<Send>(magic, core::Names::buildArray(), a->loc, vars, locs, isPrivateOk));
                ret = current;
            },

            [&](ast::Cast *c) {
                core::LocalVariable tmp = cctx.newTemporary(core::Names::castTemp());
                current = walk(cctx.withTarget(tmp), c->arg.get(), current);
                current->exprs.emplace_back(cctx.target, c->loc, make_unique<Cast>(tmp, c->type, c->cast));
                if (c->cast == core::Names::let()) {
                    cctx.inWhat.minLoops[cctx.target] = CFG::MIN_LOOP_LET;
                }

                ret = current;
            },

            [&](ast::EmptyTree *n) { ret = current; },

            [&](ast::ClassDef *c) { Exception::raise("Should have been removed by FlattenWalk"); },
            [&](ast::MethodDef *c) { Exception::raise("Should have been removed by FlattenWalk"); },

            [&](ast::Expression *n) { Exception::raise("Unimplemented AST Node: {}", n->nodeName()); });

        // For, Rescue,
        // Symbol, Array,
        ENFORCE(ret != nullptr, "CFB builder ret unset");
        return ret;
    } catch (SorbetException &) {
        Exception::failInFuzzer();
        if (auto e = cctx.ctx.state.beginError(what->loc, core::errors::Internal::InternalError)) {
            e.setHeader("Failed to convert tree to CFG (backtrace is above )");
        }
        throw;
    }
}

core::LocalVariable CFGContext::newTemporary(core::NameRef name) {
    return core::LocalVariable{name, ++temporaryCounter};
}

} // namespace sorbet::cfg
