#define KBUILD_MODNAME ""
#include <kheaders.h>
#include <bpf_helpers.h>
#include <bpf_endian.h>
#include <bpf_tracing.h>

#include "shared.h"
#include <skb_utils.h>

#include "kprobe_trace.h"

#define MODE_SKIP_LIFE_MASK (TRACE_MODE_BASIC_MASK | TRACE_MODE_DROP_MASK)
#define TRACE_PREFIX __trace_

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(int));
	__uint(max_entries, TRACE_MAX);
} m_ret SEC(".maps");

#ifdef KERN_VER
__u32 kern_ver SEC("version") = KERN_VER;
#endif

struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__uint(max_entries, 102400);
	__uint(key_size, sizeof(u64));
	__uint(value_size, sizeof(u8));
} m_lookup SEC(".maps");

enum args_status {
	ARGS_END_OFFSET,
	ARGS_STACK_OFFSET,
	ARGS_RET_OFFSET,
	ARGS_RET_ONLY_OFFSET,
};

#define ARGS_END	(1 << ARGS_END_OFFSET)
#define ARGS_STACK	(1 << ARGS_STACK_OFFSET)
#define ARGS_RET	(1 << ARGS_RET_OFFSET)
#define ARGS_RET_ONLY	(1 << ARGS_RET_ONLY_OFFSET)

typedef struct {
	u16	func;
	u16	status;
} args_t;

static try_inline void get_ret(int func)
{
	int *ref = bpf_map_lookup_elem(&m_ret, &func);
	if (!ref)
		return;
	(*ref)++;
}

static try_inline int put_ret(int func)
{
	int *ref = bpf_map_lookup_elem(&m_ret, &func);
	if (!ref || *ref <= 0)
		return 1;
	(*ref)--;
	return 0;
}

static try_inline int handle_entry(void *regs, struct sk_buff *skb, event_t *e,
			       int size, int func)
{
	packet_t *pkt = &e->pkt;
	bool *matched;
	ARGS_INIT();
	u32 pid;

	if (!ARGS_GET(ready))
		return -1;

	pid = (u32)bpf_get_current_pid_tgid();
	if (ARGS_GET(trace_mode) & MODE_SKIP_LIFE_MASK) {
		if (!probe_parse_skb(skb, pkt))
			goto skip_life;
		return -1;
	}

	matched = bpf_map_lookup_elem(&m_lookup, &skb);
	if (matched && *matched) {
		probe_parse_skb_no_filter(skb, pkt);
	} else if (!ARGS_CHECK(pid, pid) && !probe_parse_skb(skb, pkt)) {
		bool _matched = true;
		bpf_map_update_elem(&m_lookup, &skb, &_matched, 0);
	} else {
		return -1;
	}

skip_life:
	if (!ARGS_GET(detail))
		goto out;

	/* store more (detail) information about net or task. */
	struct net_device *dev = _(skb->dev);
	detail_event_t *detail = (void *)e;

	bpf_get_current_comm(detail->task, sizeof(detail->task));
	detail->pid = pid;
	if (dev) {
		bpf_probe_read_str(detail->ifname, sizeof(detail->ifname) - 1,
				   dev->name);
		detail->ifindex = _(dev->ifindex);
	} else {
		detail->ifindex = _(skb->skb_iif);
	}

out:
	pkt->ts = bpf_ktime_get_ns();
	e->key = (u64)(void *)skb;

	if (size)
		bpf_perf_event_output(regs, &m_event, BPF_F_CURRENT_CPU,
				      e, size);
	get_ret(func);
	return 0;
}

static try_inline int handle_destroy(struct sk_buff *skb)
{
	if (!(ARGS_GET_CONFIG(trace_mode) & MODE_SKIP_LIFE_MASK))
		bpf_map_delete_elem(&m_lookup, &skb);
	return 0;
}

static try_inline int default_handle_entry(struct pt_regs *ctx,
				       struct sk_buff *skb,
				       int func)
{
	if (ARGS_GET_CONFIG(detail)) {
		detail_event_t e = { .func = func };
		handle_entry(ctx, skb, (void *)&e, sizeof(e), func);
	} else {
		event_t e = { .func = func };
		handle_entry(ctx, skb, &e, sizeof(e), func);
	}

	if (func == INDEX_consume_skb || func == INDEX___kfree_skb)
		handle_destroy(skb);

	return 0;
}

static try_inline int handle_exit(struct pt_regs *regs, int func)
{
	retevent_t event = {
		.ts = bpf_ktime_get_ns(),
		.func = func,
		.val = PT_REGS_RC(regs),
	};

	if (!ARGS_GET_CONFIG(ready) || put_ret(func))
		return 0;

	if (func == INDEX_skb_clone) {
		bool matched = true;
		bpf_map_update_elem(&m_lookup, &event.val, &matched, 0);
	}

	EVENT_OUTPUT(regs, event);
	return 0;
}

#define DEFINE_KPROBE_RAW(name, skb_init)			\
	static try_inline int fake__##name(struct pt_regs *ctx,	\
				       struct sk_buff *skb,	\
				       int func);		\
	SEC("kretprobe/"#name)					\
	int BPF_KPROBE(ret__trace_##name)			\
	{							\
		return handle_exit(ctx, INDEX_##name);		\
	}							\
	SEC("kprobe/"#name)					\
	int BPF_KPROBE(__trace_##name)				\
	{							\
		struct sk_buff *skb = (void *)skb_init;		\
		return fake__##name(ctx, skb, INDEX_##name);	\
	}							\
	static try_inline int fake__##name(struct pt_regs *ctx,	\
				       struct sk_buff *skb,	\
				       int func)
#define DEFINE_KPROBE(name, skb_index)				\
	DEFINE_KPROBE_RAW(name, PT_REGS_PARM##skb_index(ctx))

#define KPROBE_DEFAULT(name, skb_index)				\
	DEFINE_KPROBE(name, skb_index)				\
	{							\
		return default_handle_entry(ctx, skb, func);	\
	}

#define DEFINE_TP(name, cata, tp, offset)			\
	static try_inline int fake_##name(void *ctx, struct sk_buff *skb,	\
				      int func);		\
	SEC("tp/"#cata"/"#tp)					\
	int __trace_##name(void *ctx) {				\
		struct sk_buff *skb = *(void **)(ctx + offset);	\
		return fake_##name(ctx, skb, INDEX_##name);	\
	}							\
	static try_inline int fake_##name(void *ctx, struct sk_buff *skb,	\
				  int func)
#define TP_DEFAULT(name, cata, tp, offset)			\
	DEFINE_TP(name, cata, tp, offset)			\
	{							\
		return default_handle_entry(ctx, skb, func);	\
	}
_DEFINE_PROBE(KPROBE_DEFAULT, TP_DEFAULT)

struct kfree_skb_args {
	u64 pad;
	void *skb;
	void *location;
	unsigned short protocol;
	int reason;
};

DEFINE_TP(kfree_skb, skb, kfree_skb, 8)
{
	drop_event_t e = { .event = { .func = func } };
	struct kfree_skb_args *args = ctx;

	e.location = (unsigned long)args->location;
	if (ARGS_GET_CONFIG(drop_reason))
		e.reason = _(args->reason);

	handle_entry(ctx, skb, &e.event, sizeof(e), func);
	handle_destroy(skb);
	return 0;
}

DEFINE_KPROBE_RAW(__netif_receive_skb_core_pskb,
		  _(*(void **)(PT_REGS_PARM1(ctx))))
{
	return default_handle_entry(ctx, skb, func);
}

DEFINE_KPROBE(ipt_do_table, 1)
{
	struct nf_hook_state *state = (void *)PT_REGS_PARM2(ctx);
	struct xt_table *table = (void *)PT_REGS_PARM3(ctx);
	nf_event_t e = {
		.event = { .func = func, },
		.hook = _C(state, hook),
	};

	bpf_probe_read(e.table, sizeof(e.table) - 1, _C(table, name));
	return handle_entry(ctx, skb, &e.event, sizeof(e), func);
}

DEFINE_KPROBE(nf_hook_slow, 1)
{
	nf_event_t e = { .event = { .func = func, } };
	struct nf_hook_entries *entries;
	struct nf_hook_state *state;
	int num;

	state = (void *)PT_REGS_PARM2(ctx);
	if (ARGS_GET_CONFIG(hooks))
		goto on_hooks;

	if (handle_entry(ctx, skb, &e.event, 0, func))
		return 0;

	e.hook = _C(state, hook);
	e.pf = _C(state, pf);
	EVENT_OUTPUT(ctx, e);
	return 0;

on_hooks:;
	entries = (void *)PT_REGS_PARM3(ctx);
	nf_hooks_event_t hooks_event = { .event = { .func = func, } };

	if (handle_entry(ctx, skb, &hooks_event.event, 0, func))
		return 0;

	hooks_event.hook = _C(state, hook);
	hooks_event.pf = _C(state, pf);
	num = _(entries->num_hook_entries);

#define COPY_HOOK(i) do {					\
	if (i >= num) goto out;					\
	hooks_event.hooks[i] = (u64)_(entries->hooks[i].hook);	\
} while (0)

	COPY_HOOK(0);
	COPY_HOOK(1);
	COPY_HOOK(2);
	COPY_HOOK(3);
	COPY_HOOK(4);
	COPY_HOOK(5);
	COPY_HOOK(6);
	COPY_HOOK(7);

	/* following code can't unroll, don't know why......:
	 * 
	 * #pragma clang loop unroll(full)
	 * 	for (i = 0; i < 8; i++)
	 * 		COPY_HOOK(i);
	 */
out:
	EVENT_OUTPUT(ctx, hooks_event);
	return 0;
}

DEFINE_KPROBE_RAW(nft_do_chain, NULL)
{
	struct nft_pktinfo *pkt = (void *)PT_REGS_PARM1(ctx);
	nf_event_t e = { .event = { .func = func, } };
	struct nf_hook_state *state;
	struct nft_chain *chain;
	struct nft_table *table;

	skb = (struct sk_buff *)_(pkt->skb);
	if (handle_entry(ctx, skb, &e.event, 0, func))
		return 0;

	if (ARGS_GET_CONFIG(nft_high))
		state = _(((struct _nft_pktinfo_new *)pkt)->state);
	else
		state = _(((struct _nft_pktinfo *)pkt)->xt.state);

	chain	= (void *)PT_REGS_PARM2(ctx);
	table	= _CT(chain, table);
	e.hook	= _C(state, hook);
	e.pf	= _C(state, pf);

	bpf_probe_read_kernel_str(e.chain, sizeof(e.chain),
				  _CT(chain, name));
	bpf_probe_read_kernel_str(e.table, sizeof(e.table),
				  _CT(table, name));

	EVENT_OUTPUT(ctx, e);
	return 0;
}

char _license[] SEC("license") = "GPL";
