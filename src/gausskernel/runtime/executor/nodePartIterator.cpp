/*
 * Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 *
 * openGauss is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *
 *          http://license.coscl.org.cn/MulanPSL2
 *
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 * ---------------------------------------------------------------------------------------
 *
 *  nodePartIterator.cpp
 *        data partition: routines to support Partition Wise Join
 *
 * IDENTIFICATION
 *        src/gausskernel/runtime/executor/nodePartIterator.cpp
 *
 * ---------------------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include "executor/execdebug.h"
#include "executor/nodePartIterator.h"
#include "executor/tuptable.h"
#include "utils/memutils.h"
#include "nodes/execnodes.h"
#include "nodes/plannodes.h"
#include "vecexecutor/vecnodes.h"

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: initialize PartIterator for partition iteration
 * Description	:
 * Notes		: it is used for partitioned-table only
 */
PartIteratorState* ExecInitPartIterator(PartIterator* node, EState* estate, int eflags)
{
    PartIteratorState* state = NULL;

    state = makeNode(PartIteratorState);
    state->ps.plan = (Plan*)node;
    state->ps.state = estate;

    /* initiate sub node */
    state->ps.lefttree = ExecInitNode(node->plan.lefttree, estate, eflags);
    state->ps.qual = NULL;
    state->ps.righttree = NULL;
    state->ps.subPlan = NULL;
    state->ps.ps_TupFromTlist = false;
    state->ps.ps_ProjInfo = NULL;
    state->currentItr = -1;

    return state;
}

static void init_scan_partition(PartIteratorState* node)
{
    int paramno = 0;
    unsigned int itr_idx = 0;
    PartIterator* pi_node = (PartIterator*)node->ps.plan;
    ParamExecData* param = NULL;

    Assert(ForwardScanDirection == pi_node->direction || BackwardScanDirection == pi_node->direction);

    /* set iterator parameter */
    node->currentItr++;
    itr_idx = node->currentItr;
    if (BackwardScanDirection == pi_node->direction)
        itr_idx = pi_node->itrs - itr_idx - 1;

    paramno = pi_node->param->paramno;
    param = &(node->ps.state->es_param_exec_vals[paramno]);
    param->isnull = false;
    param->value = (Datum)itr_idx;
    node->ps.lefttree->chgParam = bms_add_member(node->ps.lefttree->chgParam, paramno);

    /* reset the plan node so that next partition can be scanned */
    ExecReScan(node->ps.lefttree);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Scans the partitioned table with partition iteration and returns
 *			: the next qualifying tuple in the direction specified
 * Description	: partition iteration is a frame of the Planstate for scan a partitioned
 *			: table. it is like a monitor. The real job is done by SeqScan .e.g
 * Notes		:
 */
TupleTableSlot* ExecPartIterator(PartIteratorState* node)
{
    TupleTableSlot* slot = NULL;
    PartIterator* pi_node = (PartIterator*)node->ps.plan;
    EState* state = node->ps.lefttree->state;
    node->ps.lefttree->do_not_reset_rownum = true;
    bool orig_early_free = state->es_skip_early_free;

    PlanState* noden = (PlanState*)node->ps.lefttree;
    int partitionScan;
    switch (nodeTag(noden)) {
        case T_SeqScanState:
            partitionScan =  ((SeqScanState*)noden)->part_id;
            break;
        case T_IndexScanState:
            partitionScan =  ((IndexScanState*)noden)->part_id;
            break;
        case T_IndexOnlyScanState:
            partitionScan =  ((IndexOnlyScanState*)noden)->part_id;
            break;
        case T_BitmapHeapScanState:
            partitionScan =  ((BitmapHeapScanState*)noden)->part_id;
            break;
        case T_VecToRowState:
            partitionScan = ((VecToRowState*)noden)->part_id;
            break;
        default:
            partitionScan = pi_node->itrs;
            break;
    }

    if (partitionScan == 0) {
        /* return NULL if no partition is selected */
        return NULL;
    }

    /* init first scanned partition */
    if (node->currentItr == -1)
        init_scan_partition(node);

    /* For partition wise join, can not early free left tree's caching memory */
    state->es_skip_early_free = true;
    slot = ExecProcNode(node->ps.lefttree);
    state->es_skip_early_free = orig_early_free;

    if (!TupIsNull(slot))
        return slot;

    node->ps.lefttree->ps_rownum--;
    /* switch to next partition until we get a unempty tuple */
    for (;;) {
        if (node->currentItr + 1 >= partitionScan) /* have scanned all partitions */
            return NULL;

        /* switch to next partiiton */
        init_scan_partition(node);

        /* For partition wise join, can not early free left tree's caching memory */
        orig_early_free = state->es_skip_early_free;
        state->es_skip_early_free = true;
        slot = ExecProcNode(node->ps.lefttree);
        state->es_skip_early_free = orig_early_free;

        if (!TupIsNull(slot))
            return slot;
    }
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: clear out the partition iterator
 * Description	:
 * Notes		:
 */
void ExecEndPartIterator(PartIteratorState* node)
{
    /* close down subplans */
    ExecEndNode(node->ps.lefttree);
}

/*
 * @@GaussDB@@
 * Target		: data partition
 * Brief		: Reset the partition iterator node so that its output
 *			: can be re-scanned.
 * Description	:
 * Notes		:
 */
void ExecReScanPartIterator(PartIteratorState* node)
{
    PartIterator* pi_node = NULL;
    int paramno = -1;
    ParamExecData* param = NULL;
    PartIterator* piterator = NULL;

    piterator = (PartIterator*)node->ps.plan;

    /* do nothing if there is no partition to scan */
    if (piterator->itrs == 0)
        return;

    node->currentItr = -1;

    pi_node = (PartIterator*)node->ps.plan;
    paramno = pi_node->param->paramno;
    param = &(node->ps.state->es_param_exec_vals[paramno]);
    param->isnull = false;
    param->value = (Datum)0;
    node->ps.lefttree->chgParam = bms_add_member(node->ps.lefttree->chgParam, paramno);

    /*
     * if the pruning result isnot null, Reset the subplan node so
     * that its output can be re-scanned.
     */
    ExecReScan(node->ps.lefttree);
}
