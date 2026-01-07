/* SPDX-License-Identifier: GPL-2.0 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM ufshcd
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH trace/hooks
#if !defined(_TRACE_HOOK_UFSHCD_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_HOOK_UFSHCD_H
#include <trace/hooks/vendor_hooks.h>
/*
 * Following tracepoints are not exported in tracefs and provide a
 * mechanism for vendor modules to hook and extend functionality
 */
struct ufs_hba;
struct scsi_cmnd;

DECLARE_HOOK(android_vh_ufs_fill_prdt,
	TP_PROTO(struct ufs_hba *hba, struct scsi_cmnd *cmd,
		 unsigned int segments, int *err),
	TP_ARGS(hba, cmd, segments, err));

DECLARE_RESTRICTED_HOOK(android_rvh_ufs_reprogram_all_keys,
			TP_PROTO(struct ufs_hba *hba, int *err),
			TP_ARGS(hba, err), 1);

DECLARE_HOOK(android_vh_ufs_prepare_command,
	TP_PROTO(struct ufs_hba *hba, struct scsi_cmnd *cmd, int *err),
	TP_ARGS(hba, cmd, err));

DECLARE_HOOK(android_vh_ufs_update_sysfs,
	TP_PROTO(struct ufs_hba *hba),
	TP_ARGS(hba));

DECLARE_HOOK(android_vh_ufs_send_command,
	TP_PROTO(struct ufs_hba *hba, struct scsi_cmnd *cmd),
	TP_ARGS(hba, cmd));

DECLARE_HOOK(android_vh_ufs_compl_command,
	TP_PROTO(struct ufs_hba *hba, struct scsi_cmnd *cmd),
	TP_ARGS(hba, cmd));

struct uic_command;
DECLARE_HOOK(android_vh_ufs_send_uic_command,
	TP_PROTO(struct ufs_hba *hba, const struct uic_command *ucmd,
		 int str_t),
	TP_ARGS(hba, ucmd, str_t));

DECLARE_HOOK(android_vh_ufs_send_tm_command,
	TP_PROTO(struct ufs_hba *hba, int tag, int str_t),
	TP_ARGS(hba, tag, str_t));

DECLARE_HOOK(android_vh_ufs_check_int_errors,
	TP_PROTO(struct ufs_hba *hba, bool queue_eh_work),
	TP_ARGS(hba, queue_eh_work));

#endif /* _TRACE_HOOK_UFSHCD_H */
/* This part must be outside protection */
#include <trace/define_trace.h>
