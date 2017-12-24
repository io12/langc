#include <stdbool.h>
#include <stdio.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/Core.h>
#include <llvm-c/TargetMachine.h>
#include "ds.h"
#include "ast.h"
#include "check_semantics.h"
#include "lex.h"
#include "quoftc.h"
#include "symbol_table.h"
#include "code_gen.h"

static struct symbol_table sym_tbl;

static LLVMTypeRef get_fat_ptr_type(LLVMTypeRef item_type)
{
	LLVMTypeRef *struct_item_types;

	struct_item_types = xmalloc(sizeof(LLVMTypeRef) * 2);
	struct_item_types[0] = LLVMInt16Type();
	struct_item_types[1] = LLVMPointerType(item_type, 0);
	return LLVMStructType(struct_item_types, 2, false);
}

static LLVMTypeRef get_llvm_type(struct type *type)
{
	switch (type->kind) {
	case UNSIZED_INT_TYPE:
		// TODO: Base this on the compilation target
		return LLVMInt32Type();
	case U8_TYPE:
	case I8_TYPE:
		return LLVMInt8Type();
	case U16_TYPE:
	case I16_TYPE:
		return LLVMInt16Type();
	case U32_TYPE:
	case I32_TYPE:
	case CHAR_TYPE:
		return LLVMInt32Type();
	case U64_TYPE:
	case I64_TYPE:
		return LLVMInt64Type();
	case F32_TYPE:
		return LLVMFloatType();
	case F64_TYPE:
		return LLVMDoubleType();
	case BOOL_TYPE:
		return LLVMInt1Type();
	case VOID_TYPE:
		return LLVMVoidType();
	case ALIAS_TYPE:
		// TODO: Resolve type
	case PARAM_TYPE:
		// TODO: Resolve type
	case ARRAY_TYPE: {
		LLVMTypeRef item_type = get_llvm_type(type->u.array.l);
		unsigned len = type->u.array.len;

		if (len == 0) {
			return get_fat_ptr_type(item_type);
		} else {
			return LLVMArrayType(item_type, len);
		}
	}
	case POINTER_TYPE:
		return LLVMPointerType(get_llvm_type(type->u.pointer.l), 0);
	case TUPLE_TYPE: {
		Vec *types = type->u.tuple.types;
		size_t len = vec_len(types), i;
		LLVMTypeRef *llvm_types = xmalloc(sizeof(LLVMTypeRef) * len);

		for (i = 0; i < len; i++) {
			llvm_types[i] = get_llvm_type(vec_get(types, i));
		}
		return LLVMStructType(llvm_types, len, false);
	}
	case FUNC_TYPE: {
		Vec *params = type->u.func.params;
		size_t len = vec_len(params);
		LLVMTypeRef *llvm_params = xmalloc(sizeof(LLVMTypeRef) * len);

		return LLVMFunctionType(get_llvm_type(type->u.func.ret),
				llvm_params, len, false);
	}
	}
	internal_error();
}

static LLVMValueRef emit_expr(LLVMBuilderRef, struct expr *);

static LLVMValueRef emit_inc_or_dec_expr(LLVMBuilderRef builder,
		struct expr *expr)
{
	enum unary_op op = expr->u.unary_op.op;
	LLVMValueRef ptr_val = emit_expr(builder, expr->u.unary_op.operand);
	LLVMTypeRef type = get_llvm_type(expr->type);
	LLVMValueRef old_val, one_val, new_val;
	bool is_signed, is_inc, is_prefix;

	is_signed = !is_unsigned_int_type(expr->type);
	is_inc = (op == PRE_INC_OP || op == POST_INC_OP);
	is_prefix = (op == PRE_INC_OP || op == PRE_DEC_OP);
	old_val = LLVMBuildLoad(builder, ptr_val, "inc_or_dec_load");
	one_val = LLVMConstInt(type, 1, is_signed);
	if (is_inc) {
		new_val = LLVMBuildAdd(builder, old_val, one_val, "inc_add");
	} else {
		new_val = LLVMBuildSub(builder, old_val, one_val, "dec_add");
	}
	LLVMBuildStore(builder, new_val, ptr_val);
	if (is_prefix) {
		return new_val;
	} else {
		return old_val;
	}
}

static LLVMValueRef emit_unary_op_expr(LLVMBuilderRef builder,
		struct expr *expr)
{
	enum unary_op op = expr->u.unary_op.op;
	LLVMValueRef operand = emit_expr(builder, expr->u.unary_op.operand);
	LLVMTypeRef type = get_llvm_type(expr->type);
	bool is_const_expr = (builder == NULL);

	switch (op) {
	case NEG_OP:
		if (is_const_expr) {
			return LLVMConstNeg(operand);
		} else {
			return LLVMBuildNeg(builder, operand, "neg");
		}
	case PRE_INC_OP:
	case POST_INC_OP:
	case PRE_DEC_OP:
	case POST_DEC_OP:
		return emit_inc_or_dec_expr(builder, expr);
	case DEREF_OP:
		return LLVMBuildLoad(builder, operand, "deref");
	case REF_OP:
		return operand;
	case BIT_NOT_OP:
		if (is_const_expr) {
			return LLVMConstNot(operand);
		} else {
			return LLVMBuildNot(builder, operand, "bitwise_not");
		}
	case LOG_NOT_OP: {
		LLVMValueRef zero_val;

		zero_val = LLVMConstInt(type, 0, false);
		if (is_const_expr) {
			return LLVMConstICmp(LLVMIntEQ, operand, zero_val);
		} else {
			return LLVMBuildICmp(builder, LLVMIntEQ, operand,
					zero_val, "logical_not");
		}
	}
	}
	internal_error();
}

static LLVMValueRef emit_add(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFAdd(l, r);
		} else {
			return LLVMConstAdd(l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFAdd(builder, l, r, "add");
		} else {
			return LLVMBuildAdd(builder, l, r, "add");
		}
	}
}

static LLVMValueRef emit_sub(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFSub(l, r);
		} else {
			return LLVMConstSub(l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFSub(builder, l, r, "sub");
		} else {
			return LLVMBuildSub(builder, l, r, "sub");
		}
	}
}

static LLVMValueRef emit_mul(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFMul(l, r);
		} else {
			return LLVMConstMul(l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFMul(builder, l, r, "mul");
		} else {
			return LLVMBuildMul(builder, l, r, "mul");
		}
	}
}

static LLVMValueRef emit_div(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFDiv(l, r);
		} else if (is_unsigned_int_type(type)) {
			return LLVMConstUDiv(l, r);
		} else {
			return LLVMConstSDiv(l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFDiv(builder, l, r, "div");
		} else if (is_unsigned_int_type(type)) {
			return LLVMBuildUDiv(builder, l, r, "div");
		} else {
			return LLVMBuildSDiv(builder, l, r, "div");
		}
	}
}

static LLVMValueRef emit_mod(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFRem(l, r);
		} else if (is_unsigned_int_type(type)) {
			return LLVMConstURem(l, r);
		} else {
			return LLVMConstSRem(l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFRem(builder, l, r, "mod");
		} else if (is_unsigned_int_type(type)) {
			return LLVMBuildURem(builder, l, r, "mod");
		} else {
			return LLVMBuildSRem(builder, l, r, "mod");
		}
	}
}

static LLVMValueRef emit_lt(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFCmp(LLVMRealOLT, l, r);
		} else if (is_unsigned_int_type(type)) {
			return LLVMConstICmp(LLVMIntULT, l, r);
		} else {
			return LLVMConstICmp(LLVMIntSLT, l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFCmp(builder, LLVMRealOLT, l, r, "lt");
		} else if (is_unsigned_int_type(type)) {
			return LLVMBuildICmp(builder, LLVMIntULT, l, r, "lt");
		} else {
			return LLVMBuildICmp(builder, LLVMIntSLT, l, r, "lt");
		}
	}
}

static LLVMValueRef emit_gt(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFCmp(LLVMRealOGT, l, r);
		} else if (is_unsigned_int_type(type)) {
			return LLVMConstICmp(LLVMIntUGT, l, r);
		} else {
			return LLVMConstICmp(LLVMIntSGT, l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFCmp(builder, LLVMRealOGT, l, r, "gt");
		} else if (is_unsigned_int_type(type)) {
			return LLVMBuildICmp(builder, LLVMIntUGT, l, r, "gt");
		} else {
			return LLVMBuildICmp(builder, LLVMIntSGT, l, r, "gt");
		}
	}
}

static LLVMValueRef emit_le(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFCmp(LLVMRealOLE, l, r);
		} else if (is_unsigned_int_type(type)) {
			return LLVMConstICmp(LLVMIntULE, l, r);
		} else {
			return LLVMConstICmp(LLVMIntSLE, l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFCmp(builder, LLVMRealOLE, l, r, "le");
		} else if (is_unsigned_int_type(type)) {
			return LLVMBuildICmp(builder, LLVMIntULE, l, r, "le");
		} else {
			return LLVMBuildICmp(builder, LLVMIntSLE, l, r, "le");
		}
	}
}

static LLVMValueRef emit_ge(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFCmp(LLVMRealOGE, l, r);
		} else if (is_unsigned_int_type(type)) {
			return LLVMConstICmp(LLVMIntUGE, l, r);
		} else {
			return LLVMConstICmp(LLVMIntSGE, l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFCmp(builder, LLVMRealOGE, l, r, "ge");
		} else if (is_unsigned_int_type(type)) {
			return LLVMBuildICmp(builder, LLVMIntUGE, l, r, "ge");
		} else {
			return LLVMBuildICmp(builder, LLVMIntSGE, l, r, "ge");
		}
	}
}

static LLVMValueRef emit_eq(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFCmp(LLVMRealOEQ, l, r);
		} else {
			return LLVMConstICmp(LLVMIntEQ, l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFCmp(builder, LLVMRealOEQ, l, r, "eq");
		} else {
			return LLVMBuildICmp(builder, LLVMIntEQ, l, r, "eq");
		}
	}
}

static LLVMValueRef emit_ne(LLVMBuilderRef builder, LLVMValueRef l,
		LLVMValueRef r, struct type *type)
{
	bool is_const_expr = (builder == NULL);

	if (is_const_expr) {
		if (is_float_type(type)) {
			return LLVMConstFCmp(LLVMRealONE, l, r);
		} else {
			return LLVMConstICmp(LLVMIntNE, l, r);
		}
	} else {
		if (is_float_type(type)) {
			return LLVMBuildFCmp(builder, LLVMRealONE, l, r, "ne");
		} else {
			return LLVMBuildICmp(builder, LLVMIntNE, l, r, "ne");
		}
	}
}

static bool is_assignment(enum bin_op op)
{
	switch (op) {
	case ASSIGN_OP:
	case ADD_ASSIGN_OP:
	case SUB_ASSIGN_OP:
	case MUL_ASSIGN_OP:
	case DIV_ASSIGN_OP:
	case MOD_ASSIGN_OP:
	case BIT_AND_ASSIGN_OP:
	case BIT_OR_ASSIGN_OP:
	case BIT_XOR_ASSIGN_OP:
	case BIT_SHIFT_L_ASSIGN_OP:
	case BIT_SHIFT_R_ASSIGN_OP:
		return true;
	default:
		return false;
	}
}

static LLVMValueRef emit_bin_op_expr(LLVMBuilderRef builder, struct expr *expr)
{
	enum bin_op op = expr->u.bin_op.op;
	LLVMValueRef l = emit_expr(builder, expr->u.bin_op.l),
	             r = emit_expr(builder, expr->u.bin_op.r);
	struct type *type = expr->type;
	LLVMValueRef old_val = NULL, new_val = NULL;
	bool is_const_expr = (builder == NULL);

	if (is_assignment(op)) {
		old_val = LLVMBuildLoad(builder, l, "assign_load");
	}
	switch (op) {
	case ADD_OP:
		return emit_add(builder, l, r, type);
	case SUB_OP:
		return emit_sub(builder, l, r, type);
	case MUL_OP:
		return emit_mul(builder, l, r, type);
	case DIV_OP:
		return emit_div(builder, l, r, type);
	case MOD_OP:
		return emit_mod(builder, l, r, type);
	case LT_OP:
		return emit_lt(builder, l, r, type);
	case GT_OP:
		return emit_gt(builder, l, r, type);
	case LT_EQ_OP:
		return emit_le(builder, l, r, type);
	case GT_EQ_OP:
		return emit_ge(builder, l, r, type);
	case EQ_OP:
		return emit_eq(builder, l, r, type);
	case NOT_EQ_OP:
		return emit_ne(builder, l, r, type);
	case BIT_AND_OP:
	case LOG_AND_OP:
		if (is_const_expr) {
			return LLVMConstAnd(l, r);
		} else {
			return LLVMBuildAnd(builder, l, r, "and");
		}
	case BIT_OR_OP:
	case LOG_OR_OP:
		if (is_const_expr) {
			return LLVMConstOr(l, r);
		} else {
			return LLVMBuildOr(builder, l, r, "or");
		}
	case BIT_XOR_OP:
		if (is_const_expr) {
			return LLVMConstXor(l, r);
		} else {
			return LLVMBuildXor(builder, l, r, "xor");
		}
	case BIT_SHIFT_L_OP:
		if (is_const_expr) {
			return LLVMConstShl(l, r);
		} else {
			return LLVMBuildShl(builder, l, r, "shl");
		}
	case BIT_SHIFT_R_OP:
		if (is_const_expr) {
			return LLVMConstLShr(l, r);
		} else {
			return LLVMBuildLShr(builder, l, r, "lshr");
		}
	case ASSIGN_OP:
		return LLVMBuildStore(builder, r, l);
	case ADD_ASSIGN_OP:
		new_val = emit_add(builder, old_val, r, type);
		goto assign_store;
	case SUB_ASSIGN_OP:
		new_val = emit_sub(builder, old_val, r, type);
		goto assign_store;
	case MUL_ASSIGN_OP:
		new_val = emit_mul(builder, old_val, r, type);
		goto assign_store;
	case DIV_ASSIGN_OP:
		new_val = emit_div(builder, old_val, r, type);
		goto assign_store;
	case MOD_ASSIGN_OP:
		new_val = emit_mod(builder, old_val, r, type);
		goto assign_store;
	case BIT_AND_ASSIGN_OP:
		new_val = LLVMBuildAnd(builder, old_val, r, "and");
		goto assign_store;
	case BIT_OR_ASSIGN_OP:
		new_val = LLVMBuildOr(builder, old_val, r, "or");
		goto assign_store;
	case BIT_XOR_ASSIGN_OP:
		new_val = LLVMBuildXor(builder, old_val, r, "xor");
		goto assign_store;
	case BIT_SHIFT_L_ASSIGN_OP:
		new_val = LLVMBuildShl(builder, old_val, r, "shl");
		goto assign_store;
	case BIT_SHIFT_R_ASSIGN_OP:
		new_val = LLVMBuildLShr(builder, old_val, r, "lshr");
		goto assign_store;
	case FIELD_OP:
		return NULL; // TODO: Stub
	}
assign_store:
	return LLVMBuildStore(builder, new_val, l);
}

static LLVMValueRef emit_expr(LLVMBuilderRef builder, struct expr *expr)
{
	LLVMTypeRef llvm_type = get_llvm_type(expr->type);

	switch (expr->kind) {
	case BOOL_LIT_EXPR: {
		bool val = expr->u.bool_lit.val;

		return LLVMConstInt(llvm_type, val, false);
	}
	case INT_LIT_EXPR: {
		uint64_t val = expr->u.int_lit.val;

		return LLVMConstInt(llvm_type, val, false);
	}
	case FLOAT_LIT_EXPR: {
		double val = expr->u.float_lit.val;

		return LLVMConstReal(llvm_type, val);
	}
	case CHAR_LIT_EXPR: {
		uint32_t val = expr->u.char_lit.val;

		return LLVMConstInt(llvm_type, val, false);
	}
	case STRING_LIT_EXPR: {
		char *val = expr->u.string_lit.val;
		unsigned len = expr->u.string_lit.len;

		return LLVMConstString(val, len, true);
	}
	case UNARY_OP_EXPR:
		return emit_unary_op_expr(builder, expr);
	case BIN_OP_EXPR:
		return emit_bin_op_expr(builder, expr);
	case LAMBDA_EXPR:
	case ARRAY_LIT_EXPR:
	case IDENT_EXPR:
	case BLOCK_EXPR:
	case IF_EXPR:
	case SWITCH_EXPR:
	case TUPLE_EXPR:
	default:
		return NULL; // TODO: Stub
	}
}

static LLVMValueRef emit_const_expr(struct expr *expr)
{
	return emit_expr(NULL, expr);
}

static void emit_global_val(LLVMModuleRef module, bool is_const,
		struct type *type, const char *name, struct expr *init)
{
	LLVMValueRef global;

	global = LLVMAddGlobal(module, get_llvm_type(type), name);
	LLVMSetInitializer(global, emit_const_expr(init));
	LLVMSetGlobalConstant(global, is_const);
}

static void emit_local_val(LLVMBuilderRef builder, struct type *type,
		char *name, struct expr *init)
{
	LLVMValueRef local_ptr;

	local_ptr = LLVMBuildAlloca(builder, get_llvm_type(type), name);
	if (init != NULL) {
		LLVMBuildStore(builder, emit_expr(builder, init), local_ptr);
	}
	insert_symbol(sym_tbl, name, local_ptr);
}

static LLVMValueRef get_cur_func(LLVMBuilderRef builder)
{
	return LLVMGetBasicBlockParent(LLVMGetInsertBlock(builder));
}

static void emit_stmts(LLVMBuilderRef, Vec *);

static void emit_if_stmt(LLVMBuilderRef builder, struct stmt *stmt)
{
	struct expr *cond = stmt->u.if_.cond;
	Vec *then_stmts = stmt->u.if_.then_stmts,
	    *else_stmts = stmt->u.if_.else_stmts;
	LLVMValueRef func_val, cond_val;
	LLVMBasicBlockRef then_block, else_block, merge_block;

	func_val = get_cur_func(builder);
	cond_val = emit_expr(builder, cond);
	then_block = LLVMAppendBasicBlock(func_val, "then");
	else_block = LLVMAppendBasicBlock(func_val, "else");
	merge_block = LLVMAppendBasicBlock(func_val, "merge");
	LLVMBuildCondBr(builder, cond_val, then_block, else_block);
	LLVMPositionBuilderAtEnd(builder, then_block);
	emit_stmts(builder, then_stmts);
	LLVMBuildBr(builder, merge_block);
	LLVMPositionBuilderAtEnd(builder, else_block);
	emit_stmts(builder, else_stmts);
	LLVMBuildBr(builder, merge_block);
	LLVMPositionBuilderAtEnd(builder, merge_block);
}

static void emit_do_stmt(LLVMBuilderRef builder, struct stmt *stmt)
{
	Vec *stmts = stmt->u.do_.stmts;
	struct expr *cond = stmt->u.do_.cond;
	LLVMValueRef func_val, cond_val;
	LLVMBasicBlockRef do_block, after_do_block;

	func_val = get_cur_func(builder);
	cond_val = emit_expr(builder, cond);
	do_block = LLVMAppendBasicBlock(func_val, "do");
	after_do_block = LLVMAppendBasicBlock(func_val, "after_do");
	LLVMPositionBuilderAtEnd(builder, do_block);
	emit_stmts(builder, stmts);
	LLVMBuildCondBr(builder, cond_val, do_block, after_do_block);
	LLVMPositionBuilderAtEnd(builder, after_do_block);
}

static void emit_stmt(LLVMBuilderRef, struct stmt *);

static void emit_stmts(LLVMBuilderRef builder, Vec *stmts)
{
	size_t i;

	for (i = 0; i < vec_len(stmts); i++) {
		emit_stmt(builder, vec_get(stmts, i));
	}
}

static void emit_stmt(LLVMBuilderRef builder, struct stmt *stmt)
{
	switch (stmt->kind) {
	case DECL_STMT: {
		struct decl *decl = stmt->u.decl.decl;

		emit_local_val(builder, decl->type, decl->name, decl->init);
		break;
	}
	case EXPR_STMT:
		emit_expr(builder, stmt->u.expr.expr);
		break;
	case IF_STMT:
		emit_if_stmt(builder, stmt);
		break;
	case DO_STMT:
		emit_do_stmt(builder, stmt);
		break;
	case WHILE_STMT:
		// TODO: emit_while_stmt(builder, stmt);
		break;
	case FOR_STMT:
		// TODO: emit_for_stmt(builder, stmt);
		break;
	}
}

static void emit_func(LLVMModuleRef module, struct type *type,
		const char *name, struct expr *expr)
{
	Vec *param_names = expr->u.lambda.params;
	struct expr *body = expr->u.lambda.body;
	LLVMValueRef func_val;
	LLVMBasicBlockRef block;
	LLVMBuilderRef builder;
	size_t i;
	char *param_name;
	LLVMValueRef param_val;

	func_val = LLVMAddFunction(module, name, get_llvm_type(type));
	block = LLVMAppendBasicBlock(func_val, name);
	builder = LLVMCreateBuilder();
	LLVMPositionBuilderAtEnd(builder, block);
	enter_new_scope(sym_tbl);
	for (i = 0; i < vec_len(param_names); i++) {
		param_name = vec_get(param_names, i);
		param_val = LLVMGetParam(func_val, i);
		insert_symbol(sym_tbl, param_name, param_val);
	}
	// TODO: Return the result
	emit_expr(builder, body);
	leave_scope(sym_tbl);
	LLVMDisposeBuilder(builder);
}

static void emit_global_decl(LLVMModuleRef module, struct decl *decl)
{
	switch (decl->type->kind) {
	case UNSIZED_INT_TYPE:
	case VOID_TYPE:
		internal_error();
	case U8_TYPE:
	case U16_TYPE:
	case U32_TYPE:
	case U64_TYPE:
	case I8_TYPE:
	case I16_TYPE:
	case I32_TYPE:
	case I64_TYPE:
	case F32_TYPE:
	case F64_TYPE:
	case BOOL_TYPE:
	case CHAR_TYPE:
	case ALIAS_TYPE:
	case PARAM_TYPE:
	case ARRAY_TYPE:
	case POINTER_TYPE:
	case TUPLE_TYPE:
		emit_global_val(module, decl->is_const, decl->type, decl->name,
				decl->init);
		break;
	case FUNC_TYPE:
		emit_func(module, decl->type, decl->name, decl->init);
		break;
	}
}

static LLVMModuleRef emit_ast(struct ast ast)
{
	LLVMModuleRef module;
	Vec *decls = ast.decls;
	size_t i;

	sym_tbl = alloc_symbol_table();
	enter_new_scope(sym_tbl); // Global scope
	module = LLVMModuleCreateWithName(get_filename());
	for (i = 0; i < vec_len(decls); i++) {
		emit_global_decl(module, vec_get(decls, i));
	}
	free_symbol_table(sym_tbl);
	return module;
}

static void compile_module(LLVMModuleRef module)
{
	const char *target_triplet, *cpu, *features;
	LLVMTargetRef target;
	char *errmsg;
	LLVMTargetMachineRef target_machine;
#if 0
	LLVMTargetDataRef data_layout;
#endif

	LLVMVerifyModule(module, LLVMAbortProcessAction, NULL);
	LLVMInitializeAllTargetInfos();
	LLVMInitializeAllTargets();
	LLVMInitializeAllTargetMCs();
	LLVMInitializeAllAsmParsers();
	LLVMInitializeAllAsmPrinters();
	target_triplet = LLVMGetDefaultTargetTriple();
	LLVMGetTargetFromTriple(target_triplet, &target, &errmsg);
	fprintf(stderr, "%s\n", errmsg);
	cpu = "generic";
	features = "";
	target_machine = LLVMCreateTargetMachine(target, target_triplet, cpu,
			features, LLVMCodeGenLevelDefault, LLVMRelocDefault,
			LLVMCodeModelDefault);
#if 0
TODO: Add data layout to module
	data_layout = LLVMCreateDataLayout(target_machine);
	LLVMSetDataLayout(module, data_layout);
#endif
	LLVMSetTarget(module, target_triplet);
	// TODO: Free duplicated string
	LLVMTargetMachineEmitToFile(target_machine, module, xstrdup("a.out"),
			LLVMObjectFile, &errmsg);
	fprintf(stderr, "%s\n", errmsg);
	LLVMDisposeMessage(errmsg);
	LLVMDisposeTargetMachine(target_machine);
}

void compile_ast(struct ast ast)
{
	LLVMModuleRef module;

	module = emit_ast(ast);
	compile_module(module);
	LLVMDisposeModule(module);
}