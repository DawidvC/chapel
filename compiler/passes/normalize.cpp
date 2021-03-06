/*
 * Copyright 2004-2018 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*** normalize
 ***
 *** This pass and function normalizes parsed and scope-resolved AST.
 ***/

#include "passes.h"

#include "astutil.h"
#include "build.h"
#include "driver.h"
#include "errorHandling.h"
#include "ForallStmt.h"
#include "IfExpr.h"
#include "initializerRules.h"
#include "stlUtil.h"
#include "stringutil.h"
#include "TransformLogicalShortCircuit.h"
#include "typeSpecifier.h"
#include "UnmanagedClassType.h"
#include "wellknown.h"

#include <cctype>
#include <set>
#include <vector>

bool normalized = false;

static void        insertModuleInit();
static FnSymbol*   toModuleDeinitFn(ModuleSymbol* mod, Expr* stmt);
static void        handleModuleDeinitFn(ModuleSymbol* mod);
static void        transformLogicalShortCircuit();
static void        handleReduceAssign();

static bool        isArrayFormal(ArgSymbol* arg);

static void        makeExportWrapper(FnSymbol* fn);

static void        fixupArrayFormals(FnSymbol* fn);

static bool        includesParameterizedPrimitive(FnSymbol* fn);
static void        replaceFunctionWithInstantiationsOfPrimitive(FnSymbol* fn);
static void        fixupQueryFormals(FnSymbol* fn);

static bool        isConstructor(FnSymbol* fn);

static void        updateConstructor(FnSymbol* fn);
static void        updateInitMethod (FnSymbol* fn);

static void        checkUseBeforeDefs();
static void        moveGlobalDeclarationsToModuleScope();
static void        insertUseForExplicitModuleCalls(void);

static void        lowerIfExprs(BaseAST* base);

static void        hack_resolve_types(ArgSymbol* arg);

static void        find_printModuleInit_stuff();

static void        normalizeBase(BaseAST* base);
static void        processSyntacticDistributions(CallExpr* call);
static void        processManagedNew(CallExpr* call);
static void        normalizeReturns(FnSymbol* fn);
static void        normalizeYields(FnSymbol* fn);

static bool        isCallToConstructor(CallExpr* call);
static void        normalizeCallToConstructor(CallExpr* call);
static void        fixStringLiteralInit(FnSymbol* fn);

static bool        isCallToTypeConstructor(CallExpr* call);
static void        normalizeCallToTypeConstructor(CallExpr* call);

static void        applyGetterTransform(CallExpr* call);
static void        insertCallTemps(CallExpr* call);
static void        insertCallTempsWithStmt(CallExpr* call, Expr* stmt);

static void        normalizeTypeAlias(DefExpr* defExpr);
static void        normalizeConfigVariableDefinition(DefExpr* defExpr);
static void        normalizeVariableDefinition(DefExpr* defExpr);

static void        normRefVar(DefExpr* defExpr);

static void        init_untyped_var(VarSymbol* var,
                                    Expr*      init,
                                    Expr*      insert,
                                    VarSymbol* constTemp);

static void        init_typed_var(VarSymbol* var,
                                  Expr*      type,
                                  Expr*      insert,
                                  VarSymbol* constTemp);

static void        init_typed_var(VarSymbol* var,
                                  Expr*      type,
                                  Expr*      init,
                                  Expr*      insert,
                                  VarSymbol* constTemp);

static void        init_noinit_var(VarSymbol* var,
                                   Expr*      type,
                                   Expr*      init,
                                   Expr*      insert,
                                   VarSymbol* constTemp);

static bool        moduleHonorsNoinit(Symbol* var, Expr* init);

static void        insertPostInit(Symbol* var, CallExpr* anchor);

static void        updateVariableAutoDestroy(DefExpr* defExpr);

static TypeSymbol* expandTypeAlias(SymExpr* se);

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

void normalize() {
  insertModuleInit();

  transformLogicalShortCircuit();

  handleReduceAssign();

  forv_Vec(AggregateType, at, gAggregateTypes) {
    if (isClassWithInitializers(at)  == true ||
        isRecordWithInitializers(at) == true) {
      preNormalizeFields(at);
    }

    preNormalizePostInit(at);
  }

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    SET_LINENO(fn);

    if (fn->hasFlag(FLAG_EXPORT) &&
        fn->hasFlag(FLAG_COMPILER_GENERATED)  == false) {
      makeExportWrapper(fn);
    }

    if (fn->hasFlag(FLAG_TYPE_CONSTRUCTOR)    == false &&
        fn->hasFlag(FLAG_DEFAULT_CONSTRUCTOR) == false) {
      fixupArrayFormals(fn);
    }

    if (includesParameterizedPrimitive(fn) == true) {
      replaceFunctionWithInstantiationsOfPrimitive(fn);

    } else {
      fixupQueryFormals(fn);

      if (isConstructor(fn) == true) {
        updateConstructor(fn);

      } else if (fn->isInitializer() == true) {
        updateInitMethod(fn);
      }
    }
  }

  normalizeBase(theProgram);

  normalized = true;

  checkUseBeforeDefs();

  moveGlobalDeclarationsToModuleScope();

  insertUseForExplicitModuleCalls();

  if (!fMinimalModules) {
    // Calls to _statementLevelSymbol() are inserted here and in
    // function resolution to ensure that sync vars are in the correct
    // state (empty) if they are used but not assigned to anything.
    forv_Vec(SymExpr, se, gSymExprs) {
      if (FnSymbol* parentFn = toFnSymbol(se->parentSymbol)) {
        if (se == se->getStmtExpr() &&
            // avoid exprs under ForallIntents
            (isDirectlyUnderBlockStmt(se) || !isBlockStmt(se->parentExpr))) {
          // Don't add these calls for the return type, since
          // _statementLevelSymbol would do nothing in that case
          // anyway, and it contributes to order-of-resolution issues for
          // extern functions with declared return type.
          if (parentFn->retExprType != se->parentExpr) {
            SET_LINENO(se);

            CallExpr* call = new CallExpr("_statementLevelSymbol");

            se->insertBefore(call);

            call->insertAtTail(se->remove());
          }
        }
      }
    }
  }

  forv_Vec(ArgSymbol, arg, gArgSymbols) {
    if (arg->defPoint->parentSymbol) {
      hack_resolve_types(arg);
    }
  }

  // perform some checks on destructors
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    if (fn->hasFlag(FLAG_DESTRUCTOR)) {
      if (fn->formals.length           <  2 ||
          fn->getFormal(1)->typeInfo() != gMethodToken->typeInfo()) {
        USR_FATAL(fn, "destructors must be methods");

      } else if (fn->formals.length > 2) {
        USR_FATAL(fn, "destructors must not have arguments");

      } else {
        DefExpr*       thisDef = toDefExpr(fn->formals.get(2));
        AggregateType* ct      = toAggregateType(thisDef->sym->type);

        INT_ASSERT(thisDef);

        // verify the name of the destructor
        bool notTildeName = (fn->name[0] != '~') ||
                             strcmp(fn->name + 1, ct->symbol->name) != 0;
        bool notDeinit    = (fn->name != astrDeinit);

        if (ct && notDeinit && notTildeName) {
          USR_FATAL(fn,
                    "destructor name must match class/record name "
                    "or deinit()");

        }

        if (!notDeinit && fn->hasFlag(FLAG_NO_PARENS)) {
          USR_FATAL_CONT(fn, "deinitializers must have parentheses");
        }

        fn->name = astrDeinit;
      }

    // make sure methods don't attempt to overload operators
    } else if (isalpha(fn->name[0])         == 0   &&
               fn->name[0]                  != '_' &&
               fn->formals.length           >  1   &&
               fn->getFormal(1)->typeInfo() == gMethodToken->typeInfo()) {
      USR_FATAL(fn, "invalid method name");
    }
  }

  find_printModuleInit_stuff();
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

void normalize(FnSymbol* fn) {
  if (fn->isNormalized() == false) {
    normalizeBase(fn);
    fn->setNormalized(true);
  }
}

void normalize(Expr* expr) {
  normalizeBase(expr);
}

/************************************* | **************************************
*                                                                             *
* Insert the module initFn in to every module in allModules.  The current     *
* implementation pulls the entire module in to the prototypical initFn and    *
* then lets the rest of normalize sort things out.                            *
* Also stash away the module deinitFn, if the user has written one.           *
*                                                                             *
************************************** | *************************************/

static void insertModuleInit() {
  // Insert an init function into every module
  forv_Vec(ModuleSymbol, mod, allModules) {
    SET_LINENO(mod);

    mod->initFn          = new FnSymbol(astr("chpl__init_", mod->name));
    mod->initFn->retType = dtVoid;

    mod->initFn->addFlag(FLAG_MODULE_INIT);
    mod->initFn->addFlag(FLAG_INSERT_LINE_FILE_INFO);

    //
    // move module-level statements into module's init function
    //
    for_alist(stmt, mod->block->body) {
      if (stmt->isModuleDefinition() == false) {
        if (FnSymbol* deinitFn = toModuleDeinitFn(mod, stmt)) {
          mod->deinitFn = deinitFn; // the rest is in handleModuleDeinitFn()

        } else {
          mod->initFn->insertAtTail(stmt->remove());
        }
      }
    }

    mod->block->insertAtHead(new DefExpr(mod->initFn));
    handleModuleDeinitFn(mod);

    //
    // If the module has the EXPORT_INIT flag then
    // propagate it to the module's init function
    //
    if (mod->hasFlag(FLAG_EXPORT_INIT) == true ||
        (fLibraryCompile == true && mod->modTag == MOD_USER)) {
      mod->initFn->addFlag(FLAG_EXPORT);
      mod->initFn->addFlag(FLAG_LOCAL_ARGS);
    }
  }

  USR_STOP();
}

static FnSymbol* toModuleDeinitFn(ModuleSymbol* mod, Expr* stmt) {
  FnSymbol* retval = NULL;

  if (DefExpr* def = toDefExpr(stmt)) {
    if (FnSymbol* fn = toFnSymbol(def->sym)) {
      if (fn->name == astrDeinit) {
        if (fn->numFormals() == 0) {
          if (mod->deinitFn == NULL) {
            retval = fn;

          } else {
            // Already got one deinit() before.
            // We could allow multiple deinit() fns and merge their contents.
            // If so, beware of possible 'return' stmts in each.
            USR_FATAL_CONT(def,
                           "an additional module deinit() "
                           "function is not allowed");

            USR_PRINT(mod->deinitFn,
                      "the first deinit() function is declared here");
          }
        }
      }
    }
  }

  return retval;
}

static void handleModuleDeinitFn(ModuleSymbol* mod) {
  if (FnSymbol* deinitFn = mod->deinitFn) {
    if (deinitFn->hasFlag(FLAG_NO_PARENS) == true) {
      USR_FATAL_CONT(deinitFn,
                     "module deinit() functions must have parentheses");
    }

    deinitFn->name = astr("chpl__deinit_", mod->name);
    deinitFn->removeFlag(FLAG_DESTRUCTOR);
  }
}

/************************************* | **************************************
*                                                                             *
* Historically, parser/build converted                                        *
*                                                                             *
*    <expr1> && <expr2>                                                       *
*    <expr1> || <expr2>                                                       *
*                                                                             *
* into an IfExpr (which itself currently has a complex implementation).       *
*                                                                             *
* Now we allow the parser to generate a simple unresolvable call to either    *
* && or || and then replace it with the original IF/THEN/ELSE expansion.      *
*                                                                             *
************************************** | *************************************/

static void transformLogicalShortCircuit() {
  std::set<Expr*>           stmts;
  std::set<Expr*>::iterator iter;

  // Collect the distinct stmts that contain logical AND/OR expressions
  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->primitive == 0) {
      if (UnresolvedSymExpr* expr = toUnresolvedSymExpr(call->baseExpr)) {
        if (strcmp(expr->unresolved, "&&") == 0 ||
            strcmp(expr->unresolved, "||") == 0) {
          stmts.insert(call->getStmtExpr());
        }
      }
    }
  }

  // Transform each expression.
  //
  // In general this will insert new IF-expressions immediately before the
  // current statement.  This approach interacts with Chapel's scoping
  // rule for do-while stmts.  We need to ensure that the additional
  // scope has been wrapped around the do-while before we perform this
  // transform.
  //
  for (iter = stmts.begin(); iter != stmts.end(); iter++) {
    Expr* stmt = *iter;
    TransformLogicalShortCircuit transform;

    if (isAlive(stmt)) {
      stmt->accept(&transform);
    }
  }
}

//
// handleReduceAssign(): check+process the reduce= calls
//
static void handleReduceAssign() {
  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->isPrimitive(PRIM_REDUCE_ASSIGN) == true) {
      INT_ASSERT(call->numActuals() == 2); // comes from the parser

      SET_LINENO(call);

      int rOpIdx;

      // l.h.s. must be a single variable
      if (SymExpr* lhsSE = toSymExpr(call->get(1))) {
        Symbol*     lhsVar      = lhsSE->symbol();
        ForallStmt* enclosingFS = enclosingForallStmt(call);

        if (enclosingFS == NULL) {
          USR_FATAL_CONT(call,
                         "The reduce= operator must occur within "
                         "a forall statement.");

        } else if ((rOpIdx = enclosingFS->reduceIntentIdx(lhsVar)) >= 0) {
          call->insertAtHead(new_IntSymbol(rOpIdx, INT_SIZE_64));

        } else {
          USR_FATAL(lhsSE,
                    "The l.h.s. of a reduce= operator, '%s', "
                    "must be passed by a reduce intent into the "
                    "nearest enclosing forall loop",
                    lhsVar->name);
        }

      } else {
        USR_FATAL(call->get(1),
                  "The l.h.s. of a reduce= operator must be just a variable");
      }
    }
  }
}

//
// handle reduce specs of shadow vars
//
static void insertCallTempsForRiSpecs(BaseAST* base) {
  std::vector<ForallStmt*> forallStmts;

  collectForallStmts(base, forallStmts);

  for_vector(ForallStmt, fs, forallStmts) {
    for_shadow_vars(svar, temp, fs) {
      if (CallExpr* specCall = toCallExpr(svar->reduceOpExpr())) {
        insertCallTempsWithStmt(specCall, fs);
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void normalizeBase(BaseAST* base) {
  //
  // Phase 0
  //
  normalizeErrorHandling(base);

  //
  // Phase 1
  //
  std::vector<CallExpr*> calls1;

  collectCallExprs(base, calls1);

  for_vector(CallExpr, call, calls1) {
    processSyntacticDistributions(call);
    processManagedNew(call);
  }


  //
  // Phase 2
  //
  std::vector<Symbol*> symbols;

  collectSymbols(base, symbols);

  for_vector(Symbol, symbol, symbols) {
    if (FnSymbol* fn = toFnSymbol(symbol)) {
      if (fn->isNormalized() == false) {
        normalizeReturns(fn);

        if (fn->isIterator() == true) {
          normalizeYields(fn);
        }
      }
    }
  }


  //
  // Phase 3
  //
  for_vector(Symbol, symbol, symbols) {
    if (VarSymbol* var = toVarSymbol(symbol)) {
      DefExpr* defExpr = var->defPoint;

      if (FnSymbol* fn = toFnSymbol(defExpr->parentSymbol)) {
        if (fn == stringLiteralModule->initFn) {
          fixStringLiteralInit(fn);
        } else if (fn->isNormalized() == false) {
          Expr* type = defExpr->exprType;
          Expr* init = defExpr->init;

          if (type != NULL || init != NULL) {
            if (var->isType() == true) {
              normalizeTypeAlias(defExpr);

            } else if (var->hasFlag(FLAG_CONFIG) == true) {
              normalizeConfigVariableDefinition(defExpr);

            } else {
              normalizeVariableDefinition(defExpr);
            }

            updateVariableAutoDestroy(defExpr);
          }
        }
      }
    }
  }

  lowerIfExprs(base);


  //
  // Phase 4
  //
  std::vector<CallExpr*> calls2;

  collectCallExprs(base, calls2);

  for_vector(CallExpr, call, calls2) {
    applyGetterTransform(call);
    insertCallTemps(call);
  }

  insertCallTempsForRiSpecs(base);

  // Handle calls to "type" constructor or "value" constructor
  for_vector(CallExpr, call, calls2) {
    if (isAlive(call) == true) {
      if (isCallToConstructor(call) == true) {
        normalizeCallToConstructor(call);

      } else if (isCallToTypeConstructor(call) == true) {
        normalizeCallToTypeConstructor(call);
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
* We can't really do this before resolution, because we need to know if       *
* symbols used as actual arguments are passed by ref, inout, or out           *
* (all of which would be considered definitions).                             *
*                                                                             *
* The workaround for this has been early initialization -- which is redundant *
* with guaranteed initialization, at least with respect to class instances.   *
*                                                                             *
************************************** | *************************************/

static Symbol* theDefinedSymbol(BaseAST* ast);

void checkUseBeforeDefs(FnSymbol* fn) {
  if (fn->defPoint->parentSymbol) {
    ModuleSymbol*         mod = fn->getModule();

    std::set<Symbol*>     defined;

    std::set<Symbol*>     undefined;
    std::set<const char*> undeclared;

    std::vector<BaseAST*> asts;

    collect_asts_postorder(fn, asts);

    for_vector(BaseAST, ast, asts) {
      if (Symbol* sym = theDefinedSymbol(ast)) {
        defined.insert(sym);

      } else if (SymExpr* se = toSymExpr(ast)) {
        Symbol* sym = se->symbol();

        if (isModuleSymbol(sym)                    == true  &&
            isFnSymbol(fn->defPoint->parentSymbol) == false &&
            isUseStmt(se->parentExpr)              == false) {
          SymExpr* prev = toSymExpr(se->prev);

          if (prev == NULL || prev->symbol() != gModuleToken) {
            USR_FATAL_CONT(se, "illegal use of module '%s'", sym->name);
          }

        } else if (isLcnSymbol(sym) == true) {
          if (sym->defPoint->parentExpr != rootModule->block) {
            Symbol* parent = sym->defPoint->parentSymbol;

            if (parent == fn || (parent == mod && mod->initFn == fn)) {
              if (defined.find(sym)           == defined.end() &&

                  sym->hasFlag(FLAG_ARG_THIS) == false         &&
                  sym->hasFlag(FLAG_EXTERN)   == false         &&
                  sym->hasFlag(FLAG_TEMP)     == false) {

                // Only complain one time
                if (undefined.find(sym) == undefined.end()) {
                  USR_FATAL_CONT(se,
                                 "'%s' used before defined (first used here)",
                                 sym->name);

                  undefined.insert(sym);
                }
              }
            }
          }
        }

      } else if (UnresolvedSymExpr* use = toUnresolvedSymExpr(ast)) {
        CallExpr* call = toCallExpr(use->parentExpr);

        if (call == NULL ||
            (call->baseExpr                              != use   &&
             call->isPrimitive(PRIM_CAPTURE_FN_FOR_CHPL) == false &&
             call->isPrimitive(PRIM_CAPTURE_FN_FOR_C)    == false)) {
          if (isFnSymbol(fn->defPoint->parentSymbol) == false) {
            const char* name = use->unresolved;

            // Only complain one time
            if (undeclared.find(name) == undeclared.end()) {
              USR_FATAL_CONT(use,
                             "'%s' undeclared (first use this function)",
                             name);

              undeclared.insert(name);
            }
          }
        }
      }
    }
  }
}

static void checkUseBeforeDefs() {
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    checkUseBeforeDefs(fn);
  }
  USR_STOP();
}

// If the AST node defines a symbol, then extract that symbol
static Symbol* theDefinedSymbol(BaseAST* ast) {
  Symbol* retval = NULL;

  // A symbol is "defined" if it is the LHS of a move, an assign,
  // or a variable initialization.
  //
  // The caller performs a post-order traversal and so we find the
  // symExpr before we see the callExpr
  //
  // TODO reacting to SymExprs, like it is done here, allows things like
  //   "var a: int = a" to sneak in.
  // Instead, we should react to CallExprs that are PRIM_MOVE, init, etc.
  // Reacting to CallExprs is also more economical, as there are fewer
  // CallExprs than there are SymExprs.
  //
  if (SymExpr* se = toSymExpr(ast)) {
    if (CallExpr* call = toCallExpr(se->parentExpr)) {
      if (call->isPrimitive(PRIM_MOVE)     == true  ||
          call->isPrimitive(PRIM_ASSIGN)   == true  ||
          call->isPrimitive(PRIM_INIT_VAR) == true)  {
        if (call->get(1) == se) {
          retval = se->symbol();
        }
      }
      // Allow for init() for a task-private variable, which occurs in
      // ShadowVarSymbol::initBlock(), which does not include its DefExpr.
      else if (ShadowVarSymbol* svar = toShadowVarSymbol(se->symbol())) {
        if (svar->isTaskPrivate()  &&
            call->isNamed("init")  &&
            // the first argument is gMethodToken
            call->get(2) == se)
          retval = svar;
      }
    }

  } else if (DefExpr* def = toDefExpr(ast)) {
    Symbol* sym = def->sym;

    // All arg symbols and loop induction variables are defined.
    // All shadow variables are defined.
    if (isArgSymbol(sym) ||
        isShadowVarSymbol(sym) ||
        sym->hasFlag(FLAG_INDEX_VAR)
    ) {
      retval = sym;

    } else if (VarSymbol* var = toVarSymbol(sym)) {
      // All type aliases are taken as defined.
      if (var->isType() == true) {
        retval = var;
      } else {
        Type* type = var->typeInfo();

        // All variables of type 'void' are treated as defined.
        if (type == dtVoid) {
          retval = var;

        // records with initializers are defined
        } else if (AggregateType* at = toAggregateType(type)) {
          if (isRecordWithInitializers(at) == true) {
            retval = var;
          }
        }
      }
    }
  }

  return retval;
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void moveGlobalDeclarationsToModuleScope() {
  bool move = false;

  forv_Vec(ModuleSymbol, mod, allModules) {
    for_alist(expr, mod->initFn->body->body) {
      // If the last iteration set "move" to true, move this block to the end
      // of the module (see below).
      if (move == true) {
        INT_ASSERT(isBlockStmt(expr));

        mod->block->insertAtTail(expr->remove());

        move = false;

      } else if (DefExpr* def = toDefExpr(expr)) {
        // Non-temporary variable declarations are moved out to module scope.
        if (VarSymbol* vs = toVarSymbol(def->sym)) {
          // Ignore compiler-inserted temporaries.
          // Only non-compiler-generated variables in the module init
          // function are moved out to module scope.
          //
          // Make an exception for references to array slices.
          if (vs->hasFlag(FLAG_TEMP) == true) {
            // is this a call_tmp that is later stored in a ref variable?
            // if so, move the call_tmp to global scope as well. E.g.
            //   var MyArray:[1..20] int;
            //   ref MySlice = MyArray[1..10];

            // Look for global = PRIM_ADDR_OF var
            //          global with flag FLAG_REF_VAR.
            bool refToTempInGlobal = false;

            for_SymbolSymExprs(se, vs) {
              if (CallExpr* addrOf = toCallExpr(se->parentExpr)) {
                if (addrOf->isPrimitive(PRIM_ADDR_OF) == true) {
                  if (CallExpr* move = toCallExpr(addrOf->parentExpr)) {
                    if (move->isPrimitive(PRIM_MOVE) == true) {
                      SymExpr* lhs = toSymExpr(move->get(1));

                      if (lhs->symbol()->hasFlag(FLAG_REF_VAR) == true) {
                        refToTempInGlobal = true;
                      }
                    }
                  }
                }
              }
            }

            if (refToTempInGlobal == false) {
              continue;
            }
          }

          // If the var declaration is an extern, we want to move its
          // initializer block with it.
          if (vs->hasFlag(FLAG_EXTERN) == true) {
            if (BlockStmt* block = toBlockStmt(def->next)) {
              // Mark this as a type block, so it is removed later.
              // Casts are because C++ is lame.
              (unsigned&)(block->blockTag) |= (unsigned) BLOCK_TYPE_ONLY;

              // Set the flag, so we move it out to module scope.
              move = true;
            }
          }

          mod->block->insertAtTail(def->remove());
        }

        // All type and function symbols are moved out to module scope.
        if (isTypeSymbol(def->sym) == true || isFnSymbol(def->sym) == true) {
          mod->block->insertAtTail(def->remove());
        }
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void insertUseForExplicitModuleCalls() {
  forv_Vec(SymExpr, se, gSymExprs) {
    if (se->inTree() && se->symbol() == gModuleToken) {
      SET_LINENO(se);

      CallExpr*     call  = toCallExpr(se->parentExpr);
      INT_ASSERT(call);

      SymExpr*      mse   = toSymExpr(call->get(2));
      INT_ASSERT(mse);

      ModuleSymbol* mod   = toModuleSymbol(mse->symbol());
      INT_ASSERT(mod);

      Expr*         stmt  = se->getStmtExpr();
      BlockStmt*    block = new BlockStmt();

      stmt->insertBefore(block);

      block->insertAtHead(stmt->remove());
      block->useListAdd(mod);
    }
  }
}

//
// Inserts a temporary for the result if the last statement is a call.
//
// Inserts PRIM_LOGICAL_FOLDER from the local branch result into the argument
// 'result' based on the argument 'cond'. PRIM_LOGICAL_FOLDER will either turn
// into an addr-of during resolution or be replaced by its second actual.
//
// BHARSH INIT TODO: Do we really need PRIM_LOGICAL_FOLDER anymore?
//
static void normalizeIfExprBranch(VarSymbol* cond, VarSymbol* result, BlockStmt* stmt) {
  Expr* last = stmt->body.tail->remove();
  Symbol* localResult = NULL;

  if (isCallExpr(last) || isIfExpr(last)) {
    localResult = newTemp();
    localResult->addFlag(FLAG_MAYBE_TYPE);
    localResult->addFlag(FLAG_MAYBE_PARAM);
    localResult->addFlag(FLAG_EXPR_TEMP);
    localResult->addFlag(FLAG_NO_AUTO_DESTROY);

    stmt->body.insertAtTail(new DefExpr(localResult));
    stmt->body.insertAtTail(new CallExpr(PRIM_MOVE, localResult, last));
  } else if (SymExpr* se = toSymExpr(last)) {
    localResult = se->symbol();
  } else {
    INT_FATAL("Unexpected AST node at the end of IfExpr branch");
  }

  stmt->body.insertAtTail(new CallExpr(PRIM_MOVE, result, new CallExpr(PRIM_LOGICAL_FOLDER, cond, localResult)));
}

//
// Transforms an IfExpr into a CondStmt.
//
// The expression at the end of each branch is moved into a temporary. This
// temporary will take the place of the IfExpr, and the CondStmt will be
// inserted before the IfExpr's parent statement.
//
class LowerIfExprVisitor : public AstVisitorTraverse
{
  public:
    LowerIfExprVisitor() { }
    virtual ~LowerIfExprVisitor() { }

    virtual void exitIfExpr(IfExpr* node);
};

void LowerIfExprVisitor::exitIfExpr(IfExpr* ife) {
  if (isAlive(ife) == false) return;
  if (isDefExpr(ife->parentExpr)) return;

  SET_LINENO(ife);

  VarSymbol* result = newTemp();
  result->addFlag(FLAG_MAYBE_TYPE);
  result->addFlag(FLAG_EXPR_TEMP);
  result->addFlag(FLAG_IF_EXPR_RESULT);

  // Don't auto-destroy local result if returning from a branch of a parent
  // if-expression.
  const bool parentIsIfExpr = isBlockStmt(ife->parentExpr) &&
                              isIfExpr(ife->parentExpr->parentExpr);
  if (parentIsIfExpr == false) {
    result->addFlag(FLAG_INSERT_AUTO_DESTROY);
  }

  VarSymbol* cond = newTemp();
  cond->addFlag(FLAG_MAYBE_PARAM);

  Expr* anchor = ife->getStmtExpr();
  anchor->insertBefore(new DefExpr(result));
  anchor->insertBefore(new DefExpr(cond));

  CallExpr* condTest = new CallExpr("_cond_test", ife->getCondition()->remove());
  anchor->insertBefore(new CallExpr(PRIM_MOVE, cond, condTest));

  normalizeIfExprBranch(cond, result, ife->getThenStmt());
  normalizeIfExprBranch(cond, result, ife->getElseStmt());

  CondStmt* cs = new CondStmt(new SymExpr(cond),
                              ife->getThenStmt()->remove(),
                              ife->getElseStmt()->remove());

  // Remove nested BlockStmts
  toBlockStmt(cs->thenStmt->body.tail)->flattenAndRemove();
  toBlockStmt(cs->elseStmt->body.tail)->flattenAndRemove();

  anchor->insertBefore(cs);

  ife->replace(new SymExpr(result));
}

static void lowerIfExprs(BaseAST* base) {
  LowerIfExprVisitor vis;
  base->accept(&vis);
}

/************************************* | **************************************
*                                                                             *
* Two cases are handled here:                                                 *
*    1. ('new' (dmap arg)) ==> (chpl__buildDistValue arg)                     *
*    2. (chpl__distributed (Dist args)) ==>                                   *
*       (chpl__distributed (chpl__buildDistValue ('new' (Dist args)))),       *
*        where isDistClass(Dist).                                             *
*                                                                             *
*  In 1., the only type that has FLAG_SYNTACTIC_DISTRIBUTION on it is "dmap". *
*  This is a dummy record type that must be replaced.  The call to            *
*  chpl__buildDistValue() performs this task, returning _newDistribution(x),  *
*  where x is a distribution.                                                 *
*                                                                             *
*    1. supports e.g.  var x = new dmap(new Block(...));                      *
*    2. supports e.g.  var y = space dmapped Block (...);                     *
*                                                                             *
************************************** | *************************************/

static void processSyntacticDistributions(CallExpr* call) {
  SET_LINENO(call);

  if (call->isPrimitive(PRIM_NEW) == true) {
    if (CallExpr* type = toCallExpr(call->get(1))) {
      if (SymExpr* base = toSymExpr(type->baseExpr)) {
        if (base->symbol()->hasFlag(FLAG_SYNTACTIC_DISTRIBUTION) == true) {
          const char* name = "chpl__buildDistValue";

          type->baseExpr->replace(new UnresolvedSymExpr(name));

          call->replace(type->remove());
        }
      }
    }
  }

  if (call->isNamed("chpl__distributed")) {
    if (CallExpr* distCall = toCallExpr(call->get(1))) {
      if (SymExpr* distClass = toSymExpr(distCall->baseExpr)) {
        if (TypeSymbol* ts = expandTypeAlias(distClass)) {
          if (isDistClass(ts->type) == true) {
            CallExpr* newExpr = new CallExpr(PRIM_NEW,
                new CallExpr(PRIM_TO_UNMANAGED_CLASS, distCall->remove()));

            call->insertAtHead(new CallExpr("chpl__buildDistValue", newExpr));

            processManagedNew(newExpr);
          }
        }
      }
    }
  }
}

/* Find patterns like
     (new (call <manager> (call ClassType <init-args>)))
     ... where <manager> might be _owned _to_unmanaged _shared

   and replace them with
     (new (call ClassType init-args _chpl_manager=<manager>)))

   Here the "manager" indicates to function resolution whether
   the new pointer should be:
    * unmanaged
    * owned
    * shared

   This happens before call-tmps are added because they
   would obscure the situation.
 */
static void processManagedNew(CallExpr* newCall) {
  SET_LINENO(newCall);
  if (newCall->inTree() && newCall->isPrimitive(PRIM_NEW)) {
    if (CallExpr* callManager = toCallExpr(newCall->get(1))) {
      if (callManager->numActuals() == 1) {
        if (CallExpr* callClass = toCallExpr(callManager->get(1))) {
          if (!callClass->isPrimitive() &&
              !isUnresolvedSymExpr(callClass->baseExpr)) {
            bool isunmanaged = callManager->isNamed("_to_unmanaged") ||
                               callManager->isPrimitive(PRIM_TO_UNMANAGED_CLASS);
            bool isborrowed = callManager->isNamed("_to_borrowed") ||
                              callManager->isPrimitive(PRIM_TO_BORROWED_CLASS);
            bool isowned = callManager->isNamed("_owned");
            bool isshared = callManager->isNamed("_shared");

            if (isunmanaged || isborrowed || isowned || isshared) {
              callClass->remove();
              callManager->remove();

              Expr* replace = new CallExpr(PRIM_NEW, callClass);

              Expr* manager = NULL;
              if (isunmanaged) {
                manager = new SymExpr(dtUnmanaged->symbol);
              } else if (isborrowed) {
                manager = new SymExpr(dtBorrowed->symbol);
              } else {
                manager = callManager->baseExpr->copy();
              }

              callClass->insertAtTail(new NamedExpr(astr_chpl_manager, manager));

              newCall->replace(replace);
            }
          }
        }
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
* Following normalization, each function contains only one return statement   *
* preceded by a label.  The first half of the function counts the total       *
* number of returns and the number of void returns.                           *
*                                                                             *
* The big IF beginning with if (rets.n == 1) determines if the function is    *
* already normal.                                                             *
*                                                                             *
* The last half of the function performs the normalization steps.             *
*                                                                             *
************************************** | *************************************/

static bool isVoidReturn(CallExpr* call);
static void insertRetMove(FnSymbol* fn, VarSymbol* retval, CallExpr* ret);

static void normalizeReturns(FnSymbol* fn) {
  SET_LINENO(fn);

  std::vector<CallExpr*> rets;
  std::vector<CallExpr*> calls;
  size_t                 numVoidReturns = 0;
  CallExpr*              theRet         = NULL;
  bool                   isIterator     = fn->isIterator();

  collectMyCallExprs(fn, calls, fn);

  for_vector(CallExpr, call, calls) {
    if (call->isPrimitive(PRIM_RETURN) == true) {
      rets.push_back(call);

      theRet = call;

      if (isVoidReturn(call) == true) {
        numVoidReturns++;
      }
    }
  }

  // Check if this function's returns are already normal.
  if (rets.size() == 1 && theRet == fn->body->body.last()) {
    if (SymExpr* se = toSymExpr(theRet->get(1))) {
      if (fn->hasFlag(FLAG_CONSTRUCTOR)         == true ||
          fn->hasFlag(FLAG_TYPE_CONSTRUCTOR)    == true ||
          strncmp("_if_fn", fn->name, 6)        ==    0 ||
          strcmp ("=",      fn->name)           ==    0 ||
          strcmp ("_init",  fn->name)           ==    0||
          strcmp ("_ret",   se->symbol()->name) ==    0) {
        return;
      }
    }
  }

  // Add a void return if needed.
  if (isIterator == false && rets.size() == 0 && fn->retExprType == NULL) {
    fn->insertAtTail(new CallExpr(PRIM_RETURN, gVoid));
    return;
  }

  LabelSymbol* label       = new LabelSymbol(astr("_end_", fn->name));
  bool         labelIsUsed = false;
  VarSymbol*   retval      = NULL;

  label->addFlag(FLAG_EPILOGUE_LABEL);

  fn->insertAtTail(new DefExpr(label));

  // Check that iterators do not return 'void'
  if (isIterator == true) {
    if (fn->retExprType != NULL && fn->retTag != RET_REF) {
      if (SymExpr* lastRTE = toSymExpr(fn->retExprType->body.tail)) {
        if (TypeSymbol* retSym = toTypeSymbol(lastRTE->symbol())) {
          if (retSym->type == dtVoid) {
            USR_FATAL_CONT(fn,
                           "an iterator's return type cannot be 'void'; "
                           "if specified, it must be the type of the "
                           "expressions the iterator yields");
          }
        }
      }
    }
  }

  // If a proc has a void return, do not return any values ever.
  // (Types are not resolved yet, so we judge by presence of "void returns"
  // i.e. returns with no expr. See also a related check in semanticChecks.)
  // (Note iterators always need an RVV so resolution knows to resolve the
  //  return/yield type)
  if (isIterator == false && numVoidReturns != 0) {
    fn->insertAtTail(new CallExpr(PRIM_RETURN, gVoid));

  } else {
    // Handle declared return type.
    retval = newTemp("ret", fn->retType);

    retval->addFlag(FLAG_RVV);

    if (fn->retTag == RET_PARAM) {
      retval->addFlag(FLAG_PARAM);
    }

    if (fn->retTag == RET_TYPE) {
      retval->addFlag(FLAG_TYPE_VARIABLE);
    }

    if (fn->hasFlag(FLAG_MAYBE_TYPE)) {
      retval->addFlag(FLAG_MAYBE_TYPE);
    }

    fn->insertAtHead(new DefExpr(retval));
    fn->insertAtTail(new CallExpr(PRIM_RETURN, retval));
  }

  // Now, for each return statement appearing in the function body,
  // move the value of its body into the declared return value.
  for_vector(CallExpr, ret, rets) {
    SET_LINENO(ret);

    if (isIterator == false && retval != NULL) {
      insertRetMove(fn, retval, ret);
    }

    // replace with GOTO(label)
    if (ret->next != label->defPoint) {
      ret->replace(new GotoStmt(GOTO_RETURN, label));

      labelIsUsed = true;
    } else {
      ret->remove();
    }
  }

  if (labelIsUsed == false) {
    label->defPoint->remove();
  }
}

static void normalizeYields(FnSymbol* fn) {
  INT_ASSERT(fn->isIterator());
  SET_LINENO(fn);

  std::vector<CallExpr*> calls;

  collectMyCallExprs(fn, calls, fn);

  for_vector(CallExpr, call, calls) {
    if (call->isPrimitive(PRIM_YIELD)) {

      CallExpr* yield = call;

      // For each yield statement, adjust it similarly to a return.
      SET_LINENO(yield);

      // Create a new YVV variable
      // MPF: I don't think YVV or RVV need to exist in the long term,
      // but using YVV enables minor adjustment in most of the rest of
      // the compiler.
      VarSymbol* retval = newTemp("yret", fn->retType);
      retval->addFlag(FLAG_YVV);

      yield->insertBefore(new DefExpr(retval));
      insertRetMove(fn, retval, yield);
      yield->insertBefore(new CallExpr(PRIM_YIELD, retval));
      yield->remove();
    }
  }
}

static bool isVoidReturn(CallExpr* call) {
  bool retval = false;

  if (call->isPrimitive(PRIM_RETURN) == true) {
    if (SymExpr* arg = toSymExpr(call->get(1))) {
      retval = (arg->symbol() == gVoid) ? true : false;
    }
  }

  return retval;
}

static void insertRetMove(FnSymbol* fn, VarSymbol* retval, CallExpr* ret) {
  Expr* retExpr = ret->get(1)->remove();

  if (fn->returnsRefOrConstRef() == true) {
    CallExpr* addrOf = new CallExpr(PRIM_ADDR_OF, retExpr);

    ret->insertBefore(new CallExpr(PRIM_MOVE, retval, addrOf));

  } else if (fn->retExprType != NULL) {
    Expr*     tail   = fn->retExprType->body.tail;
    CallExpr* coerce = new CallExpr(PRIM_COERCE, retExpr, tail->copy());

    ret->insertBefore(new CallExpr(PRIM_MOVE, retval, coerce));

  } else if (fn->hasFlag(FLAG_MAYBE_REF) == true) {
    ret->insertBefore(new CallExpr(PRIM_MOVE, retval, retExpr));

  } else if (fn->hasFlag(FLAG_WRAPPER)             == false &&
             strcmp(fn->name, "iteratorIndex")     !=     0 &&
             strcmp(fn->name, "iteratorIndexHelp") !=     0) {
    CallExpr* deref = new CallExpr(PRIM_DEREF, retExpr);

    ret->insertBefore(new CallExpr(PRIM_MOVE, retval, deref));

  } else {
    ret->insertBefore(new CallExpr(PRIM_MOVE, retval, retExpr));
  }
}

/************************************* | **************************************
*                                                                             *
* Transform   new (call C args...) args2...                                   *
*      into   new       C args...  args2...                                   *
*                                                                             *
* Transform   new (call (call (partial) C _mt this) args...)) args2...        *
*      into   new (call       (partial) C _mt this) args...   args2...        *
*                                                                             *
* Do not transform calls that are nested within a DefExpr (if stmt-expr is    *
* NULL). Calls within DefExprs do not have call-temps inserted, which if      *
* transformed would lead to incorrect AST like:                               *
*   (new C (call foo))                                                        *
* The expectation is that the expressions will be normalized later once the   *
* DefExpr's init/type expressions are copied into a BlockStmt.                *
*                                                                             *
************************************** | *************************************/

static void fixPrimNew(CallExpr* primNewToFix);

static bool isCallToConstructor(CallExpr* call) {
  return call->isPrimitive(PRIM_NEW);
}

static void normalizeCallToConstructor(CallExpr* call) {
  if (call->getStmtExpr() != NULL) {
    if (CallExpr* arg1 = toCallExpr(call->get(1))) {
      if (isSymExpr(arg1->baseExpr) == true) {
        if (arg1->partialTag == false) {
          fixPrimNew(call);
        }

      } else if (CallExpr* subCall = toCallExpr(arg1->baseExpr)) {
        if (isSymExpr(subCall->baseExpr) == true) {
          if (subCall->partialTag == true) {
            fixPrimNew(call);
          }
        }
      }
    }
  }
}

static void fixPrimNew(CallExpr* primNewToFix) {
  SET_LINENO(primNewToFix);

  CallExpr* callInNew    = toCallExpr(primNewToFix->get(1));
  CallExpr* newNew       = new CallExpr(PRIM_NEW);
  Expr*     exprModToken = NULL;
  Expr*     exprMod      = NULL;

  if (callInNew->numActuals() >= 2) {
    if (SymExpr* se1 = toSymExpr(callInNew->get(1))) {
      if (se1->symbol() == gModuleToken) {
        exprModToken = callInNew->get(1)->remove();
        exprMod      = callInNew->get(1)->remove();
      }
    }
  }

  Expr* baseExpr = callInNew->baseExpr->remove();
  callInNew->remove();

  primNewToFix->replace(newNew);

  newNew->insertAtHead(baseExpr);

  // Move the actuals from the call to the new PRIM_NEW
  for_actuals(actual, callInNew) {
    newNew->insertAtTail(actual->remove());
  }

  // Move actual from the PRIM_NEW as well
  // This is not the expected AST form, but keeping this
  // code here adds some resiliency.
  for_actuals(actual, primNewToFix) {
    newNew->insertAtTail(actual->remove());
  }

  if (exprModToken != NULL) {
    newNew->insertAtHead(exprMod);
    newNew->insertAtHead(exprModToken);
  }
}

static void fixStringLiteralInit(FnSymbol* fn) {
  // BHARSH 2018-05-10: Using something like 'collectCallExprs' here resulted
  // in nontrivial compilation slowdown. We know we're looking for MOVEs from
  // a NEW, so we can just walk the statements.
  Expr* first = fn->body->body.head;
  while (first != NULL) {
    CallExpr* call = toCallExpr(first);
    if (call != NULL && call->isPrimitive(PRIM_MOVE)) {
      CallExpr* rhs = toCallExpr(call->get(2));
      if (rhs != NULL && rhs->isPrimitive(PRIM_NEW)) {
        if (UnresolvedSymExpr* use = toUnresolvedSymExpr(rhs->get(1))) {
          use->replace(new SymExpr(dtString->symbol));
        }
      }
    }
    first = first->next;
  }
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static SymExpr* callUsedInRiSpec(Expr* call);
static void     restoreReduceIntentSpecCall(SymExpr* riSpec, CallExpr* call);

static bool isCallToTypeConstructor(CallExpr* call) {
  bool retval = false;

  if (SymExpr* se = toSymExpr(call->baseExpr)) {
    if (TypeSymbol* ts = expandTypeAlias(se)) {
      if (isAggregateType(ts->type) == true) {
        // Ensure it is not nested within a new expr
        CallExpr* parent = toCallExpr(call->parentExpr);

        if (parent == NULL) {
          retval = true;

        } else if (parent->isPrimitive(PRIM_NEW) == true) {
          retval = false;

        } else if (CallExpr* parentParent = toCallExpr(parent->parentExpr)) {
          retval = parentParent->isPrimitive(PRIM_NEW) == false;

        } else {
          retval = true;
        }
      }
    }
  }

  return retval;
}

static void normalizeCallToTypeConstructor(CallExpr* call) {
  if (call->getStmtExpr() != NULL) {
    if (SymExpr* se = toSymExpr(call->baseExpr)) {
      if (TypeSymbol* ts = expandTypeAlias(se)) {
        if (AggregateType* at = toAggregateType(ts->type)) {
          SET_LINENO(call);

          if (at->symbol->hasFlag(FLAG_SYNTACTIC_DISTRIBUTION) == true) {
            // Call chpl__buildDistType for syntactic distributions.
            se->replace(new UnresolvedSymExpr("chpl__buildDistType"));

          } else if (SymExpr* riSpec = callUsedInRiSpec(call)) {
            restoreReduceIntentSpecCall(riSpec, call);

          } else {
            // Transform C ( ... ) into _type_construct_C ( ... )

            // The old constructor-based implementation of nested types made
            // the type constructor a method on the enclosing type. Using a
            // method would not be allowed within an initializer because 'this'
            // may not yet be initialized/instantiated.
            //
            // Instead the initializer-based implementation of nested types
            // hoists the nested type constructor outside of the enclosing type
            // as a standalone function, like other type constructors.
            //
            // This could lead to problems with resolution if a type at the
            // same scope as the enclosing type had the same name as the nested
            // type. E.g.:
            //
            //   class Node {}
            //   class List {
            //     var n : Node; // which _type_construct_Node ?
            //     class Node {}
            //   }
            //
            // To work around this, use a SymExpr pointing to the type
            // constructor we know to be correct.
            if (at->hasInitializers()) {
              se->replace(new SymExpr(at->typeConstructor));
            } else {
              const char* name = at->typeConstructor->name;
              se->replace(new UnresolvedSymExpr(name));
            }
          }
        }
      }
    }
  }
}

//
// These helpers handle RiSpec (Reduce Intent Specification) i.e.:
//
//   forall ... with (<RiSpec> reduce <outer variable>) { ... }
//
// In particular, they implement RiSpecs of the form type(someArg).
// See e.g. test/parallel/forall/vass/3types-*.
// We want to keep these reduce intents in their original form
// until we process reduce intents later.
//
// We do it here to avoid transforming it into _type_construct_C ( ... ).
// That would be incorrect because this is a special syntax for reduce intent.
//
static SymExpr* callUsedInRiSpec(Expr* call) {
  SymExpr* retval = NULL;

  if (CallExpr* parent = toCallExpr(call->parentExpr)) {
    if (parent->isPrimitive(PRIM_MOVE) == true) {
      Symbol*  dest        = toSymExpr(parent->get(1))->symbol();
      SymExpr* riSpecMaybe = dest->firstSymExpr();
      Symbol*  symParent   = riSpecMaybe->parentSymbol;

      if (ShadowVarSymbol* svar = toShadowVarSymbol(symParent)) {
        if (riSpecMaybe == svar->reduceOpExpr()) {
          retval = riSpecMaybe;
        }
      }
    }
  }

  return retval;
}

//
// This function partially un-does normalization
// so that reduce intents specs (see above) don't get messed up.
//
static void restoreReduceIntentSpecCall(SymExpr* riSpec, CallExpr* call) {
  Symbol* temp = riSpec->symbol();

  // Verify the pattern that occurs if callUsedInRiSpec() returns true.
  // If any of these fail, either the pattern changed or callUsedInRiSpec()
  // returns true when it shouldn't.
  INT_ASSERT(temp->firstSymExpr()      == riSpec);
  INT_ASSERT(temp->lastSymExpr()->next == call);

  // 'temp' has only 2 SymExprs
  INT_ASSERT(riSpec->symbolSymExprsNext == temp->lastSymExpr());

  // Remove 'temp'.
  temp->defPoint->remove();

  call->parentExpr->remove();

  // Put 'call' back into riSpec.
  riSpec->replace(call->remove());
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void applyGetterTransform(CallExpr* call) {
  // Most generally:
  //   x.f(a) --> f(_mt, x)(a)
  // which is the same as
  //   call(call(. x "f") a) --> call(call(f _mt x) a)
  // Also:
  //   x.f --> f(_mt, x)
  // Note:
  //   call(call or )( indicates partial
  if (call->isNamedAstr(astrSdot)) {
    SET_LINENO(call);

    if (SymExpr* symExpr = toSymExpr(call->get(2))) {

      symExpr->remove();

      if (VarSymbol* var = toVarSymbol(symExpr->symbol())) {
        if (var->immediate->const_kind == CONST_KIND_STRING) {
          const char* str = var->immediate->v_string;

          call->baseExpr->replace(new UnresolvedSymExpr(str));

          call->insertAtHead(gMethodToken);

        } else {
          INT_FATAL(call, "unexpected case");
        }

      } else if (TypeSymbol* type = toTypeSymbol(symExpr->symbol())) {
        call->baseExpr->replace(new SymExpr(type));
        call->insertAtHead(gMethodToken);

      } else {
        INT_FATAL(call, "unexpected case");
      }

    } else if (UnresolvedSymExpr* symExpr = toUnresolvedSymExpr(call->get(2))) {
      call->baseExpr->replace(symExpr->remove());
      call->insertAtHead(gMethodToken);

    } else {
      INT_FATAL(call, "unexpected case");
    }

    call->methodTag = true;

    if (CallExpr* parent = toCallExpr(call->parentExpr)) {
      if (parent->baseExpr == call) {
        call->partialTag = true;
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static bool shouldInsertCallTemps(CallExpr* call);
static void evaluateAutoDestroy(CallExpr* call, VarSymbol* tmp);
static bool moveMakesTypeAlias(CallExpr* call);

static void insertCallTemps(CallExpr* call) {
  if (shouldInsertCallTemps(call) == true) {
    insertCallTempsWithStmt(call, call->getStmtExpr());
  }
}

static void insertCallTempsWithStmt(CallExpr* call, Expr* stmt) {
  SET_LINENO(call);

  CallExpr*  parentCall = toCallExpr(call->parentExpr);
  VarSymbol* tmp        = newTemp("call_tmp");

  stmt->insertBefore(new DefExpr(tmp));

  if (call->isPrimitive(PRIM_NEW) == true) {
    tmp->addFlag(FLAG_INSERT_AUTO_DESTROY_FOR_EXPLICIT_NEW);

  } else {
    // Add FLAG_EXPR_TEMP unless this tmp is being used
    // as a sub-expression for a variable initialization.
    // This flag triggers autoCopy/autoDestroy behavior.
    if (parentCall == NULL ||
        (parentCall->isNamed("chpl__initCopy")  == false &&
         parentCall->isPrimitive(PRIM_INIT_VAR) == false)) {
      tmp->addFlag(FLAG_EXPR_TEMP);
    }
  }

  if (call->isPrimitive(PRIM_TYPEOF) == true) {
    tmp->addFlag(FLAG_TYPE_VARIABLE);
  }

  evaluateAutoDestroy(call, tmp);

  tmp->addFlag(FLAG_MAYBE_PARAM);
  tmp->addFlag(FLAG_MAYBE_TYPE);

  if (call->isNamed("super")            == true &&
      parentCall                        != NULL &&
      parentCall->isNamedAstr(astrSdot) == true &&
      parentCall->get(1)                == call) {
    // We've got an access to a method or field on the super type.
    // This means we should preserve that knowledge for when we
    // attempt to access the method on the super type.
    tmp->addFlag(FLAG_SUPER_TEMP);
  }

  call->replace(new SymExpr(tmp));

  stmt->insertBefore(new CallExpr(PRIM_MOVE, tmp, call));
}

static bool shouldInsertCallTemps(CallExpr* call) {
  Expr*     parentExpr = call->parentExpr;
  CallExpr* parentCall = toCallExpr(parentExpr);
  Expr*     stmt       = call->getStmtExpr();
  bool      retval     = false;

  if        (parentExpr                               == NULL) {
    retval = false;

  } else if (isDefExpr(parentExpr)                    == true) {
    retval = false;

  } else if (isContextCallExpr(parentExpr)            == true) {
    retval = false;

  } else if (stmt                                     == NULL) {
    retval = false;

  } else if (call                                     == stmt) {
    retval = false;

  } else if (call->partialTag                         == true) {
    retval = false;

  } else if (call->isPrimitive(PRIM_TUPLE_EXPAND)     == true) {
    retval = false;

  } else if (parentCall && parentCall->isPrimitive(PRIM_MOVE)) {
    retval = false;

  } else if (parentCall && parentCall->isPrimitive(PRIM_NEW))  {
    retval = false;

  } else {
    retval =  true;
  }

  return retval;
}

static void evaluateAutoDestroy(CallExpr* call, VarSymbol* tmp) {
  Expr*     parentExpr = call->parentExpr;
  CallExpr* parentCall = toCallExpr(parentExpr);
  FnSymbol* fn         = call->getFunction();

  // Noakes 2015/11/02
  //   The expansion of _build_tuple() creates temps that need to be
  //   autoDestroyed.  This is a short-cut to arrange for that to occur.
  //   A better long term solution would be preferred
  if (call->isNamed("chpl__initCopy")     == true &&
      parentCall                          != NULL &&
      parentCall->isNamed("_build_tuple") == true) {
    tmp->addFlag(FLAG_INSERT_AUTO_DESTROY);
  }

  // MPF 2016-10-20
  // This is a workaround for a problem in
  //   types/typedefs/bradc/arrayTypedef
  //
  // I'm sure that there is a better way to handle this either in the
  // module init function or in a sequence of parloopexpr functions
  // computing an array type that are in a module init fn

  while (fn->hasFlag(FLAG_MAYBE_ARRAY_TYPE) == true) {
    fn = fn->defPoint->getFunction();
  }

  if (fn == fn->getModule()->initFn) {
    CallExpr* cur = parentCall;
    CallExpr* sub = call;

    // Look for a parent call that is either:
    //  making an array type alias, or
    //  passing the result into the 2nd argument of buildArrayRuntimeType.
    while (cur != NULL) {
      if (moveMakesTypeAlias(cur) == true) {
        break;

      } else if (cur->isNamed("chpl__buildArrayRuntimeType") == true &&
                 cur->get(2)                                 == sub) {
        break;

      } else {
        sub = cur;
        cur = toCallExpr(cur->parentExpr);
      }
    }

    if (cur) {
      tmp->addFlag(FLAG_NO_AUTO_DESTROY);
    }
  }
}

static bool moveMakesTypeAlias(CallExpr* call) {
  bool retval = false;

  if (call->isPrimitive(PRIM_MOVE)) {
    if (SymExpr* se = toSymExpr(call->get(1))) {
      if (VarSymbol* var = toVarSymbol(se->symbol())) {
        retval = var->isType();
      }
    }
  }

  return retval;
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void normalizeTypeAlias(DefExpr* defExpr) {
  SET_LINENO(defExpr);

  Symbol* var  = defExpr->sym;
  Expr*   type = defExpr->exprType;
  Expr*   init = defExpr->init;

  INT_ASSERT(type == NULL);
  INT_ASSERT(init != NULL);

  defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, init->copy()));
}

/************************************* | **************************************
*                                                                             *
* Config variables are fundamentally different form non-configs especially    *
* for multi-locale programs. Non-param config variables e.g.                  *
*                                                                             *
*   config var x : int = 10;                                                  *
*                                                                             *
* should be "initialized" in a manner that is approximately                   *
*                                                                             *
*   var x : int = no-init;                                                    *
*                                                                             *
*   if (!chpl_config_has_value("x", <module-name>)) then                      *
*     x = 10;                                                                 *
*   else                                                                      *
*     x = chpl_config_get_value("x", <module-name>);                          *
*                                                                             *
* and such that the conditional arms of the if-stmt implement initialization  *
* rather than assignment.  This requires additional care for config const and *
* multi-locale in order to enable privatization to be implemented correctly.  *
*                                                                             *
* Noakes Feb 17, 2017:                                                        *
*   The compiler has weaknesses with variable initialization which are a      *
* little more evident for config variables.  Configs have been split from     *
* non-configs to enable them to evolve independently in the nearer term.      *
*                                                                             *
* Additionally the current implementation has, undocumented and confusing,    *
* support for config ref and config const ref.  There has been discussion     *
* on whether to turn this in to a compile-time error or to continue the       *
* current support.                                                            *
*                                                                             *
************************************** | *************************************/

static CondStmt* assignConfig(VarSymbol* var,
                              VarSymbol* varTmp,
                              Expr*      noop);

static Symbol*   varModuleName(VarSymbol* var);

static void normalizeConfigVariableDefinition(DefExpr* defExpr) {
  SET_LINENO(defExpr);

  VarSymbol* var  = toVarSymbol(defExpr->sym);
  Expr*      type = defExpr->exprType;
  Expr*      init = defExpr->init;

  // Noakes: Feb 17, 2017
  //   config ref / const ref can be overridden at compile time.
  //   There is a proposal to convert this to a compile time error.
  if (var->hasFlag(FLAG_REF_VAR)) {
    normRefVar(defExpr);

  } else {
    VarSymbol* varTmp = var;
    Expr*      insert = defExpr;

    // insert code to initialize a config var/const
    // config param is evaluated at compile time
    if (var->hasFlag(FLAG_PARAM) == false) {
      if (var->hasFlag(FLAG_CONST)  ==  true &&
          var->hasFlag(FLAG_EXTERN) == false) {
        varTmp = newTemp("tmp");

        defExpr->insertBefore(new DefExpr(varTmp));
        defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, varTmp));
      }

      insert = new CallExpr(PRIM_NOOP);
      defExpr->insertAfter(assignConfig(var, varTmp, insert));
    }

    if (type == NULL) {
      init_untyped_var(var, init, insert, varTmp);

    } else if (init == NULL) {
      init_typed_var(var, type, insert, varTmp);

    } else if (var->hasFlag(FLAG_PARAM) == true) {
      CallExpr* cast = createCast(init->remove(), type->remove());

      insert->insertAfter(new CallExpr(PRIM_MOVE, var, cast));

    } else if (init->isNoInitExpr() == true) {
      init_noinit_var(var, type, init, insert, varTmp);

    } else {
      init_typed_var(var, type, init, insert, varTmp);
    }
  }
}

static CondStmt* assignConfig(VarSymbol* var, VarSymbol* varTmp, Expr* noop) {
  Symbol*    modName  = varModuleName(var);

  //
  // A fragment for the conditional test
  //
  SymExpr*   name0    = new SymExpr(new_CStringSymbol(var->name));
  CallExpr*  hasValue = new CallExpr("chpl_config_has_value", name0, modName);
  CallExpr*  test     = new CallExpr("!", hasValue);

  //
  // An "empty" block stmt for the consequent
  //
  BlockStmt* cons     = new BlockStmt(noop);

  //
  // The alternative sets the config from the command line
  //
  SymExpr*   name1    = new SymExpr(new_CStringSymbol(var->name));
  CallExpr*  typeOf   = new CallExpr(PRIM_TYPEOF, varTmp);

  SymExpr*   name2    = new SymExpr(new_CStringSymbol(var->name));
  CallExpr*  getValue = new CallExpr("chpl_config_get_value", name2, modName);

  CallExpr*  strToVal = new CallExpr("_command_line_cast",
                                     name1,
                                     typeOf,
                                     getValue);

  CallExpr*  moveTmp  = new CallExpr(PRIM_MOVE, varTmp, strToVal);
  BlockStmt* alt      = new BlockStmt(moveTmp);

  return new CondStmt(test, cons, alt);
}

static Symbol* varModuleName(VarSymbol* var) {
  ModuleSymbol* module     = var->getModule();
  bool          isInternal = module->modTag == MOD_INTERNAL;

  return new_CStringSymbol(isInternal ? "Built-in" : module->name);
}

static void init_untyped_var(VarSymbol* var,
                             Expr*      init,
                             Expr*      insert,
                             VarSymbol* constTemp) {
  init = init->remove();

  if (var->hasFlag(FLAG_NO_COPY)) {
    insert->insertAfter(new CallExpr(PRIM_MOVE, var, init));

  } else {
    // See Note 4.
    //
    // initialize untyped variable with initialization expression
    //
    // sjd: this new specialization of PRIM_NEW addresses the test
    //         test/classes/diten/test_destructor.chpl
    //      in which we call an explicit record destructor and avoid
    //      calling the default constructor.  However, if written with
    //      an explicit type, this would happen.  The record in this
    //      test is an issue since its destructor deletes field c, but
    //      the default constructor does not 'new' it.  Thus if we
    //      pass the record to a function and it is copied, we have an
    //      issue since we will do a double free.
    //
    CallExpr* initCall = toCallExpr(init);
    Expr*     rhs      = NULL;

    if (initCall && initCall->isPrimitive(PRIM_NEW)) {
      rhs = init;
    } else {
      rhs = new CallExpr("chpl__initCopy", init);
    }

    insert->insertAfter(new CallExpr(PRIM_MOVE, constTemp, rhs));
  }
}

static void init_typed_var(VarSymbol* var,
                           Expr*      type,
                           Expr*      insert,
                           VarSymbol* constTemp) {
  VarSymbol* typeTemp = newTemp("type_tmp");
  DefExpr*   typeDefn = new DefExpr(typeTemp);
  CallExpr*  initCall = new CallExpr(PRIM_INIT, type->remove());
  CallExpr*  initMove = new CallExpr(PRIM_MOVE, typeTemp, initCall);

  //
  // Noakes 2016/02/02
  // The code for resolving the type of an extern variable
  //
  //   functionResolution.cpp : resolveExternVarSymbols()
  //
  // expects to find the init code inside a block stmt.
  //
  // However the remaining cases do not need it.
  //
  if (var->hasFlag(FLAG_EXTERN) == true) {
    INT_ASSERT(var->hasFlag(FLAG_PARAM) == false);

    BlockStmt* block = new BlockStmt(NULL, BLOCK_EXTERN_TYPE);

    block->insertAtTail(typeDefn);
    block->insertAtTail(initMove);
    block->insertAtTail(new CallExpr(PRIM_MOVE, constTemp, typeTemp));

    insert->insertAfter(block);

  } else {
    if (var->hasFlag(FLAG_PARAM) == true) {
      typeTemp->addFlag(FLAG_PARAM);
    }

    insert->insertAfter(typeDefn);
    typeDefn->insertAfter(initMove);
    initMove->insertAfter(new CallExpr(PRIM_MOVE, constTemp, typeTemp));
  }
}

static void init_typed_var(VarSymbol* var,
                           Expr*      type,
                           Expr*      init,
                           Expr*      insert,
                           VarSymbol* constTemp) {
  VarSymbol* typeTemp = newTemp("type_tmp");
  DefExpr*   typeDefn = new DefExpr(typeTemp);
  CallExpr*  initCall = new CallExpr(PRIM_INIT, type->remove());
  CallExpr*  initMove = new CallExpr(PRIM_MOVE, typeTemp,  initCall);
  CallExpr*  assign   = new CallExpr("=",       typeTemp,  init->remove());
  CallExpr*  varMove  = new CallExpr(PRIM_MOVE, constTemp, typeTemp);

  insert->insertAfter(typeDefn);
  typeDefn->insertAfter(initMove);
  initMove->insertAfter(assign);
  assign->insertAfter(varMove);
}

static void init_noinit_var(VarSymbol* var,
                            Expr*      type,
                            Expr*      init,
                            Expr*      insert,
                            VarSymbol* constTemp) {
  init->remove();

  if (fUseNoinit == true || moduleHonorsNoinit(var, init) == true) {
    CallExpr* noinitCall = new CallExpr(PRIM_NO_INIT, type->remove());

    insert->insertAfter(new CallExpr(PRIM_MOVE, var, noinitCall));

  } else {
    // Ignore no-init expression and fall back on default init
    init_typed_var(var, type, insert, constTemp);
  }
}

/************************************* | **************************************
*                                                                             *
* normalizeVariableDefinition removes DefExpr::exprType and DefExpr::init     *
* from a variable's def expression, normalizing the AST with primitive        *
* moves, calls to chpl__initCopy, _init, and _cast, and assignments.          *
*                                                                             *
************************************** | *************************************/

static void           normVarTypeInference(DefExpr* expr);
static void           normVarTypeWoutInit(DefExpr* expr);
static void           normVarTypeWithInit(DefExpr* expr);
static void           normVarNoinit(DefExpr* defExpr);

static bool           isNewExpr(Expr* expr);
static AggregateType* typeForNewExpr(CallExpr* expr);

static Expr* prepareShadowVarForNormalize(DefExpr* def, VarSymbol* var);
static void  restoreShadowVarForNormalize(DefExpr* def, Expr* svarMark);

static void normalizeVariableDefinition(DefExpr* defExpr) {
  SET_LINENO(defExpr);

  VarSymbol* var  = toVarSymbol(defExpr->sym);
  Expr*      type = defExpr->exprType;
  Expr*      init = defExpr->init;
  Expr*  svarMark = prepareShadowVarForNormalize(defExpr, var);

  // handle ref variables
  if (var->hasFlag(FLAG_REF_VAR)) {
    normRefVar(defExpr);

  } else if (type == NULL && init != NULL) {
    normVarTypeInference(defExpr);

  } else if (type != NULL && init == NULL) {
    normVarTypeWoutInit(defExpr);

  } else if (type != NULL && init != NULL) {
    if (var->hasFlag(FLAG_PARAM) == true) {
      CallExpr* cast = createCast(init->remove(), type->remove());

      defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, cast));

    } else if (init->isNoInitExpr() == true) {
      normVarNoinit(defExpr);

    } else {
      normVarTypeWithInit(defExpr);
    }

  } else {
    INT_ASSERT(false);
  }
  restoreShadowVarForNormalize(defExpr, svarMark);
}

static void normRefVar(DefExpr* defExpr) {
  VarSymbol* var         = toVarSymbol(defExpr->sym);
  Expr*      init        = defExpr->init;
  Expr*      varLocation = NULL;

  if (init == NULL) {
    USR_FATAL_CONT(var,
                   "References must be initialized when they are defined.");
  }

  // If this is a const reference to an immediate, we need to insert a temp
  // variable so we can take the address of it, non-const references to an
  // immediate are not allowed.
  if (var->hasFlag(FLAG_CONST)) {
    if (SymExpr* initSym = toSymExpr(init)) {
      if (initSym->symbol()->isImmediate()) {
        VarSymbol* constRefTemp  = newTemp("const_ref_immediate_tmp");

        defExpr->insertBefore(new DefExpr(constRefTemp));
        defExpr->insertBefore(new CallExpr(PRIM_MOVE,
                                           constRefTemp,
                                           init->remove()));

        varLocation = new SymExpr(constRefTemp);
      }
    }
  }

  if (varLocation == NULL && init != NULL) {
    varLocation = init->remove();
  }

  if (SymExpr* sym = toSymExpr(varLocation)) {
    Symbol* symbol = sym->symbol();
    bool    error  = var->hasFlag(FLAG_CONST) == false &&
                     symbol->isConstant()     == true;

    // This is a workaround for the fact that isConstant for an
    // ArgSymbol with blank intent and type dtUnknown returns true,
    // but blank intent isn't necessarily const.
    if (ArgSymbol* arg = toArgSymbol(symbol)) {
      if (arg->intent == INTENT_BLANK && arg->type == dtUnknown) {
        error = false;
      }
    }

    if (error == true) {
      USR_FATAL_CONT(sym,
                     "Cannot set a non-const reference to a const variable.");
    }
  }

  defExpr->insertAfter(new CallExpr(PRIM_MOVE,
                                    var,
                                    new CallExpr(PRIM_ADDR_OF, varLocation)));
}

//
// const <name> = <value>;
// param <name> = <value>;
// var   <name> = <value>;
//
// The type of <name> will be inferred from the type of <value>
//
static void normVarTypeInference(DefExpr* defExpr) {
  Symbol* var      = defExpr->sym;
  Expr*   initExpr = defExpr->init->remove();

  // BHARSH INIT TODO: Many of these branches can and should be merged.
  //
  // Do not complain here.  Put this stub in to the AST and let
  // checkUseBeforeDefs() generate a consistent error message.
  if (isUnresolvedSymExpr(initExpr) == true) {
    defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr));

  // e.g.
  //   var x = <immediate>;
  //   var y = <identifier>;
  } else if (SymExpr* initSym = toSymExpr(initExpr)) {
    Type* type = initSym->symbol()->type;

    if (isPrimitiveScalar(type) == true) {
      defExpr->insertAfter(new CallExpr(PRIM_MOVE,     var, initExpr));

      var->type = type;
    } else {
      defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr));
    }

  // e.g.
  //   var x = f(...);
  //   var y = new MyRecord(...);
  } else if (CallExpr* initCall = toCallExpr(initExpr)) {
    if (initCall->isPrimitive(PRIM_NEW) == true) {
      AggregateType* type = typeForNewExpr(initCall);

      if (type != NULL) {
        if (type->isGeneric()                     == false ||
            isGenericRecordWithInitializers(type) == true) {
          var->type = type;
        }
      }

      if (isRecordWithInitializers(type) == true) {
        Expr*     arg1     = initCall->get(1)->remove();
        CallExpr* argExpr  = toCallExpr(arg1);
        SymExpr*  modToken = NULL;
        SymExpr*  modValue = NULL;

        if (argExpr->numActuals() >= 2) {
          if (SymExpr* se = toSymExpr(argExpr->get(1))) {
            if (se->symbol() == gModuleToken) {
              modToken = toSymExpr(argExpr->get(1)->remove());
              modValue = toSymExpr(argExpr->get(1)->remove());
            }
          }
        }

        // Insert the arg portion of the initExpr back into tree
        defExpr->insertAfter(argExpr);

        // Convert it in to a use of the init method
        argExpr->baseExpr->replace(new UnresolvedSymExpr("init"));

        // Add _mt and _this (insert at head in reverse order)
        if (isGenericRecord(type) == true) {
          // We need the actual for the "this" argument to be named in the
          // generic record case ...
          argExpr->insertAtHead(new NamedExpr("this", new SymExpr(var)));

          var->addFlag(FLAG_DELAY_GENERIC_EXPANSION);

        } else {
          // ... but not in the non-generic record case
          argExpr->insertAtHead(var);
        }

        argExpr->insertAtHead(gMethodToken);

        if (modToken != NULL) {
          argExpr->insertAtHead(modValue);
          argExpr->insertAtHead(modToken);
        }

        // Add a call to postinit() if present
        insertPostInit(var, argExpr);

      } else {
        defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, initExpr));
      }

    } else {
      defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr));
    }
  } else if (IfExpr* ife = toIfExpr(initExpr)) {
    defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, ife));

  } else {
    INT_ASSERT(false);
  }
}

//
// const <name> : <type>;
// param <name> : <type>;
// var   <name> : <type>;
//
// The type is explicit and the initial value is implied by the type
//

static void normVarTypeWoutInit(DefExpr* defExpr) {
  Symbol* var      = defExpr->sym;
  Expr*   typeExpr = defExpr->exprType->remove();
  Type*   type     = typeForTypeSpecifier(typeExpr, false);

  // Noakes 2016/02/02
  // The code for resolving the type of an extern variable
  //
  //   functionResolution.cpp : resolveExternVarSymbols()
  //
  // expects to find the init code inside a block stmt.
  if (var->hasFlag(FLAG_EXTERN) == true) {
    BlockStmt* block    = new BlockStmt(NULL, BLOCK_EXTERN_TYPE);

    VarSymbol* typeTemp = newTemp("type_tmp");
    DefExpr*   typeDefn = new DefExpr(typeTemp);
    CallExpr*  initCall = new CallExpr(PRIM_INIT, typeExpr);
    CallExpr*  initMove = new CallExpr(PRIM_MOVE, typeTemp, initCall);

    block->insertAtTail(typeDefn);
    block->insertAtTail(initMove);
    block->insertAtTail(new CallExpr(PRIM_MOVE, var, typeTemp));

    defExpr->insertAfter(block);

  } else if (isPrimitiveScalar(type) == true) {
    CallExpr* defVal = new CallExpr("_defaultOf", type->symbol);

    defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, defVal));

    var->type = type;

  } else if (isNonGenericClass(type) == true) {
    CallExpr* defVal = new CallExpr("_defaultOf", type->symbol);

    defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, defVal));

    var->type = type;

  } else if (isNonGenericRecordWithInitializers(type) == true &&
             needsGenericRecordInitializer(type)      == false) {
    CallExpr* init = new CallExpr("init", gMethodToken, var);

    var->type = type;

    defExpr->insertAfter(init);

    // Add a call to postinit() if present
    insertPostInit(var, init);

  } else {
    VarSymbol* typeTemp = newTemp("type_tmp");
    DefExpr*   typeDefn = new DefExpr(typeTemp);
    CallExpr*  initCall = new CallExpr(PRIM_INIT, typeExpr);
    CallExpr*  initMove = new CallExpr(PRIM_MOVE, typeTemp, initCall);

    if (var->hasFlag(FLAG_PARAM) == true) {
      typeTemp->addFlag(FLAG_PARAM);
    }

    defExpr ->insertAfter(typeDefn);
    typeDefn->insertAfter(initMove);
    initMove->insertAfter(new CallExpr(PRIM_MOVE, var, typeTemp));
  }
}

static void normVarTypeWithInit(DefExpr* defExpr) {
  Symbol* var      = defExpr->sym;
  Expr*   typeExpr = defExpr->exprType->remove();
  Expr*   initExpr = defExpr->init->remove();
  Type*   type     = typeForTypeSpecifier(typeExpr, false);

  // Note: the above line will not obtain a type if the typeExpr is a CallExpr
  // for a generic record or class, as that is a more complicated set of AST.

  //
  // e.g. const x : int     = 10;
  //      var   y : int(32) = 20;
  //
  //      var   x : MyCls   = new MyCls(1, 2);
  //
  if (isPrimitiveScalar(type) == true ||
      isNonGenericClass(type) == true) {
    defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr, type->symbol));
    var->type = type;

  } else if (isNonGenericRecordWithInitializers(type) == true) {
    var->type = type;

    if        (isSymExpr(initExpr) == true) {
      CallExpr* initCall = new CallExpr("init", gMethodToken, var, initExpr);

      defExpr->insertAfter(initCall);

      // Add a call to postinit() if present
      insertPostInit(var, initCall);

    } else if (isNewExpr(initExpr) == false) {
      defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr, type->symbol));

    } else {
      Expr*     arg     = toCallExpr(initExpr)->get(1)->remove();
      CallExpr* argExpr = toCallExpr(arg);

      // This call must be in tree before extending argExpr
      defExpr->insertAfter(argExpr);

      // Convert it to a use of the init method
      argExpr->baseExpr->replace(new UnresolvedSymExpr("init"));

      // Add _mt and _this (insert at head in reverse order)
      argExpr->insertAtHead(var);
      argExpr->insertAtHead(gMethodToken);

      // Add a call to postinit() if present
      insertPostInit(var, argExpr);
    }

  } else if (isNewExpr(initExpr) == true) {
    // This check is necessary because the "typeForTypeSpecifier"
    // call will not obtain a type if the typeExpr is a CallExpr,
    // as it is for generic records and classes

    CallExpr*      origCall = toCallExpr(initExpr);
    AggregateType* rhsType  = typeForNewExpr(origCall);

    if (isGenericRecordWithInitializers(rhsType)) {
      // Create a temporary to hold the result of the rhs "new" call
      VarSymbol* initExprTemp = newTemp("init_tmp", rhsType);
      DefExpr*   initExprDefn = new DefExpr(initExprTemp);
      Expr*      arg          = origCall->get(1)->remove();
      CallExpr*  argExpr      = toCallExpr(arg);

      defExpr->insertAfter(initExprDefn);
      initExprDefn->insertAfter(argExpr);

      // Modify the "new" call so that it is in the appropriate form for
      // types with initializers
      argExpr->baseExpr->replace(new UnresolvedSymExpr("init"));
      argExpr->insertAtHead(new NamedExpr("this", new SymExpr(initExprTemp)));
      argExpr->insertAtHead(gMethodToken);

      // Add a call to postinit() if present
      insertPostInit(initExprTemp, argExpr);

      initExprTemp->addFlag(FLAG_DELAY_GENERIC_EXPANSION);

      argExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExprTemp, typeExpr));

    } else {
      defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr, typeExpr));
    }

  } else {
    defExpr->insertAfter(new CallExpr(PRIM_INIT_VAR, var, initExpr, typeExpr));
  }
}

static bool isNewExpr(Expr* expr) {
  bool retval = false;

  if (CallExpr* callExpr = toCallExpr(expr)) {
    retval = callExpr->isPrimitive(PRIM_NEW);
  }

  return retval;
}

static AggregateType* typeForNewExpr(CallExpr* newExpr) {
  AggregateType* retval = NULL;

  if (CallExpr* constructor = toCallExpr(newExpr->get(1))) {
    if (SymExpr* baseExpr = toSymExpr(constructor->baseExpr)) {
      if (TypeSymbol* sym = toTypeSymbol(baseExpr->symbol())) {
        if (AggregateType* type = toAggregateType(sym->type)) {
          if (isClass(type) == true || isRecord(type) == true) {
            retval = type;
          }
        }
      }
    }
  }

  return retval;
}

// Internal and Standard modules always honor no-init
//
// As a minimum, the complex type appears to rely on this
static bool moduleHonorsNoinit(Symbol* var, Expr* init) {
  bool isNoinit = init->isNoInitExpr();
  bool retval   = false;

  if (isNoinit == true && fUseNoinit == false) {
    Symbol* moduleSource = var;

    while (isModuleSymbol(moduleSource)  == false &&
           moduleSource                  != NULL &&
           moduleSource->defPoint        != NULL) {
      moduleSource = moduleSource->defPoint->parentSymbol;
    }

    if (ModuleSymbol* mod = toModuleSymbol(moduleSource)) {
      if (moduleSource->defPoint != NULL) {
        retval = mod->modTag == MOD_INTERNAL || mod->modTag == MOD_STANDARD;
      }
    }
  }

  return retval;
}

static void normVarNoinit(DefExpr* defExpr) {
  Symbol* var  = defExpr->sym;
  Expr*   init = defExpr->init;

  init->remove();

  if (fUseNoinit == true || moduleHonorsNoinit(var, init) == true) {
    Expr*     type   = defExpr->exprType;
    CallExpr* noinit = new CallExpr(PRIM_NO_INIT, type->remove());

    defExpr->insertAfter(new CallExpr(PRIM_MOVE, var, noinit));
  } else {
    // Ignore no-init expression and fall back on default init
    normVarTypeWoutInit(defExpr);
  }
}

//
// We want the initialization code for a task-private variable to be
// in that variable's initBlock(). This is not where its DefExpr is,
// however. So, in order to use existing normalization code as-is, we
// move the DefExpr into a temporary BlockStmt. This BlockStmt also
// preserves the DefExpr's position in the ForallStmt::shadowVariables()
// list, which is where it lives. Once normalization completes, we
// move the resulting ASTs into the initBlock(), and the DefExpr back
// to its home list.

static Expr* prepareShadowVarForNormalize(DefExpr* def, VarSymbol* var) {
  ShadowVarSymbol* svar = toShadowVarSymbol(var);
  if (svar == NULL || !svar->isTaskPrivate())
    // Not a task-private variable, nothing to do.
    return NULL;

  BlockStmt* normBlock = new BlockStmt();
  def->replace(normBlock);
  normBlock->insertAtTail(def);

  return normBlock;
}

static void  restoreShadowVarForNormalize(DefExpr* def, Expr* svarMark) {
  if (!svarMark)
    return;

  BlockStmt* normBlock = toBlockStmt(svarMark);
  BlockStmt* initBlock = toShadowVarSymbol(def->sym)->initBlock();
  AList&     initList  = initBlock->body;
  // If there is stuff in initBlock(), we need to be careful about
  // where we place 'def'.
  INT_ASSERT(initList.empty());

  normBlock->insertAfter(def->remove());

  for_alist(stmt, normBlock->body)
    initList.insertAtTail(stmt->remove());

  normBlock->remove();
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void insertPostInit(Symbol* var, CallExpr* anchor) {
  AggregateType* at = toAggregateType(var->type);

  if (at->hasPostInitializer() == true) {
    anchor->insertAfter(new CallExpr("postinit", gMethodToken, var));
  }
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void updateVariableAutoDestroy(DefExpr* defExpr) {
  VarSymbol* var = toVarSymbol(defExpr->sym);
  FnSymbol*  fn  = toFnSymbol(defExpr->parentSymbol);

  if (var->hasFlag(FLAG_NO_AUTO_DESTROY) == false &&
      var->hasFlag(FLAG_PARAM)           == false && // Note 1.
      var->hasFlag(FLAG_REF_VAR)         == false &&

      fn->_this                          != var   && // Note 2.
      fn->hasFlag(FLAG_INIT_COPY_FN)     == false && // Note 3.
      fn->hasFlag(FLAG_TYPE_CONSTRUCTOR) == false) {

    // Variables in a module initializer need special attention
    if (defExpr->parentExpr == fn->getModule()->initFn->body) {

      // Noakes 2016/04/27
      //
      // Most variables in a module init function will become global and
      // should not be auto destroyed.  The challenging case is
      //
      // var (a1, a2) = fnReturnTuple();
      //
      // The parser expands this as
      //
      // var tmp = fnReturnTuple();
      // var a1  = tmp.x1;
      // var a2  = tmp.x2;
      //
      // This pseudo-tuple must be auto-destroyed to ensure the components
      // are managed correctly. However the AST doesn't provide us with a
      // strong/easy way to determine that we're dealing with this case.
      // In practice it appears to be sufficient to flag any TMP
      if (var->hasFlag(FLAG_TEMP)) {
        var->addFlag(FLAG_INSERT_AUTO_DESTROY);
      }

    } else {
      var->addFlag(FLAG_INSERT_AUTO_DESTROY);
    }
  }
}

// Note 1: Since param variables can only be of primitive or enumerated type,
// their destructors are trivial.  Allowing this case to proceed could result
// in a regularization (reduction in # of conditionals == reduction in code
// complexity).

// Note 2: "this" should be passed by reference.  Then, no constructor call
// is made, and therefore no autodestroy call is needed.

// Note 3: If a record arg to an init copy function is passed by value,
// infinite recursion would ensue.  This is an unreachable case (assuming that
// magic conversions from R -> ref R are removed and all existing
// implementations of chpl__initCopy are rewritten using "ref" or "const ref"
// intent on the record argument).


// Note 4: These two cases should be regularized.  Either the copy constructor
// should *always* be called (and the corresponding destructor always called),
// or we should ensure that the destructor is called only if a constructor is
// called on the same variable.  The latter case is an optimization, so the
// simplest implementation calls the copy-constructor in both cases.

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void hack_resolve_types(ArgSymbol* arg) {
  // Look only at unknown or arbitrary types.
  if (arg->type == dtUnknown || arg->type == dtAny) {
    if (!arg->typeExpr) {
      if (!arg->hasFlag(FLAG_TYPE_VARIABLE) && arg->defaultExpr) {
        SymExpr* se = NULL;
        if (arg->defaultExpr->body.length == 1)
          se = toSymExpr(arg->defaultExpr->body.tail);
        if (!se || se->symbol() != gTypeDefaultToken) {
          SET_LINENO(arg->defaultExpr);
          arg->typeExpr = arg->defaultExpr->copy();
          insert_help(arg->typeExpr, NULL, arg);
        }
      }
    } else {
      INT_ASSERT(arg->typeExpr);

      // If there is a simple type expression, and its type is something more specific than
      // dtUnknown or dtAny, then replace the type expression with that type.
      // hilde sez: don't we lose information here?
      if (arg->typeExpr->body.length == 1) {
        Expr* only = arg->typeExpr->body.only();
        Type* type = only->typeInfo();

        // Works around an issue with generic types:
        //
        // The function 'normalizeCallToTypeConstructor' may create a SymExpr
        // to a type constructor, in which case the return type will likely
        // not be 'dtUnknown' or 'dtAny' and may be generic. If the return
        // type is generic we do not want to remove the type constructor call
        // because resolution will not be able to handle the resulting AST.
        if (CallExpr* call = toCallExpr(only)) {
          if (SymExpr* se = toSymExpr(call->baseExpr)) {
            if (FnSymbol* fn = toFnSymbol(se->symbol())) {
              if (fn->hasFlag(FLAG_TYPE_CONSTRUCTOR)) {
                // Set dtUnknown, causing the upcoming conditional to fail
                type = dtUnknown;
              }
            }
          }
        }

        if (type != dtUnknown && type != dtAny) {
          // This test ensures that we are making progress.
          arg->type = type;
          arg->typeExpr->remove();
        }
      }
    }
  }
}

/************************************* | **************************************
*                                                                             *
* We cannot export a function with an array argument directly, due to the     *
* type being considered generic and due to other languages not understanding  *
* our array structure, as well as our normal array structure assuming it has  *
* control over the memory involved.  Instead, build a wrapper for the         *
* function.  This wrapper will be modified to take in an instance of our      *
* runtime-defined array wrapper type instead of the original array type, in   *
* addition to its other arguments.                                            *
*                                                                             *
************************************** | *************************************/
static bool hasNonVoidReturnStmt(FnSymbol* fn);

static void makeExportWrapper(FnSymbol* fn) {
  bool argsToReplace = false;
  for_formals(formal, fn) {
    if (isArrayFormal(formal)) {
      argsToReplace = true;
    }
  }

  // TODO: Also check if return type is an array.  Don't make two wrappers,
  // pls.
  if (argsToReplace) {
    // We have at least one array argument.  Need to make a version of this
    // function that can be exported
    FnSymbol* newFn = fn->copy();
    newFn->addFlag(FLAG_COMPILER_GENERATED);

    fn->defPoint->insertBefore(new DefExpr(newFn));

    if ((fn->retExprType != NULL &&
         fn->retExprType->body.tail->typeInfo() != dtVoid) ||
        hasNonVoidReturnStmt(fn)) {
      newFn->body->replace(new BlockStmt(new CallExpr(PRIM_RETURN,
                                                      new CallExpr(fn->name))));
    } else {
      newFn->body->replace(new BlockStmt(new CallExpr(fn->name)));
    }
    fn->removeFlag(FLAG_EXPORT);
  }
}

// If we run into issues due to this computation, just require explicitly
// declared return types for exported functions
static bool hasNonVoidReturnStmt(FnSymbol* fn) {
  std::vector<CallExpr*> calls;

  collectMyCallExprs(fn, calls, fn);
  for_vector(CallExpr, call, calls) {
    if (call->isPrimitive(PRIM_RETURN) == true) {
      if (isVoidReturn(call) == false) {
        return true;
      }
    }
  }
  return false;
}
/************************************* | **************************************
*                                                                             *
* The parser represents formals with an array type specifier as a formal with *
* a typeExpr that use chpl__buildArrayRuntimeType e.g.                        *
*                                                                             *
*   : []            -> buildArrayRuntimeType(symExpr(nil))                    *
*   : [D]           -> buildArrayRuntimeType(symExpr('D'))                    *
*   : [1..3]        -> buildArrayRuntimeType(buildRange(1, 3));               *
*   : [?D]          -> buildArrayRuntimeType(defExpr('D'))                    *
*                                                                             *
*   : []     string -> buildArrayRuntimeType(symExpr(nil), symExpr(string))   *
*   : [D]    int    -> buildArrayRuntimeType(symExpr('D'), symExpr(int)       *
*   : [D]    ?t     -> buildArrayRuntimeType(symExpr('D'), defExpr('t'))      *
*                                                                             *
* Replace these with uses of the generic _array type and make other changes   *
* as necessary.                                                               *
*                                                                             *
************************************** | *************************************/

static void fixupExportedArrayFormals(FnSymbol* fn);

static void fixupArrayFormal(FnSymbol* fn, ArgSymbol* formal);

static void fixupArrayDomainExpr(FnSymbol*                    fn,
                                 ArgSymbol*                   formal,
                                 Expr*                        domExpr,
                                 const std::vector<SymExpr*>& symExprs);

static void fixupArrayElementExpr(FnSymbol*                    fn,
                                  ArgSymbol*                   formal,
                                  Expr*                        eltExpr,
                                  const std::vector<SymExpr*>& symExprs);

static void fixupArrayFormals(FnSymbol* fn) {
  if (fn->hasFlag(FLAG_EXPORT) && fn->hasFlag(FLAG_COMPILER_GENERATED) &&
      fn->hasFlag(FLAG_MODULE_INIT) == false) {
    fixupExportedArrayFormals(fn);

  } else {
    for_formals(formal, fn) {
      if (isArrayFormal(formal)) {
        fixupArrayFormal(fn, formal);
      }
    }
  }
}

static bool skipFixup(ArgSymbol* formal, Expr* domExpr, Expr* eltExpr) {
  if (formal->intent & INTENT_FLAG_IN) {
    if (isDefExpr(domExpr) || isDefExpr(eltExpr)) {
      return false;
    } else if (SymExpr* se = toSymExpr(domExpr)) {
      if (se->symbol() == gNil) {
        return false;
      }
    } else {
      return true;
    }
  }

  return false;
}

static bool isArrayFormal(ArgSymbol* arg) {
  if (BlockStmt* typeExpr = arg->typeExpr) {
    //
    // The body is usually a single callExpr.  However there are rare
    // cases in which normalization generates one or more call_temps
    // i.e. a sequence of defExpr/primMove pairs.
    //
    // In either case the desired callExpr is the tail of the body.
    //

    if (CallExpr* call = toCallExpr(typeExpr->body.tail)) {
      if (call->isNamed("chpl__buildArrayRuntimeType")) {
        return true;
      }
    }
  }
  return false;
}

static void fixupExportedArrayFormals(FnSymbol* fn) {
  CallExpr* retCall = toCallExpr(fn->body->body.tail);
  INT_ASSERT(retCall);
  CallExpr* callOrigFn = retCall;
  if (retCall->isPrimitive(PRIM_RETURN)) {
    INT_ASSERT(retCall->numActuals() == 1);
    callOrigFn = toCallExpr(retCall->get(1));
  }

  INT_ASSERT(callOrigFn);

  for_formals(formal, fn) {
    if (isArrayFormal(formal)) {
      BlockStmt*            typeExpr = formal->typeExpr;

      // call is chpl__buildArrayRuntimeType, which takes in a domain and
      // optionally a type expression.  The domain information will get
      // validated during resolution, but we need the type expression to
      // correctly create our Chapel array wrapper.
      CallExpr*             call     = toCallExpr(typeExpr->body.tail);
      int                   nArgs    = call->numActuals();
      Expr*                 eltExpr  = nArgs == 2 ? call->get(2) : NULL;

      if (eltExpr == NULL) {
        USR_FATAL(formal, "array argument '%s' in exported function '%s'"
                  " must specify its type", formal->name, fn->name);
        continue;
      }

      // Create a representation of the array argument that is accessible
      // outside of Chapel (in the form of a pointer and a corresponding size)
      formal->typeExpr->replace(new BlockStmt(new SymExpr(dtExternalArray->symbol)));

      // Transform the outside representation into a Chapel array, and send that
      // in the call to the original function.
      CallExpr* makeChplArray = new CallExpr("makeArrayFromExternArray",
                                             new SymExpr(formal),
                                             eltExpr->copy());
      VarSymbol* chplArr = new VarSymbol(astr(formal->name, "_arr"));
      retCall->insertBefore(new DefExpr(chplArr));
      retCall->insertBefore(new CallExpr(PRIM_MOVE, chplArr, makeChplArray));

      callOrigFn->insertAtTail(new SymExpr(chplArr));

    } else {
      callOrigFn->insertAtTail(new SymExpr(formal));
    }
  }
}
// Preliminary validation is performed within the caller
static void fixupArrayFormal(FnSymbol* fn, ArgSymbol* formal) {
  BlockStmt*            typeExpr = formal->typeExpr;

  CallExpr*             call     = toCallExpr(typeExpr->body.tail);
  int                   nArgs    = call->numActuals();
  Expr*                 domExpr  = call->get(1);
  Expr*                 eltExpr  = nArgs == 2 ? call->get(2) : NULL;

  std::vector<SymExpr*> symExprs;

  //
  // Only fix array formals with 'in' intent if there was:
  // - a type query, or
  // - a domain query, or
  // - no domain expression
  //
  // This 'fixing' makes it difficult to find runtime type information later
  // when we need it.
  //
  if (skipFixup(formal, domExpr, eltExpr)) return;

  // Replace the type expression with "_array" to make it generic.
  typeExpr->replace(new BlockStmt(new SymExpr(dtArray->symbol), BLOCK_TYPE));

  if (isDefExpr(domExpr) == true || isDefExpr(eltExpr) == true) {
    collectSymExprs(fn, symExprs);
  }

  fixupArrayDomainExpr(fn, formal, domExpr, symExprs);

  if (eltExpr != NULL) {
    fixupArrayElementExpr(fn, formal, eltExpr, symExprs);
  }
}

static void fixupArrayDomainExpr(FnSymbol*                    fn,
                                 ArgSymbol*                   formal,
                                 Expr*                        domExpr,
                                 const std::vector<SymExpr*>& symExprs) {
  // : [?D]   -> defExpr('D')
  if (DefExpr* queryDomain = toDefExpr(domExpr)) {
    // Walk the body of 'fn' and replace uses of 'D' with 'D'._dom
    for_vector(SymExpr, se, symExprs) {
      if (se->symbol() == queryDomain->sym) {
        SET_LINENO(se);

        se->replace(new CallExpr(".", formal, new_CStringSymbol("_dom")));
      }
    }

  // : []     -> symExpr('nil')
  // : [D]    -> symExpr('D')
  // : [1..3] -> callExpr('buildRange', 1, 3)
  } else {
    bool insertCheck = true;

    if (SymExpr* dom = toSymExpr(domExpr)) {
      if (dom->symbol() == gNil) {
        insertCheck = false;
      }
    }

    if (insertCheck == true) {
      Symbol* checkDoms = new_CStringSymbol("chpl_checkArrArgDoms");

      fn->insertAtHead(new CallExpr(new CallExpr(".", formal, checkDoms),
                                    domExpr->copy(),
                                    fNoFormalDomainChecks ? gFalse : gTrue));
    }
  }
}

static void fixupArrayElementExpr(FnSymbol*                    fn,
                                  ArgSymbol*                   formal,
                                  Expr*                        eltExpr,
                                  const std::vector<SymExpr*>& symExprs) {
  // e.g. : [1..3] ?t
  if (DefExpr* queryEltType = toDefExpr(eltExpr)) {
    // Walk the body of 'fn' and replace uses of 't' with 't'.eltType
    for_vector(SymExpr, se, symExprs) {
      if (se->symbol() == queryEltType->sym) {
        SET_LINENO(se);

        se->replace(new CallExpr(".", formal, new_CStringSymbol("eltType")));
      }
    }

  } else if (eltExpr != NULL) {
    if (fn->where == NULL) {
      fn->where = new BlockStmt(new SymExpr(gTrue));

      insert_help(fn->where, NULL, fn);

      fn->addFlag(FLAG_COMPILER_ADDED_WHERE);
    }

    formal->addFlag(FLAG_NOT_FULLY_GENERIC);

    Expr*     oldWhere   = fn->where->body.tail;
    CallExpr* newWhere   = new CallExpr("&");
    Symbol*   eltType    = new_CStringSymbol("eltType");
    CallExpr* getEltType = new CallExpr(".", formal, eltType);

    oldWhere->replace(newWhere);

    newWhere->insertAtTail(oldWhere);
    newWhere->insertAtTail(new CallExpr("==", eltExpr->remove(), getEltType));
  }
}

/************************************* | **************************************
*                                                                             *
* Consider a function of the form                                             *
*                                                                             *
*   proc +(a: int(?w), b: int(w)) return __primitive("+", a, b);              *
*                                                                             *
* This function is *replaced* with 4 instantiations :-                        *
*                                                                             *
*   proc +(a: int( 8), b: int( 8)) return __primitive("+", a, b);             *
*   proc +(a: int(16), b: int(16)) return __primitive("+", a, b);             *
*   proc +(a: int(32), b: int(32)) return __primitive("+", a, b);             *
*   proc +(a: int(64), b: int(64)) return __primitive("+", a, b);             *
*                                                                             *
* If a function has multiple formals with parameterized primitives this       *
* process will be repeated iteratively with one set of expansions per formal. *
* This iteration relies on the fact that the first set of expansions are      *
* appended to gFnSymbols while it is being processed.                         *
*                                                                             *
************************************** | *************************************/

static bool isParameterizedPrimitive(CallExpr* typeSpecifier);

static void cloneParameterizedPrimitive(FnSymbol* fn, ArgSymbol* formal);

static void cloneParameterizedPrimitive(FnSymbol* fn,
                                        DefExpr*  def,
                                        int       width);

static bool includesParameterizedPrimitive(FnSymbol* fn) {
  bool retval = false;

  for_formals(formal, fn) {
    if (BlockStmt* typeExpr = formal->typeExpr) {
      if (CallExpr* typeSpecifier = toCallExpr(typeExpr->body.tail)) {
        if (isParameterizedPrimitive(typeSpecifier) == true) {
          retval = true;
          break;
        }
      }
    }
  }

  return retval;
}

static void replaceFunctionWithInstantiationsOfPrimitive(FnSymbol* fn) {
  for_formals(formal, fn) {
    if (BlockStmt* typeExpr = formal->typeExpr) {
      if (CallExpr* typeSpecifier = toCallExpr(typeExpr->body.tail)) {
        if (isParameterizedPrimitive(typeSpecifier) == true) {
          cloneParameterizedPrimitive(fn, formal);

          break;
        }
      }
    }
  }
}

// e.g. x : int(?w)
static bool isParameterizedPrimitive(CallExpr* typeSpecifier) {
  bool retval = false;

  if (SymExpr* callFnSymExpr = toSymExpr(typeSpecifier->baseExpr)) {
    if (typeSpecifier->numActuals()      ==    1 &&
        isDefExpr(typeSpecifier->get(1)) == true) {
      Symbol* callFnSym = callFnSymExpr->symbol();

      if (callFnSym == dtBools[BOOL_SIZE_DEFAULT]->symbol ||
          callFnSym == dtInt[INT_SIZE_DEFAULT]->symbol    ||
          callFnSym == dtUInt[INT_SIZE_DEFAULT]->symbol   ||
          callFnSym == dtReal[FLOAT_SIZE_DEFAULT]->symbol ||
          callFnSym == dtImag[FLOAT_SIZE_DEFAULT]->symbol ||
          callFnSym == dtComplex[COMPLEX_SIZE_DEFAULT]->symbol) {
        retval = true;
      }
    }
  }

  return retval;
}

// 'formal' is certain to be a parameterized primitive e.g int(?w)
static void cloneParameterizedPrimitive(FnSymbol* fn, ArgSymbol* formal) {
  BlockStmt* typeExpr      = formal->typeExpr;
  CallExpr*  typeSpecifier = toCallExpr(typeExpr->body.tail);
  Symbol*    callFnSym     = toSymExpr(typeSpecifier->baseExpr)->symbol();
  DefExpr*   def           = toDefExpr(typeSpecifier->get(1));

  if (callFnSym == dtBools[BOOL_SIZE_DEFAULT]->symbol) {
    for (int i = BOOL_SIZE_8; i < BOOL_SIZE_NUM; i++) {
      cloneParameterizedPrimitive(fn, def, get_width(dtBools[i]));
    }

  } else if (callFnSym == dtInt [INT_SIZE_DEFAULT]->symbol ||
             callFnSym == dtUInt[INT_SIZE_DEFAULT]->symbol) {
    for (int i = INT_SIZE_8; i < INT_SIZE_NUM; i++) {
      cloneParameterizedPrimitive(fn, def, get_width(dtInt[i]));
    }

  } else if (callFnSym == dtReal[FLOAT_SIZE_DEFAULT]->symbol ||
             callFnSym == dtImag[FLOAT_SIZE_DEFAULT]->symbol) {
    for (int i = FLOAT_SIZE_32; i < FLOAT_SIZE_NUM; i++) {
      cloneParameterizedPrimitive(fn, def, get_width(dtReal[i]));
    }

  } else if (callFnSym == dtComplex[COMPLEX_SIZE_DEFAULT]->symbol) {
    for (int i = COMPLEX_SIZE_64; i < COMPLEX_SIZE_NUM; i++) {
      cloneParameterizedPrimitive(fn, def, get_width(dtComplex[i]));
    }
  }

  fn->defPoint->remove();
}

static void cloneParameterizedPrimitive(FnSymbol* fn,
                                        DefExpr*  def,
                                        int       width) {
  SymbolMap             map;
  FnSymbol*             newFn  = fn->copy(&map);
  Symbol*               newSym = map.get(def->sym);
  std::vector<SymExpr*> symExprs;

  newSym->defPoint->replace(new SymExpr(new_IntSymbol(width)));

  collectSymExprs(newFn, symExprs);

  for_vector(SymExpr, se, symExprs) {
    if (se->symbol() == newSym) {
      se->setSymbol(new_IntSymbol(width));
    }
  }

  fn->defPoint->insertAfter(new DefExpr(newFn));
}

/************************************* | **************************************
*                                                                             *
* Query formals appear in two/four forms                                      *
*                                                                             *
*   1)  proc chpl__autoDestroy(x: ?t) ...                                     *
*       proc foo(x: ?t, y : t) ...                                            *
*                                                                             *
*       The identifier 't' may appear in the formals list, in a where clause, *
*       or in the body of the function.                                       *
*                                                                             *
*       The parser represents the definition of the query variable as a       *
*       DefExpr in the BlockStmt that is created for any formal type-expr.    *
*                                                                             *
*       This case is handled by replacing every use of 't' with x.type.       *
*                                                                             *
*                                                                             *
*                                                                             *
*                                                                             *
*   2)  proc =(ref a: _ddata(?t), b: _ddata(t)) ...                           *
*                                                                             *
*       The important difference is that 't' is a type field for a generic    *
*       type; in this example the generic type _ddata.                        *
*                                                                             *
*       The BlockStmt for the type-expr of the formal will include a          *
*       CallExpr with an actual that is a DefExpr.                            *
*                                                                             *
*                                                                             *
*   2a) proc _getView(r:range(?))                                             *
*                                                                             *
*       This generic function handles any actual that is a specialization of  *
*       the generic type range.  The compiler uses the same representation    *
*       as for (2) and supplies a unique name for the query variable.         *
*                                                                             *
*       The current implementation does not attempt to distinguish this case  *
*       from (2).                                                             *
*                                                                             *
*                                                                             *
*   2b) proc +(a: int(?w), b: int(w)) return __primitive("+", a, b);          *
*                                                                             *
*       This appears to be another example of (2) but it is handled           *
*       completely differently.  An earlier sub-phase notices these queries   *
*       for a generic primitive type, instantiates new versions for all       *
*       allowable values of 'w', and then deletes the original AST.           *
*       Hence these functions are never observed by the following code.       *
*                                                                             *
*   NB: This code does not handle the count for variadic functions e.g.       *
*                                                                             *
*       proc min(x, y, z...?k) ...                                            *
*                                                                             *
*       The identifier 'k' is not part of the type of 'z'.  The current AST   *
*       stores the DefExpr for 'k' in the variableExpr of 'z'.                *
*                                                                             *
************************************** | *************************************/

static void replaceUsesWithPrimTypeof(FnSymbol* fn, ArgSymbol* formal);

static bool isQueryForGenericTypeSpecifier(ArgSymbol* formal);

static void expandQueryForGenericTypeSpecifier(FnSymbol*  fn,
                                               ArgSymbol* formal);

static void replaceQueryUses(ArgSymbol*             formal,
                             DefExpr*               def,
                             CallExpr*              query,
                             std::vector<SymExpr*>& symExprs);

static void addToWhereClause(FnSymbol*  fn,
                             ArgSymbol* formal,
                             Expr*      test);

static void fixupQueryFormals(FnSymbol* fn) {
  for_formals(formal, fn) {
    if (BlockStmt* typeExpr = formal->typeExpr) {
      Expr* tail = typeExpr->body.tail;

      if  (isDefExpr(tail) == true) {
        replaceUsesWithPrimTypeof(fn, formal);

      } else if (isQueryForGenericTypeSpecifier(formal) == true) {
        expandQueryForGenericTypeSpecifier(fn, formal);
      }
    }
  }
}

// The type-expr is known to be a simple DefExpr
static void replaceUsesWithPrimTypeof(FnSymbol* fn, ArgSymbol* formal) {
  BlockStmt*            typeExpr = formal->typeExpr;
  DefExpr*              def      = toDefExpr(typeExpr->body.tail);
  std::vector<SymExpr*> symExprs;

  collectSymExprs(fn, symExprs);

  if (formal->variableExpr) // varargs argument e.g. proc f(x...)
    addToWhereClause(fn, formal, new CallExpr(PRIM_IS_STAR_TUPLE_TYPE, formal));

  for_vector(SymExpr, se, symExprs) {
    if (se->symbol() == def->sym) {
      if (formal->variableExpr)
        // e.g. proc foo(arg:?t ...)
        // formal is a tuple but the query should be of the tuple elements
        // 1 is the size field, 2 is the index of the first tuple field
        se->replace(new CallExpr(PRIM_QUERY, formal, new_IntSymbol(2)));
      else
        se->replace(new CallExpr(PRIM_TYPEOF, formal));
    }
  }

  formal->typeExpr->remove();

  formal->type = dtAny;
}

static bool doesCallContainDefActual(CallExpr* call) {
  for_actuals(actual, call) {
    if (isDefExpr(actual)) {
      return true;

    } else if (NamedExpr* named = toNamedExpr(actual)) {
      if (isDefExpr(named->actual)) {
        return true;
      }

    } else if (CallExpr* subcall = toCallExpr(actual)) {
      if (doesCallContainDefActual(subcall)) {
        return true;
      }
    }
  }

  return false;
}

static bool isQueryForGenericTypeSpecifier(ArgSymbol* formal) {
  bool retval = false;

  if (CallExpr* call = toCallExpr(formal->typeExpr->body.tail)) {
    retval = doesCallContainDefActual(call);
  }

  return retval;
}

static void expandQueryForGenericTypeSpecifier(FnSymbol*  fn,
                                               std::vector<SymExpr*>& symExprs,
                                               ArgSymbol* formal,
                                               CallExpr* call,
                                               BaseAST* queried);

static TypeSymbol* getTypeForSpecialConstructor(CallExpr* call);

// The type-expr is known to be a CallExpr with a query definition
static void expandQueryForGenericTypeSpecifier(FnSymbol*  fn,
                                               ArgSymbol* formal) {
  BlockStmt*            typeExpr  = formal->typeExpr;
  Expr*                 tail      = typeExpr->body.tail;
  CallExpr*             call      = toCallExpr(tail);

  std::vector<SymExpr*> symExprs;

  collectSymExprs(fn, symExprs);

  BaseAST* queried = formal;

  // Queries access the 1st tuple element for varargs functions
  if (formal->variableExpr) {
    // 1 is the size field, 2 is the index of the first tuple field
    queried = new CallExpr(PRIM_QUERY, formal, new_IntSymbol(2));
    // Add check that passed tuple is homogeneous
    addToWhereClause(fn, formal, new CallExpr(PRIM_IS_STAR_TUPLE_TYPE, formal));
  }

  expandQueryForGenericTypeSpecifier(fn, symExprs, formal, call, queried);

  // Remove the queries from the formal argument type
  Expr* usetype = NULL;
  if (TypeSymbol* ts = getTypeForSpecialConstructor(call)) {
    usetype = new SymExpr(ts);
  } else if (call->baseExpr) {
    usetype = call->baseExpr->remove();
  } else {
    usetype = call->remove();
    INT_ASSERT(!doesCallContainDefActual(call));
  }

  formal->typeExpr->replace(new BlockStmt(usetype));

  formal->addFlag(FLAG_MARKED_GENERIC);
}

static TypeSymbol* getTypeForSpecialConstructor(CallExpr* call) {
  if (call->isNamed("_build_tuple")) {
    return dtTuple->symbol;
  } else if (call->isNamed("_to_unmanaged")) {
    return dtUnmanaged->symbol;
  } else if (call->isPrimitive(PRIM_TO_UNMANAGED_CLASS)) {
    return dtUnmanaged->symbol;
  }
  return NULL;
}


// Constructs a PRIM_QUERY with arguments a, b
// Copies an expr argument but not a symbol argument
static CallExpr* makePrimQuery(BaseAST* a, BaseAST* b=NULL) {
  Expr* aExpr = NULL;
  Expr* bExpr = toExpr(b);
  if (Symbol* sym = toSymbol(a))
    aExpr = new SymExpr(sym);
  else if (Expr* expr = toExpr(a))
    aExpr = expr->copy();

  INT_ASSERT(aExpr);

  if (b) {
    if (Symbol* symB = toSymbol(b))
      bExpr = new SymExpr(symB);
    else if (Expr* expr = toExpr(b))
      bExpr = expr->copy();

    INT_ASSERT(bExpr);
  }

  if (bExpr == NULL)
    return new CallExpr(PRIM_QUERY, aExpr);
  else
    return new CallExpr(PRIM_QUERY, aExpr, bExpr);
}

// query - the just-created PRIM_QUERY to add to the ast somewhere
// actual - the actual argument to the nested type constructor
static void expandQueryForActual(FnSymbol*  fn,
                                 std::vector<SymExpr*>& symExprs,
                                 ArgSymbol* formal,
                                 CallExpr* call,
                                 CallExpr* query,
                                 Expr* actual) {
  DefExpr* def = toDefExpr(actual);
  CallExpr* subcall = toCallExpr(actual);

  if (def) {
    replaceQueryUses(formal, def, query, symExprs);
  } else if (subcall && doesCallContainDefActual(subcall)) {
    Expr* subtype = NULL;
    if (TypeSymbol* ts = getTypeForSpecialConstructor(subcall)) {
      subtype = new SymExpr(ts);
    } else if (subcall->baseExpr) {
      subtype = subcall->baseExpr->copy();
    } else {
      subtype = subcall->copy();
      INT_ASSERT(!doesCallContainDefActual(subcall));
    }
    // Add check that actual type satisfies
    addToWhereClause(fn, formal,
                     new CallExpr(PRIM_IS_SUBTYPE, subtype, query->copy()));
    // Recurse to handle any nested DefExprs
    expandQueryForGenericTypeSpecifier(fn, symExprs, formal,
                                       subcall, query);
  } else {
    // Add check that formal type/param matches actual
    addToWhereClause(fn, formal,
                     new CallExpr("==", actual->copy(), query));
  }
}

// call - the type constructor or build tuple call currently being considered
// queryToCopy - a PRIM_QUERY formal, ... recording the path to the current call
static void expandQueryForGenericTypeSpecifier(FnSymbol*  fn,
                                               std::vector<SymExpr*>& symExprs,
                                               ArgSymbol* formal,
                                               CallExpr* call,
                                               BaseAST* queried) {

  int position = 1;

  if (call->isNamed("_build_tuple")) {
    Expr*     actual = new SymExpr(new_IntSymbol(call->numActuals()));
    CallExpr* query  = makePrimQuery(queried, new_CStringSymbol("size"));

    // Add check that actual tuple size == formal tuple size
    addToWhereClause(fn, formal, new CallExpr("==", actual, query));

    call->baseExpr->replace(new SymExpr(dtTuple->symbol));

    position = position + 1; // tuple size is technically 1st param/type
  } else if (call->isPrimitive(PRIM_TO_UNMANAGED_CLASS)) {
    if (CallExpr* subCall = toCallExpr(call->get(1))) {
      if (SymExpr* subBase = toSymExpr(subCall->baseExpr)) {
        if (AggregateType* at = toAggregateType(subBase->symbol()->type)) {
          if (isClass(at)) {
            // TODO -- should this move to scope resolve?

            // Replace PRIM_TO_UNMANAGED( MyClass( Def ?t ) )
            // with
            // unmanaged MyClass ( Def ?t )

            Type* unm = at->getUnmanagedClass();
            subCall->baseExpr->replace(new SymExpr(unm->symbol));
            call->replace(subCall->remove());
            call = subCall;
          }
        }
      }
    }
  }

  CallExpr* gatheringNamedArgs = makePrimQuery(queried);

  for_actuals(actual, call) {
    if (NamedExpr* named = toNamedExpr(actual)) {
      Symbol*   name = new_CStringSymbol(named->name);
      CallExpr* query = makePrimQuery(queried, name);

      gatheringNamedArgs->insertAtTail(new SymExpr(name));

      expandQueryForActual(fn, symExprs, formal, call, query, named->actual);
    }

    // not a NamedExpr? handled in next loop.
  }

  for_actuals(actual, call) {
    if (isNamedExpr(actual) == false) {
      CallExpr* query = gatheringNamedArgs->copy();
      query->insertAtTail(new SymExpr(new_IntSymbol(position)));

      expandQueryForActual(fn, symExprs, formal, call, query, actual);

      position = position + 1;
    }
  }
}

static void replaceQueryUses(ArgSymbol*             formal,
                             DefExpr*               def,
                             CallExpr*              query,
                             std::vector<SymExpr*>& symExprs) {
  for_vector(SymExpr, se, symExprs) {
    if (se->symbol() == def->sym) {
      CallExpr* myQuery = query->copy();
      se->replace(myQuery);
    }
  }
}

static void addToWhereClause(FnSymbol*  fn,
                             ArgSymbol* formal,
                             Expr*      test) {
  Expr*     where  = NULL;

  if (fn->where == NULL) {
    where = new SymExpr(gTrue);

    fn->where = new BlockStmt(where);

    insert_help(fn->where, NULL, fn);

    fn->addFlag(FLAG_COMPILER_ADDED_WHERE);

  } else {
    where = fn->where->body.tail;
  }

  formal->addFlag(FLAG_NOT_FULLY_GENERIC);

  // Replace where with (where & test)
  CallExpr* combine = new CallExpr("&");
  where->replace(combine);
  combine->insertAtTail(where);
  combine->insertAtTail(test);
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static bool isConstructor(FnSymbol* fn) {
  bool retval = false;

  if (fn->numFormals()       >= 2 &&
      fn->getFormal(1)->type == dtMethodToken) {

    retval = strcmp(fn->name, fn->getFormal(2)->type->symbol->name) == 0;
  }

  return retval;
}

static void updateConstructor(FnSymbol* fn) {
  SymbolMap      map;
  Type*          type = fn->getFormal(2)->type;
  AggregateType* ct   = toAggregateType(type);

  if (ct == NULL) {
    if (type == dtUnknown) {
      INT_FATAL(fn, "'this' argument has unknown type");
    } else {
      INT_FATAL(fn, "initializer on non-class type");
    }
  }

  if (fn->hasFlag(FLAG_NO_PARENS)) {
    USR_FATAL(fn, "a constructor cannot be declared without parentheses");
  }

  // Call the constructor, passing in just the generic arguments.
  // This call ensures that the object is default-initialized before the
  // user's constructor body is called.
  // NOTE: This operation is not necessary for initializers, as Phase 1 of
  // the initializer body is intended to perform this operation on its own.
  CallExpr* call = new CallExpr(ct->defaultInitializer);

  for_formals(typeConstructorArg, ct->typeConstructor) {
    ArgSymbol* arg = NULL;

    for_formals(methodArg, fn) {
      if (typeConstructorArg->name == methodArg->name) {
        arg = methodArg;
      }
    }

    if (arg == NULL) {
      if (typeConstructorArg->defaultExpr == NULL) {
        USR_FATAL_CONT(fn,
                       "constructor for class '%s' requires a generic "
                       "argument called '%s'",
                       ct->symbol->name,
                       typeConstructorArg->name);
      }
    } else {
      call->insertAtTail(new NamedExpr(arg->name, new SymExpr(arg)));
    }
  }

  fn->_this = new VarSymbol("this");
  fn->_this->addFlag(FLAG_ARG_THIS);

  fn->insertAtHead(new CallExpr(PRIM_MOVE, fn->_this, call));

  fn->insertAtHead(new DefExpr(fn->_this));
  fn->insertAtTail(new CallExpr(PRIM_RETURN, new SymExpr(fn->_this)));

  map.put(fn->getFormal(2), fn->_this);

  fn->formals.get(2)->remove();
  fn->formals.get(1)->remove();

  update_symbols(fn, &map);

  // The constructor's name is the name of the type.
  // Replace it with _construct_typename
  fn->name = ct->defaultInitializer->name;

  if (fNoUserConstructors) {
    ModuleSymbol* mod = fn->getModule();
    if (mod && mod->modTag != MOD_INTERNAL && mod->modTag != MOD_STANDARD) {
      USR_FATAL_CONT(fn, "Type '%s' defined a constructor here",
                     ct->symbol->name);
    }
  }

  fn->addFlag(FLAG_CONSTRUCTOR);
}

static void updateInitMethod(FnSymbol* fn) {
  if (isAggregateType(fn->_this->type) == true) {
    preNormalizeInitMethod(fn);

  } else if (fn->_this->type == dtUnknown) {
    INT_FATAL(fn, "'this' argument has unknown type");

  } else {
    INT_FATAL(fn, "initializer on non-class type");
  }
}

/************************************* | **************************************
*                                                                             *
* If se is a type alias, resolves it recursively, or fails and returns NULL.  *
*                                                                             *
************************************** | *************************************/

static TypeSymbol* expandTypeAlias(SymExpr* se) {
  TypeSymbol* retval = NULL;

  while (se != NULL && retval == NULL) {
    Symbol* sym = se->symbol();

    if (TypeSymbol* ts = toTypeSymbol(sym)) {
      retval = ts;

    } else if (VarSymbol* vs = toVarSymbol(sym)) {
      if (vs->isType() == true) {
        // The definition in the init field of its declaration.
        DefExpr* def = vs->defPoint;

        se = toSymExpr(def->init);

      } else {
        se = NULL;
      }

    } else {
      se = NULL;
    }
  }

  return retval;
}

/************************************* | **************************************
*                                                                             *
*                                                                             *
*                                                                             *
************************************** | *************************************/

static void find_printModuleInit_stuff() {
  std::vector<Symbol*> symbols;

  collectSymbols(printModuleInitModule, symbols);

  for_vector(Symbol, symbol, symbols) {

    // TODO -- move this logic to wellknown.cpp
    if (symbol->hasFlag(FLAG_PRINT_MODULE_INIT_INDENT_LEVEL)) {
      gModuleInitIndentLevel = toVarSymbol(symbol);
      INT_ASSERT(gModuleInitIndentLevel);
    }
  }
}
