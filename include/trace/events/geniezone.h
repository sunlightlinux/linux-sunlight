/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2023 MediaTek Inc.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM geniezone

#define _TRACE_GENIEZONE_H

#include <linux/tracepoint.h>

TRACE_EVENT(mtk_hypcall_enter,
	    TP_PROTO(unsigned long id),

	    TP_ARGS(id),

	    TP_STRUCT__entry(__field(unsigned long, id)),

	    TP_fast_assign(__entry->id = id;),

	    TP_printk("id=0x%lx", __entry->id)
);

TRACE_EVENT(mtk_hypcall_leave,
	    TP_PROTO(unsigned long id, unsigned long invalid),

	    TP_ARGS(id, invalid),

	    TP_STRUCT__entry(__field(unsigned long, id)
			     __field(unsigned long, invalid)
	    ),

	    TP_fast_assign(__entry->id = id;
			   __entry->invalid = invalid;
	    ),

	    TP_printk("id=0x%lx invalid=%lu", __entry->id, __entry->invalid)
);

TRACE_EVENT(mtk_vcpu_exit,
	    TP_PROTO(unsigned long exit_reason),

	    TP_ARGS(exit_reason),

	    TP_STRUCT__entry(__field(unsigned long, exit_reason)),

	    TP_fast_assign(__entry->exit_reason = exit_reason;),

	    TP_printk("vcpu exit_reason=0x%lx", __entry->exit_reason)
);

/* This part must be outside protection */
#include <trace/define_trace.h>
