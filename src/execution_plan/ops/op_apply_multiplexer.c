/*
 * Copyright 2018-2019 Redis Labs Ltd. and Contributors
 *
 * This file is available under the Redis Labs Source Available License Agreement
 */

#include "op_apply_multiplexer.h"

// Forward declerations.
OpResult OpApplyMultiplexerInit(OpBase *opBase);
OpResult OpApplyMultiplexerReset(OpBase *opBase);
Record OpApplyMultiplexerConsume(OpBase *opBase);
void OpApplyMultiplexerFree(OpBase *opBase);

static Record _pullFromBranchStream(OpApplyMultiplexer *apply_multiplexer, int branch_index) {
	// Propegate record to the top of the match stream.
	Argument_AddRecord(apply_multiplexer->branches_arguments[branch_index - 1],
					   Record_Clone(apply_multiplexer->r));
	return OpBase_Consume(apply_multiplexer->op.children[branch_index]);
}

static Record _OpApplyMultiplexer_OrApplyLogic(OpApplyMultiplexer *op) {
	while(true) {
		// Try to get a record from bound stream.
		op->r = OpBase_Consume(op->bound_branch);
		if(!op->r) return NULL; // Depleted.

		// Try to get a record from some stream.
		for(int i = 1; i < op->op.childCount; i++) {
			Record branch_record = _pullFromBranchStream(op, i);
			if(branch_record) {
				// Don't care about the branch record.
				Record_Free(branch_record);
				Record r = op->r;
				op->r = NULL;   // Null to avoid double free.
				return r;
			}
		}
		// Did not managed to get a record from any branch, loop back and restart.
		Record_Free(op->r);
		op->r = NULL;
	}
}

static Record _OpApplyMultiplexer_AndApplyLogic(OpApplyMultiplexer *op) {
	while(true) {
		// Try to get a record from bound stream.
		op->r = OpBase_Consume(op->bound_branch);
		if(!op->r) return NULL; // Depleted.

		// Try to get a record from some stream.
		for(int i = 1; i < op->op.childCount; i++) {
			Record branch_record = _pullFromBranchStream(op, i);
			// Don't care about the branch record.
			if(branch_record) Record_Free(branch_record);
			else {
				// Did not managed to get a record from some branch, loop back and restart.
				Record_Free(op->r);
				op->r = NULL;
				break;
			}
		}
		// All branches returned record => all filters are satisfied by the bounded record.
		Record r = op->r;
		op->r = NULL;   // Null to avoid double free.
		return r;
	}
}

OpBase *NewApplyMultiplexerOp(ExecutionPlan *plan, AST_Operator boolean_operator) {

	OpApplyMultiplexer *op = rm_calloc(1, sizeof(OpApplyMultiplexer));
	op->boolean_operator = boolean_operator;
	const char *name;
	if(boolean_operator == OP_OR) {
		name = "OR Apply Multiplexer";
		op->apply_func = _OpApplyMultiplexer_OrApplyLogic;
	} else if(boolean_operator == OP_AND) {
		name = "OR Apply Multiplexer";
		op->apply_func = _OpApplyMultiplexer_AndApplyLogic;
	} else {
		assert(false);
	}

	// Set our Op operations
	OpBase_Init((OpBase *)op, OPType_APPLY_MULTIPLEXER, name, OpApplyMultiplexerInit,
				OpApplyMultiplexerConsume, OpApplyMultiplexerReset, NULL, OpApplyMultiplexerFree, false, plan);

	return (OpBase *) op;
}

Record OpApplyMultiplexerConsume(OpBase *opBase) {
	OpApplyMultiplexer *op = (OpApplyMultiplexer *) opBase;
	return op->apply_func(op);
}

static void _OpApplyMultiplexer_SortChildren(OpBase *op) {
	for(int i = 1; i < op->childCount; i++) {
		OpBase *child = op->children[i];
		if(child->type & (OPType_APPLY_MULTIPLEXER | OPType_SEMI_APPLY)) {
			bool swapped = false;
			for(int j = i + 1; j < op->childCount; j++) {
				OpBase *candidate = op->children[j];
				if(candidate->type & OPType_FILTER) {
					OpBase *tmp = candidate;
					op->children[i] = candidate;
					op->children[j] = child;
					swapped = true;
				}
			}
			if(!swapped) return;
		}
	}
}

OpResult OpApplyMultiplexerInit(OpBase *opBase) {
	_OpApplyMultiplexer_SortChildren(opBase);
	OpApplyMultiplexer *apply_multiplexer = (OpApplyMultiplexer *) opBase;
	apply_multiplexer->bound_branch = opBase->children[0];
	int childCount = opBase->childCount;
	apply_multiplexer->branches_arguments = array_new(Argument *, childCount - 1);
	for(int i = 1; i < childCount; i++) {
		OpBase *child = opBase->children[i];
		Argument *arg = (Argument *)ExecutionPlan_LocateFirstOp(child, OPType_ARGUMENT);
		apply_multiplexer->branches_arguments = array_append(apply_multiplexer->branches_arguments, arg);
	}
	return OP_OK;
}

OpResult OpApplyMultiplexerReset(OpBase *opBase) {
	OpApplyMultiplexer *op = (OpApplyMultiplexer *)opBase;
	if(op->r) {
		Record_Free(op->r);
		op->r = NULL;
	}
	return OP_OK;
}

void OpApplyMultiplexerFree(OpBase *opBase) {
	OpApplyMultiplexer *op = (OpApplyMultiplexer *)opBase;

	if(op->r) {
		Record_Free(op->r);
		op->r = NULL;
	}
}