#define pr_fmt(fmt) "<fnet> " fmt

#include <linux/cpumask.h>
#include <linux/etherdevice.h>
#include <linux/idr.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/syscalls.h>
#include <linux/timer.h>
#include <linux/sched/clock.h>
#include <uapi/linux/sched/types.h>
#include <asm/host_ops.h>
#include <asm/softirq_stack.h>

#include "core.h"
#include "mbuf.h"
#include "fnet.h"
#include "net.h"

static DEFINE_IDR(fnet_index_idr);

static DEFINE_MUTEX(fnet_mutex);

struct fnet_netdev *fnet_dev = NULL;
DEFINE_PER_CPU_ALIGNED(struct fnet_cpu, fnet_cpus) = { 0 };

#ifdef CONFIG_STAT_FNET
struct fnet_pcpu_stat {
	u64 rx_pkts;
	u64 rx_cycles;
	u64 tx_comp_pkts;
	u64 tx_comp_cycles;
	u64 tx_pkts;
	u64 tx_cycles;
};
DEFINE_PER_CPU_ALIGNED(struct fnet_pcpu_stat, fnet_pcpu_stats);
#define fnet_stat_start() u64 __start = sched_clock();
#define fnet_stat_end(field)                        \
	this_cpu_inc(fnet_pcpu_stats.field##_pkts); \
	this_cpu_add(fnet_pcpu_stats.field##_cycles, sched_clock() - __start);
#else
#define fnet_stat_start() \
	do {              \
	} while (0)
#define fnet_stat_end(field) \
	do {                 \
	} while (0)
#endif

static int fnet_open(struct net_device *dev)
{
#ifdef CONFIG_FLUX_FAST_NET
	flux_tcp_init();
	flux_trans_init();
#endif

	netif_tx_start_all_queues(dev);
	netif_carrier_on(dev);

	pr_info("netdev %s opened\n", dev->name);

	return 0;
}

static int fnet_close(struct net_device *dev)
{
	netif_carrier_off(dev);
	netif_tx_stop_all_queues(dev);

	pr_info("netdev %s closed\n", dev->name);

	return 0;
}

static u16 skb_ip_proto(struct sk_buff *skb)
{
	return (ip_hdr(skb)->version == 4) ? ip_hdr(skb)->protocol :
					     ipv6_hdr(skb)->nexthdr;
}

static void fnet_release_copied_skb_mbuf(struct mbuf *m)
{
	struct sk_buff *skb = (struct sk_buff *)(uintptr_t)m->timestamp;

	net_tx_release_mbuf(m);
	dev_kfree_skb_any(skb);
}

static bool fnet_xmit_skb(struct fnet_cpu *cpu, struct sk_buff *skb)
{
	union flux_txpktq_cmd cmd;
	struct mbuf *m;
	unsigned char *dst;
	uint64_t payload;

	cmd.dst_ip = ip_hdr(skb)->daddr;
	cmd.len = skb->len;
	cmd.txcmd = FLUX_TXPKT_NET_XMIT;
	cmd.olflags = 0;

	if (skb->ip_summed == CHECKSUM_PARTIAL) {
		unsigned int protocol = skb_ip_proto(skb);

		if (ip_hdr(skb)->version == 4)
			cmd.olflags |= FLUX_OLFLAG_IPV4 | FLUX_OLFLAG_IP_CHKSUM;
		else
			cmd.olflags |= FLUX_OLFLAG_IPV6;

		if (protocol == IPPROTO_TCP || protocol == IPPROTO_UDP)
			cmd.olflags |= FLUX_OLFLAG_L3_CHKSUM;
	}

	m = net_tx_alloc_mbuf(0);
	if (unlikely(!m))
		return false;

	if (unlikely(mbuf_tailroom(m) < skb->len)) {
		pr_warn_ratelimited(
			"tx: skb len %u exceeds shared mbuf tailroom %u\n",
			skb->len, mbuf_tailroom(m));
		mbuf_free(m);
		return false;
	}

	dst = mbuf_put(m, skb->len);
	memcpy(dst, skb->data, skb->len);
	m->tx_dst_ip = cmd.dst_ip;
	m->txflags = cmd.olflags;
	m->hash = skb->hash;
	m->timestamp = (uint64_t)(uintptr_t)skb;
	m->release = fnet_release_copied_skb_mbuf;

	payload = flux_txpkt_payload_from_ptr((uint64_t)m, FLUX_MEMORY_ADDR,
					      (uint16_t)skb->hash);

	if (likely(lrpc_send(&cpu->txpktq, cmd.lrpc_cmd, payload)))
		return true;

	m->timestamp = 0;
	m->release = net_tx_release_mbuf;
	mbuf_free(m);
	return false;
}

static bool fnet_xmit_drain_ofq(struct fnet_cpu *cpu)
{
	struct sk_buff *skb;

	while ((skb = skb_peek(&cpu->txofq_skb)) != NULL) {
		if (unlikely(!fnet_xmit_skb(cpu, skb)))
			return false;
		skb_dequeue(&cpu->txofq_skb);
	}
	return true;
}

static bool fnet_prepare_tx_skb(struct sk_buff *skb)
{
	if (unlikely(skb->data_len != 0 || skb_shinfo(skb)->nr_frags != 0)) {
		pr_warn("tx: fragmented skb not supported\n");
		return false;
	}

	if (unlikely(skb->len > FLUX_FNET_MTU + ETH_HLEN)) {
		pr_warn_ratelimited("tx: oversized skb len %u not supported\n",
				    skb->len);
		return false;
	}

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return true;

	if (skb_ip_proto(skb) == IPPROTO_TCP ||
	    skb_ip_proto(skb) == IPPROTO_UDP)
		return true;

	return skb_checksum_help(skb) == 0;
}

/**
 * fnet_start_xmit - Start packet transmission for the fnet netdev
 *
 * No lock is needed because we use per-CPU queues.
 */
static netdev_tx_t fnet_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct fnet_cpu *cpu = this_cpu_ptr(&fnet_cpus);

	fnet_stat_start();
	if (unlikely(!fnet_prepare_tx_skb(skb))) {
		dev_kfree_skb_any(skb);
		goto out;
	}

	if (unlikely(!skb_queue_empty(&cpu->txofq_skb))) {
		if (!fnet_xmit_drain_ofq(cpu)) {
			skb_queue_tail(&cpu->txofq_skb, skb);
			goto out;
		}
	}

	if (unlikely(!fnet_xmit_skb(cpu, skb)))
		skb_queue_tail(&cpu->txofq_skb, skb);

out:
	fnet_stat_end(tx);
	return NETDEV_TX_OK;
}

static u16 fnet_select_queue(struct net_device *dev, struct sk_buff *skb,
			     struct net_device *sb_dev)
{
	return smp_processor_id();
}

struct net_device_ops fnet_netdev_ops = {
	.ndo_open = fnet_open,
	.ndo_stop = fnet_close,
	.ndo_start_xmit = fnet_start_xmit,
	.ndo_select_queue = fnet_select_queue,
};

static inline void fnet_send_comp(struct fnet_cpu *cpu, unsigned long offset)
{
	union flux_txcmdq_cmd cmd = { .txcmd = FLUX_TXCMD_NET_COMP };

	if (!lrpc_send(&cpu->txcmdq, cmd.lrpc_cmd, offset))
		pr_warn("fnet_send_comp: lrpc_send failed\n");
}

static struct sk_buff *fnet_build_rx_skb(struct fnet_cpu *cpu,
					 union flux_rxq_cmd cmd,
					 unsigned long offset)
{
	struct sk_buff *skb = NULL;
	struct net_device *dev = cpu->fnet->dev;
	void *src;

	src = fnet_rx_data_from_offset(cpu->fnet, offset, cmd.len);
	if (unlikely(!src)) {
		pr_warn_ratelimited("rx: invalid shared offset %#lx\n", offset);
		goto out;
	}

	skb = __netdev_alloc_skb(dev, cmd.len, GFP_ATOMIC);
	if (unlikely(!skb))
		goto out;

	memcpy(skb->data, src, cmd.len);

	skb->len = cmd.len; // don't set skb->data_len
	skb_set_tail_pointer(skb, cmd.len);
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = cmd.csum_type == FLUX_CHKSUM_TYPE_UNNECESSARY ?
				 CHECKSUM_UNNECESSARY :
				 CHECKSUM_NONE;

out:
	fnet_send_comp(cpu, offset);
	return skb;
}

static int fnet_rx_poll(struct fnet_cpu *cpu, int budget)
{
	union flux_rxq_cmd cmd;
	unsigned long payload;
	int i = 0;

	while (!budget || i < budget) {
		if (!lrpc_recv(&cpu->rxq, &cmd.lrpc_cmd, &payload))
			break;

		switch (cmd.rxcmd) {
		case FLUX_RX_NET_RECV: {
			struct sk_buff *skb;

			fnet_stat_start();

#ifdef CONFIG_FLUX_FAST_NET
			if (flux_fast_net_rx_recv(payload, cmd)) {
				fnet_stat_end(rx);
				continue;
			}
#endif
			payload = flux_rxq_payload_to_rx_offset(payload);
			skb = fnet_build_rx_skb(cpu, cmd, payload);
			if (unlikely(!skb))
				continue;

			/*
			 * The Linux receive stack normally runs from NET_RX softirq
			 * context. Keep timer softirqs from re-entering TCP while this
			 * polling kthread holds a socket's BH lock.
			 */
			local_bh_disable();
			netif_receive_skb(skb);
			local_bh_enable();

			fnet_stat_end(rx);
			break;
		}
		case FLUX_RX_NET_COMP: {
			fnet_stat_start();
			mbuf_free((struct mbuf *)(uintptr_t)
					  flux_txpkt_ptr_from_payload(
						  payload, FLUX_MEMORY_ADDR));
			fnet_stat_end(tx_comp);
			break;
		}
		default:
			pr_err("unknown rx cmd %u\n", cmd.rxcmd);
			break;
		}

		i++;
	}

	return i;
}

#ifdef CONFIG_FLUX_FAST_NET_EPOLL
void flux_fast_net_poll(void)
{
	struct fnet_cpu *cpu;

	if (unlikely(!READ_ONCE(fnet_dev)))
		return;

	migrate_disable();
	cpu = this_cpu_ptr(&fnet_cpus);
	if (likely(READ_ONCE(cpu->init))) {
		while (fnet_rx_poll(cpu, 0))
			;
	}
	migrate_enable();
}
#endif

static int fnet_rx_thread(void *data)
{
	struct fnet_cpu *cpu = data;
#ifdef CONFIG_FLUX_FNET_RX_SCHED_IDLE
	struct sched_param param = { .sched_priority = 0 };

	sched_setscheduler(current, SCHED_IDLE, &param);
#endif

	while (!kthread_should_stop()) {
		while (fnet_rx_poll(cpu, 0))
			;
		schedule();
	}

	return 0;
}

static int fnet_start_queues(struct fnet_netdev *fnet,
			     struct flux_fnet_netdev *arg)
{
	int err, i;
	struct fnet_cpu *cpu;

	if (arg->nb_rx_queues != NR_CPUS || arg->nb_tx_queues != NR_CPUS) {
		pr_err("fnet mode requires nb_rx_queues and nb_tx_queues to be %d\n",
		       NR_CPUS);
		return -EINVAL;
	}

	for (i = 0; i < NR_CPUS; i++) {
		cpu = fnet->cpus[i] = &per_cpu(fnet_cpus, i);
		cpu->fnet = fnet;
		skb_queue_head_init(&cpu->txofq_skb);
		mbufq_init(&cpu->txofq_mbuf);

		lrpc_init_in(&cpu->rxq, (struct lrpc_msg *)arg->rxqs[i].tbl,
			     arg->rxqs[i].size, arg->rxqs[i].wb);

		lrpc_init_out(&cpu->txpktq,
			      (struct lrpc_msg *)arg->txpktqs[i].tbl,
			      arg->txpktqs[i].size, arg->txpktqs[i].wb);
		lrpc_init_out(&cpu->txcmdq,
			      (struct lrpc_msg *)arg->txcmdqs[i].tbl,
			      arg->txcmdqs[i].size, arg->txcmdqs[i].wb);

		cpu->thread = kthread_run_on_cpu(fnet_rx_thread, cpu, i,
						 "fnet-rx/%d");
		if (IS_ERR(cpu->thread)) {
			err = PTR_ERR(cpu->thread);
			cpu->thread = NULL;
			goto out_free_cpus;
		}
		cpu->init = true;
	}

	return 0;

out_free_cpus:
	for (i = 0; i < NR_CPUS; i++) {
		cpu = fnet->cpus[i];
		if (!cpu)
			continue;
		if (cpu->thread)
			kthread_stop(cpu->thread);
		cpu->thread = NULL;
		cpu->init = false;
	}
	return err;
}

static int fnet_create_netdev(struct flux_fnet_netdev *arg)
{
	struct net_device *netdev;
	struct fnet_netdev *fnet;
	int err;
	int idx;
	netdev_features_t features;

	if (arg->mode != FLUX_FNET_MODE_KERNEL) {
		pr_err("driver only supports IOK mode\n");
		return -EINVAL;
	}

	netdev = alloc_etherdev_mqs(sizeof(struct fnet_netdev),
				    arg->nb_tx_queues, arg->nb_rx_queues);
	if (!netdev)
		return -ENOMEM;

	fnet = (struct fnet_netdev *)netdev_priv(netdev);

	err = idr_alloc(&fnet_index_idr, fnet, 0, 0, GFP_KERNEL);
	if (err < 0)
		goto out_free_netdev;
	idx = err;

	/* setup netdev */
	sprintf(netdev->name, "fnet%d", idx);

	/* setup netdev MAC address */
	ether_addr_copy((void *)netdev->dev_addr, arg->eth_addr);
	ether_addr_copy(netdev->perm_addr, netdev->dev_addr);
	ether_addr_copy(netdev->dev_addr_shadow, netdev->dev_addr);
	if (arg->mtu)
		netdev->mtu = arg->mtu;

	features = NETIF_F_HIGHDMA;
	features |= NETIF_F_LLTX; /* no lock for tx queues */
	features |= NETIF_F_RXHASH | NETIF_F_RXCSUM; /* default rx offloads */
	/* can we enable more offloads? */
	// features |= NETIF_F_SG;
	// features |= NETIF_F_GRO;
	if (arg->csum_offload)
		features |= NETIF_F_HW_CSUM;
	if (arg->gso_offload)
		features |= NETIF_F_TSO | NETIF_F_TSO6;
	netdev->features |= features;
	netdev->hw_features |= features;
	netdev->hw_enc_features |= features;

	netdev->netdev_ops = &fnet_netdev_ops;

	/* we don't use pfifo_fast! */
	netdev->priv_flags |= IFF_NO_QUEUE;

	err = register_netdev(netdev);
	if (err)
		goto out_free_idr;

	fnet->id = idx;
	memcpy(&fnet->arg, arg, sizeof(*arg));
	fnet->dev = netdev;

	fnet_dev = fnet;

#ifdef CONFIG_FLUX_FAST_NET
	ether_addr_copy(fnet->fast_net_mac, arg->eth_addr);
	fnet->fast_net_ip = arg->addr;
	fnet->fast_net_netmask = arg->netmask;
	fnet->fast_net_gateway = arg->gateway;
	if (!fnet->fast_net_ip || !fnet->fast_net_netmask ||
	    !fnet->fast_net_gateway) {
		pr_err("invalid ip address/netmask/gateway\n");
		err = -EINVAL;
		goto out_unregister;
	}
#endif

	err = fnet_start_queues(fnet, arg);
	if (err)
		goto out_unregister;

	pr_info("added netdev %d:%s for port %d\n", netdev->ifindex,
		netdev->name, arg->port_id);
	pr_info("  mac=%pM mtu=%d\n", netdev->dev_addr, netdev->mtu);
	pr_info("  rxqs=%d txqs=%d\n", arg->nb_rx_queues, arg->nb_tx_queues);

	return netdev->ifindex;
out_unregister:
	unregister_netdev(netdev);
	fnet_dev = NULL;
out_free_idr:
	idr_remove(&fnet_index_idr, idx);
out_free_netdev:
	free_netdev(netdev);
	return err;
}

void fnet_destroy_netdev(struct fnet_netdev *fnet)
{
	struct fnet_cpu *cpu;
	int i;

	pr_info("removing netdev %s\n", fnet->dev->name);

	unregister_netdev(fnet->dev);

	for (i = 0; i < NR_CPUS; i++) {
		cpu = fnet->cpus[i];
		if (!cpu || !cpu->thread)
			continue;
		kthread_stop(cpu->thread);
		cpu->thread = NULL;
		cpu->init = false;
	}

	if (fnet_dev == fnet)
		fnet_dev = NULL;
	idr_remove(&fnet_index_idr, fnet->id);
	free_netdev(fnet->dev);
}

static long fnet_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	long ret = -ENOSYS;
	struct flux_fnet_netdev netdev_arg;

	switch (cmd) {
	case FLUX_FNET_IOCTL_ADD:
		mutex_lock(&fnet_mutex);
		if (copy_from_user(&netdev_arg, (void __user *)arg,
				   sizeof(netdev_arg)))
			ret = -EFAULT;
		else
			ret = fnet_create_netdev(&netdev_arg);
		mutex_unlock(&fnet_mutex);
		break;
	default:
		pr_err("unknown ioctl %u\n", cmd);
		break;
	}

	return ret;
}

static const struct file_operations fnet_fops = {
	.open = nonseekable_open,
	.unlocked_ioctl = fnet_ioctl,
	.compat_ioctl = fnet_ioctl,
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
};

static struct miscdevice fnet_misc = {
	.name = "fnet-control",
	.minor = MISC_DYNAMIC_MINOR,
	.fops = &fnet_fops,
};

#ifdef CONFIG_STAT_FNET

static struct timer_list stat_timer;

static void fnet_stat_timer(struct timer_list *t)
{
	int cpu;

	pr_info("fnet stats:\n");

	for_each_possible_cpu(cpu) {
		struct fnet_pcpu_stat *stat = &per_cpu(fnet_pcpu_stats, cpu);
		pr_info(" CPU %d: TX pkts=%llu RX pkts=%llu\n", cpu,
			stat->tx_pkts, stat->rx_pkts);
		/* reset stats */
		stat->rx_pkts = 0;
		stat->rx_cycles = 0;
		stat->tx_comp_pkts = 0;
		stat->tx_comp_cycles = 0;
		stat->tx_pkts = 0;
		stat->tx_cycles = 0;
	}

	pr_info("Memory stats: free %luMB\n",
		global_zone_page_state(NR_FREE_PAGES) * 4 / 1024);

	mod_timer(t, jiffies + msecs_to_jiffies(1000));
}
#endif

static int __init fnet_init(void)
{
	int err;

	err = misc_register(&fnet_misc);
	if (err < 0)
		goto out;

#ifdef CONFIG_STAT_FNET
	timer_setup(&stat_timer, fnet_stat_timer, 0);
	mod_timer(&stat_timer, jiffies + msecs_to_jiffies(1000));
#endif

	pr_info("module loaded minor %d\n", fnet_misc.minor);

out:
	return 0;
}

static void fnet_exit(void)
{
	misc_deregister(&fnet_misc);
}

static void __exit _fnet_exit(void)
{
	fnet_exit();
}

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Flux fnet net device driver");

module_init(fnet_init);
module_exit(_fnet_exit);
