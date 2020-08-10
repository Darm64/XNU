#include <sys/param.h>
#include <sys/systm.h>          /* XXX printf() */

#include <sys/types.h>
#include <sys/fcntl.h>
#include <sys/file.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/msg.h>
#include <sys/proc.h>
#include <sys/socketvar.h>
#include <sys/vnode.h>
#include <security/mac.h>
#include <security/mac_policy.h>

#include <libkern/section_keywords.h>
#include <libkern/OSDebug.h>    /* OSBPrintBacktrace */


/* forward declaration; see bsd_init.c */
errno_t check_policy_init(int);
int get_thread_lock_count(thread_t th);         /* forced forward */

/*
 * Policy flags used when the policy is enabled
 *
 * Note:	CHECK_POLICY_CHECK is probably not very useful unless you
 *		are kernel debugging and set a breakpoint.
 */
#define CHECK_POLICY_CHECK      0x00000001      /* Check on calls */
#define CHECK_POLICY_FAIL       0x00000002      /* EPERM on fails */
#define CHECK_POLICY_BACKTRACE  0x00000004      /* Show call stack on fails */
#define CHECK_POLICY_PANIC      0x00000008      /* Panic on fails */
#define CHECK_POLICY_PERIODIC   0x00000010      /* Show fails periodically */

static int policy_flags = 0;


#define CHECK_SET_HOOK(x)       .mpo_##x = (mpo_##x##_t *)common_hook,

/*
 * Init; currently, we only print our arrival notice.
 */
static void
hook_policy_init(struct mac_policy_conf *mpc)
{
	printf("Policy '%s' = '%s' ready\n", mpc->mpc_name, mpc->mpc_fullname);
}

static void
hook_policy_initbsd(struct mac_policy_conf *mpc)
{
	/* called with policy_grab_exclusive mutex held; exempt */
	printf("hook_policy_initbsd: %s\n", mpc->mpc_name);
}


/* Implementation */
#define CLASS_PERIOD_LIMIT      10000
#define CLASS_PERIOD_MULT       20

static int policy_check_event = 1;
static int policy_check_period = 1;
static int policy_check_next = CLASS_PERIOD_MULT;


static int
common_hook(void)
{
	int     i;
	int     rv = 0;

	if ((i = get_thread_lock_count(current_thread())) != 0) {
		/*
		 * fail the MACF check if we hold a lock; this assumes a
		 * a non-void (authorization) MACF hook.
		 */
		if (policy_flags & CHECK_POLICY_FAIL) {
			rv = EPERM;
		}

		/*
		 * display a backtrace if we hold a lock and we are not
		 * going to panic
		 */
		if ((policy_flags & (CHECK_POLICY_BACKTRACE | CHECK_POLICY_PANIC)) == CHECK_POLICY_BACKTRACE) {
			if (policy_flags & CHECK_POLICY_PERIODIC) {
				/* at exponentially increasing intervals */
				if (!(policy_check_event % policy_check_period)) {
					if (policy_check_event <= policy_check_next || policy_check_period == CLASS_PERIOD_LIMIT) {
						/*
						 * According to Derek, we could
						 * technically get a symbolicated name
						 * here, if we refactered some code
						 * and set the "keepsyms=1" boot
						 * argument...
						 */
						OSReportWithBacktrace("calling MACF hook with mutex count %d (event %d) ", i, policy_check_event);
					}
				} else {
					if (policy_check_period < CLASS_PERIOD_LIMIT) {
						policy_check_next *= CLASS_PERIOD_MULT;
						policy_check_period *= CLASS_PERIOD_MULT;
					}
				}
			} else {
				/* always */
				OSReportWithBacktrace("calling MACF hook with mutex count %d (event %d) ", i, policy_check_event);
			}
		}

		/* Panic */
		if (policy_flags & CHECK_POLICY_PANIC) {
			panic("calling MACF hook with mutex count %d\n", i);
		}

		/* count for non-fatal tracing */
		policy_check_event++;
	}

	return rv;
}

#if (MAC_POLICY_OPS_VERSION != 59)
# error "struct mac_policy_ops doesn't match definition in mac_policy.h"
#endif
/*
 * Policy hooks; one per possible hook
 *
 * Please note that this struct initialization should be kept in sync with
 * security/mac_policy.h (mac_policy_ops struct definition).
 */
const static struct mac_policy_ops policy_ops = {
	CHECK_SET_HOOK(audit_check_postselect)
	CHECK_SET_HOOK(audit_check_preselect)

	CHECK_SET_HOOK(bpfdesc_label_associate)
	CHECK_SET_HOOK(bpfdesc_label_destroy)
	CHECK_SET_HOOK(bpfdesc_label_init)
	CHECK_SET_HOOK(bpfdesc_check_receive)

	CHECK_SET_HOOK(cred_check_label_update_execve)
	CHECK_SET_HOOK(cred_check_label_update)
	CHECK_SET_HOOK(cred_check_visible)
	CHECK_SET_HOOK(cred_label_associate_fork)
	CHECK_SET_HOOK(cred_label_associate_kernel)
	CHECK_SET_HOOK(cred_label_associate)
	CHECK_SET_HOOK(cred_label_associate_user)
	CHECK_SET_HOOK(cred_label_destroy)
	CHECK_SET_HOOK(cred_label_externalize_audit)
	CHECK_SET_HOOK(cred_label_externalize)
	CHECK_SET_HOOK(cred_label_init)
	CHECK_SET_HOOK(cred_label_internalize)
	CHECK_SET_HOOK(cred_label_update_execve)
	CHECK_SET_HOOK(cred_label_update)

	CHECK_SET_HOOK(devfs_label_associate_device)
	CHECK_SET_HOOK(devfs_label_associate_directory)
	CHECK_SET_HOOK(devfs_label_copy)
	CHECK_SET_HOOK(devfs_label_destroy)
	CHECK_SET_HOOK(devfs_label_init)
	CHECK_SET_HOOK(devfs_label_update)

	CHECK_SET_HOOK(file_check_change_offset)
	CHECK_SET_HOOK(file_check_create)
	CHECK_SET_HOOK(file_check_dup)
	CHECK_SET_HOOK(file_check_fcntl)
	CHECK_SET_HOOK(file_check_get_offset)
	CHECK_SET_HOOK(file_check_get)
	CHECK_SET_HOOK(file_check_inherit)
	CHECK_SET_HOOK(file_check_ioctl)
	CHECK_SET_HOOK(file_check_lock)
	CHECK_SET_HOOK(file_check_mmap_downgrade)
	CHECK_SET_HOOK(file_check_mmap)
	CHECK_SET_HOOK(file_check_receive)
	CHECK_SET_HOOK(file_check_set)
	CHECK_SET_HOOK(file_label_init)
	CHECK_SET_HOOK(file_label_destroy)
	CHECK_SET_HOOK(file_label_associate)

	CHECK_SET_HOOK(ifnet_check_label_update)
	CHECK_SET_HOOK(ifnet_check_transmit)
	CHECK_SET_HOOK(ifnet_label_associate)
	CHECK_SET_HOOK(ifnet_label_copy)
	CHECK_SET_HOOK(ifnet_label_destroy)
	CHECK_SET_HOOK(ifnet_label_externalize)
	CHECK_SET_HOOK(ifnet_label_init)
	CHECK_SET_HOOK(ifnet_label_internalize)
	CHECK_SET_HOOK(ifnet_label_update)
	CHECK_SET_HOOK(ifnet_label_recycle)

	CHECK_SET_HOOK(inpcb_check_deliver)
	CHECK_SET_HOOK(inpcb_label_associate)
	CHECK_SET_HOOK(inpcb_label_destroy)
	CHECK_SET_HOOK(inpcb_label_init)
	CHECK_SET_HOOK(inpcb_label_recycle)
	CHECK_SET_HOOK(inpcb_label_update)

	CHECK_SET_HOOK(iokit_check_device)

	CHECK_SET_HOOK(ipq_label_associate)
	CHECK_SET_HOOK(ipq_label_compare)
	CHECK_SET_HOOK(ipq_label_destroy)
	CHECK_SET_HOOK(ipq_label_init)
	CHECK_SET_HOOK(ipq_label_update)

	CHECK_SET_HOOK(file_check_library_validation)

	CHECK_SET_HOOK(vnode_notify_setacl)
	CHECK_SET_HOOK(vnode_notify_setattrlist)
	CHECK_SET_HOOK(vnode_notify_setextattr)
	CHECK_SET_HOOK(vnode_notify_setflags)
	CHECK_SET_HOOK(vnode_notify_setmode)
	CHECK_SET_HOOK(vnode_notify_setowner)
	CHECK_SET_HOOK(vnode_notify_setutimes)
	CHECK_SET_HOOK(vnode_notify_truncate)

	CHECK_SET_HOOK(mbuf_label_associate_bpfdesc)
	CHECK_SET_HOOK(mbuf_label_associate_ifnet)
	CHECK_SET_HOOK(mbuf_label_associate_inpcb)
	CHECK_SET_HOOK(mbuf_label_associate_ipq)
	CHECK_SET_HOOK(mbuf_label_associate_linklayer)
	CHECK_SET_HOOK(mbuf_label_associate_multicast_encap)
	CHECK_SET_HOOK(mbuf_label_associate_netlayer)
	CHECK_SET_HOOK(mbuf_label_associate_socket)
	CHECK_SET_HOOK(mbuf_label_copy)
	CHECK_SET_HOOK(mbuf_label_destroy)
	CHECK_SET_HOOK(mbuf_label_init)

	CHECK_SET_HOOK(mount_check_fsctl)
	CHECK_SET_HOOK(mount_check_getattr)
	CHECK_SET_HOOK(mount_check_label_update)
	CHECK_SET_HOOK(mount_check_mount)
	CHECK_SET_HOOK(mount_check_remount)
	CHECK_SET_HOOK(mount_check_setattr)
	CHECK_SET_HOOK(mount_check_stat)
	CHECK_SET_HOOK(mount_check_umount)
	CHECK_SET_HOOK(mount_label_associate)
	CHECK_SET_HOOK(mount_label_destroy)
	CHECK_SET_HOOK(mount_label_externalize)
	CHECK_SET_HOOK(mount_label_init)
	CHECK_SET_HOOK(mount_label_internalize)

	CHECK_SET_HOOK(netinet_fragment)
	CHECK_SET_HOOK(netinet_icmp_reply)
	CHECK_SET_HOOK(netinet_tcp_reply)

	CHECK_SET_HOOK(pipe_check_ioctl)
	CHECK_SET_HOOK(pipe_check_kqfilter)
	CHECK_SET_HOOK(pipe_check_label_update)
	CHECK_SET_HOOK(pipe_check_read)
	CHECK_SET_HOOK(pipe_check_select)
	CHECK_SET_HOOK(pipe_check_stat)
	CHECK_SET_HOOK(pipe_check_write)
	CHECK_SET_HOOK(pipe_label_associate)
	CHECK_SET_HOOK(pipe_label_copy)
	CHECK_SET_HOOK(pipe_label_destroy)
	CHECK_SET_HOOK(pipe_label_externalize)
	CHECK_SET_HOOK(pipe_label_init)
	CHECK_SET_HOOK(pipe_label_internalize)
	CHECK_SET_HOOK(pipe_label_update)

	CHECK_SET_HOOK(policy_destroy)
	/* special hooks for policy init's */
	.mpo_policy_init = hook_policy_init,
	.mpo_policy_initbsd = hook_policy_initbsd,
	CHECK_SET_HOOK(policy_syscall)

	CHECK_SET_HOOK(system_check_sysctlbyname)
	CHECK_SET_HOOK(proc_check_inherit_ipc_ports)
	CHECK_SET_HOOK(vnode_check_rename)
	CHECK_SET_HOOK(kext_check_query)
	CHECK_SET_HOOK(proc_notify_exec_complete)
	.mpo_reserved4 = (mpo_reserved_hook_t *)common_hook,
	CHECK_SET_HOOK(proc_check_syscall_unix)
	CHECK_SET_HOOK(proc_check_expose_task)
	CHECK_SET_HOOK(proc_check_set_host_special_port)
	CHECK_SET_HOOK(proc_check_set_host_exception_port)
	CHECK_SET_HOOK(exc_action_check_exception_send)
	CHECK_SET_HOOK(exc_action_label_associate)
	CHECK_SET_HOOK(exc_action_label_populate)
	CHECK_SET_HOOK(exc_action_label_destroy)
	CHECK_SET_HOOK(exc_action_label_init)
	CHECK_SET_HOOK(exc_action_label_update)

	CHECK_SET_HOOK(vnode_check_trigger_resolve)
	CHECK_SET_HOOK(mount_check_mount_late)
	.mpo_reserved1 = (mpo_reserved_hook_t *)common_hook,
	.mpo_reserved2 = (mpo_reserved_hook_t *)common_hook,
	CHECK_SET_HOOK(skywalk_flow_check_connect)
	CHECK_SET_HOOK(skywalk_flow_check_listen)

	CHECK_SET_HOOK(posixsem_check_create)
	CHECK_SET_HOOK(posixsem_check_open)
	CHECK_SET_HOOK(posixsem_check_post)
	CHECK_SET_HOOK(posixsem_check_unlink)
	CHECK_SET_HOOK(posixsem_check_wait)
	CHECK_SET_HOOK(posixsem_label_associate)
	CHECK_SET_HOOK(posixsem_label_destroy)
	CHECK_SET_HOOK(posixsem_label_init)
	CHECK_SET_HOOK(posixshm_check_create)
	CHECK_SET_HOOK(posixshm_check_mmap)
	CHECK_SET_HOOK(posixshm_check_open)
	CHECK_SET_HOOK(posixshm_check_stat)
	CHECK_SET_HOOK(posixshm_check_truncate)
	CHECK_SET_HOOK(posixshm_check_unlink)
	CHECK_SET_HOOK(posixshm_label_associate)
	CHECK_SET_HOOK(posixshm_label_destroy)
	CHECK_SET_HOOK(posixshm_label_init)

	CHECK_SET_HOOK(proc_check_debug)
	CHECK_SET_HOOK(proc_check_fork)
	CHECK_SET_HOOK(proc_check_get_task_name)
	CHECK_SET_HOOK(proc_check_get_task)
	CHECK_SET_HOOK(proc_check_getaudit)
	CHECK_SET_HOOK(proc_check_getauid)
	CHECK_SET_HOOK(proc_check_getlcid)
	CHECK_SET_HOOK(proc_check_mprotect)
	CHECK_SET_HOOK(proc_check_sched)
	CHECK_SET_HOOK(proc_check_setaudit)
	CHECK_SET_HOOK(proc_check_setauid)
	CHECK_SET_HOOK(proc_check_setlcid)
	CHECK_SET_HOOK(proc_check_signal)
	CHECK_SET_HOOK(proc_check_wait)
	CHECK_SET_HOOK(proc_check_dump_core)

	.mpo_reserved5 = (mpo_reserved_hook_t *)common_hook,

	CHECK_SET_HOOK(socket_check_accept)
	CHECK_SET_HOOK(socket_check_accepted)
	CHECK_SET_HOOK(socket_check_bind)
	CHECK_SET_HOOK(socket_check_connect)
	CHECK_SET_HOOK(socket_check_create)
	CHECK_SET_HOOK(socket_check_deliver)
	CHECK_SET_HOOK(socket_check_kqfilter)
	CHECK_SET_HOOK(socket_check_label_update)
	CHECK_SET_HOOK(socket_check_listen)
	CHECK_SET_HOOK(socket_check_receive)
	CHECK_SET_HOOK(socket_check_received)
	CHECK_SET_HOOK(socket_check_select)
	CHECK_SET_HOOK(socket_check_send)
	CHECK_SET_HOOK(socket_check_stat)
	CHECK_SET_HOOK(socket_check_setsockopt)
	CHECK_SET_HOOK(socket_check_getsockopt)
	CHECK_SET_HOOK(socket_label_associate_accept)
	CHECK_SET_HOOK(socket_label_associate)
	CHECK_SET_HOOK(socket_label_copy)
	CHECK_SET_HOOK(socket_label_destroy)
	CHECK_SET_HOOK(socket_label_externalize)
	CHECK_SET_HOOK(socket_label_init)
	CHECK_SET_HOOK(socket_label_internalize)
	CHECK_SET_HOOK(socket_label_update)

	CHECK_SET_HOOK(socketpeer_label_associate_mbuf)
	CHECK_SET_HOOK(socketpeer_label_associate_socket)
	CHECK_SET_HOOK(socketpeer_label_destroy)
	CHECK_SET_HOOK(socketpeer_label_externalize)
	CHECK_SET_HOOK(socketpeer_label_init)

	CHECK_SET_HOOK(system_check_acct)
	CHECK_SET_HOOK(system_check_audit)
	CHECK_SET_HOOK(system_check_auditctl)
	CHECK_SET_HOOK(system_check_auditon)
	CHECK_SET_HOOK(system_check_host_priv)
	CHECK_SET_HOOK(system_check_nfsd)
	CHECK_SET_HOOK(system_check_reboot)
	CHECK_SET_HOOK(system_check_settime)
	CHECK_SET_HOOK(system_check_swapoff)
	CHECK_SET_HOOK(system_check_swapon)
	CHECK_SET_HOOK(socket_check_ioctl)

	CHECK_SET_HOOK(sysvmsg_label_associate)
	CHECK_SET_HOOK(sysvmsg_label_destroy)
	CHECK_SET_HOOK(sysvmsg_label_init)
	CHECK_SET_HOOK(sysvmsg_label_recycle)
	CHECK_SET_HOOK(sysvmsq_check_enqueue)
	CHECK_SET_HOOK(sysvmsq_check_msgrcv)
	CHECK_SET_HOOK(sysvmsq_check_msgrmid)
	CHECK_SET_HOOK(sysvmsq_check_msqctl)
	CHECK_SET_HOOK(sysvmsq_check_msqget)
	CHECK_SET_HOOK(sysvmsq_check_msqrcv)
	CHECK_SET_HOOK(sysvmsq_check_msqsnd)
	CHECK_SET_HOOK(sysvmsq_label_associate)
	CHECK_SET_HOOK(sysvmsq_label_destroy)
	CHECK_SET_HOOK(sysvmsq_label_init)
	CHECK_SET_HOOK(sysvmsq_label_recycle)
	CHECK_SET_HOOK(sysvsem_check_semctl)
	CHECK_SET_HOOK(sysvsem_check_semget)
	CHECK_SET_HOOK(sysvsem_check_semop)
	CHECK_SET_HOOK(sysvsem_label_associate)
	CHECK_SET_HOOK(sysvsem_label_destroy)
	CHECK_SET_HOOK(sysvsem_label_init)
	CHECK_SET_HOOK(sysvsem_label_recycle)
	CHECK_SET_HOOK(sysvshm_check_shmat)
	CHECK_SET_HOOK(sysvshm_check_shmctl)
	CHECK_SET_HOOK(sysvshm_check_shmdt)
	CHECK_SET_HOOK(sysvshm_check_shmget)
	CHECK_SET_HOOK(sysvshm_label_associate)
	CHECK_SET_HOOK(sysvshm_label_destroy)
	CHECK_SET_HOOK(sysvshm_label_init)
	CHECK_SET_HOOK(sysvshm_label_recycle)

	CHECK_SET_HOOK(proc_notify_exit)
	CHECK_SET_HOOK(mount_check_snapshot_revert)
	CHECK_SET_HOOK(vnode_check_getattr)
	CHECK_SET_HOOK(mount_check_snapshot_create)
	CHECK_SET_HOOK(mount_check_snapshot_delete)
	CHECK_SET_HOOK(vnode_check_clone)
	CHECK_SET_HOOK(proc_check_get_cs_info)
	CHECK_SET_HOOK(proc_check_set_cs_info)

	CHECK_SET_HOOK(iokit_check_hid_control)

	CHECK_SET_HOOK(vnode_check_access)
	CHECK_SET_HOOK(vnode_check_chdir)
	CHECK_SET_HOOK(vnode_check_chroot)
	CHECK_SET_HOOK(vnode_check_create)
	CHECK_SET_HOOK(vnode_check_deleteextattr)
	CHECK_SET_HOOK(vnode_check_exchangedata)
	CHECK_SET_HOOK(vnode_check_exec)
	CHECK_SET_HOOK(vnode_check_getattrlist)
	CHECK_SET_HOOK(vnode_check_getextattr)
	CHECK_SET_HOOK(vnode_check_ioctl)
	CHECK_SET_HOOK(vnode_check_kqfilter)
	CHECK_SET_HOOK(vnode_check_label_update)
	CHECK_SET_HOOK(vnode_check_link)
	CHECK_SET_HOOK(vnode_check_listextattr)
	CHECK_SET_HOOK(vnode_check_lookup)
	CHECK_SET_HOOK(vnode_check_open)
	CHECK_SET_HOOK(vnode_check_read)
	CHECK_SET_HOOK(vnode_check_readdir)
	CHECK_SET_HOOK(vnode_check_readlink)
	CHECK_SET_HOOK(vnode_check_rename_from)
	CHECK_SET_HOOK(vnode_check_rename_to)
	CHECK_SET_HOOK(vnode_check_revoke)
	CHECK_SET_HOOK(vnode_check_select)
	CHECK_SET_HOOK(vnode_check_setattrlist)
	CHECK_SET_HOOK(vnode_check_setextattr)
	CHECK_SET_HOOK(vnode_check_setflags)
	CHECK_SET_HOOK(vnode_check_setmode)
	CHECK_SET_HOOK(vnode_check_setowner)
	CHECK_SET_HOOK(vnode_check_setutimes)
	CHECK_SET_HOOK(vnode_check_stat)
	CHECK_SET_HOOK(vnode_check_truncate)
	CHECK_SET_HOOK(vnode_check_unlink)
	CHECK_SET_HOOK(vnode_check_write)
	CHECK_SET_HOOK(vnode_label_associate_devfs)
	CHECK_SET_HOOK(vnode_label_associate_extattr)
	CHECK_SET_HOOK(vnode_label_associate_file)
	CHECK_SET_HOOK(vnode_label_associate_pipe)
	CHECK_SET_HOOK(vnode_label_associate_posixsem)
	CHECK_SET_HOOK(vnode_label_associate_posixshm)
	CHECK_SET_HOOK(vnode_label_associate_singlelabel)
	CHECK_SET_HOOK(vnode_label_associate_socket)
	CHECK_SET_HOOK(vnode_label_copy)
	CHECK_SET_HOOK(vnode_label_destroy)
	CHECK_SET_HOOK(vnode_label_externalize_audit)
	CHECK_SET_HOOK(vnode_label_externalize)
	CHECK_SET_HOOK(vnode_label_init)
	CHECK_SET_HOOK(vnode_label_internalize)
	CHECK_SET_HOOK(vnode_label_recycle)
	CHECK_SET_HOOK(vnode_label_store)
	CHECK_SET_HOOK(vnode_label_update_extattr)
	CHECK_SET_HOOK(vnode_label_update)
	CHECK_SET_HOOK(vnode_notify_create)
	CHECK_SET_HOOK(vnode_check_signature)
	CHECK_SET_HOOK(vnode_check_uipc_bind)
	CHECK_SET_HOOK(vnode_check_uipc_connect)

	CHECK_SET_HOOK(proc_check_run_cs_invalid)
	CHECK_SET_HOOK(proc_check_suspend_resume)

	CHECK_SET_HOOK(thread_userret)

	CHECK_SET_HOOK(iokit_check_set_properties)

	.mpo_reserved3 = (mpo_reserved_hook_t *)common_hook,

	CHECK_SET_HOOK(vnode_check_searchfs)

	CHECK_SET_HOOK(priv_check)
	CHECK_SET_HOOK(priv_grant)

	CHECK_SET_HOOK(proc_check_map_anon)

	CHECK_SET_HOOK(vnode_check_fsgetpath)

	CHECK_SET_HOOK(iokit_check_open)

	CHECK_SET_HOOK(proc_check_ledger)

	CHECK_SET_HOOK(vnode_notify_rename)

	CHECK_SET_HOOK(vnode_check_setacl)

	CHECK_SET_HOOK(vnode_notify_deleteextattr)

	CHECK_SET_HOOK(system_check_kas_info)

	CHECK_SET_HOOK(vnode_check_lookup_preflight)

	CHECK_SET_HOOK(vnode_notify_open)

	CHECK_SET_HOOK(system_check_info)

	CHECK_SET_HOOK(pty_notify_grant)
	CHECK_SET_HOOK(pty_notify_close)

	CHECK_SET_HOOK(vnode_find_sigs)


	CHECK_SET_HOOK(kext_check_load)
	CHECK_SET_HOOK(kext_check_unload)

	CHECK_SET_HOOK(proc_check_proc_info)

	CHECK_SET_HOOK(vnode_notify_link)

	CHECK_SET_HOOK(iokit_check_filter_properties)
	CHECK_SET_HOOK(iokit_check_get_property)
};

/*
 * Policy definition
 */
static SECURITY_READ_ONLY_LATE(struct mac_policy_conf) policy_conf = {
	.mpc_name               = "CHECK",
	.mpc_fullname           = "Check Assumptions Policy",
	.mpc_field_off          = NULL,         /* no label slot */
	.mpc_labelnames         = NULL,         /* no policy label names */
	.mpc_labelname_count    = 0,            /* count of label names is 0 */
	.mpc_ops                = &policy_ops,  /* policy operations */
	.mpc_loadtime_flags     = 0,
	.mpc_runtime_flags      = 0,
};

static SECURITY_READ_ONLY_LATE(mac_policy_handle_t) policy_handle;

/*
 * Init routine; for a loadable policy, this would be called during the KEXT
 * initialization; we're going to call this from bsd_init() if the boot
 * argument for checking is present.
 */
errno_t
check_policy_init(int flags)
{
	/* Only instantiate the module if we have been asked to do checking */
	if (!flags) {
		return 0;
	}

	policy_flags = flags;

	return mac_policy_register(&policy_conf, &policy_handle, NULL);
}
