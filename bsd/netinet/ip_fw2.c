/*
 * Copyright (c) 2004-2016 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * Copyright (c) 2002 Luigi Rizzo, Universita` di Pisa
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/netinet/ip_fw2.c,v 1.6.2.18 2003/10/17 11:01:03 scottl Exp $
 */

#define        DEB(x)
#define        DDB(x) x

/*
 * Implement IP packet firewall (new version)
 */

#ifndef INET
#error IPFIREWALL requires INET.
#endif /* INET */

#if IPFW2

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/mcache.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/socket.h>
#include <sys/socketvar.h>
#include <sys/sysctl.h>
#include <sys/syslog.h>
#include <sys/ucred.h>
#include <sys/kern_event.h>
#include <sys/kauth.h>

#include <net/if.h>
#include <net/net_kev.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/in_pcb.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip_fw.h>
#include <netinet/ip_divert.h>

#if DUMMYNET
#include <netinet/ip_dummynet.h>
#endif /* DUMMYNET */

#include <netinet/tcp.h>
#include <netinet/tcp_timer.h>
#include <netinet/tcp_var.h>
#include <netinet/tcpip.h>
#include <netinet/udp.h>
#include <netinet/udp_var.h>

#ifdef IPSEC
#include <netinet6/ipsec.h>
#endif

#include <netinet/if_ether.h> /* XXX for ETHERTYPE_IP */

#include "ip_fw2_compat.h"

#include <sys/kern_event.h>
#include <stdarg.h>

/*
#include <machine/in_cksum.h>
*/	/* XXX for in_cksum */

/*
 * XXX This one should go in sys/mbuf.h. It is used to avoid that
 * a firewall-generated packet loops forever through the firewall.
 */
#ifndef	M_SKIP_FIREWALL
#define M_SKIP_FIREWALL         0x4000
#endif

/*
 * set_disable contains one bit per set value (0..31).
 * If the bit is set, all rules with the corresponding set
 * are disabled. Set RESVD_SET(31) is reserved for the default rule
 * and rules that are not deleted by the flush command,
 * and CANNOT be disabled.
 * Rules in set RESVD_SET can only be deleted explicitly.
 */
static u_int32_t set_disable;

int fw_verbose;
static int verbose_limit;
extern int fw_bypass;

#define IPFW_RULE_INACTIVE 1

/*
 * list of rules for layer 3
 */
static struct ip_fw *layer3_chain;

MALLOC_DEFINE(M_IPFW, "IpFw/IpAcct", "IpFw/IpAcct chain's");

static int fw_debug = 0;
static int autoinc_step = 100; /* bounded to 1..1000 in add_rule() */

static void ipfw_kev_post_msg(u_int32_t );

static int Get32static_len(void);
static int Get64static_len(void);

#ifdef SYSCTL_NODE

static int ipfw_sysctl SYSCTL_HANDLER_ARGS;

SYSCTL_NODE(_net_inet_ip, OID_AUTO, fw, CTLFLAG_RW|CTLFLAG_LOCKED, 0, "Firewall");
SYSCTL_PROC(_net_inet_ip_fw, OID_AUTO, enable,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    &fw_enable, 0, ipfw_sysctl, "I", "Enable ipfw");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, autoinc_step, CTLFLAG_RW | CTLFLAG_LOCKED,
    &autoinc_step, 0, "Rule number autincrement step");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, one_pass,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &fw_one_pass, 0,
    "Only do a single pass through ipfw when using dummynet(4)");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, debug,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &fw_debug, 0, "Enable printing of debug ip_fw statements");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, verbose,
    CTLFLAG_RW | CTLFLAG_LOCKED,
    &fw_verbose, 0, "Log matches to ipfw rules");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, verbose_limit, CTLFLAG_RW | CTLFLAG_LOCKED,
    &verbose_limit, 0, "Set upper limit of matches of ipfw rules logged");

/*
 * IP FW Stealth Logging:
 */
typedef enum ipfw_stealth_stats_type {
  IPFW_STEALTH_STATS_UDP,
  IPFW_STEALTH_STATS_TCP,
  IPFW_STEALTH_STATS_UDPv6,
  IPFW_STEALTH_STATS_TCPv6,
  IPFW_STEALTH_STATS_MAX,
} ipfw_stealth_stats_type_t;

#define IPFW_STEALTH_TIMEOUT_SEC 30

#define	DYN_KEEPALIVE_LEEWAY	15

// Piggybagging Stealth stats with ipfw_tick().
#define IPFW_STEALTH_TIMEOUT_FREQUENCY (30 / dyn_keepalive_period)

static const char* ipfw_stealth_stats_str [IPFW_STEALTH_STATS_MAX] = {
  "UDP", "TCP", "UDP v6", "TCP v6",
};

static uint32_t ipfw_stealth_stats_needs_flush = FALSE;
static uint32_t ipfw_stealth_stats[IPFW_STEALTH_STATS_MAX];

static void ipfw_stealth_flush_stats(void);
void ipfw_stealth_stats_incr_udp(void);
void ipfw_stealth_stats_incr_tcp(void);
void ipfw_stealth_stats_incr_udpv6(void);
void ipfw_stealth_stats_incr_tcpv6(void);

/*
 * Description of dynamic rules.
 *
 * Dynamic rules are stored in lists accessed through a hash table
 * (ipfw_dyn_v) whose size is curr_dyn_buckets. This value can
 * be modified through the sysctl variable dyn_buckets which is
 * updated when the table becomes empty.
 *
 * XXX currently there is only one list, ipfw_dyn.
 *
 * When a packet is received, its address fields are first masked
 * with the mask defined for the rule, then hashed, then matched
 * against the entries in the corresponding list.
 * Dynamic rules can be used for different purposes:
 *  + stateful rules;
 *  + enforcing limits on the number of sessions;
 *  + in-kernel NAT (not implemented yet)
 *
 * The lifetime of dynamic rules is regulated by dyn_*_lifetime,
 * measured in seconds and depending on the flags.
 *
 * The total number of dynamic rules is stored in dyn_count.
 * The max number of dynamic rules is dyn_max. When we reach
 * the maximum number of rules we do not create anymore. This is
 * done to avoid consuming too much memory, but also too much
 * time when searching on each packet (ideally, we should try instead
 * to put a limit on the length of the list on each bucket...).
 *
 * Each dynamic rule holds a pointer to the parent ipfw rule so
 * we know what action to perform. Dynamic rules are removed when
 * the parent rule is deleted. XXX we should make them survive.
 *
 * There are some limitations with dynamic rules -- we do not
 * obey the 'randomized match', and we do not do multiple
 * passes through the firewall. XXX check the latter!!!
 */
static ipfw_dyn_rule **ipfw_dyn_v = NULL;
static u_int32_t dyn_buckets = 256; /* must be power of 2 */
static u_int32_t curr_dyn_buckets = 256; /* must be power of 2 */

/*
 * Timeouts for various events in handing dynamic rules.
 */
static u_int32_t dyn_ack_lifetime = 300;
static u_int32_t dyn_syn_lifetime = 20;
static u_int32_t dyn_fin_lifetime = 1;
static u_int32_t dyn_rst_lifetime = 1;
static u_int32_t dyn_udp_lifetime = 10;
static u_int32_t dyn_short_lifetime = 5;

/*
 * Keepalives are sent if dyn_keepalive is set. They are sent every
 * dyn_keepalive_period seconds, in the last dyn_keepalive_interval
 * seconds of lifetime of a rule.
 * dyn_rst_lifetime and dyn_fin_lifetime should be strictly lower
 * than dyn_keepalive_period.
 */

static u_int32_t dyn_keepalive_interval = 25;
static u_int32_t dyn_keepalive_period = 5;
static u_int32_t dyn_keepalive = 1;	/* do send keepalives */

static u_int32_t static_count;	/* # of static rules */
static u_int32_t static_len;	/* size in bytes of static rules */
static u_int32_t static_len_32;	/* size in bytes of static rules for 32 bit client */
static u_int32_t static_len_64;	/* size in bytes of static rules for 64 bit client */
static u_int32_t dyn_count;		/* # of dynamic rules */
static u_int32_t dyn_max = 4096;	/* max # of dynamic rules */

SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_buckets, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_buckets, 0, "Number of dyn. buckets");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, curr_dyn_buckets, CTLFLAG_RD | CTLFLAG_LOCKED,
    &curr_dyn_buckets, 0, "Current Number of dyn. buckets");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_count, CTLFLAG_RD | CTLFLAG_LOCKED,
    &dyn_count, 0, "Number of dyn. rules");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_max, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_max, 0, "Max number of dyn. rules");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, static_count, CTLFLAG_RD | CTLFLAG_LOCKED,
    &static_count, 0, "Number of static rules");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_ack_lifetime, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_ack_lifetime, 0, "Lifetime of dyn. rules for acks");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_syn_lifetime, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_syn_lifetime, 0, "Lifetime of dyn. rules for syn");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_fin_lifetime, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_fin_lifetime, 0, "Lifetime of dyn. rules for fin");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_rst_lifetime, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_rst_lifetime, 0, "Lifetime of dyn. rules for rst");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_udp_lifetime, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_udp_lifetime, 0, "Lifetime of dyn. rules for UDP");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_short_lifetime, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_short_lifetime, 0, "Lifetime of dyn. rules for other situations");
SYSCTL_INT(_net_inet_ip_fw, OID_AUTO, dyn_keepalive, CTLFLAG_RW | CTLFLAG_LOCKED,
    &dyn_keepalive, 0, "Enable keepalives for dyn. rules");


static int
ipfw_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(arg1, arg2)
	int error;
	
	error = sysctl_handle_int(oidp, oidp->oid_arg1, oidp->oid_arg2, req);
	if (error || !req->newptr)
		return (error);
	
	ipfw_kev_post_msg(KEV_IPFW_ENABLE);
	
	return error;
}

#endif /* SYSCTL_NODE */


static ip_fw_chk_t	ipfw_chk;

/* firewall lock */
lck_grp_t         *ipfw_mutex_grp;
lck_grp_attr_t    *ipfw_mutex_grp_attr;
lck_attr_t        *ipfw_mutex_attr;
decl_lck_mtx_data(,ipfw_mutex_data);
lck_mtx_t         *ipfw_mutex = &ipfw_mutex_data;

extern  void    ipfwsyslog( int level, const char *format,...);

#define         ipfwstring      "ipfw:"
static          size_t		ipfwstringlen;

#define dolog( a ) {		\
	if ( fw_verbose == 2 )  	/* Apple logging, log to ipfw.log */ \
		ipfwsyslog a ; 	\
	else log a ;		\
}

#define RULESIZE64(rule)  (sizeof(struct ip_fw_64) + \
							((struct ip_fw *)(rule))->cmd_len * 4 - 4)

#define RULESIZE32(rule)  (sizeof(struct ip_fw_32) + \
							((struct ip_fw *)(rule))->cmd_len * 4 - 4)

void    ipfwsyslog( int level, const char *format,...)
{
#define		msgsize		100

    struct kev_msg        ev_msg;
    va_list             ap;
    char                msgBuf[msgsize];
    char                *dptr = msgBuf;
    unsigned char       pri;
    int			loglen;

	bzero(msgBuf, msgsize);
	bzero(&ev_msg, sizeof(struct kev_msg));
	va_start( ap, format );
        loglen = vsnprintf(msgBuf, msgsize, format, ap);
        va_end( ap );

        ev_msg.vendor_code    = KEV_VENDOR_APPLE;
        ev_msg.kev_class      = KEV_NETWORK_CLASS;
        ev_msg.kev_subclass   = KEV_LOG_SUBCLASS;
        ev_msg.event_code         = IPFWLOGEVENT;

	/* get rid of the trailing \n */
	if (loglen < msgsize)
		dptr[loglen-1] = 0;
	else
		dptr[msgsize-1] = 0;

        pri = LOG_PRI(level);

        /* remove "ipfw:" prefix if logging to ipfw log */
        if ( !(strncmp( ipfwstring, msgBuf, ipfwstringlen))){
                dptr = msgBuf+ipfwstringlen;
        }
	
        ev_msg.dv[0].data_ptr = &pri;
        ev_msg.dv[0].data_length = 1;
        ev_msg.dv[1].data_ptr    = dptr;
        ev_msg.dv[1].data_length = 100; /* bug in kern_post_msg, it can't handle size > 256-msghdr */
        ev_msg.dv[2].data_length = 0;

        kev_post_msg(&ev_msg);
}

static inline void ipfw_stealth_stats_incr(uint32_t type)
{
    if (type >= IPFW_STEALTH_STATS_MAX)
        return;

    ipfw_stealth_stats[type]++;

    if (!ipfw_stealth_stats_needs_flush) {
        ipfw_stealth_stats_needs_flush = TRUE;
    }
}

void ipfw_stealth_stats_incr_udp(void)
{
    ipfw_stealth_stats_incr(IPFW_STEALTH_STATS_UDP);
}

void ipfw_stealth_stats_incr_tcp(void)
{
    ipfw_stealth_stats_incr(IPFW_STEALTH_STATS_TCP);
}

void ipfw_stealth_stats_incr_udpv6(void)
{
    ipfw_stealth_stats_incr(IPFW_STEALTH_STATS_UDPv6);
}

void ipfw_stealth_stats_incr_tcpv6(void)
{
    ipfw_stealth_stats_incr(IPFW_STEALTH_STATS_TCPv6);
}

static void ipfw_stealth_flush_stats(void)
{
    int i;

    for (i = 0; i < IPFW_STEALTH_STATS_MAX; i++) {
        if (ipfw_stealth_stats[i]) {
           ipfwsyslog (LOG_INFO, "Stealth Mode connection attempt to %s %d times",
                       ipfw_stealth_stats_str[i], ipfw_stealth_stats[i]);
           ipfw_stealth_stats[i] = 0;
       }
    }
    ipfw_stealth_stats_needs_flush = FALSE;
}

/*
 * This macro maps an ip pointer into a layer3 header pointer of type T
 */
#define	L3HDR(T, ip) ((T *)((u_int32_t *)(ip) + (ip)->ip_hl))

static __inline int
icmptype_match(struct ip *ip, ipfw_insn_u32 *cmd)
{
	int type = L3HDR(struct icmp,ip)->icmp_type;

	return (type <= ICMP_MAXTYPE && (cmd->d[0] & (1<<type)) );
}

#define TT	( (1 << ICMP_ECHO) | (1 << ICMP_ROUTERSOLICIT) | \
    (1 << ICMP_TSTAMP) | (1 << ICMP_IREQ) | (1 << ICMP_MASKREQ) )

static int
is_icmp_query(struct ip *ip)
{
	int type = L3HDR(struct icmp, ip)->icmp_type;
	return (type <= ICMP_MAXTYPE && (TT & (1<<type)) );
}
#undef TT

static int
Get32static_len(void)
{
	int	diff;
	int len = static_len_32;
	struct ip_fw *rule;
	char		 *useraction;

	for (rule = layer3_chain; rule ; rule = rule->next) {
		if (rule->reserved_1 == IPFW_RULE_INACTIVE) {
			continue;
		}
		if ( rule->act_ofs ){
			useraction =  (char*)ACTION_PTR( rule ); 
			if ( ((ipfw_insn*)useraction)->opcode == O_QUEUE || ((ipfw_insn*)useraction)->opcode == O_PIPE){
				diff = sizeof(ipfw_insn_pipe) - sizeof(ipfw_insn_pipe_32);
				if (diff)
					len -= diff;
			}
		}
	}
	return len;
}

static int
Get64static_len(void)
{
	int	diff;
	int len = static_len_64;
	struct ip_fw *rule;
	char		 *useraction;

	for (rule = layer3_chain; rule ; rule = rule->next) {
		if (rule->reserved_1 == IPFW_RULE_INACTIVE) {
			continue;
		}
		if ( rule->act_ofs ){
			useraction =  (char *)ACTION_PTR( rule ); 
			if ( ((ipfw_insn*)useraction)->opcode == O_QUEUE || ((ipfw_insn*)useraction)->opcode == O_PIPE){
				diff = sizeof(ipfw_insn_pipe_64) - sizeof(ipfw_insn_pipe);
				if (diff)
					len += diff;
			}
		}
	}
	return len;
}

static void 
copyto32fw_insn( struct ip_fw_32 *fw32 , struct ip_fw *user_ip_fw, int cmdsize)
{
	char		*end;
	char		*fw32action;
	char		*useraction;
	int			justcmdsize;
	int			diff=0;
	int			actioncopysize;

	end = ((char*)user_ip_fw->cmd) + cmdsize;
	useraction = (char*)ACTION_PTR( user_ip_fw );
	fw32action = (char*)fw32->cmd + (user_ip_fw->act_ofs * sizeof(uint32_t));
	if ( ( justcmdsize = ( fw32action - (char*)fw32->cmd)))
		bcopy( user_ip_fw->cmd, fw32->cmd, justcmdsize); 
	while ( useraction < end ){
		if ( ((ipfw_insn*)useraction)->opcode == O_QUEUE || ((ipfw_insn*)useraction)->opcode == O_PIPE){
			actioncopysize = sizeof(ipfw_insn_pipe_32);
			((ipfw_insn*)fw32action)->opcode = ((ipfw_insn*)useraction)->opcode;
			((ipfw_insn*)fw32action)->arg1 = ((ipfw_insn*)useraction)->arg1;
			((ipfw_insn*)fw32action)->len = F_INSN_SIZE(ipfw_insn_pipe_32);
			diff = ((ipfw_insn*)useraction)->len - ((ipfw_insn*)fw32action)->len;
			if ( diff ){
				fw32->cmd_len -= diff;
			}
		} else{
			actioncopysize =  (F_LEN((ipfw_insn*)useraction) ? (F_LEN((ipfw_insn*)useraction)) : 1 ) * sizeof(uint32_t);
			bcopy( useraction, fw32action, actioncopysize );
		}
		useraction += (F_LEN((ipfw_insn*)useraction) ? (F_LEN((ipfw_insn*)useraction)) : 1 ) * sizeof(uint32_t);
		fw32action += actioncopysize;
	}
}

static void
copyto64fw_insn( struct ip_fw_64 *fw64 , struct ip_fw *user_ip_fw, int cmdsize)
{
	char		*end;
	char		*fw64action;
	char		*useraction;
	int			justcmdsize;
	int			diff;
	int			actioncopysize;

	end = ((char *)user_ip_fw->cmd) + cmdsize;
	useraction = (char*)ACTION_PTR( user_ip_fw );
	if ( (justcmdsize = (useraction - (char*)user_ip_fw->cmd)))
		bcopy( user_ip_fw->cmd, fw64->cmd, justcmdsize); 
	fw64action = (char*)fw64->cmd + justcmdsize;
	while ( useraction < end ){
		if ( ((ipfw_insn*)user_ip_fw)->opcode == O_QUEUE || ((ipfw_insn*)user_ip_fw)->opcode == O_PIPE){
			actioncopysize = sizeof(ipfw_insn_pipe_64);
			((ipfw_insn*)fw64action)->opcode = ((ipfw_insn*)useraction)->opcode;
			((ipfw_insn*)fw64action)->arg1 = ((ipfw_insn*)useraction)->arg1;
			((ipfw_insn*)fw64action)->len = F_INSN_SIZE(ipfw_insn_pipe_64);
			diff = ((ipfw_insn*)fw64action)->len - ((ipfw_insn*)useraction)->len;
			if (diff)
				fw64->cmd_len += diff;
			
		} else{
			actioncopysize = (F_LEN((ipfw_insn*)useraction) ? (F_LEN((ipfw_insn*)useraction)) : 1 ) * sizeof(uint32_t);
			bcopy( useraction, fw64action, actioncopysize );
		}
		useraction += (F_LEN((ipfw_insn*)useraction) ? (F_LEN((ipfw_insn*)useraction)) : 1 ) * sizeof(uint32_t);
		fw64action += actioncopysize;
	}
}

static void 
copyto32fw( struct ip_fw *user_ip_fw, struct ip_fw_32 *fw32 , __unused size_t copysize)
{
	size_t	rulesize, cmdsize;
	
	fw32->version = user_ip_fw->version;
	fw32->context = CAST_DOWN_EXPLICIT( user32_addr_t, user_ip_fw->context);
	fw32->next = CAST_DOWN_EXPLICIT(user32_addr_t, user_ip_fw->next);
	fw32->next_rule = CAST_DOWN_EXPLICIT(user32_addr_t, user_ip_fw->next_rule);
	fw32->act_ofs = user_ip_fw->act_ofs;
	fw32->cmd_len = user_ip_fw->cmd_len;
	fw32->rulenum = user_ip_fw->rulenum;
	fw32->set = user_ip_fw->set;
	fw32->set_masks[0] = user_ip_fw->set_masks[0];
	fw32->set_masks[1] = user_ip_fw->set_masks[1];
	fw32->pcnt = user_ip_fw->pcnt;
	fw32->bcnt = user_ip_fw->bcnt;
	fw32->timestamp = user_ip_fw->timestamp;
	fw32->reserved_1 = user_ip_fw->reserved_1;
	fw32->reserved_2 = user_ip_fw->reserved_2;
	rulesize = sizeof(struct ip_fw_32) + (user_ip_fw->cmd_len * sizeof(ipfw_insn) - 4);
	cmdsize = user_ip_fw->cmd_len * sizeof(u_int32_t);
	copyto32fw_insn( fw32, user_ip_fw, cmdsize );
}

static void
copyto64fw( struct ip_fw *user_ip_fw, struct ip_fw_64	*fw64, size_t copysize)
{
	size_t	rulesize, cmdsize;

	fw64->version = user_ip_fw->version;
	fw64->context = CAST_DOWN_EXPLICIT(__uint64_t, user_ip_fw->context);
	fw64->next = CAST_DOWN_EXPLICIT(user64_addr_t, user_ip_fw->next);
	fw64->next_rule = CAST_DOWN_EXPLICIT(user64_addr_t, user_ip_fw->next_rule);
	fw64->act_ofs = user_ip_fw->act_ofs;
	fw64->cmd_len = user_ip_fw->cmd_len;
	fw64->rulenum = user_ip_fw->rulenum;
	fw64->set = user_ip_fw->set;
	fw64->set_masks[0] = user_ip_fw->set_masks[0];
	fw64->set_masks[1] = user_ip_fw->set_masks[1];
	fw64->pcnt = user_ip_fw->pcnt;
	fw64->bcnt = user_ip_fw->bcnt;
	fw64->timestamp = user_ip_fw->timestamp;
	fw64->reserved_1 = user_ip_fw->reserved_1;
	fw64->reserved_2 = user_ip_fw->reserved_2;
	rulesize = sizeof(struct ip_fw_64) + (user_ip_fw->cmd_len * sizeof(ipfw_insn) - 4);
	if (rulesize > copysize)
		cmdsize = copysize - sizeof(struct ip_fw_64) + 4;
	else
		cmdsize = user_ip_fw->cmd_len * sizeof(u_int32_t);
	copyto64fw_insn( fw64, user_ip_fw, cmdsize);
}

static int
copyfrom32fw_insn( struct ip_fw_32 *fw32 , struct ip_fw *user_ip_fw, int cmdsize)
{
	char		*end;
	char		*fw32action;
	char		*useraction;
	int			justcmdsize;
	int			diff;
	int			actioncopysize;

	end = ((char*)fw32->cmd) + cmdsize;
	fw32action = (char*)ACTION_PTR( fw32 );
	if ((justcmdsize = (fw32action - (char*)fw32->cmd)))
		bcopy( fw32->cmd, user_ip_fw->cmd, justcmdsize); 
	useraction = (char*)user_ip_fw->cmd + justcmdsize;
	while ( fw32action < end ){
		if ( ((ipfw_insn*)fw32action)->opcode == O_QUEUE || ((ipfw_insn*)fw32action)->opcode == O_PIPE){
			actioncopysize = sizeof(ipfw_insn_pipe);
			((ipfw_insn*)useraction)->opcode = ((ipfw_insn*)fw32action)->opcode;
			((ipfw_insn*)useraction)->arg1 = ((ipfw_insn*)fw32action)->arg1;
			((ipfw_insn*)useraction)->len = F_INSN_SIZE(ipfw_insn_pipe);
			diff = ((ipfw_insn*)useraction)->len - ((ipfw_insn*)fw32action)->len;
			if (diff){
				/* readjust the cmd_len */
				user_ip_fw->cmd_len += diff;
			}
		} else{
			actioncopysize = (F_LEN((ipfw_insn*)fw32action) ? (F_LEN((ipfw_insn*)fw32action)) : 1 ) * sizeof(uint32_t);
			bcopy( fw32action, useraction, actioncopysize );
		}
		fw32action += (F_LEN((ipfw_insn*)fw32action) ? (F_LEN((ipfw_insn*)fw32action)) : 1 ) * sizeof(uint32_t);
		useraction += actioncopysize;
	}

	return( useraction - (char*)user_ip_fw->cmd );
}

static int
copyfrom64fw_insn( struct ip_fw_64 *fw64 , struct ip_fw *user_ip_fw, int cmdsize)
{
	char		*end;
	char		*fw64action;
	char		*useraction;
	int			justcmdsize;
	int			diff;
	int			actioncopysize;

	end = ((char *)fw64->cmd) + cmdsize ;
	fw64action = (char*)ACTION_PTR( fw64 );
	if ( (justcmdsize = (fw64action - (char*)fw64->cmd)))
		bcopy( fw64->cmd, user_ip_fw->cmd, justcmdsize); 
	useraction = (char*)user_ip_fw->cmd + justcmdsize;
	while ( fw64action < end ){
		if ( ((ipfw_insn*)fw64action)->opcode == O_QUEUE || ((ipfw_insn*)fw64action)->opcode == O_PIPE){
			actioncopysize = sizeof(ipfw_insn_pipe);
			((ipfw_insn*)useraction)->opcode = ((ipfw_insn*)fw64action)->opcode;
			((ipfw_insn*)useraction)->arg1 = ((ipfw_insn*)fw64action)->arg1;
			((ipfw_insn*)useraction)->len = F_INSN_SIZE(ipfw_insn_pipe);
			diff = ((ipfw_insn*)fw64action)->len - ((ipfw_insn*)useraction)->len; 
			if (diff) {
				/* readjust the cmd_len */
				user_ip_fw->cmd_len -= diff;
			}
		} else{
			actioncopysize = (F_LEN((ipfw_insn*)fw64action) ? (F_LEN((ipfw_insn*)fw64action)) : 1 ) * sizeof(uint32_t);
			bcopy( fw64action, useraction, actioncopysize );
		}
		fw64action += (F_LEN((ipfw_insn*)fw64action) ? (F_LEN((ipfw_insn*)fw64action)) : 1 ) * sizeof(uint32_t); 
		useraction += actioncopysize;
	}
	return( useraction - (char*)user_ip_fw->cmd );
}

static size_t 
copyfrom32fw( struct ip_fw_32	*fw32, struct ip_fw *user_ip_fw, size_t copysize)
{
	size_t rulesize, cmdsize;
	 
	user_ip_fw->version = fw32->version;
	user_ip_fw->context = CAST_DOWN(void *, fw32->context);
	user_ip_fw->next = CAST_DOWN(struct ip_fw*, fw32->next);
	user_ip_fw->next_rule = CAST_DOWN_EXPLICIT(struct ip_fw*, fw32->next_rule);
	user_ip_fw->act_ofs = fw32->act_ofs;
	user_ip_fw->cmd_len = fw32->cmd_len;
	user_ip_fw->rulenum = fw32->rulenum;
	user_ip_fw->set = fw32->set;
	user_ip_fw->set_masks[0] = fw32->set_masks[0];
	user_ip_fw->set_masks[1] = fw32->set_masks[1];
	user_ip_fw->pcnt = fw32->pcnt;
	user_ip_fw->bcnt = fw32->bcnt;
	user_ip_fw->timestamp = fw32->timestamp;
	user_ip_fw->reserved_1 = fw32->reserved_1;
	user_ip_fw->reserved_2 = fw32->reserved_2;
	rulesize = sizeof(struct ip_fw_32) + (fw32->cmd_len * sizeof(ipfw_insn) - 4);
	if ( rulesize > copysize )
		cmdsize = copysize - sizeof(struct ip_fw_32)-4;
	else
		cmdsize = fw32->cmd_len * sizeof(ipfw_insn);
	cmdsize = copyfrom32fw_insn( fw32, user_ip_fw, cmdsize);
	return( sizeof(struct ip_fw) + cmdsize - 4);
}

static size_t 
copyfrom64fw( struct ip_fw_64 *fw64, struct ip_fw *user_ip_fw, size_t copysize)
{
	size_t rulesize, cmdsize;
	
	user_ip_fw->version = fw64->version;
	user_ip_fw->context = CAST_DOWN_EXPLICIT( void *, fw64->context);
	user_ip_fw->next = CAST_DOWN_EXPLICIT(struct ip_fw*, fw64->next);
	user_ip_fw->next_rule = CAST_DOWN_EXPLICIT(struct ip_fw*, fw64->next_rule);
	user_ip_fw->act_ofs = fw64->act_ofs;
	user_ip_fw->cmd_len = fw64->cmd_len;
	user_ip_fw->rulenum = fw64->rulenum;
	user_ip_fw->set = fw64->set;
	user_ip_fw->set_masks[0] = fw64->set_masks[0];
	user_ip_fw->set_masks[1] = fw64->set_masks[1];
	user_ip_fw->pcnt = fw64->pcnt;
	user_ip_fw->bcnt = fw64->bcnt;
	user_ip_fw->timestamp = fw64->timestamp;
	user_ip_fw->reserved_1 = fw64->reserved_1;
	user_ip_fw->reserved_2 = fw64->reserved_2;
	//bcopy( fw64->cmd, user_ip_fw->cmd, fw64->cmd_len * sizeof(ipfw_insn));
	rulesize = sizeof(struct ip_fw_64) + (fw64->cmd_len * sizeof(ipfw_insn) - 4);
	if ( rulesize > copysize )
		cmdsize = copysize - sizeof(struct ip_fw_64)-4;
	else
		cmdsize = fw64->cmd_len * sizeof(ipfw_insn);
	cmdsize = copyfrom64fw_insn( fw64, user_ip_fw, cmdsize);
	return( sizeof(struct ip_fw) + cmdsize - 4);
}

void
externalize_flow_id(struct ipfw_flow_id *dst, struct ip_flow_id *src);
void
externalize_flow_id(struct ipfw_flow_id *dst, struct ip_flow_id *src)
{
	dst->dst_ip = src->dst_ip;
	dst->src_ip = src->src_ip;
	dst->dst_port = src->dst_port;
	dst->src_port = src->src_port;
	dst->proto = src->proto;
	dst->flags = src->flags;
}

static
void cp_dyn_to_comp_32( struct ipfw_dyn_rule_compat_32 *dyn_rule_vers1, int *len)
{
	struct ipfw_dyn_rule_compat_32 *dyn_last=NULL;
	ipfw_dyn_rule 	*p;
	int i;

	if (ipfw_dyn_v) {
		for (i = 0; i < curr_dyn_buckets; i++) {
			for ( p = ipfw_dyn_v[i] ; p != NULL ; p = p->next) {
				dyn_rule_vers1->chain = (user32_addr_t)(p->rule->rulenum);
				externalize_flow_id(&dyn_rule_vers1->id, &p->id);
				externalize_flow_id(&dyn_rule_vers1->mask, &p->id);
				dyn_rule_vers1->type = p->dyn_type;
				dyn_rule_vers1->expire = p->expire;
				dyn_rule_vers1->pcnt = p->pcnt;
				dyn_rule_vers1->bcnt = p->bcnt;
				dyn_rule_vers1->bucket = p->bucket;
				dyn_rule_vers1->state = p->state;
				
				dyn_rule_vers1->next = CAST_DOWN_EXPLICIT( user32_addr_t, p->next);
				dyn_last = dyn_rule_vers1;
				
				*len += sizeof(*dyn_rule_vers1);
				dyn_rule_vers1++;
			}
		}
		
		if (dyn_last != NULL) {
			dyn_last->next = ((user32_addr_t)0);
		}
	}
}


static
void cp_dyn_to_comp_64( struct ipfw_dyn_rule_compat_64 *dyn_rule_vers1, int *len)
{
	struct ipfw_dyn_rule_compat_64 *dyn_last=NULL;
	ipfw_dyn_rule 	*p;
	int i;

	if (ipfw_dyn_v) {
		for (i = 0; i < curr_dyn_buckets; i++) {
			for ( p = ipfw_dyn_v[i] ; p != NULL ; p = p->next) {
				dyn_rule_vers1->chain = (user64_addr_t) p->rule->rulenum;
				externalize_flow_id(&dyn_rule_vers1->id, &p->id);
				externalize_flow_id(&dyn_rule_vers1->mask, &p->id);
				dyn_rule_vers1->type = p->dyn_type;
				dyn_rule_vers1->expire = p->expire;
				dyn_rule_vers1->pcnt = p->pcnt;
				dyn_rule_vers1->bcnt = p->bcnt;
				dyn_rule_vers1->bucket = p->bucket;
				dyn_rule_vers1->state = p->state;
				
				dyn_rule_vers1->next = CAST_DOWN(user64_addr_t, p->next);
				dyn_last = dyn_rule_vers1;
				
				*len += sizeof(*dyn_rule_vers1);
				dyn_rule_vers1++;
			}
		}
		
		if (dyn_last != NULL) {
			dyn_last->next = CAST_DOWN(user64_addr_t, NULL);
		}
	}
}

static int
sooptcopyin_fw( struct sockopt *sopt, struct ip_fw *user_ip_fw, size_t *size )
{
	size_t	valsize, copyinsize = 0;
	int	error = 0;

	valsize = sopt->sopt_valsize;	
	if ( size )
		copyinsize = *size;
	if (proc_is64bit(sopt->sopt_p)) {
		struct ip_fw_64	*fw64=NULL;
		
		if ( valsize < sizeof(struct ip_fw_64) ) {
			return(EINVAL);
		}
		if ( !copyinsize )
			copyinsize = sizeof(struct ip_fw_64);
		if ( valsize > copyinsize )
			sopt->sopt_valsize = valsize = copyinsize;
			
		if ( sopt->sopt_p != 0) {
			fw64 = _MALLOC(copyinsize, M_TEMP, M_WAITOK);
			if ( fw64 == NULL )
				return(ENOBUFS);
			if ((error = copyin(sopt->sopt_val, fw64, valsize)) != 0){
				_FREE(fw64, M_TEMP);
				return error;
			}
		}
		else {
			bcopy(CAST_DOWN(caddr_t, sopt->sopt_val), fw64, valsize);
		}
		valsize = copyfrom64fw( fw64, user_ip_fw, valsize );
		_FREE( fw64, M_TEMP);
	}else {
		struct ip_fw_32 *fw32=NULL;
	
		if ( valsize < sizeof(struct ip_fw_32) ) {
			return(EINVAL);
		}
		if ( !copyinsize)
			copyinsize = sizeof(struct ip_fw_32);
		if ( valsize > copyinsize)
			sopt->sopt_valsize = valsize = copyinsize;
			
		if ( sopt->sopt_p != 0) {
			fw32 = _MALLOC(copyinsize, M_TEMP, M_WAITOK);
			if ( fw32 == NULL )
				return(ENOBUFS);
			if ( (error = copyin(sopt->sopt_val, fw32, valsize)) != 0){
				_FREE( fw32, M_TEMP);
				return( error );
			}
		}
		else {
			bcopy(CAST_DOWN(caddr_t, sopt->sopt_val), fw32, valsize);
		}
		valsize = copyfrom32fw( fw32, user_ip_fw, valsize);
		_FREE( fw32, M_TEMP);
	}
	if ( size )
		*size = valsize;
	return error;
}

/*
 * The following checks use two arrays of 8 or 16 bits to store the
 * bits that we want set or clear, respectively. They are in the
 * low and high half of cmd->arg1 or cmd->d[0].
 *
 * We scan options and store the bits we find set. We succeed if
 *
 *	(want_set & ~bits) == 0 && (want_clear & ~bits) == want_clear
 *
 * The code is sometimes optimized not to store additional variables.
 */

static int
flags_match(ipfw_insn *cmd, u_int8_t bits)
{
	u_char want_clear;
	bits = ~bits;

	if ( ((cmd->arg1 & 0xff) & bits) != 0)
		return 0; /* some bits we want set were clear */
	want_clear = (cmd->arg1 >> 8) & 0xff;
	if ( (want_clear & bits) != want_clear)
		return 0; /* some bits we want clear were set */
	return 1;
}

static int
ipopts_match(struct ip *ip, ipfw_insn *cmd)
{
	int optlen, bits = 0;
	u_char *cp = (u_char *)(ip + 1);
	int x = (ip->ip_hl << 2) - sizeof (struct ip);

	for (; x > 0; x -= optlen, cp += optlen) {
		int opt = cp[IPOPT_OPTVAL];

		if (opt == IPOPT_EOL)
			break;
		if (opt == IPOPT_NOP)
			optlen = 1;
		else {
			optlen = cp[IPOPT_OLEN];
			if (optlen <= 0 || optlen > x)
				return 0; /* invalid or truncated */
		}
		switch (opt) {

		default:
			break;

		case IPOPT_LSRR:
			bits |= IP_FW_IPOPT_LSRR;
			break;

		case IPOPT_SSRR:
			bits |= IP_FW_IPOPT_SSRR;
			break;

		case IPOPT_RR:
			bits |= IP_FW_IPOPT_RR;
			break;

		case IPOPT_TS:
			bits |= IP_FW_IPOPT_TS;
			break;
		}
	}
	return (flags_match(cmd, bits));
}

static int
tcpopts_match(struct ip *ip, ipfw_insn *cmd)
{
	int optlen, bits = 0;
	struct tcphdr *tcp = L3HDR(struct tcphdr,ip);
	u_char *cp = (u_char *)(tcp + 1);
	int x = (tcp->th_off << 2) - sizeof(struct tcphdr);

	for (; x > 0; x -= optlen, cp += optlen) {
		int opt = cp[0];
		if (opt == TCPOPT_EOL)
			break;
		if (opt == TCPOPT_NOP)
			optlen = 1;
		else {
			optlen = cp[1];
			if (optlen <= 0)
				break;
		}

		switch (opt) {

		default:
			break;

		case TCPOPT_MAXSEG:
			bits |= IP_FW_TCPOPT_MSS;
			break;

		case TCPOPT_WINDOW:
			bits |= IP_FW_TCPOPT_WINDOW;
			break;

		case TCPOPT_SACK_PERMITTED:
		case TCPOPT_SACK:
			bits |= IP_FW_TCPOPT_SACK;
			break;

		case TCPOPT_TIMESTAMP:
			bits |= IP_FW_TCPOPT_TS;
			break;

		case TCPOPT_CC:
		case TCPOPT_CCNEW:
		case TCPOPT_CCECHO:
			bits |= IP_FW_TCPOPT_CC;
			break;
		}
	}
	return (flags_match(cmd, bits));
}

static int
iface_match(struct ifnet *ifp, ipfw_insn_if *cmd)
{
	if (ifp == NULL)	/* no iface with this packet, match fails */
		return 0;
	/* Check by name or by IP address */
	if (cmd->name[0] != '\0') { /* match by name */
		/* Check unit number (-1 is wildcard) */
		if (cmd->p.unit != -1 && cmd->p.unit != ifp->if_unit)
			return(0);
		/* Check name */
		if (!strncmp(ifp->if_name, cmd->name, IFNAMSIZ))
			return(1);
	} else {
		struct ifaddr *ia;

		ifnet_lock_shared(ifp);
		TAILQ_FOREACH(ia, &ifp->if_addrhead, ifa_link) {
			IFA_LOCK(ia);
			if (ia->ifa_addr->sa_family != AF_INET) {
				IFA_UNLOCK(ia);
				continue;
			}
			if (cmd->p.ip.s_addr == ((struct sockaddr_in *)
			    (ia->ifa_addr))->sin_addr.s_addr) {
				IFA_UNLOCK(ia);
				ifnet_lock_done(ifp);
				return(1);	/* match */
			}
			IFA_UNLOCK(ia);
		}
		ifnet_lock_done(ifp);
	}
	return(0);	/* no match, fail ... */
}

/*
 * The 'verrevpath' option checks that the interface that an IP packet
 * arrives on is the same interface that traffic destined for the
 * packet's source address would be routed out of. This is a measure
 * to block forged packets. This is also commonly known as "anti-spoofing"
 * or Unicast Reverse Path Forwarding (Unicast RFP) in Cisco-ese. The
 * name of the knob is purposely reminisent of the Cisco IOS command,
 *
 *   ip verify unicast reverse-path
 *
 * which implements the same functionality. But note that syntax is
 * misleading. The check may be performed on all IP packets whether unicast,
 * multicast, or broadcast.
 */
static int
verify_rev_path(struct in_addr src, struct ifnet *ifp)
{
	static struct route ro;
	struct sockaddr_in *dst;

	bzero(&ro, sizeof (ro));
	dst = (struct sockaddr_in *)&(ro.ro_dst);

	/* Check if we've cached the route from the previous call. */
	if (src.s_addr != dst->sin_addr.s_addr) {
		dst->sin_family = AF_INET;
		dst->sin_len = sizeof(*dst);
		dst->sin_addr = src;

		rtalloc_ign(&ro, RTF_CLONING|RTF_PRCLONING, false);
	}
	if (ro.ro_rt != NULL) {
		RT_LOCK_SPIN(ro.ro_rt);
	} else {
		ROUTE_RELEASE(&ro);
		return 0;	/* No route */
	}
	if ((ifp == NULL) ||
	    (ro.ro_rt->rt_ifp->if_index != ifp->if_index)) {
		RT_UNLOCK(ro.ro_rt);
		ROUTE_RELEASE(&ro);
		return 0;
        }
	RT_UNLOCK(ro.ro_rt);
	ROUTE_RELEASE(&ro);
	return 1;
}


static u_int64_t norule_counter;	/* counter for ipfw_log(NULL...) */

#define SNPARGS(buf, len) buf + len, sizeof(buf) > len ? sizeof(buf) - len : 0
#define SNP(buf) buf, sizeof(buf)

/*
 * We enter here when we have a rule with O_LOG.
 * XXX this function alone takes about 2Kbytes of code!
 */
static void
ipfw_log(struct ip_fw *f, u_int hlen, struct ether_header *eh,
	struct mbuf *m, struct ifnet *oif)
{
	const char *action;
	int limit_reached = 0;
	char ipv4str[MAX_IPv4_STR_LEN];
	char action2[40], proto[48], fragment[28];
	
	fragment[0] = '\0';
	proto[0] = '\0';

	if (f == NULL) {	/* bogus pkt */
		if (verbose_limit != 0 && norule_counter >= verbose_limit)
			return;
		norule_counter++;
		if (norule_counter == verbose_limit)
			limit_reached = verbose_limit;
		action = "Refuse";
	} else {	/* O_LOG is the first action, find the real one */
		ipfw_insn *cmd = ACTION_PTR(f);
		ipfw_insn_log *l = (ipfw_insn_log *)cmd;

		if (l->max_log != 0 && l->log_left == 0)
			return;
		l->log_left--;
		if (l->log_left == 0)
			limit_reached = l->max_log;
		cmd += F_LEN(cmd);	/* point to first action */
		if (cmd->opcode == O_PROB)
			cmd += F_LEN(cmd);

		action = action2;
		switch (cmd->opcode) {
		case O_DENY:
			action = "Deny";
			break;

		case O_REJECT:
			if (cmd->arg1==ICMP_REJECT_RST)
				action = "Reset";
			else if (cmd->arg1==ICMP_UNREACH_HOST)
				action = "Reject";
			else
				snprintf(SNPARGS(action2, 0), "Unreach %d",
					cmd->arg1);
			break;

		case O_ACCEPT:
			action = "Accept";
			break;
		case O_COUNT:
			action = "Count";
			break;
		case O_DIVERT:
			snprintf(SNPARGS(action2, 0), "Divert %d",
				cmd->arg1);
			break;
		case O_TEE:
			snprintf(SNPARGS(action2, 0), "Tee %d",
				cmd->arg1);
			break;
		case O_SKIPTO:
			snprintf(SNPARGS(action2, 0), "SkipTo %d",
				cmd->arg1);
			break;
		case O_PIPE:
			snprintf(SNPARGS(action2, 0), "Pipe %d",
				cmd->arg1);
			break;
		case O_QUEUE:
			snprintf(SNPARGS(action2, 0), "Queue %d",
				cmd->arg1);
			break;
		case O_FORWARD_IP: {
			ipfw_insn_sa *sa = (ipfw_insn_sa *)cmd;
			int len;

			if (f->reserved_1 == IPFW_RULE_INACTIVE) {
				break;
			}
			len = snprintf(SNPARGS(action2, 0), "Forward to %s",
				inet_ntop(AF_INET, &sa->sa.sin_addr, ipv4str, sizeof(ipv4str)));
			if (sa->sa.sin_port)
				snprintf(SNPARGS(action2, len), ":%d",
				    sa->sa.sin_port);
			}
			break;
		default:
			action = "UNKNOWN";
			break;
		}
	}

	if (hlen == 0) {	/* non-ip */
		snprintf(SNPARGS(proto, 0), "MAC");
	} else {
		struct ip *ip = mtod(m, struct ip *);
		/* these three are all aliases to the same thing */
		struct icmp *const icmp = L3HDR(struct icmp, ip);
		struct tcphdr *const tcp = (struct tcphdr *)icmp;
		struct udphdr *const udp = (struct udphdr *)icmp;

		int ip_off, offset, ip_len;

		int len;

		if (eh != NULL) { /* layer 2 packets are as on the wire */
			ip_off = ntohs(ip->ip_off);
			ip_len = ntohs(ip->ip_len);
		} else {
			ip_off = ip->ip_off;
			ip_len = ip->ip_len;
		}
		offset = ip_off & IP_OFFMASK;
		switch (ip->ip_p) {
		case IPPROTO_TCP:
			len = snprintf(SNPARGS(proto, 0), "TCP %s",
			    inet_ntop(AF_INET, &ip->ip_src, ipv4str, sizeof(ipv4str)));
			if (offset == 0)
				snprintf(SNPARGS(proto, len), ":%d %s:%d",
				    ntohs(tcp->th_sport),
				    inet_ntop(AF_INET, &ip->ip_dst, ipv4str, sizeof(ipv4str)),
				    ntohs(tcp->th_dport));
			else
				snprintf(SNPARGS(proto, len), " %s",
				    inet_ntop(AF_INET, &ip->ip_dst, ipv4str, sizeof(ipv4str)));
			break;

		case IPPROTO_UDP:
			len = snprintf(SNPARGS(proto, 0), "UDP %s",
				inet_ntop(AF_INET, &ip->ip_src, ipv4str, sizeof(ipv4str)));
			if (offset == 0)
				snprintf(SNPARGS(proto, len), ":%d %s:%d",
				    ntohs(udp->uh_sport),
				    inet_ntop(AF_INET, &ip->ip_dst, ipv4str, sizeof(ipv4str)),
				    ntohs(udp->uh_dport));
			else
				snprintf(SNPARGS(proto, len), " %s",
				    inet_ntop(AF_INET, &ip->ip_dst, ipv4str, sizeof(ipv4str)));
			break;

		case IPPROTO_ICMP:
			if (offset == 0)
				len = snprintf(SNPARGS(proto, 0),
				    "ICMP:%u.%u ",
				    icmp->icmp_type, icmp->icmp_code);
			else
				len = snprintf(SNPARGS(proto, 0), "ICMP ");
			len += snprintf(SNPARGS(proto, len), "%s",
			    inet_ntop(AF_INET, &ip->ip_src, ipv4str, sizeof(ipv4str)));
			snprintf(SNPARGS(proto, len), " %s",
			    inet_ntop(AF_INET, &ip->ip_dst, ipv4str, sizeof(ipv4str)));
			break;

		default:
			len = snprintf(SNPARGS(proto, 0), "P:%d %s", ip->ip_p,
			    inet_ntop(AF_INET, &ip->ip_src, ipv4str, sizeof(ipv4str)));
			snprintf(SNPARGS(proto, len), " %s",
			    inet_ntop(AF_INET, &ip->ip_dst, ipv4str, sizeof(ipv4str)));
			break;
		}

		if (ip_off & (IP_MF | IP_OFFMASK))
			snprintf(SNPARGS(fragment, 0), " (frag %d:%d@%d%s)",
			     ntohs(ip->ip_id), ip_len - (ip->ip_hl << 2),
			     offset << 3,
			     (ip_off & IP_MF) ? "+" : "");
	}
	if (oif || m->m_pkthdr.rcvif)
	{
		dolog((LOG_AUTHPRIV | LOG_INFO,
		    "ipfw: %d %s %s %s via %s%d%s\n",
		    f ? f->rulenum : -1,
		    action, proto, oif ? "out" : "in",
		    oif ? oif->if_name : m->m_pkthdr.rcvif->if_name,
		    oif ? oif->if_unit : m->m_pkthdr.rcvif->if_unit,
		    fragment)); 
	}
	else{
		dolog((LOG_AUTHPRIV | LOG_INFO,
		    "ipfw: %d %s %s [no if info]%s\n",
		    f ? f->rulenum : -1,
		    action, proto, fragment));
	}
	if (limit_reached){
		dolog((LOG_AUTHPRIV | LOG_NOTICE,
		    "ipfw: limit %d reached on entry %d\n",
		    limit_reached, f ? f->rulenum : -1));
	}
}

/*
 * IMPORTANT: the hash function for dynamic rules must be commutative
 * in source and destination (ip,port), because rules are bidirectional
 * and we want to find both in the same bucket.
 */
static __inline int
hash_packet(struct ip_flow_id *id)
{
	u_int32_t i;

	i = (id->dst_ip) ^ (id->src_ip) ^ (id->dst_port) ^ (id->src_port);
	i &= (curr_dyn_buckets - 1);
	return i;
}

/**
 * unlink a dynamic rule from a chain. prev is a pointer to
 * the previous one, q is a pointer to the rule to delete,
 * head is a pointer to the head of the queue.
 * Modifies q and potentially also head.
 */
#define UNLINK_DYN_RULE(prev, head, q) {				\
	ipfw_dyn_rule *old_q = q;					\
									\
	/* remove a refcount to the parent */				\
	if (q->dyn_type == O_LIMIT)					\
		q->parent->count--;					\
	DEB(printf("ipfw: unlink entry 0x%08x %d -> 0x%08x %d, %d left\n",\
		(q->id.src_ip), (q->id.src_port),			\
		(q->id.dst_ip), (q->id.dst_port), dyn_count-1 ); )	\
	if (prev != NULL)						\
		prev->next = q = q->next;				\
	else								\
		head = q = q->next;					\
	dyn_count--;							\
	_FREE(old_q, M_IPFW); }

#define TIME_LEQ(a,b)       ((int)((a)-(b)) <= 0)

/**
 * Remove dynamic rules pointing to "rule", or all of them if rule == NULL.
 *
 * If keep_me == NULL, rules are deleted even if not expired,
 * otherwise only expired rules are removed.
 *
 * The value of the second parameter is also used to point to identify
 * a rule we absolutely do not want to remove (e.g. because we are
 * holding a reference to it -- this is the case with O_LIMIT_PARENT
 * rules). The pointer is only used for comparison, so any non-null
 * value will do.
 */
static void
remove_dyn_rule(struct ip_fw *rule, ipfw_dyn_rule *keep_me)
{
	static u_int32_t last_remove = 0;

#define FORCE (keep_me == NULL)

	ipfw_dyn_rule *prev, *q;
	int i, pass = 0, max_pass = 0;
	struct timeval timenow;

	getmicrotime(&timenow);

	if (ipfw_dyn_v == NULL || dyn_count == 0)
		return;
	/* do not expire more than once per second, it is useless */
	if (!FORCE && last_remove == timenow.tv_sec)
		return;
	last_remove = timenow.tv_sec;

	/*
	 * because O_LIMIT refer to parent rules, during the first pass only
	 * remove child and mark any pending LIMIT_PARENT, and remove
	 * them in a second pass.
	 */
next_pass:
	for (i = 0 ; i < curr_dyn_buckets ; i++) {
		for (prev=NULL, q = ipfw_dyn_v[i] ; q ; ) {
			/*
			 * Logic can become complex here, so we split tests.
			 */
			if (q == keep_me)
				goto next;
			if (rule != NULL && rule != q->rule)
				goto next; /* not the one we are looking for */
			if (q->dyn_type == O_LIMIT_PARENT) {
				/*
				 * handle parent in the second pass,
				 * record we need one.
				 */
				max_pass = 1;
				if (pass == 0)
					goto next;
				if (FORCE && q->count != 0 ) {
					/* XXX should not happen! */
					printf("ipfw: OUCH! cannot remove rule,"
					     " count %d\n", q->count);
				}
			} else {
				if (!FORCE &&
				    !TIME_LEQ( q->expire, timenow.tv_sec ))
					goto next;
			}
			if (q->dyn_type != O_LIMIT_PARENT || !q->count) {
				UNLINK_DYN_RULE(prev, ipfw_dyn_v[i], q);
				continue;
			}
next:
			prev=q;
			q=q->next;
		}
	}
	if (pass++ < max_pass)
		goto next_pass;
}


/**
 * lookup a dynamic rule.
 */
static ipfw_dyn_rule *
lookup_dyn_rule(struct ip_flow_id *pkt, int *match_direction,
	struct tcphdr *tcp)
{
	/*
	 * stateful ipfw extensions.
	 * Lookup into dynamic session queue
	 */
#define MATCH_REVERSE	0
#define MATCH_FORWARD	1
#define MATCH_NONE	2
#define MATCH_UNKNOWN	3
#define BOTH_SYN        (TH_SYN | (TH_SYN << 8))
#define BOTH_FIN        (TH_FIN | (TH_FIN << 8))

	int i, dir = MATCH_NONE;
	ipfw_dyn_rule *prev, *q=NULL;
	struct timeval timenow;

	getmicrotime(&timenow);

	if (ipfw_dyn_v == NULL)
		goto done;	/* not found */
	i = hash_packet( pkt );
	for (prev=NULL, q = ipfw_dyn_v[i] ; q != NULL ; ) {
		if (q->dyn_type == O_LIMIT_PARENT && q->count)
			goto next;
		if (TIME_LEQ( q->expire, timenow.tv_sec)) { /* expire entry */
                        int     dounlink = 1;

			/* check if entry is TCP */
                        if ( q->id.proto == IPPROTO_TCP )
                        {
                                /* do not delete an established TCP connection which hasn't been closed by both sides */
                                if ( (q->state & (BOTH_SYN | BOTH_FIN)) != (BOTH_SYN | BOTH_FIN) )
                                        dounlink = 0;
                        }
                        if ( dounlink ){
                                UNLINK_DYN_RULE(prev, ipfw_dyn_v[i], q);
                                continue;
                        }
		}
		if (pkt->proto == q->id.proto &&
		    q->dyn_type != O_LIMIT_PARENT) {
			if (pkt->src_ip == q->id.src_ip &&
			    pkt->dst_ip == q->id.dst_ip &&
			    pkt->src_port == q->id.src_port &&
			    pkt->dst_port == q->id.dst_port ) {
				dir = MATCH_FORWARD;
				break;
			}
			if (pkt->src_ip == q->id.dst_ip &&
			    pkt->dst_ip == q->id.src_ip &&
			    pkt->src_port == q->id.dst_port &&
			    pkt->dst_port == q->id.src_port ) {
				dir = MATCH_REVERSE;
				break;
			}
		}
next:
		prev = q;
		q = q->next;
	}
	if (q == NULL)
		goto done; /* q = NULL, not found */

	if ( prev != NULL) { /* found and not in front */
		prev->next = q->next;
		q->next = ipfw_dyn_v[i];
		ipfw_dyn_v[i] = q;
	}
	if (pkt->proto == IPPROTO_TCP) { /* update state according to flags */
		u_char flags = pkt->flags & (TH_FIN|TH_SYN|TH_RST);

		q->state |= (dir == MATCH_FORWARD ) ? flags : (flags << 8);
		switch (q->state) {
		case TH_SYN:				/* opening */
			q->expire = timenow.tv_sec + dyn_syn_lifetime;
			break;

		case BOTH_SYN:			/* move to established */
		case BOTH_SYN | TH_FIN :	/* one side tries to close */
		case BOTH_SYN | (TH_FIN << 8) :
 			if (tcp) {
#define _SEQ_GE(a,b) ((int)(a) - (int)(b) >= 0)
			    u_int32_t ack = ntohl(tcp->th_ack);
			    if (dir == MATCH_FORWARD) {
				if (q->ack_fwd == 0 || _SEQ_GE(ack, q->ack_fwd))
				    q->ack_fwd = ack;
				else { /* ignore out-of-sequence */
				    break;
				}
			    } else {
				if (q->ack_rev == 0 || _SEQ_GE(ack, q->ack_rev))
				    q->ack_rev = ack;
				else { /* ignore out-of-sequence */
				    break;
				}
			    }
			}
			q->expire = timenow.tv_sec + dyn_ack_lifetime;
			break;

		case BOTH_SYN | BOTH_FIN:	/* both sides closed */
			if (dyn_fin_lifetime >= dyn_keepalive_period)
				dyn_fin_lifetime = dyn_keepalive_period - 1;
			q->expire = timenow.tv_sec + dyn_fin_lifetime;
			break;

		default:
#if 0
			/*
			 * reset or some invalid combination, but can also
			 * occur if we use keep-state the wrong way.
			 */
			if ( (q->state & ((TH_RST << 8)|TH_RST)) == 0)
				printf("invalid state: 0x%x\n", q->state);
#endif
			if (dyn_rst_lifetime >= dyn_keepalive_period)
				dyn_rst_lifetime = dyn_keepalive_period - 1;
			q->expire = timenow.tv_sec + dyn_rst_lifetime;
			break;
		}
	} else if (pkt->proto == IPPROTO_UDP) {
		q->expire = timenow.tv_sec + dyn_udp_lifetime;
	} else {
		/* other protocols */
		q->expire = timenow.tv_sec + dyn_short_lifetime;
	}
done:
	if (match_direction)
		*match_direction = dir;
	return q;
}

static void
realloc_dynamic_table(void)
{
	/*
	 * Try reallocation, make sure we have a power of 2 and do
	 * not allow more than 64k entries. In case of overflow,
	 * default to 1024.
	 */

	if (dyn_buckets > 65536)
		dyn_buckets = 1024;
	if ((dyn_buckets & (dyn_buckets-1)) != 0) { /* not a power of 2 */
		dyn_buckets = curr_dyn_buckets; /* reset */
		return;
	}
	curr_dyn_buckets = dyn_buckets;
	if (ipfw_dyn_v != NULL)
		_FREE(ipfw_dyn_v, M_IPFW);
	for (;;) {
		ipfw_dyn_v = _MALLOC(curr_dyn_buckets * sizeof(ipfw_dyn_rule *),
		       M_IPFW, M_NOWAIT | M_ZERO);
		if (ipfw_dyn_v != NULL || curr_dyn_buckets <= 2)
			break;
		curr_dyn_buckets /= 2;
	}
}

/**
 * Install state of type 'type' for a dynamic session.
 * The hash table contains two type of rules:
 * - regular rules (O_KEEP_STATE)
 * - rules for sessions with limited number of sess per user
 *   (O_LIMIT). When they are created, the parent is
 *   increased by 1, and decreased on delete. In this case,
 *   the third parameter is the parent rule and not the chain.
 * - "parent" rules for the above (O_LIMIT_PARENT).
 */
static ipfw_dyn_rule *
add_dyn_rule(struct ip_flow_id *id, u_int8_t dyn_type, struct ip_fw *rule)
{
	ipfw_dyn_rule *r;
	int i;
	struct timeval timenow;

	getmicrotime(&timenow);

	if (ipfw_dyn_v == NULL ||
	    (dyn_count == 0 && dyn_buckets != curr_dyn_buckets)) {
		realloc_dynamic_table();
		if (ipfw_dyn_v == NULL)
			return NULL; /* failed ! */
	}
	i = hash_packet(id);

	r = _MALLOC(sizeof *r, M_IPFW, M_NOWAIT | M_ZERO);
	if (r == NULL) {
#if IPFW_DEBUG
		printf ("ipfw: sorry cannot allocate state\n");
#endif
		return NULL;
	}

	/* increase refcount on parent, and set pointer */
	if (dyn_type == O_LIMIT) {
		ipfw_dyn_rule *parent = (ipfw_dyn_rule *)rule;
		if ( parent->dyn_type != O_LIMIT_PARENT)
			panic("invalid parent");
		parent->count++;
		r->parent = parent;
		rule = parent->rule;
	}

	r->id = *id;
	r->expire = timenow.tv_sec + dyn_syn_lifetime;
	r->rule = rule;
	r->dyn_type = dyn_type;
	r->pcnt = r->bcnt = 0;
	r->count = 0;

	r->bucket = i;
	r->next = ipfw_dyn_v[i];
	ipfw_dyn_v[i] = r;
	dyn_count++;
	DEB(printf("ipfw: add dyn entry ty %d 0x%08x %d -> 0x%08x %d, total %d\n",
	   dyn_type,
	   (r->id.src_ip), (r->id.src_port),
	   (r->id.dst_ip), (r->id.dst_port),
	   dyn_count ); )
	return r;
}

/**
 * lookup dynamic parent rule using pkt and rule as search keys.
 * If the lookup fails, then install one.
 */
static ipfw_dyn_rule *
lookup_dyn_parent(struct ip_flow_id *pkt, struct ip_fw *rule)
{
	ipfw_dyn_rule *q;
	int i;
	struct timeval timenow;

	getmicrotime(&timenow);

	if (ipfw_dyn_v) {
		i = hash_packet( pkt );
		for (q = ipfw_dyn_v[i] ; q != NULL ; q=q->next)
			if (q->dyn_type == O_LIMIT_PARENT &&
			    rule== q->rule &&
			    pkt->proto == q->id.proto &&
			    pkt->src_ip == q->id.src_ip &&
			    pkt->dst_ip == q->id.dst_ip &&
			    pkt->src_port == q->id.src_port &&
			    pkt->dst_port == q->id.dst_port) {
				q->expire = timenow.tv_sec + dyn_short_lifetime;
				DEB(printf("ipfw: lookup_dyn_parent found "
				    "0x%llx\n", (uint64_t)VM_KERNEL_ADDRPERM(q));)
				return q;
			}
	}
	return add_dyn_rule(pkt, O_LIMIT_PARENT, rule);
}

/**
 * Install dynamic state for rule type cmd->o.opcode
 *
 * Returns 1 (failure) if state is not installed because of errors or because
 * session limitations are enforced.
 */
static int
install_state(struct ip_fw *rule, ipfw_insn_limit *cmd,
	struct ip_fw_args *args)
{
	static int last_log;
	struct timeval timenow;

	ipfw_dyn_rule *q;
	getmicrotime(&timenow);

	DEB(printf("ipfw: install state type %d 0x%08x %u -> 0x%08x %u\n",
	    cmd->o.opcode,
	    (args->fwa_id.src_ip), (args->fwa_id.src_port),
	    (args->fwa_id.dst_ip), (args->fwa_id.dst_port) );)

	q = lookup_dyn_rule(&args->fwa_id, NULL, NULL);

	if (q != NULL) { /* should never occur */
		if (last_log != timenow.tv_sec) {
			last_log = timenow.tv_sec;
			printf("ipfw: install_state: entry already present, done\n");
		}
		return 0;
	}

	if (dyn_count >= dyn_max)
		/*
		 * Run out of slots, try to remove any expired rule.
		 */
		remove_dyn_rule(NULL, (ipfw_dyn_rule *)1);

	if (dyn_count >= dyn_max) {
		if (last_log != timenow.tv_sec) {
			last_log = timenow.tv_sec;
			printf("ipfw: install_state: Too many dynamic rules\n");
		}
		return 1; /* cannot install, notify caller */
	}

	switch (cmd->o.opcode) {
	case O_KEEP_STATE: /* bidir rule */
		add_dyn_rule(&args->fwa_id, O_KEEP_STATE, rule);
		break;

	case O_LIMIT: /* limit number of sessions */
	    {
		u_int16_t limit_mask = cmd->limit_mask;
		struct ip_flow_id id;
		ipfw_dyn_rule *parent;

		DEB(printf("ipfw: installing dyn-limit rule %d\n",
		    cmd->conn_limit);)

		id.dst_ip = id.src_ip = 0;
		id.dst_port = id.src_port = 0;
		id.proto = args->fwa_id.proto;

		if (limit_mask & DYN_SRC_ADDR)
			id.src_ip = args->fwa_id.src_ip;
		if (limit_mask & DYN_DST_ADDR)
			id.dst_ip = args->fwa_id.dst_ip;
		if (limit_mask & DYN_SRC_PORT)
			id.src_port = args->fwa_id.src_port;
		if (limit_mask & DYN_DST_PORT)
			id.dst_port = args->fwa_id.dst_port;
		parent = lookup_dyn_parent(&id, rule);
		if (parent == NULL) {
			printf("ipfw: add parent failed\n");
			return 1;
		}
		if (parent->count >= cmd->conn_limit) {
			/*
			 * See if we can remove some expired rule.
			 */
			remove_dyn_rule(rule, parent);
			if (parent->count >= cmd->conn_limit) {
				if (fw_verbose && last_log != timenow.tv_sec) {
					last_log = timenow.tv_sec;
					dolog((LOG_AUTHPRIV | LOG_DEBUG,
					    "drop session, too many entries\n"));
				}
				return 1;
			}
		}
		add_dyn_rule(&args->fwa_id, O_LIMIT, (struct ip_fw *)parent);
	    }
		break;
	default:
		printf("ipfw: unknown dynamic rule type %u\n", cmd->o.opcode);
		return 1;
	}
	lookup_dyn_rule(&args->fwa_id, NULL, NULL); /* XXX just set lifetime */
	return 0;
}

/*
 * Generate a TCP packet, containing either a RST or a keepalive.
 * When flags & TH_RST, we are sending a RST packet, because of a
 * "reset" action matched the packet.
 * Otherwise we are sending a keepalive, and flags & TH_
 */
static struct mbuf *
send_pkt(struct ip_flow_id *id, u_int32_t seq, u_int32_t ack, int flags)
{
	struct mbuf *m;
	struct ip *ip;
	struct tcphdr *tcp;

	MGETHDR(m, M_DONTWAIT, MT_HEADER);	/* MAC-OK */
	if (m == 0)
		return NULL;
	m->m_pkthdr.rcvif = (struct ifnet *)0;
	m->m_pkthdr.len = m->m_len = sizeof(struct ip) + sizeof(struct tcphdr);
	m->m_data += max_linkhdr;

	ip = mtod(m, struct ip *);
	bzero(ip, m->m_len);
	tcp = (struct tcphdr *)(ip + 1); /* no IP options */
	ip->ip_p = IPPROTO_TCP;
	tcp->th_off = 5;
	/*
	 * Assume we are sending a RST (or a keepalive in the reverse
	 * direction), swap src and destination addresses and ports.
	 */
	ip->ip_src.s_addr = htonl(id->dst_ip);
	ip->ip_dst.s_addr = htonl(id->src_ip);
	tcp->th_sport = htons(id->dst_port);
	tcp->th_dport = htons(id->src_port);
	if (flags & TH_RST) {	/* we are sending a RST */
		if (flags & TH_ACK) {
			tcp->th_seq = htonl(ack);
			tcp->th_ack = htonl(0);
			tcp->th_flags = TH_RST;
		} else {
			if (flags & TH_SYN)
				seq++;
			tcp->th_seq = htonl(0);
			tcp->th_ack = htonl(seq);
			tcp->th_flags = TH_RST | TH_ACK;
		}
	} else {
		/*
		 * We are sending a keepalive. flags & TH_SYN determines
		 * the direction, forward if set, reverse if clear.
		 * NOTE: seq and ack are always assumed to be correct
		 * as set by the caller. This may be confusing...
		 */
		if (flags & TH_SYN) {
			/*
			 * we have to rewrite the correct addresses!
			 */
			ip->ip_dst.s_addr = htonl(id->dst_ip);
			ip->ip_src.s_addr = htonl(id->src_ip);
			tcp->th_dport = htons(id->dst_port);
			tcp->th_sport = htons(id->src_port);
		}
		tcp->th_seq = htonl(seq);
		tcp->th_ack = htonl(ack);
		tcp->th_flags = TH_ACK;
	}
	/*
	 * set ip_len to the payload size so we can compute
	 * the tcp checksum on the pseudoheader
	 * XXX check this, could save a couple of words ?
	 */
	ip->ip_len = htons(sizeof(struct tcphdr));
	tcp->th_sum = in_cksum(m, m->m_pkthdr.len);
	/*
	 * now fill fields left out earlier
	 */
	ip->ip_ttl = ip_defttl;
	ip->ip_len = m->m_pkthdr.len;
	m->m_flags |= M_SKIP_FIREWALL;
	
	return m;
}

/*
 * sends a reject message, consuming the mbuf passed as an argument.
 */
static void
send_reject(struct ip_fw_args *args, int code, int offset, __unused int ip_len)
{

	if (code != ICMP_REJECT_RST) { /* Send an ICMP unreach */
		/* We need the IP header in host order for icmp_error(). */
		if (args->fwa_eh != NULL) {
			struct ip *ip = mtod(args->fwa_m, struct ip *);
			ip->ip_len = ntohs(ip->ip_len);
			ip->ip_off = ntohs(ip->ip_off);
		}
		args->fwa_m->m_flags |= M_SKIP_FIREWALL;
		icmp_error(args->fwa_m, ICMP_UNREACH, code, 0L, 0);
	} else if (offset == 0 && args->fwa_id.proto == IPPROTO_TCP) {
		struct tcphdr *const tcp =
		    L3HDR(struct tcphdr, mtod(args->fwa_m, struct ip *));
		if ( (tcp->th_flags & TH_RST) == 0) {
			struct mbuf *m;

			m = send_pkt(&(args->fwa_id), ntohl(tcp->th_seq),
				ntohl(tcp->th_ack),
				tcp->th_flags | TH_RST);
			if (m != NULL) {
				struct route sro;	/* fake route */

				bzero (&sro, sizeof (sro));
				ip_output(m, NULL, &sro, 0, NULL, NULL);
				ROUTE_RELEASE(&sro);
			}
		}
		m_freem(args->fwa_m);
	} else
		m_freem(args->fwa_m);
	args->fwa_m = NULL;
}

/**
 *
 * Given an ip_fw *, lookup_next_rule will return a pointer
 * to the next rule, which can be either the jump
 * target (for skipto instructions) or the next one in the list (in
 * all other cases including a missing jump target).
 * The result is also written in the "next_rule" field of the rule.
 * Backward jumps are not allowed, so start looking from the next
 * rule...
 *
 * This never returns NULL -- in case we do not have an exact match,
 * the next rule is returned. When the ruleset is changed,
 * pointers are flushed so we are always correct.
 */

static struct ip_fw *
lookup_next_rule(struct ip_fw *me)
{
	struct ip_fw *rule = NULL;
	ipfw_insn *cmd;

	/* look for action, in case it is a skipto */
	cmd = ACTION_PTR(me);
	if (cmd->opcode == O_LOG)
		cmd += F_LEN(cmd);
	if ( cmd->opcode == O_SKIPTO )
		for (rule = me->next; rule ; rule = rule->next)
			if (rule->rulenum >= cmd->arg1)
				break;
	if (rule == NULL)			/* failure or not a skipto */
		rule = me->next;
	me->next_rule = rule;
	return rule;
}

/*
 * The main check routine for the firewall.
 *
 * All arguments are in args so we can modify them and return them
 * back to the caller.
 *
 * Parameters:
 *
 *	args->fwa_m	(in/out) The packet; we set to NULL when/if we nuke it.
 *		Starts with the IP header.
 *	args->fwa_eh (in)	Mac header if present, or NULL for layer3 packet.
 *	args->fwa_oif	Outgoing interface, or NULL if packet is incoming.
 *		The incoming interface is in the mbuf. (in)
 *	args->fwa_divert_rule (in/out)
 *		Skip up to the first rule past this rule number;
 *		upon return, non-zero port number for divert or tee.
 *
 *	args->fwa_ipfw_rule	Pointer to the last matching rule (in/out)
 *	args->fwa_next_hop	Socket we are forwarding to (out).
 *	args->fwa_id	Addresses grabbed from the packet (out)
 *
 * Return value:
 *
 *	IP_FW_PORT_DENY_FLAG	the packet must be dropped.
 *	0	The packet is to be accepted and routed normally OR
 *      	the packet was denied/rejected and has been dropped;
 *		in the latter case, *m is equal to NULL upon return.
 *	port	Divert the packet to port, with these caveats:
 *
 *		- If IP_FW_PORT_TEE_FLAG is set, tee the packet instead
 *		  of diverting it (ie, 'ipfw tee').
 *
 *		- If IP_FW_PORT_DYNT_FLAG is set, interpret the lower
 *		  16 bits as a dummynet pipe number instead of diverting
 */

static int
ipfw_chk(struct ip_fw_args *args)
{
	/*
	 * Local variables hold state during the processing of a packet.
	 *
	 * IMPORTANT NOTE: to speed up the processing of rules, there
	 * are some assumption on the values of the variables, which
	 * are documented here. Should you change them, please check
	 * the implementation of the various instructions to make sure
	 * that they still work.
	 *
	 * args->fwa_eh	The MAC header. It is non-null for a layer2
	 *	packet, it is NULL for a layer-3 packet.
	 *
	 * m | args->fwa_m	Pointer to the mbuf, as received from the caller.
	 *	It may change if ipfw_chk() does an m_pullup, or if it
	 *	consumes the packet because it calls send_reject().
	 *	XXX This has to change, so that ipfw_chk() never modifies
	 *	or consumes the buffer.
	 * ip	is simply an alias of the value of m, and it is kept
	 *	in sync with it (the packet is	supposed to start with
	 *	the ip header).
	 */
	struct mbuf *m = args->fwa_m;
	struct ip *ip = mtod(m, struct ip *);

	/*
	 * oif | args->fwa_oif	If NULL, ipfw_chk has been called on the
	 *	inbound path (ether_input, bdg_forward, ip_input).
	 *	If non-NULL, ipfw_chk has been called on the outbound path
	 *	(ether_output, ip_output).
	 */
	struct ifnet *oif = args->fwa_oif;

	struct ip_fw *f = NULL;		/* matching rule */
	int retval = 0;

	/*
	 * hlen	The length of the IPv4 header.
	 *	hlen >0 means we have an IPv4 packet.
	 */
	u_int hlen = 0;		/* hlen >0 means we have an IP pkt */

	/*
	 * offset	The offset of a fragment. offset != 0 means that
	 *	we have a fragment at this offset of an IPv4 packet.
	 *	offset == 0 means that (if this is an IPv4 packet)
	 *	this is the first or only fragment.
	 */
	u_short offset = 0;

	/*
	 * Local copies of addresses. They are only valid if we have
	 * an IP packet.
	 *
	 * proto	The protocol. Set to 0 for non-ip packets,
	 *	or to the protocol read from the packet otherwise.
	 *	proto != 0 means that we have an IPv4 packet.
	 *
	 * src_port, dst_port	port numbers, in HOST format. Only
	 *	valid for TCP and UDP packets.
	 *
	 * src_ip, dst_ip	ip addresses, in NETWORK format.
	 *	Only valid for IPv4 packets.
	 */
	u_int8_t proto;
	u_int16_t src_port = 0, dst_port = 0;	/* NOTE: host format	*/
	struct in_addr src_ip = { 0 } , dst_ip = { 0 };		/* NOTE: network format	*/
	u_int16_t ip_len=0;
	int pktlen;
	int dyn_dir = MATCH_UNKNOWN;
	ipfw_dyn_rule *q = NULL;
	struct timeval timenow;

	if (m->m_flags & M_SKIP_FIREWALL || fw_bypass) {
		return 0;	/* accept */
	}

	/* 
	 * Clear packet chain if we find one here.
	 */
	
	if (m->m_nextpkt != NULL) {
		m_freem_list(m->m_nextpkt);
		m->m_nextpkt = NULL;
	}
	
	lck_mtx_lock(ipfw_mutex);

	getmicrotime(&timenow);
	/*
	 * dyn_dir = MATCH_UNKNOWN when rules unchecked,
	 * 	MATCH_NONE when checked and not matched (q = NULL),
	 *	MATCH_FORWARD or MATCH_REVERSE otherwise (q != NULL)
	 */

	pktlen = m->m_pkthdr.len;
	if (args->fwa_eh == NULL ||		/* layer 3 packet */
		( m->m_pkthdr.len >= sizeof(struct ip) &&
		    ntohs(args->fwa_eh->ether_type) == ETHERTYPE_IP))
			hlen = ip->ip_hl << 2;

	/*
	 * Collect parameters into local variables for faster matching.
	 */
	if (hlen == 0) {	/* do not grab addresses for non-ip pkts */
		proto = args->fwa_id.proto = 0;	/* mark f_id invalid */
		goto after_ip_checks;
	}

	proto = args->fwa_id.proto = ip->ip_p;
	src_ip = ip->ip_src;
	dst_ip = ip->ip_dst;
	if (args->fwa_eh != NULL) { /* layer 2 packets are as on the wire */
		offset = ntohs(ip->ip_off) & IP_OFFMASK;
		ip_len = ntohs(ip->ip_len);
	} else {
		offset = ip->ip_off & IP_OFFMASK;
		ip_len = ip->ip_len;
	}
	pktlen = ip_len < pktlen ? ip_len : pktlen;

#define PULLUP_TO(len)						\
		do {						\
			if ((m)->m_len < (len)) {		\
			    args->fwa_m = m = m_pullup(m, (len));	\
			    if (m == 0)				\
				goto pullup_failed;		\
			    ip = mtod(m, struct ip *);		\
			}					\
		} while (0)

	if (offset == 0) {
		switch (proto) {
		case IPPROTO_TCP:
		    {
			struct tcphdr *tcp;

			PULLUP_TO(hlen + sizeof(struct tcphdr));
			tcp = L3HDR(struct tcphdr, ip);
			dst_port = tcp->th_dport;
			src_port = tcp->th_sport;
			args->fwa_id.flags = tcp->th_flags;
			}
			break;

		case IPPROTO_UDP:
		    {
			struct udphdr *udp;

			PULLUP_TO(hlen + sizeof(struct udphdr));
			udp = L3HDR(struct udphdr, ip);
			dst_port = udp->uh_dport;
			src_port = udp->uh_sport;
			}
			break;

		case IPPROTO_ICMP:
			PULLUP_TO(hlen + 4);	/* type, code and checksum. */
			args->fwa_id.flags = L3HDR(struct icmp, ip)->icmp_type;
			break;

		default:
			break;
		}
#undef PULLUP_TO
	}

	args->fwa_id.src_ip = ntohl(src_ip.s_addr);
	args->fwa_id.dst_ip = ntohl(dst_ip.s_addr);
	args->fwa_id.src_port = src_port = ntohs(src_port);
	args->fwa_id.dst_port = dst_port = ntohs(dst_port);

after_ip_checks:
	if (args->fwa_ipfw_rule) {
		/*
		 * Packet has already been tagged. Look for the next rule
		 * to restart processing.
		 *
		 * If fw_one_pass != 0 then just accept it.
		 * XXX should not happen here, but optimized out in
		 * the caller.
		 */
		if (fw_one_pass) {
			lck_mtx_unlock(ipfw_mutex);
			return 0;
		}

		f = args->fwa_ipfw_rule->next_rule;
		if (f == NULL)
			f = lookup_next_rule(args->fwa_ipfw_rule);
	} else {
		/*
		 * Find the starting rule. It can be either the first
		 * one, or the one after divert_rule if asked so.
		 */
		int skipto = args->fwa_divert_rule;

		f = layer3_chain;
		if (args->fwa_eh == NULL && skipto != 0) {
			if (skipto >= IPFW_DEFAULT_RULE) {
				lck_mtx_unlock(ipfw_mutex);
				return(IP_FW_PORT_DENY_FLAG); /* invalid */
			}
			while (f && f->rulenum <= skipto)
				f = f->next;
			if (f == NULL) {	/* drop packet */
				lck_mtx_unlock(ipfw_mutex);
				return(IP_FW_PORT_DENY_FLAG);
			}
		}
	}
	args->fwa_divert_rule = 0;	/* reset to avoid confusion later */

	/*
	 * Now scan the rules, and parse microinstructions for each rule.
	 */
	for (; f; f = f->next) {
		int l, cmdlen;
		ipfw_insn *cmd;
		int skip_or; /* skip rest of OR block */

again:
		if (f->reserved_1 == IPFW_RULE_INACTIVE) {
			continue;
		}
		
		if (set_disable & (1 << f->set) )
			continue;

		skip_or = 0;
		for (l = f->cmd_len, cmd = f->cmd ; l > 0 ;
		    l -= cmdlen, cmd += cmdlen) {
			int match;

			/*
			 * check_body is a jump target used when we find a
			 * CHECK_STATE, and need to jump to the body of
			 * the target rule.
			 */

check_body:
			cmdlen = F_LEN(cmd);
			/*
			 * An OR block (insn_1 || .. || insn_n) has the
			 * F_OR bit set in all but the last instruction.
			 * The first match will set "skip_or", and cause
			 * the following instructions to be skipped until
			 * past the one with the F_OR bit clear.
			 */
			if (skip_or) {		/* skip this instruction */
				if ((cmd->len & F_OR) == 0)
					skip_or = 0;	/* next one is good */
				continue;
			}
			match = 0; /* set to 1 if we succeed */

			switch (cmd->opcode) {
			/*
			 * The first set of opcodes compares the packet's
			 * fields with some pattern, setting 'match' if a
			 * match is found. At the end of the loop there is
			 * logic to deal with F_NOT and F_OR flags associated
			 * with the opcode.
			 */
			case O_NOP:
				match = 1;
				break;

			case O_FORWARD_MAC:
				printf("ipfw: opcode %d unimplemented\n",
				    cmd->opcode);
				break;

#ifndef __APPLE__
			case O_GID:
#endif
			case O_UID:
				/*
				 * We only check offset == 0 && proto != 0,
				 * as this ensures that we have an IPv4
				 * packet with the ports info.
				 */
				if (offset!=0)
					break;
					
			    {
				struct inpcbinfo *pi;
				int wildcard;
				struct inpcb *pcb;

				if (proto == IPPROTO_TCP) {
					wildcard = 0;
					pi = &tcbinfo;
				} else if (proto == IPPROTO_UDP) {
					wildcard = 1;
					pi = &udbinfo;
				} else
					break;

				pcb =  (oif) ?
					in_pcblookup_hash(pi,
					    dst_ip, htons(dst_port),
					    src_ip, htons(src_port),
					    wildcard, oif) :
					in_pcblookup_hash(pi,
					    src_ip, htons(src_port),
					    dst_ip, htons(dst_port),
					    wildcard, NULL);

				if (pcb == NULL || pcb->inp_socket == NULL) 
					break;
#if __FreeBSD_version < 500034
#define socheckuid(a,b)	(kauth_cred_getuid((a)->so_cred) != (b))
#endif
				if (cmd->opcode == O_UID) {
					match = 
#ifdef __APPLE__
						(kauth_cred_getuid(pcb->inp_socket->so_cred) == (uid_t)((ipfw_insn_u32 *)cmd)->d[0]);
#else
						!socheckuid(pcb->inp_socket,
						   (uid_t)((ipfw_insn_u32 *)cmd)->d[0]);
#endif
				} 
#ifndef __APPLE__
				else  {
					match = 0;
					kauth_cred_ismember_gid(pcb->inp_socket->so_cred, 
						(gid_t)((ipfw_insn_u32 *)cmd)->d[0], &match);
				}
#endif
				/* release reference on pcb */
				in_pcb_checkstate(pcb, WNT_RELEASE, 0);
				}

			break;

			case O_RECV:
				match = iface_match(m->m_pkthdr.rcvif,
				    (ipfw_insn_if *)cmd);
				break;

			case O_XMIT:
				match = iface_match(oif, (ipfw_insn_if *)cmd);
				break;

			case O_VIA:
				match = iface_match(oif ? oif :
				    m->m_pkthdr.rcvif, (ipfw_insn_if *)cmd);
				break;

			case O_MACADDR2:
				if (args->fwa_eh != NULL) {	/* have MAC header */
					u_int32_t *want = (u_int32_t *)
						((ipfw_insn_mac *)cmd)->addr;
					u_int32_t *mask = (u_int32_t *)
						((ipfw_insn_mac *)cmd)->mask;
					u_int32_t *hdr = (u_int32_t *)args->fwa_eh;

					match =
					    ( want[0] == (hdr[0] & mask[0]) &&
					      want[1] == (hdr[1] & mask[1]) &&
					      want[2] == (hdr[2] & mask[2]) );
				}
				break;

			case O_MAC_TYPE:
				if (args->fwa_eh != NULL) {
					u_int16_t t =
					    ntohs(args->fwa_eh->ether_type);
					u_int16_t *p =
					    ((ipfw_insn_u16 *)cmd)->ports;
					int i;

					for (i = cmdlen - 1; !match && i>0;
					    i--, p += 2)
						match = (t>=p[0] && t<=p[1]);
				}
				break;

			case O_FRAG:
				match = (hlen > 0 && offset != 0);
				break;

			case O_IN:	/* "out" is "not in" */
				match = (oif == NULL);
				break;

			case O_LAYER2:
				match = (args->fwa_eh != NULL);
				break;

			case O_PROTO:
				/*
				 * We do not allow an arg of 0 so the
				 * check of "proto" only suffices.
				 */
				match = (proto == cmd->arg1);
				break;

			case O_IP_SRC:
				match = (hlen > 0 &&
				    ((ipfw_insn_ip *)cmd)->addr.s_addr ==
				    src_ip.s_addr);
				break;

			case O_IP_SRC_MASK:
			case O_IP_DST_MASK:
				if (hlen > 0) {
				    uint32_t a =
					(cmd->opcode == O_IP_DST_MASK) ?
					    dst_ip.s_addr : src_ip.s_addr;
				    uint32_t *p = ((ipfw_insn_u32 *)cmd)->d;
				    int i = cmdlen-1;

				    for (; !match && i>0; i-= 2, p+= 2)
					match = (p[0] == (a & p[1]));
				}
				break;

			case O_IP_SRC_ME:
				if (hlen > 0) {
					struct ifnet *tif;

					INADDR_TO_IFP(src_ip, tif);
					match = (tif != NULL);
				}
				break;

			case O_IP_DST_SET:
			case O_IP_SRC_SET:
				if (hlen > 0) {
					u_int32_t *d = (u_int32_t *)(cmd+1);
					u_int32_t addr =
					    cmd->opcode == O_IP_DST_SET ?
						args->fwa_id.dst_ip :
						args->fwa_id.src_ip;

					    if (addr < d[0])
						    break;
					    addr -= d[0]; /* subtract base */
					    match = (addr < cmd->arg1) &&
						( d[ 1 + (addr>>5)] &
						  (1<<(addr & 0x1f)) );
				}
				break;

			case O_IP_DST:
				match = (hlen > 0 &&
				    ((ipfw_insn_ip *)cmd)->addr.s_addr ==
				    dst_ip.s_addr);
				break;

			case O_IP_DST_ME:
				if (hlen > 0) {
					struct ifnet *tif;

					INADDR_TO_IFP(dst_ip, tif);
					match = (tif != NULL);
				}
				break;

			case O_IP_SRCPORT:
			case O_IP_DSTPORT:
				/*
				 * offset == 0 && proto != 0 is enough
				 * to guarantee that we have an IPv4
				 * packet with port info.
				 */
				if ((proto==IPPROTO_UDP || proto==IPPROTO_TCP)
				    && offset == 0) {
					u_int16_t x =
					    (cmd->opcode == O_IP_SRCPORT) ?
						src_port : dst_port ;
					u_int16_t *p =
					    ((ipfw_insn_u16 *)cmd)->ports;
					int i;

					for (i = cmdlen - 1; !match && i>0;
					    i--, p += 2)
						match = (x>=p[0] && x<=p[1]);
				}
				break;

			case O_ICMPTYPE:
				match = (offset == 0 && proto==IPPROTO_ICMP &&
				    icmptype_match(ip, (ipfw_insn_u32 *)cmd) );
				break;

			case O_IPOPT:
				match = (hlen > 0 && ipopts_match(ip, cmd) );
				break;

			case O_IPVER:
				match = (hlen > 0 && cmd->arg1 == ip->ip_v);
				break;

			case O_IPID:
			case O_IPLEN:
			case O_IPTTL:
				if (hlen > 0) {	/* only for IP packets */
				    uint16_t x;
				    uint16_t *p;
				    int i;

				    if (cmd->opcode == O_IPLEN)
					x = ip_len;
				    else if (cmd->opcode == O_IPTTL)
					x = ip->ip_ttl;
				    else /* must be IPID */
					x = ntohs(ip->ip_id);
				    if (cmdlen == 1) {
					match = (cmd->arg1 == x);
					break;
				    }
				    /* otherwise we have ranges */
				    p = ((ipfw_insn_u16 *)cmd)->ports;
				    i = cmdlen - 1;
				    for (; !match && i>0; i--, p += 2)
					match = (x >= p[0] && x <= p[1]);
				}
				break;

			case O_IPPRECEDENCE:
				match = (hlen > 0 &&
				    (cmd->arg1 == (ip->ip_tos & 0xe0)) );
				break;

			case O_IPTOS:
				match = (hlen > 0 &&
				    flags_match(cmd, ip->ip_tos));
				break;

			case O_TCPFLAGS:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    flags_match(cmd,
					L3HDR(struct tcphdr,ip)->th_flags));
				break;

			case O_TCPOPTS:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    tcpopts_match(ip, cmd));
				break;

			case O_TCPSEQ:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    ((ipfw_insn_u32 *)cmd)->d[0] ==
					L3HDR(struct tcphdr,ip)->th_seq);
				break;

			case O_TCPACK:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    ((ipfw_insn_u32 *)cmd)->d[0] ==
					L3HDR(struct tcphdr,ip)->th_ack);
				break;

			case O_TCPWIN:
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    cmd->arg1 ==
					L3HDR(struct tcphdr,ip)->th_win);
				break;

			case O_ESTAB:
				/* reject packets which have SYN only */
				/* XXX should i also check for TH_ACK ? */
				match = (proto == IPPROTO_TCP && offset == 0 &&
				    (L3HDR(struct tcphdr,ip)->th_flags &
				     (TH_RST | TH_ACK | TH_SYN)) != TH_SYN);
				break;

			case O_LOG:
				if (fw_verbose)
					ipfw_log(f, hlen, args->fwa_eh, m, oif);
				match = 1;
				break;

			case O_PROB:
				match = (random()<((ipfw_insn_u32 *)cmd)->d[0]);
				break;

			case O_VERREVPATH:
				/* Outgoing packets automatically pass/match */
				match = ((oif != NULL) ||
				    (m->m_pkthdr.rcvif == NULL) ||
				    verify_rev_path(src_ip, m->m_pkthdr.rcvif));
				break;

			case O_IPSEC:
#ifdef FAST_IPSEC
				match = (m_tag_find(m,
				    PACKET_TAG_IPSEC_IN_DONE, NULL) != NULL);
#endif
#ifdef IPSEC
				match = (ipsec_gethist(m, NULL) != NULL);
#endif
				/* otherwise no match */
				break;

			/*
			 * The second set of opcodes represents 'actions',
			 * i.e. the terminal part of a rule once the packet
			 * matches all previous patterns.
			 * Typically there is only one action for each rule,
			 * and the opcode is stored at the end of the rule
			 * (but there are exceptions -- see below).
			 *
			 * In general, here we set retval and terminate the
			 * outer loop (would be a 'break 3' in some language,
			 * but we need to do a 'goto done').
			 *
			 * Exceptions:
			 * O_COUNT and O_SKIPTO actions:
			 *   instead of terminating, we jump to the next rule
			 *   ('goto next_rule', equivalent to a 'break 2'),
			 *   or to the SKIPTO target ('goto again' after
			 *   having set f, cmd and l), respectively.
			 *
			 * O_LIMIT and O_KEEP_STATE: these opcodes are
			 *   not real 'actions', and are stored right
			 *   before the 'action' part of the rule.
			 *   These opcodes try to install an entry in the
			 *   state tables; if successful, we continue with
			 *   the next opcode (match=1; break;), otherwise
			 *   the packet *   must be dropped
			 *   ('goto done' after setting retval);
			 *
			 * O_PROBE_STATE and O_CHECK_STATE: these opcodes
			 *   cause a lookup of the state table, and a jump
			 *   to the 'action' part of the parent rule
			 *   ('goto check_body') if an entry is found, or
			 *   (CHECK_STATE only) a jump to the next rule if
			 *   the entry is not found ('goto next_rule').
			 *   The result of the lookup is cached to make
			 *   further instances of these opcodes are
			 *   effectively NOPs.
			 */
			case O_LIMIT:
			case O_KEEP_STATE:
				if (install_state(f,
				    (ipfw_insn_limit *)cmd, args)) {
					retval = IP_FW_PORT_DENY_FLAG;
					goto done; /* error/limit violation */
				}
				match = 1;
				break;

			case O_PROBE_STATE:
			case O_CHECK_STATE:
				/*
				 * dynamic rules are checked at the first
				 * keep-state or check-state occurrence,
				 * with the result being stored in dyn_dir.
				 * The compiler introduces a PROBE_STATE
				 * instruction for us when we have a
				 * KEEP_STATE (because PROBE_STATE needs
				 * to be run first).
				 */
				if (dyn_dir == MATCH_UNKNOWN &&
				    (q = lookup_dyn_rule(&args->fwa_id,
				     &dyn_dir, proto == IPPROTO_TCP ?
					L3HDR(struct tcphdr, ip) : NULL))
					!= NULL) {
					/*
					 * Found dynamic entry, update stats
					 * and jump to the 'action' part of
					 * the parent rule.
					 */
					q->pcnt++;
					q->bcnt += pktlen;
					f = q->rule;
					cmd = ACTION_PTR(f);
					l = f->cmd_len - f->act_ofs;
					goto check_body;
				}
				/*
				 * Dynamic entry not found. If CHECK_STATE,
				 * skip to next rule, if PROBE_STATE just
				 * ignore and continue with next opcode.
				 */
				if (cmd->opcode == O_CHECK_STATE)
					goto next_rule;
				match = 1;
				break;

			case O_ACCEPT:
				retval = 0;	/* accept */
				goto done;

			case O_PIPE:
			case O_QUEUE:
				args->fwa_ipfw_rule = f; /* report matching rule */
				retval = cmd->arg1 | IP_FW_PORT_DYNT_FLAG;
				goto done;

			case O_DIVERT:
			case O_TEE:
				if (args->fwa_eh) /* not on layer 2 */
					break;
				args->fwa_divert_rule = f->rulenum;
				retval = (cmd->opcode == O_DIVERT) ?
				    cmd->arg1 :
				    cmd->arg1 | IP_FW_PORT_TEE_FLAG;
				goto done;

			case O_COUNT:
			case O_SKIPTO:
				f->pcnt++;	/* update stats */
				f->bcnt += pktlen;
				f->timestamp = timenow.tv_sec;
				if (cmd->opcode == O_COUNT)
					goto next_rule;
				/* handle skipto */
				if (f->next_rule == NULL)
					lookup_next_rule(f);
				f = f->next_rule;
				goto again;

			case O_REJECT:
				/*
				 * Drop the packet and send a reject notice
				 * if the packet is not ICMP (or is an ICMP
				 * query), and it is not multicast/broadcast.
				 */
				if (hlen > 0 && offset == 0 &&
				    (proto != IPPROTO_ICMP ||
				     is_icmp_query(ip)) &&
				    !(m->m_flags & (M_BCAST|M_MCAST)) &&
				    !IN_MULTICAST(dst_ip.s_addr)) {
					send_reject(args, cmd->arg1,
					    offset,ip_len);
					m = args->fwa_m;
				}
				/* FALLTHROUGH */
			case O_DENY:
				retval = IP_FW_PORT_DENY_FLAG;
				goto done;

			case O_FORWARD_IP:
				if (args->fwa_eh)	/* not valid on layer2 pkts */
					break;
				if (!q || dyn_dir == MATCH_FORWARD)
					args->fwa_next_hop =
					    &((ipfw_insn_sa *)cmd)->sa;
				retval = 0;
				goto done;

			default:
				panic("-- unknown opcode %d\n", cmd->opcode);
			} /* end of switch() on opcodes */

			if (cmd->len & F_NOT)
				match = !match;

			if (match) {
				if (cmd->len & F_OR)
					skip_or = 1;
			} else {
				if (!(cmd->len & F_OR)) /* not an OR block, */
					break;		/* try next rule    */
			}

		}	/* end of inner for, scan opcodes */

next_rule:;		/* try next rule		*/

	}		/* end of outer for, scan rules */
	printf("ipfw: ouch!, skip past end of rules, denying packet\n");
	lck_mtx_unlock(ipfw_mutex);
	return(IP_FW_PORT_DENY_FLAG);

done:
	/* Update statistics */
	f->pcnt++;
	f->bcnt += pktlen;
	f->timestamp = timenow.tv_sec;
	lck_mtx_unlock(ipfw_mutex);
	return retval;

pullup_failed:
	if (fw_verbose)
		printf("ipfw: pullup failed\n");
	lck_mtx_unlock(ipfw_mutex);
	return(IP_FW_PORT_DENY_FLAG);
}

/*
 * When a rule is added/deleted, clear the next_rule pointers in all rules.
 * These will be reconstructed on the fly as packets are matched.
 * Must be called at splimp().
 */
static void
flush_rule_ptrs(void)
{
	struct ip_fw *rule;

	for (rule = layer3_chain; rule; rule = rule->next)
		rule->next_rule = NULL;
}

/*
 * When pipes/queues are deleted, clear the "pipe_ptr" pointer to a given
 * pipe/queue, or to all of them (match == NULL).
 * Must be called at splimp().
 */
void
flush_pipe_ptrs(struct dn_flow_set *match)
{
	struct ip_fw *rule;

	for (rule = layer3_chain; rule; rule = rule->next) {
		ipfw_insn_pipe *cmd = (ipfw_insn_pipe *)ACTION_PTR(rule);

		if (cmd->o.opcode != O_PIPE && cmd->o.opcode != O_QUEUE)
			continue;
		/*
		 * XXX Use bcmp/bzero to handle pipe_ptr to overcome
		 * possible alignment problems on 64-bit architectures.
		 * This code is seldom used so we do not worry too
		 * much about efficiency.
		 */
		if (match == NULL ||
		    !bcmp(&cmd->pipe_ptr, &match, sizeof(match)) )
			bzero(&cmd->pipe_ptr, sizeof(cmd->pipe_ptr));
	}
}

/*
 * Add a new rule to the list. Copy the rule into a malloc'ed area, then
 * possibly create a rule number and add the rule to the list.
 * Update the rule_number in the input struct so the caller knows it as well.
 */
static int
add_rule(struct ip_fw **head, struct ip_fw *input_rule)
{
	struct ip_fw *rule, *f, *prev;
	int l = RULESIZE(input_rule);

	if (*head == NULL && input_rule->rulenum != IPFW_DEFAULT_RULE)
		return (EINVAL);

	rule = _MALLOC(l, M_IPFW, M_WAIT | M_ZERO);
	if (rule == NULL) {
		printf("ipfw2: add_rule MALLOC failed\n");
		return (ENOSPC);
	}
	
	bcopy(input_rule, rule, l);

	rule->next = NULL;
	rule->next_rule = NULL;

	rule->pcnt = 0;
	rule->bcnt = 0;
	rule->timestamp = 0;

	if (*head == NULL) {	/* default rule */
		*head = rule;
		goto done;
        }

	/*
	 * If rulenum is 0, find highest numbered rule before the
	 * default rule, and add autoinc_step
	 */
	if (autoinc_step < 1)
		autoinc_step = 1;
	else if (autoinc_step > 1000)
		autoinc_step = 1000;
	if (rule->rulenum == 0) {
		/*
		 * locate the highest numbered rule before default
		 */
		for (f = *head; f; f = f->next) {
			if (f->rulenum == IPFW_DEFAULT_RULE)
				break;
			rule->rulenum = f->rulenum;
		}
		if (rule->rulenum < IPFW_DEFAULT_RULE - autoinc_step)
			rule->rulenum += autoinc_step;
		input_rule->rulenum = rule->rulenum;
	}

	/*
	 * Now insert the new rule in the right place in the sorted list.
	 */
	for (prev = NULL, f = *head; f; prev = f, f = f->next) {
		if (f->rulenum > rule->rulenum) { /* found the location */
			if (prev) {
				rule->next = f;
				prev->next = rule;
			} else { /* head insert */
				rule->next = *head;
				*head = rule;
			}
			break;
		}
	}
	flush_rule_ptrs();
done:
	static_count++;
	static_len += l;
	static_len_32 += RULESIZE32(input_rule);
	static_len_64 += RULESIZE64(input_rule);
	DEB(printf("ipfw: installed rule %d, static count now %d\n",
		rule->rulenum, static_count);)
	return (0);
}

/**
 * Free storage associated with a static rule (including derived
 * dynamic rules).
 * The caller is in charge of clearing rule pointers to avoid
 * dangling pointers.
 * @return a pointer to the next entry.
 * Arguments are not checked, so they better be correct.
 * Must be called at splimp().
 */
static struct ip_fw *
delete_rule(struct ip_fw **head, struct ip_fw *prev, struct ip_fw *rule)
{
	struct ip_fw *n;
	int l = RULESIZE(rule);

	n = rule->next;
	remove_dyn_rule(rule, NULL /* force removal */);
	if (prev == NULL)
		*head = n;
	else
		prev->next = n;
	static_count--;
	static_len -= l;
	static_len_32 -= RULESIZE32(rule);
	static_len_64 -= RULESIZE64(rule);

#if DUMMYNET
	if (DUMMYNET_LOADED)
		dn_ipfw_rule_delete(rule);
#endif /* DUMMYNET */
	_FREE(rule, M_IPFW);
	return n;
}

#if DEBUG_INACTIVE_RULES
static void
print_chain(struct ip_fw **chain)
{
	struct ip_fw *rule = *chain;
	
	for (; rule; rule = rule->next) {
		ipfw_insn	*cmd = ACTION_PTR(rule);
		
		printf("ipfw: rule->rulenum = %d\n", rule->rulenum);
		
		if (rule->reserved_1 == IPFW_RULE_INACTIVE) {
			printf("ipfw: rule->reserved = IPFW_RULE_INACTIVE\n");
		}
		
		switch (cmd->opcode) {
			case O_DENY:
				printf("ipfw: ACTION: Deny\n");
				break;
	
			case O_REJECT:
				if (cmd->arg1==ICMP_REJECT_RST)
					printf("ipfw: ACTION: Reset\n");
				else if (cmd->arg1==ICMP_UNREACH_HOST)
					printf("ipfw: ACTION: Reject\n");
				break;
	
			case O_ACCEPT:
				printf("ipfw: ACTION: Accept\n");
				break;
			case O_COUNT:
				printf("ipfw: ACTION: Count\n");
				break;
			case O_DIVERT:
				printf("ipfw: ACTION: Divert\n");
				break;
			case O_TEE:
				printf("ipfw: ACTION: Tee\n");
				break;
			case O_SKIPTO:
				printf("ipfw: ACTION: SkipTo\n");
				break;
			case O_PIPE:
				printf("ipfw: ACTION: Pipe\n");
				break;
			case O_QUEUE:
				printf("ipfw: ACTION: Queue\n");
				break;
			case O_FORWARD_IP:
				printf("ipfw: ACTION: Forward\n");
				break;
			default:
				printf("ipfw: invalid action! %d\n", cmd->opcode);
		}
	}
}
#endif /* DEBUG_INACTIVE_RULES */

static void
flush_inactive(void *param)
{
	struct ip_fw *inactive_rule = (struct ip_fw *)param;
	struct ip_fw *rule, *prev;
	
	lck_mtx_lock(ipfw_mutex);
	
	for (rule = layer3_chain, prev = NULL; rule; ) {
		if (rule == inactive_rule && rule->reserved_1 == IPFW_RULE_INACTIVE) {
			struct ip_fw *n = rule;
			
			if (prev == NULL) {
				layer3_chain = rule->next;
			}
			else {
				prev->next = rule->next;
			}
			rule = rule->next;
			_FREE(n, M_IPFW);
		}
		else {
			prev = rule;
			rule = rule->next;
		}
	}
	
#if DEBUG_INACTIVE_RULES
	print_chain(&layer3_chain);
#endif
	lck_mtx_unlock(ipfw_mutex);
}

static void
mark_inactive(struct ip_fw **prev, struct ip_fw **rule)
{
	int 			l = RULESIZE(*rule);

	if ((*rule)->reserved_1 != IPFW_RULE_INACTIVE) {
		(*rule)->reserved_1 = IPFW_RULE_INACTIVE;
		static_count--;
		static_len -= l;
		static_len_32 -= RULESIZE32(*rule);
		static_len_64 -= RULESIZE64(*rule);
		
		timeout(flush_inactive, *rule, 30*hz); /* 30 sec. */
	}
	
	*prev = *rule;
	*rule = (*rule)->next;
}

/*
 * Deletes all rules from a chain (except rules in set RESVD_SET
 * unless kill_default = 1).
 * Must be called at splimp().
 */
static void
free_chain(struct ip_fw **chain, int kill_default)
{
	struct ip_fw *prev, *rule;

	flush_rule_ptrs(); /* more efficient to do outside the loop */
	for (prev = NULL, rule = *chain; rule ; )
		if (kill_default || rule->set != RESVD_SET) {
			ipfw_insn	*cmd = ACTION_PTR(rule);
			
			/* skip over forwarding rules so struct isn't 
			 * deleted while pointer is still in use elsewhere
			 */
			if (cmd->opcode == O_FORWARD_IP) {
				mark_inactive(&prev, &rule);
			}
			else {
				rule = delete_rule(chain, prev, rule);
			}
		}
		else {
			prev = rule;
			rule = rule->next;
		}
}

/**
 * Remove all rules with given number, and also do set manipulation.
 * Assumes chain != NULL && *chain != NULL.
 *
 * The argument is an u_int32_t. The low 16 bit are the rule or set number,
 * the next 8 bits are the new set, the top 8 bits are the command:
 *
 *	0	delete rules with given number
 *	1	delete rules with given set number
 *	2	move rules with given number to new set
 *	3	move rules with given set number to new set
 *	4	swap sets with given numbers
 */
static int
del_entry(struct ip_fw **chain, u_int32_t arg)
{
	struct ip_fw *prev = NULL, *rule = *chain;
	u_int16_t rulenum;	/* rule or old_set */
	u_int8_t cmd, new_set;

	rulenum = arg & 0xffff;
	cmd = (arg >> 24) & 0xff;
	new_set = (arg >> 16) & 0xff;

	if (cmd > 4)
		return EINVAL;
	if (new_set > RESVD_SET)
		return EINVAL;
	if (cmd == 0 || cmd == 2) {
		if (rulenum >= IPFW_DEFAULT_RULE)
			return EINVAL;
	} else {
		if (rulenum > RESVD_SET)	/* old_set */
			return EINVAL;
	}

	switch (cmd) {
	case 0:	/* delete rules with given number */
		/*
		 * locate first rule to delete
		 */
		for (; rule->rulenum < rulenum; prev = rule, rule = rule->next)
			;
		if (rule->rulenum != rulenum)
			return EINVAL;

		/*
		 * flush pointers outside the loop, then delete all matching
		 * rules. prev remains the same throughout the cycle.
		 */
		flush_rule_ptrs();
		while (rule->rulenum == rulenum) {
			ipfw_insn	*insn = ACTION_PTR(rule);
			
			/* keep forwarding rules around so struct isn't 
			 * deleted while pointer is still in use elsewhere
			 */
			if (insn->opcode == O_FORWARD_IP) {
				mark_inactive(&prev, &rule);
			}
			else {
				rule = delete_rule(chain, prev, rule);
			}
		}
		break;

	case 1:	/* delete all rules with given set number */
		flush_rule_ptrs();
		while (rule->rulenum < IPFW_DEFAULT_RULE) {
			if (rule->set == rulenum) {
				ipfw_insn	*insn = ACTION_PTR(rule);
				
				/* keep forwarding rules around so struct isn't 
				 * deleted while pointer is still in use elsewhere
				 */
				if (insn->opcode == O_FORWARD_IP) {
					mark_inactive(&prev, &rule);
				}
				else {
					rule = delete_rule(chain, prev, rule);
				}
			}
			else {
				prev = rule;
				rule = rule->next;
			}
		}
		break;

	case 2:	/* move rules with given number to new set */
		for (; rule->rulenum < IPFW_DEFAULT_RULE; rule = rule->next)
			if (rule->rulenum == rulenum)
				rule->set = new_set;
		break;

	case 3: /* move rules with given set number to new set */
		for (; rule->rulenum < IPFW_DEFAULT_RULE; rule = rule->next)
			if (rule->set == rulenum)
				rule->set = new_set;
		break;

	case 4: /* swap two sets */
		for (; rule->rulenum < IPFW_DEFAULT_RULE; rule = rule->next)
			if (rule->set == rulenum)
				rule->set = new_set;
			else if (rule->set == new_set)
				rule->set = rulenum;
		break;
	}
	return 0;
}

/*
 * Clear counters for a specific rule.
 */
static void
clear_counters(struct ip_fw *rule, int log_only)
{
	ipfw_insn_log *l = (ipfw_insn_log *)ACTION_PTR(rule);

	if (log_only == 0) {
		rule->bcnt = rule->pcnt = 0;
		rule->timestamp = 0;
	}
	if (l->o.opcode == O_LOG)
		l->log_left = l->max_log;
}

/**
 * Reset some or all counters on firewall rules.
 * @arg frwl is null to clear all entries, or contains a specific
 * rule number.
 * @arg log_only is 1 if we only want to reset logs, zero otherwise.
 */
static int
zero_entry(int rulenum, int log_only)
{
	struct ip_fw *rule;
	const char *msg;

	if (rulenum == 0) {
		norule_counter = 0;
		for (rule = layer3_chain; rule; rule = rule->next)
			clear_counters(rule, log_only);
		msg = log_only ? "ipfw: All logging counts reset.\n" :
				"ipfw: Accounting cleared.\n";
	} else {
		int cleared = 0;
		/*
		 * We can have multiple rules with the same number, so we
		 * need to clear them all.
		 */
		for (rule = layer3_chain; rule; rule = rule->next)
			if (rule->rulenum == rulenum) {
				while (rule && rule->rulenum == rulenum) {
					clear_counters(rule, log_only);
					rule = rule->next;
				}
				cleared = 1;
				break;
			}
		if (!cleared)	/* we did not find any matching rules */
			return (EINVAL);
		msg = log_only ? "ipfw: Entry %d logging count reset.\n" :
				"ipfw: Entry %d cleared.\n";
	}
	if (fw_verbose)
	{
		dolog((LOG_AUTHPRIV | LOG_NOTICE, msg, rulenum));
	}
	return (0);
}

/*
 * Check validity of the structure before insert.
 * Fortunately rules are simple, so this mostly need to check rule sizes.
 */
static int
check_ipfw_struct(struct ip_fw *rule, int size)
{
	int l, cmdlen = 0;
	int have_action=0;
	ipfw_insn *cmd;

	if (size < sizeof(*rule)) {
		printf("ipfw: rule too short\n");
		return (EINVAL);
	}
	/* first, check for valid size */
	l = RULESIZE(rule);
	if (l != size) {
		printf("ipfw: size mismatch (have %d want %d)\n", size, l);
		return (EINVAL);
	}
	/*
	 * Now go for the individual checks. Very simple ones, basically only
	 * instruction sizes.
	 */
	for (l = rule->cmd_len, cmd = rule->cmd ;
			l > 0 ; l -= cmdlen, cmd += cmdlen) {
		cmdlen = F_LEN(cmd);
		if (cmdlen > l) {
			printf("ipfw: opcode %d size truncated\n",
			    cmd->opcode);
			return EINVAL;
		}
		DEB(printf("ipfw: opcode %d\n", cmd->opcode);)
		switch (cmd->opcode) {
		case O_PROBE_STATE:
		case O_KEEP_STATE:
		case O_PROTO:
		case O_IP_SRC_ME:
		case O_IP_DST_ME:
		case O_LAYER2:
		case O_IN:
		case O_FRAG:
		case O_IPOPT:
		case O_IPTOS:
		case O_IPPRECEDENCE:
		case O_IPVER:
		case O_TCPWIN:
		case O_TCPFLAGS:
		case O_TCPOPTS:
		case O_ESTAB:
		case O_VERREVPATH:
		case O_IPSEC:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
			break;
		case O_UID:
#ifndef __APPLE__
		case O_GID:
#endif /* __APPLE__ */
		case O_IP_SRC:
		case O_IP_DST:
		case O_TCPSEQ:
		case O_TCPACK:
		case O_PROB:
		case O_ICMPTYPE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32))
				goto bad_size;
			break;

		case O_LIMIT:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_limit))
				goto bad_size;
			break;

		case O_LOG:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_log))
				goto bad_size;
				
			/* enforce logging limit */
			if (fw_verbose &&
				((ipfw_insn_log *)cmd)->max_log == 0 && verbose_limit != 0) {
				((ipfw_insn_log *)cmd)->max_log = verbose_limit;
			}

			((ipfw_insn_log *)cmd)->log_left =
			    ((ipfw_insn_log *)cmd)->max_log;

			break;

		case O_IP_SRC_MASK:
		case O_IP_DST_MASK:
			/* only odd command lengths */
			if ( !(cmdlen & 1) || cmdlen > 31)
				goto bad_size;
			break;

		case O_IP_SRC_SET:
		case O_IP_DST_SET:
			if (cmd->arg1 == 0 || cmd->arg1 > 256) {
				printf("ipfw: invalid set size %d\n",
					cmd->arg1);
				return EINVAL;
			}
			if (cmdlen != F_INSN_SIZE(ipfw_insn_u32) +
			    (cmd->arg1+31)/32 )
				goto bad_size;
			break;

		case O_MACADDR2:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_mac))
				goto bad_size;
			break;

		case O_NOP:
		case O_IPID:
		case O_IPTTL:
		case O_IPLEN:
			if (cmdlen < 1 || cmdlen > 31)
				goto bad_size;
			break;

		case O_MAC_TYPE:
		case O_IP_SRCPORT:
		case O_IP_DSTPORT: /* XXX artificial limit, 30 port pairs */
			if (cmdlen < 2 || cmdlen > 31)
				goto bad_size;
			break;

		case O_RECV:
		case O_XMIT:
		case O_VIA:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_if))
				goto bad_size;
			break;

		case O_PIPE:
		case O_QUEUE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_pipe))
				goto bad_size;
			goto check_action;

		case O_FORWARD_IP:
			if (cmdlen != F_INSN_SIZE(ipfw_insn_sa))
				goto bad_size;
			goto check_action;

		case O_FORWARD_MAC: /* XXX not implemented yet */
		case O_CHECK_STATE:
		case O_COUNT:
		case O_ACCEPT:
		case O_DENY:
		case O_REJECT:
		case O_SKIPTO:
		case O_DIVERT:
		case O_TEE:
			if (cmdlen != F_INSN_SIZE(ipfw_insn))
				goto bad_size;
check_action:
			if (have_action) {
				printf("ipfw: opcode %d, multiple actions"
					" not allowed\n",
					cmd->opcode);
				return EINVAL;
			}
			have_action = 1;
			if (l != cmdlen) {
				printf("ipfw: opcode %d, action must be"
					" last opcode\n",
					cmd->opcode);
				return EINVAL;
			}
			break;
		default:
			printf("ipfw: opcode %d, unknown opcode\n",
				cmd->opcode);
			return EINVAL;
		}
	}
	if (have_action == 0) {
		printf("ipfw: missing action\n");
		return EINVAL;
	}
	return 0;

bad_size:
	printf("ipfw: opcode %d size %d wrong\n",
		cmd->opcode, cmdlen);
	return EINVAL;
}


static void
ipfw_kev_post_msg(u_int32_t event_code)
{
	struct kev_msg		ev_msg;

	bzero(&ev_msg, sizeof(struct kev_msg));
	
	ev_msg.vendor_code = KEV_VENDOR_APPLE;
	ev_msg.kev_class = KEV_FIREWALL_CLASS;
	ev_msg.kev_subclass = KEV_IPFW_SUBCLASS;
	ev_msg.event_code = event_code;

	kev_post_msg(&ev_msg);

}

/**
 * {set|get}sockopt parser.
 */
static int
ipfw_ctl(struct sockopt *sopt)
{
#define	RULE_MAXSIZE	(256*sizeof(u_int32_t))
	u_int32_t api_version;
	int command;
	int error;
	size_t size;
	size_t	rulesize = RULE_MAXSIZE;
	struct ip_fw *bp , *buf, *rule;
	int	is64user = 0;
	
	/* copy of orig sopt to send to ipfw_get_command_and_version() */
	struct sockopt tmp_sopt = *sopt; 
	struct timeval timenow;

	getmicrotime(&timenow);
	
	/*
	 * Disallow modifications in really-really secure mode, but still allow
	 * the logging counters to be reset.
	 */
	if (sopt->sopt_name == IP_FW_ADD ||
	    (sopt->sopt_dir == SOPT_SET && sopt->sopt_name != IP_FW_RESETLOG)) {
#if __FreeBSD_version >= 500034
		error = securelevel_ge(sopt->sopt_td->td_ucred, 3);
		if (error)
			return (error);
#else /* FreeBSD 4.x */
		if (securelevel >= 3)
			return (EPERM);
#endif
	}

	/* first get the command and version, then do conversion as necessary */
	error = ipfw_get_command_and_version(&tmp_sopt, &command, &api_version);
	if (error) {
		/* error getting the version */
		return error;
	}
	
	if (proc_is64bit(sopt->sopt_p))
		is64user = 1;

	switch (command) {
	case IP_FW_GET:
	{
		size_t	dynrulesize;
		/*
		 * pass up a copy of the current rules. Static rules
		 * come first (the last of which has number IPFW_DEFAULT_RULE),
		 * followed by a possibly empty list of dynamic rule.
		 * The last dynamic rule has NULL in the "next" field.
		 */
		lck_mtx_lock(ipfw_mutex);
						
		if (is64user){
			size = Get64static_len();
			dynrulesize = sizeof(ipfw_dyn_rule_64);
			if (ipfw_dyn_v)
				size += (dyn_count * dynrulesize);
		}else {
			size = Get32static_len();
			dynrulesize = sizeof(ipfw_dyn_rule_32);
			if (ipfw_dyn_v)
				size += (dyn_count * dynrulesize);
		}

		/*
		 * XXX todo: if the user passes a short length just to know
		 * how much room is needed, do not bother filling up the
		 * buffer, just jump to the sooptcopyout.
		 */
		buf = _MALLOC(size, M_TEMP, M_WAITOK | M_ZERO);
		if (buf == 0) {
			lck_mtx_unlock(ipfw_mutex);
			error = ENOBUFS;
			break;
		}

		bp = buf;
		for (rule = layer3_chain; rule ; rule = rule->next) {
	
			if (rule->reserved_1 == IPFW_RULE_INACTIVE) {
				continue;
			}
			
			if (is64user){
				int rulesize_64;

				copyto64fw( rule, (struct ip_fw_64 *)bp, size);
				bcopy(&set_disable, &(( (struct ip_fw_64*)bp)->next_rule), sizeof(set_disable));
				/* do not use macro RULESIZE64 since we want RULESIZE for ip_fw_64 */
				rulesize_64 = sizeof(struct ip_fw_64) + ((struct ip_fw_64 *)(bp))->cmd_len * 4 - 4;
				bp = (struct ip_fw *)((char *)bp + rulesize_64);
			}else{
				int rulesize_32;

				copyto32fw( rule, (struct ip_fw_32*)bp, size);
				bcopy(&set_disable, &(( (struct ip_fw_32*)bp)->next_rule), sizeof(set_disable));
				/* do not use macro RULESIZE32 since we want RULESIZE for ip_fw_32 */
				rulesize_32 = sizeof(struct ip_fw_32) + ((struct ip_fw_32 *)(bp))->cmd_len * 4 - 4;
				bp = (struct ip_fw *)((char *)bp + rulesize_32);
			}
		}
		if (ipfw_dyn_v) {
			int i;
			ipfw_dyn_rule *p;
			char *dst, *last = NULL;
			
			dst = (char *)bp;
			for (i = 0 ; i < curr_dyn_buckets ; i++ )
				for ( p = ipfw_dyn_v[i] ; p != NULL ;
				    p = p->next, dst += dynrulesize ) {
					if ( is64user ){
						ipfw_dyn_rule_64	*ipfw_dyn_dst;
						
						ipfw_dyn_dst = (ipfw_dyn_rule_64 *)dst;
						/*
						 * store a non-null value in "next".
						 * The userland code will interpret a
						 * NULL here as a marker
						 * for the last dynamic rule.
						 */
						ipfw_dyn_dst->next = CAST_DOWN_EXPLICIT(user64_addr_t, dst);
						ipfw_dyn_dst->rule = p->rule->rulenum;
						ipfw_dyn_dst->parent = CAST_DOWN(user64_addr_t, p->parent);
						ipfw_dyn_dst->pcnt = p->pcnt;
						ipfw_dyn_dst->bcnt = p->bcnt;
						externalize_flow_id(&ipfw_dyn_dst->id, &p->id);
						ipfw_dyn_dst->expire =
							TIME_LEQ(p->expire, timenow.tv_sec) ?
							0 : p->expire - timenow.tv_sec;
						ipfw_dyn_dst->bucket = p->bucket;
						ipfw_dyn_dst->state = p->state;
						ipfw_dyn_dst->ack_fwd = p->ack_fwd;
						ipfw_dyn_dst->ack_rev = p->ack_rev;
						ipfw_dyn_dst->dyn_type = p->dyn_type;
						ipfw_dyn_dst->count = p->count;
						last = (char*)ipfw_dyn_dst;
					} else {
						ipfw_dyn_rule_32	*ipfw_dyn_dst;
						
						ipfw_dyn_dst = (ipfw_dyn_rule_32 *)dst;
						/*
						 * store a non-null value in "next".
						 * The userland code will interpret a
						 * NULL here as a marker
						 * for the last dynamic rule.
						 */
						ipfw_dyn_dst->next = CAST_DOWN_EXPLICIT(user32_addr_t, dst);
						ipfw_dyn_dst->rule = p->rule->rulenum;
						ipfw_dyn_dst->parent = CAST_DOWN_EXPLICIT(user32_addr_t, p->parent);
						ipfw_dyn_dst->pcnt = p->pcnt;
						ipfw_dyn_dst->bcnt = p->bcnt;
						externalize_flow_id(&ipfw_dyn_dst->id, &p->id);
						ipfw_dyn_dst->expire =
							TIME_LEQ(p->expire, timenow.tv_sec) ?
							0 : p->expire - timenow.tv_sec;
						ipfw_dyn_dst->bucket = p->bucket;
						ipfw_dyn_dst->state = p->state;
						ipfw_dyn_dst->ack_fwd = p->ack_fwd;
						ipfw_dyn_dst->ack_rev = p->ack_rev;
						ipfw_dyn_dst->dyn_type = p->dyn_type;
						ipfw_dyn_dst->count = p->count;
						last = (char*)ipfw_dyn_dst;
					}
				}
			/* mark last dynamic rule */
			if (last != NULL) {
				if (is64user)
					((ipfw_dyn_rule_64 *)last)->next = 0;
				else
					((ipfw_dyn_rule_32 *)last)->next = 0;
			}
		}
		lck_mtx_unlock(ipfw_mutex);

		/* convert back if necessary and copyout */
		if (api_version == IP_FW_VERSION_0) {
			int	i, len = 0;
			struct ip_old_fw	*buf2, *rule_vers0;
			
			lck_mtx_lock(ipfw_mutex);
			buf2 = _MALLOC(static_count * sizeof(struct ip_old_fw), M_TEMP, M_WAITOK | M_ZERO);
			if (buf2 == 0) {
				lck_mtx_unlock(ipfw_mutex);
				error = ENOBUFS;
			}
			
			if (!error) {
				bp = buf;
				rule_vers0 = buf2;
				
				for (i = 0; i < static_count; i++) {
					/* static rules have different sizes */
					int j = RULESIZE(bp);
					ipfw_convert_from_latest(bp, rule_vers0, api_version, is64user);
					bp = (struct ip_fw *)((char *)bp + j);
					len += sizeof(*rule_vers0);
					rule_vers0++;
				}
				lck_mtx_unlock(ipfw_mutex);
				error = sooptcopyout(sopt, buf2, len);
				_FREE(buf2, M_TEMP);
			}
		} else if (api_version == IP_FW_VERSION_1) {
			int	i, len = 0, buf_size;
			struct ip_fw_compat	*buf2;
			size_t	ipfwcompsize;
			size_t	ipfwdyncompsize;
			char	*rule_vers1;

			lck_mtx_lock(ipfw_mutex);
			if ( is64user ){
				ipfwcompsize = sizeof(struct ip_fw_compat_64);
				ipfwdyncompsize = sizeof(struct ipfw_dyn_rule_compat_64);
			} else {
				ipfwcompsize = sizeof(struct ip_fw_compat_32);
				ipfwdyncompsize = sizeof(struct ipfw_dyn_rule_compat_32);
			}
				
			buf_size = static_count * ipfwcompsize + 
						dyn_count * ipfwdyncompsize;
						
			buf2 = _MALLOC(buf_size, M_TEMP, M_WAITOK | M_ZERO);
			if (buf2 == 0) {
				lck_mtx_unlock(ipfw_mutex);
				error = ENOBUFS;
			}
			if (!error) {
				bp = buf;
				rule_vers1 = (char*)buf2;
				
				/* first do static rules */
				for (i = 0; i < static_count; i++) {
					/* static rules have different sizes */
					if ( is64user ){
						int rulesize_64;
						ipfw_convert_from_latest(bp, (void *)rule_vers1, api_version, is64user);
						rulesize_64 = sizeof(struct ip_fw_64) + ((struct ip_fw_64 *)(bp))->cmd_len * 4 - 4;
						bp = (struct ip_fw *)((char *)bp + rulesize_64);
					}else {
						int rulesize_32;
						ipfw_convert_from_latest(bp, (void *)rule_vers1, api_version, is64user);
						rulesize_32 = sizeof(struct ip_fw_32) + ((struct ip_fw_32 *)(bp))->cmd_len * 4 - 4;
						bp = (struct ip_fw *)((char *)bp + rulesize_32);
					}
					len += ipfwcompsize;
					rule_vers1 += ipfwcompsize;
				}
				/* now do dynamic rules */
				if ( is64user )
					cp_dyn_to_comp_64( (struct ipfw_dyn_rule_compat_64 *)rule_vers1, &len);
				else 
					cp_dyn_to_comp_32( (struct ipfw_dyn_rule_compat_32 *)rule_vers1, &len);

				lck_mtx_unlock(ipfw_mutex);
				error = sooptcopyout(sopt, buf2, len);
				_FREE(buf2, M_TEMP);
			}
		} else {
			error = sooptcopyout(sopt, buf, size);
		}
		
		_FREE(buf, M_TEMP);
		break;
	}
	
	case IP_FW_FLUSH:
		/*
		 * Normally we cannot release the lock on each iteration.
		 * We could do it here only because we start from the head all
		 * the times so there is no risk of missing some entries.
		 * On the other hand, the risk is that we end up with
		 * a very inconsistent ruleset, so better keep the lock
		 * around the whole cycle.
		 *
		 * XXX this code can be improved by resetting the head of
		 * the list to point to the default rule, and then freeing
		 * the old list without the need for a lock.
		 */

		lck_mtx_lock(ipfw_mutex);
		free_chain(&layer3_chain, 0 /* keep default rule */);
		fw_bypass = 1;
#if DEBUG_INACTIVE_RULES
			print_chain(&layer3_chain);
#endif
		lck_mtx_unlock(ipfw_mutex);
		break;

	case IP_FW_ADD:
	{
		size_t savedsopt_valsize=0;
		rule = _MALLOC(RULE_MAXSIZE, M_TEMP, M_WAITOK | M_ZERO);
		if (rule == 0) {
			error = ENOBUFS;
			break;
		}

		if (api_version != IP_FW_CURRENT_API_VERSION) {
			error = ipfw_convert_to_latest(sopt, rule, api_version, is64user);
		}
		else {
			savedsopt_valsize = sopt->sopt_valsize;   /* it might get modified in sooptcopyin_fw */
			error = sooptcopyin_fw( sopt, rule, &rulesize); 

		}
		
		if (!error) {
			if ((api_version == IP_FW_VERSION_0) || (api_version == IP_FW_VERSION_1)) {
				/* the rule has already been checked so just
				 * adjust sopt_valsize to match what would be expected.
				 */
				sopt->sopt_valsize = RULESIZE(rule);
				rulesize = RULESIZE(rule);
			}
			error = check_ipfw_struct(rule, rulesize);
			if (!error) {
				lck_mtx_lock(ipfw_mutex);
				error = add_rule(&layer3_chain, rule);
				if (!error && fw_bypass)
					fw_bypass = 0;
				lck_mtx_unlock(ipfw_mutex);
				
				size = RULESIZE(rule);
				if (!error && sopt->sopt_dir == SOPT_GET) {
					/* convert back if necessary and copyout */
					if (api_version == IP_FW_VERSION_0) {
						struct ip_old_fw	rule_vers0 = {};
						
						ipfw_convert_from_latest(rule, &rule_vers0, api_version, is64user);
						sopt->sopt_valsize = sizeof(struct ip_old_fw);
						
						error = sooptcopyout(sopt, &rule_vers0, sizeof(struct ip_old_fw));
					} else if (api_version == IP_FW_VERSION_1) {
						struct ip_fw_compat	rule_vers1 = {};
						ipfw_convert_from_latest(rule, &rule_vers1, api_version, is64user);
						sopt->sopt_valsize = sizeof(struct ip_fw_compat);
						
						error = sooptcopyout(sopt, &rule_vers1, sizeof(struct ip_fw_compat));
					} else {
						char *userrule;
						userrule = _MALLOC(savedsopt_valsize, M_TEMP, M_WAITOK | M_ZERO);
						if ( userrule == NULL )
							userrule = (char*)rule;
						if (proc_is64bit(sopt->sopt_p)){
							copyto64fw( rule, (struct ip_fw_64*)userrule, savedsopt_valsize);
						}
						else {
								copyto32fw( rule, (struct ip_fw_32*)userrule, savedsopt_valsize);
						}
						error = sooptcopyout(sopt, userrule, savedsopt_valsize);
						if ( userrule )
							_FREE(userrule, M_TEMP);
					}
				}
			}
		}
		
		_FREE(rule, M_TEMP);
		break;
	}
	case IP_FW_DEL:
	{
		/*
		 * IP_FW_DEL is used for deleting single rules or sets,
		 * and (ab)used to atomically manipulate sets. 
		 * rule->rulenum != 0 indicates single rule delete
		 * rule->set_masks used to manipulate sets
		 * rule->set_masks[0] contains info on sets to be 
		 *	disabled, swapped, or moved
		 * rule->set_masks[1] contains sets to be enabled.
		 */
		 
		/* there is only a simple rule passed in
		 * (no cmds), so use a temp struct to copy
		 */
		struct ip_fw	temp_rule;
		u_int32_t	arg;
		u_int8_t	cmd;
		
		bzero(&temp_rule, sizeof(struct ip_fw));
		if (api_version != IP_FW_CURRENT_API_VERSION) {
			error = ipfw_convert_to_latest(sopt, &temp_rule, api_version, is64user);
		}
		else {
			error = sooptcopyin_fw(sopt, &temp_rule, 0 );
		}

		if (!error) {
			/* set_masks is used to distinguish between deleting
			 * single rules or atomically manipulating sets
			 */
			lck_mtx_lock(ipfw_mutex);
			
			arg = temp_rule.set_masks[0];
			cmd = (arg >> 24) & 0xff;
			
			if (temp_rule.rulenum) {
				/* single rule */
				error = del_entry(&layer3_chain, temp_rule.rulenum);
#if DEBUG_INACTIVE_RULES
				print_chain(&layer3_chain);
#endif
			}
			else if (cmd) {
				/* set reassignment - see comment above del_entry() for details */
				error = del_entry(&layer3_chain, temp_rule.set_masks[0]);
#if DEBUG_INACTIVE_RULES
				print_chain(&layer3_chain);
#endif
			}
			else if (temp_rule.set_masks[0] != 0 ||
				temp_rule.set_masks[1] != 0) {
				/* set enable/disable */
				set_disable =
					(set_disable | temp_rule.set_masks[0]) & ~temp_rule.set_masks[1] &
					~(1<<RESVD_SET); /* set RESVD_SET always enabled */
			}
			
			if (!layer3_chain->next)
				fw_bypass = 1;
			lck_mtx_unlock(ipfw_mutex);
		}
		break;
	}
	case IP_FW_ZERO:
	case IP_FW_RESETLOG: /* using rule->rulenum */
	{
		/* there is only a simple rule passed in
		 * (no cmds), so use a temp struct to copy
		 */
		struct ip_fw temp_rule;
		
		bzero(&temp_rule, sizeof(struct ip_fw));
		
		if (api_version != IP_FW_CURRENT_API_VERSION) {
			error = ipfw_convert_to_latest(sopt, &temp_rule, api_version, is64user);
		}
		else {
			if (sopt->sopt_val != 0) {
				error = sooptcopyin_fw( sopt, &temp_rule, 0); 
			}
		}

		if (!error) {
			lck_mtx_lock(ipfw_mutex);
			error = zero_entry(temp_rule.rulenum, sopt->sopt_name == IP_FW_RESETLOG);
			lck_mtx_unlock(ipfw_mutex);
		}
		break;
	}
	default:
		printf("ipfw: ipfw_ctl invalid option %d\n", sopt->sopt_name);
		error = EINVAL;
	}

	if (error != EINVAL) {
		switch (command) {
			case IP_FW_ADD:
			case IP_OLD_FW_ADD:
				ipfw_kev_post_msg(KEV_IPFW_ADD);
				break;
			case IP_OLD_FW_DEL:
			case IP_FW_DEL:
				ipfw_kev_post_msg(KEV_IPFW_DEL);
				break;
			case IP_FW_FLUSH:
			case IP_OLD_FW_FLUSH:
				ipfw_kev_post_msg(KEV_IPFW_FLUSH);
				break;

			default:
				break;
		}
	}

	return (error);
}

/**
 * dummynet needs a reference to the default rule, because rules can be
 * deleted while packets hold a reference to them. When this happens,
 * dummynet changes the reference to the default rule (it could well be a
 * NULL pointer, but this way we do not need to check for the special
 * case, plus here he have info on the default behaviour).
 */
struct ip_fw *ip_fw_default_rule;

/*
 * This procedure is only used to handle keepalives. It is invoked
 * every dyn_keepalive_period
 */
static void
ipfw_tick(__unused void * unused)
{
	struct mbuf *m0, *m, *mnext, **mtailp;
	int i;
	ipfw_dyn_rule *q;
	struct timeval timenow;
	static int stealth_cnt = 0;

	if (ipfw_stealth_stats_needs_flush) {
	    stealth_cnt++;
	    if (!(stealth_cnt % IPFW_STEALTH_TIMEOUT_FREQUENCY)) {
	        ipfw_stealth_flush_stats();
	    }
	}

	if (dyn_keepalive == 0 || ipfw_dyn_v == NULL || dyn_count == 0)
		goto done;

	getmicrotime(&timenow);

	/*
	 * We make a chain of packets to go out here -- not deferring
	 * until after we drop the ipfw lock would result
	 * in a lock order reversal with the normal packet input -> ipfw
	 * call stack.
	 */
	m0 = NULL;
	mtailp = &m0;
	
	lck_mtx_lock(ipfw_mutex);
	for (i = 0 ; i < curr_dyn_buckets ; i++) {
		for (q = ipfw_dyn_v[i] ; q ; q = q->next ) {
			if (q->dyn_type == O_LIMIT_PARENT)
				continue;
			if (q->id.proto != IPPROTO_TCP)
				continue;
			if ( (q->state & BOTH_SYN) != BOTH_SYN)
				continue;
			if (TIME_LEQ( timenow.tv_sec+dyn_keepalive_interval,
			    q->expire))
				continue;	/* too early */
			if (TIME_LEQ(q->expire, timenow.tv_sec))
				continue;	/* too late, rule expired */

			*mtailp = send_pkt(&(q->id), q->ack_rev - 1, q->ack_fwd, TH_SYN);
			if (*mtailp != NULL)
				mtailp = &(*mtailp)->m_nextpkt;

			*mtailp = send_pkt(&(q->id), q->ack_fwd - 1, q->ack_rev, 0);
			if (*mtailp != NULL)
				mtailp = &(*mtailp)->m_nextpkt;
		}
	}
	lck_mtx_unlock(ipfw_mutex);

	for (m = mnext = m0; m != NULL; m = mnext) {
		struct route sro;	/* fake route */

		mnext = m->m_nextpkt;
		m->m_nextpkt = NULL;
		bzero (&sro, sizeof (sro));
		ip_output(m, NULL, &sro, 0, NULL, NULL);
		ROUTE_RELEASE(&sro);
	}
done:
	timeout_with_leeway(ipfw_tick, NULL, dyn_keepalive_period*hz,
	    DYN_KEEPALIVE_LEEWAY*hz);
}

void
ipfw_init(void)
{
	struct ip_fw default_rule;

	/* setup locks */
	ipfw_mutex_grp_attr = lck_grp_attr_alloc_init();
	ipfw_mutex_grp = lck_grp_alloc_init("ipfw", ipfw_mutex_grp_attr);
	ipfw_mutex_attr = lck_attr_alloc_init();
	lck_mtx_init(ipfw_mutex, ipfw_mutex_grp, ipfw_mutex_attr);

	layer3_chain = NULL;

	bzero(&default_rule, sizeof default_rule);

	default_rule.act_ofs = 0;
	default_rule.rulenum = IPFW_DEFAULT_RULE;
	default_rule.cmd_len = 1;
	default_rule.set = RESVD_SET;

	default_rule.cmd[0].len = 1;
	default_rule.cmd[0].opcode =
#ifdef IPFIREWALL_DEFAULT_TO_ACCEPT
				(1) ? O_ACCEPT :
#endif
				O_DENY;

	if (add_rule(&layer3_chain, &default_rule)) {
		printf("ipfw2: add_rule failed adding default rule\n");
		printf("ipfw2 failed initialization!!\n");
		fw_enable = 0;
	}
	else {
		ip_fw_default_rule = layer3_chain;
	
	#ifdef IPFIREWALL_VERBOSE
		fw_verbose = 1;
	#endif
	#ifdef IPFIREWALL_VERBOSE_LIMIT
		verbose_limit = IPFIREWALL_VERBOSE_LIMIT;
	#endif
		if (fw_verbose) {
			if (!verbose_limit)
				printf("ipfw2 verbose logging enabled: unlimited logging by default\n");
			else
				printf("ipfw2 verbose logging enabled: limited to %d packets/entry by default\n",
					verbose_limit);
		}
	}

	ip_fw_chk_ptr = ipfw_chk;
	ip_fw_ctl_ptr = ipfw_ctl;

        ipfwstringlen = strlen( ipfwstring );

	timeout(ipfw_tick, NULL, hz);
}

#endif /* IPFW2 */
