#include "astutil.h"
#include "expr.h"
#include "passes.h"
#include "resolution.h"
#include "stmt.h"
#include "symbol.h"
#include "view.h"


static void
insertAutoDestroyTemp(Symbol* tmp, FnSymbol* autoDestroyFn) {
  Expr* stmt = tmp->defPoint;
  if (isArgSymbol(stmt->parentSymbol)) {
    INT_ASSERT(false); // delete this code
    BlockStmt* block = toBlockStmt(stmt->parentExpr);
    INT_ASSERT(block);
    SymExpr* se = toSymExpr(block->body.tail);
    if (!se) {
      VarSymbol* tmp2 = newTemp();
      tmp2->addFlag(FLAG_MAYBE_PARAM);
      tmp2->addFlag(FLAG_MAYBE_TYPE);
      block->insertAtHead(new DefExpr(tmp2));
      block->insertAtTail(new CallExpr(PRIM_MOVE, tmp2, block->body.tail->remove()));
      se = new SymExpr(tmp2);
      block->insertAtTail(se);
    }
    se->insertBefore(new CallExpr(autoDestroyFn, tmp));
    return;
  } else {
    while (stmt) {
      CallExpr* call = toCallExpr(stmt->next);
      if (!stmt->next ||
          (call && call->isPrimitive(PRIM_RETURN)) ||
          isGotoStmt(stmt->next)) {
        stmt->insertAfter(new CallExpr(autoDestroyFn, tmp));
        return;
      }
      stmt = stmt->next;
    }
  }
  INT_ASSERT(false);
}


//
// Cache to avoid cloning functions that return records if the copy
// of the returned argument is done in the same way as at another
// call site; the key into the cache is the old function, the values
// are stored in a vector based on the copy function (copy function
// 1, new function 1, copy function 2, new function 2, ...)
//
static Map<FnSymbol*,Vec<FnSymbol*>*> retToArgCache;

static void
changeRetToArgAndClone(CallExpr* move, Symbol* lhs,
                       CallExpr* call, FnSymbol* fn,
                       Map<Symbol*,Vec<SymExpr*>*>& defMap,
                       Map<Symbol*,Vec<SymExpr*>*>& useMap) {
  SymExpr* use = NULL;
  if (useMap.get(lhs) && useMap.get(lhs)->n == 1) {
    use = useMap.get(lhs)->v[0];
  } else {
    for (Expr* stmt = move->next; stmt && !use; stmt = stmt->next) {
      if (!isCallExpr(stmt) && !isDefExpr(stmt) && !isSymExpr(stmt))
        break;
      Vec<SymExpr*> symExprs;
      collectSymExprs(stmt, symExprs);
      forv_Vec(SymExpr, se, symExprs) {
        if (se->var == lhs) {
          use = se;
          break;
        }
      }
    }
  }

  if (use) {
    if (CallExpr* useCall = toCallExpr(use->parentExpr)) {
      if (FnSymbol* useFn = useCall->isResolved()) {
        if (!strcmp(useFn->name, "=") ||
            !strcmp(useFn->name, "chpl__initCopy") ||
            !strcmp(useFn->name, "chpl__autoCopy")) {

//           printf("CALL SITE\n");
//           list_view(move->getFunction());
//           printf("MOVE => ");
//           list_view(move);
//           printf("OLD FUNCTION\n");
//           list_view(fn);

          FnSymbol* newFn = NULL;

          //
          // check cache for new function
          //
          if (Vec<FnSymbol*>* vfn = retToArgCache.get(fn)) {
            for (int i = 0; i < vfn->n; i++) {
              if (vfn->v[i] == useFn) {
                newFn = vfn->v[i+1];
              }
            }
          }

          if (!newFn) {
            newFn = fn->copy();
            ArgSymbol* arg = new ArgSymbol(INTENT_BLANK, "_retArg", useFn->retType->refType);
            newFn->insertFormalAtTail(arg);
            Symbol* ret = newFn->getReturnSymbol();
            newFn->body->body.tail->replace(new CallExpr(PRIM_RETURN, gVoid));
            newFn->retType = dtVoid;
            fn->defPoint->insertBefore(new DefExpr(newFn));
            Vec<SymExpr*> symExprs;
            collectSymExprs(newFn, symExprs);
            forv_Vec(SymExpr, se, symExprs) {
              if (se->var == ret) {
                CallExpr* move = toCallExpr(se->parentExpr);
                if (move && move->isPrimitive(PRIM_MOVE) && move->get(1) == se) {
                  if (!strcmp(useFn->name, "=")) {
                    Symbol* tmp = newTemp(useFn->retType);
                    move->insertBefore(new DefExpr(tmp));
                    move->insertBefore(new CallExpr(PRIM_MOVE, tmp, new CallExpr(PRIM_GET_REF, arg)));
                    move->insertAfter(new CallExpr(PRIM_MOVE, arg, new CallExpr(useFn, tmp, ret)));
                  } else {
                    move->insertAfter(new CallExpr(PRIM_MOVE, arg, new CallExpr(useFn, ret)));
                  }
                } else {
                  Symbol* tmp = newTemp(useFn->retType);
                  se->getStmtExpr()->insertBefore(new DefExpr(tmp));
                  se->getStmtExpr()->insertBefore(new CallExpr(PRIM_MOVE, tmp, new CallExpr(PRIM_GET_REF, arg)));
                  se->var = tmp;
                }
              }
            }

            //
            // add new function to cache
            //
            Vec<FnSymbol*>* vfn = retToArgCache.get(fn);
            if (!vfn)
              vfn = new Vec<FnSymbol*>();
            vfn->add(useFn);
            vfn->add(newFn);
            retToArgCache.put(fn, vfn);
          }

          call->baseExpr->replace(new SymExpr(newFn));

          CallExpr* useMove = toCallExpr(useCall->parentExpr);
          INT_ASSERT(useMove && useMove->isPrimitive(PRIM_MOVE));

          Symbol* useLhs = toSymExpr(useMove->get(1))->var;
          if (!useLhs->type->symbol->hasFlag(FLAG_REF)) {
            useLhs = newTemp(useFn->retType->refType);
            move->insertBefore(new DefExpr(useLhs));
            move->insertBefore(new CallExpr(PRIM_MOVE, useLhs, new CallExpr(PRIM_SET_REF, useMove->get(1)->remove())));
          }
          // lhs->defPoint->remove();
          move->replace(call->remove());
          useMove->remove();
          call->insertAtTail(useLhs);

//           printf("NEW FUNCTION\n");
//           list_view(newFn);
//           printf("TRANSFORMED CALL SITE\n");
//           list_view(call->getFunction());
//           printf("MOVE => ");
//           list_view(move);

          return;
        }
      }
    }
    //USR_WARN(move, "possible premature free");
    //nprint_view(move);
    //  list_view(move->getFunction());
    //  list_view(fn);
  } else {
    if (useMap.get(lhs) && useMap.get(lhs)->n > 0) {
      //      USR_WARN(move, "possible premature free (use not found)");
      //      list_view(move);
    }
  }
}


void
returnRecordsByReferenceArguments() {
  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;
  buildDefUseMaps(defMap, useMap);

  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->parentSymbol) {
      if (FnSymbol* fn = requiresImplicitDestroy(call)) {
        CallExpr* move = toCallExpr(call->parentExpr);
        INT_ASSERT(move->isPrimitive(PRIM_MOVE));
        SymExpr* lhs = toSymExpr(move->get(1));
        INT_ASSERT(!lhs->var->hasFlag(FLAG_TYPE_VARIABLE));
        changeRetToArgAndClone(move, lhs->var, call, fn, defMap, useMap);
      }
    }
  }
  freeDefUseMaps(defMap, useMap);
}


static void
fixupDestructors() {
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (fn->hasFlag(FLAG_DESTRUCTOR)) {
      ClassType* ct = toClassType(fn->_this->type);
      if (ct->symbol->hasFlag(FLAG_REF))
        ct = toClassType(ct->getValueType());
      INT_ASSERT(ct);

      //
      // insert calls to destructors for all 'value' fields
      //
      for_fields_backward(field, ct) {
        if (field->type->destructor) {
          ClassType* fct = toClassType(field->type);
          INT_ASSERT(fct);
          if (!isClass(fct) || fct->symbol->hasFlag(FLAG_SYNC)) {
            bool useRefType = !fct->symbol->hasFlag(FLAG_ARRAY) && !fct->symbol->hasFlag(FLAG_DOMAIN) &&
              !fct->symbol->hasFlag(FLAG_SYNC);
            VarSymbol* tmp = useRefType ? newTemp(fct->refType) : newTemp(fct);
            fn->insertBeforeReturnAfterLabel(new DefExpr(tmp));
            fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_MOVE, tmp,
              new CallExpr(useRefType ? PRIM_GET_MEMBER : PRIM_GET_MEMBER_VALUE, fn->_this, field)));
            fn->insertBeforeReturnAfterLabel(new CallExpr(field->type->destructor, tmp));
            if (fct->symbol->hasFlag(FLAG_SYNC))
              fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_CHPL_FREE, tmp));
          }
        } else if (field->type == dtString && !ct->symbol->hasFlag(FLAG_TUPLE)) {
          VarSymbol* tmp = newTemp(dtString);
          fn->insertBeforeReturnAfterLabel(new DefExpr(tmp));
          fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_MOVE, tmp,
            new CallExpr(PRIM_GET_MEMBER_VALUE, fn->_this, field)));
          fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_CHPL_FREE, tmp));
        }
      }

      //
      // insert call to parent destructor
      //
      INT_ASSERT(ct->dispatchParents.n <= 1);
      if (ct->dispatchParents.n >= 1 && isClass(ct)) {
        // avoid destroying record fields more than once
        if (FnSymbol* parentDestructor = ct->dispatchParents.v[0]->destructor) {
          Type* tmpType = isClass(ct) // || ct->symbol->hasFlag(FLAG_ARRAY) || ct->symbol->hasFlag(FLAG_DOMAIN)
            ?
            ct->dispatchParents.v[0] : ct->dispatchParents.v[0]->refType;
          VarSymbol* tmp = newTemp(tmpType);
          fn->insertBeforeReturnAfterLabel(new DefExpr(tmp));
          fn->insertBeforeReturnAfterLabel(new CallExpr(PRIM_MOVE, tmp,
            new CallExpr(PRIM_CAST, tmpType->symbol, fn->_this)));
          fn->insertBeforeReturnAfterLabel(new CallExpr(parentDestructor, tmp));
        }
      }
    }
  }
}


void
callDestructors() {
  fixupDestructors();

  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->isPrimitive(PRIM_CALL_DESTRUCTOR)) {
      Type* type = call->get(1)->typeInfo();
      if (!type->destructor) {
        call->remove();
      } else if (call->get(1)->typeInfo()->refType == type->destructor->_this->type) {
        VarSymbol* tmp = newTemp(type->destructor->_this->type);
        call->insertBefore(new DefExpr(tmp));
        call->insertBefore(new CallExpr(PRIM_MOVE, tmp, new CallExpr(PRIM_SET_REF, call->get(1)->remove())));
        call->replace(new CallExpr(type->destructor, tmp));
      } else {
        call->replace(new CallExpr(type->destructor, call->get(1)->remove()));
      }
    }
  }

  Map<Symbol*,Vec<SymExpr*>*> defMap;
  Map<Symbol*,Vec<SymExpr*>*> useMap;
  buildDefUseMaps(defMap, useMap);

  forv_Vec(VarSymbol, sym, gVarSymbols) {
    if (sym->hasFlag(FLAG_INSERT_AUTO_COPY)) {
      CallExpr* move = NULL;
      for_defs(def, defMap, sym) {
        CallExpr* defCall = toCallExpr(def->parentExpr);
        if (defCall->isPrimitive(PRIM_MOVE)) {
          CallExpr* rhs = toCallExpr(defCall->get(2));
          if (!rhs || !rhs->isNamed("=")) {
            INT_ASSERT(!move);
            move = defCall;
          }
        }
      }
      INT_ASSERT(move);
      Symbol* tmp = newTemp(sym->type);
      move->insertBefore(new DefExpr(tmp));
      move->insertAfter(new CallExpr(PRIM_MOVE, sym, new CallExpr(autoCopyMap.get(sym->type), tmp)));
      move->get(1)->replace(new SymExpr(tmp));
    }
  }

  forv_Vec(VarSymbol, sym, gVarSymbols) {
    if (sym->hasFlag(FLAG_INSERT_AUTO_DESTROY))
      if (FnSymbol* autoDestroyFn = autoDestroyMap.get(sym->type))
        insertAutoDestroyTemp(sym, autoDestroyFn);
  }

  freeDefUseMaps(defMap, useMap);

  returnRecordsByReferenceArguments();

  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->parentSymbol) {
      if (call->isPrimitive(PRIM_YIELD)) {
        SymExpr* yieldExpr = toSymExpr(call->get(1));
        CallExpr* move = toCallExpr(call->prev);
        INT_ASSERT(move && move->isPrimitive(PRIM_MOVE));
        SymExpr* lhs = toSymExpr(move->get(1));
        INT_ASSERT(lhs && lhs->var == yieldExpr->var);
        Type* type = yieldExpr->var->type;
        if (isRecord(type) &&
            !type->symbol->hasFlag(FLAG_ITERATOR_RECORD) &&
            !type->symbol->hasFlag(FLAG_RUNTIME_TYPE_VALUE)) {
          Symbol* tmp = newTemp(type);
          move->insertBefore(new DefExpr(tmp));
          move->insertAfter(new CallExpr(PRIM_MOVE, yieldExpr->var, new CallExpr(autoCopyMap.get(tmp->type), tmp)));
          lhs->var = tmp;
        }
      }
    }
  }
}
