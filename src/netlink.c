/* Copyright (C) 2015-2017 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved. */

#include "netlink.h"
#include "device.h"
#include "peer.h"
#include "socket.h"
#include "queueing.h"
#include "messages.h"
#include "uapi/wireguard.h"
#include <linux/if.h>
#include <net/genetlink.h>
#include <net/sock.h>

static struct genl_family genl_family;

static const struct nla_policy device_policy[WGDEVICE_A_MAX + 1] = {
	[WGDEVICE_A_IFINDEX]	= { .type = NLA_U32 },
	[WGDEVICE_A_IFNAME]	= { .type = NLA_NUL_STRING, .len = IFNAMSIZ - 1 },
	[WGDEVICE_A_PRIVATE_KEY]= { .len = NOISE_PUBLIC_KEY_LEN },
	[WGDEVICE_A_PUBLIC_KEY]	= { .len = NOISE_PUBLIC_KEY_LEN },
	[WGDEVICE_A_FLAGS]	= { .type = NLA_U32 },
	[WGDEVICE_A_LISTEN_PORT]= { .type = NLA_U16 },
	[WGDEVICE_A_FWMARK]	= { .type = NLA_U32 },
	[WGDEVICE_A_PEERS]	= { .type = NLA_NESTED }
};

static const struct nla_policy peer_policy[WGPEER_A_MAX + 1] = {
	[WGPEER_A_PUBLIC_KEY]			= { .len = NOISE_PUBLIC_KEY_LEN },
	[WGPEER_A_PRESHARED_KEY]		= { .len = NOISE_SYMMETRIC_KEY_LEN },
	[WGPEER_A_FLAGS]			= { .type = NLA_U32 },
	[WGPEER_A_ENDPOINT]			= { .len = sizeof(struct sockaddr) },
	[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]= { .type = NLA_U16 },
	[WGPEER_A_LAST_HANDSHAKE_TIME]		= { .len = sizeof(struct timeval) },
	[WGPEER_A_RX_BYTES]			= { .type = NLA_U64 },
	[WGPEER_A_TX_BYTES]			= { .type = NLA_U64 },
	[WGPEER_A_ALLOWEDIPS]			= { .type = NLA_NESTED }
};

static const struct nla_policy allowedip_policy[WGALLOWEDIP_A_MAX + 1] = {
	[WGALLOWEDIP_A_FAMILY]		= { .type = NLA_U16 },
	[WGALLOWEDIP_A_IPADDR]		= { .len = sizeof(struct in_addr) },
	[WGALLOWEDIP_A_CIDR_MASK]	= { .type = NLA_U8 }
};

static struct wireguard_device *lookup_interface(struct nlattr **attrs, struct sk_buff *skb)
{
	struct net_device *dev = NULL;

	if (!attrs[WGDEVICE_A_IFINDEX] == !attrs[WGDEVICE_A_IFNAME])
		return ERR_PTR(-EBADR);
	if (attrs[WGDEVICE_A_IFINDEX])
		dev = dev_get_by_index(sock_net(skb->sk), nla_get_u32(attrs[WGDEVICE_A_IFINDEX]));
	else if (attrs[WGDEVICE_A_IFNAME])
		dev = dev_get_by_name(sock_net(skb->sk), nla_data(attrs[WGDEVICE_A_IFNAME]));
	if (!dev)
		return ERR_PTR(-ENODEV);
	if (!dev->rtnl_link_ops || !dev->rtnl_link_ops->kind || strcmp(dev->rtnl_link_ops->kind, KBUILD_MODNAME)) {
		dev_put(dev);
		return ERR_PTR(-EOPNOTSUPP);
	}
	return netdev_priv(dev);
}

struct allowedips_ctx {
	struct sk_buff *skb;
	unsigned int idx_cursor, idx;
};

static int get_allowedips(void *ctx, union nf_inet_addr ip, u8 cidr, int family)
{
	struct nlattr *allowedip_nest;
	struct allowedips_ctx *actx = ctx;
	if (++actx->idx < actx->idx_cursor)
		return 0;
	allowedip_nest = nla_nest_start(actx->skb, actx->idx - 1);
	if (!allowedip_nest)
		return -EMSGSIZE;

	if (nla_put_u8(actx->skb, WGALLOWEDIP_A_CIDR_MASK, cidr) || nla_put_u16(actx->skb, WGALLOWEDIP_A_FAMILY, family) ||
	    nla_put(actx->skb, WGALLOWEDIP_A_IPADDR, family == AF_INET6 ? sizeof(struct in6_addr) : sizeof(struct in_addr), &ip)) {
		nla_nest_cancel(actx->skb, allowedip_nest);
		return -EMSGSIZE;
	}

	nla_nest_end(actx->skb, allowedip_nest);
	return 0;
}

static int get_peer(struct wireguard_peer *peer, unsigned int index, unsigned int *allowedips_idx_cursor, struct sk_buff *skb)
{
	struct allowedips_ctx ctx = { .skb = skb, .idx_cursor = *allowedips_idx_cursor };
	struct nlattr *allowedips_nest, *peer_nest = nla_nest_start(skb, index);
	bool fail;
	if (!peer_nest)
		return -EMSGSIZE;

	down_read(&peer->handshake.lock);
	fail = nla_put(skb, WGPEER_A_PUBLIC_KEY, NOISE_PUBLIC_KEY_LEN, peer->handshake.remote_static);
	up_read(&peer->handshake.lock);
	if (fail)
		goto err;

	if (!ctx.idx_cursor) {
		down_read(&peer->handshake.lock);
		fail = nla_put(skb, WGPEER_A_PRESHARED_KEY, NOISE_SYMMETRIC_KEY_LEN, peer->handshake.preshared_key);
		up_read(&peer->handshake.lock);
		if (fail)
			goto err;

		if (nla_put(skb, WGPEER_A_LAST_HANDSHAKE_TIME, sizeof(struct timeval), &peer->walltime_last_handshake) || nla_put_u16(skb, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL, peer->persistent_keepalive_interval / HZ) ||
				nla_put_u64_64bit(skb, WGPEER_A_TX_BYTES, peer->tx_bytes, WGPEER_A_UNSPEC) || nla_put_u64_64bit(skb, WGPEER_A_RX_BYTES, peer->rx_bytes, WGPEER_A_UNSPEC))
			goto err;

		read_lock_bh(&peer->endpoint_lock);
		if (peer->endpoint.addr.sa_family == AF_INET)
			fail = nla_put(skb, WGPEER_A_ENDPOINT, sizeof(struct sockaddr_in), &peer->endpoint.addr4);
		else if (peer->endpoint.addr.sa_family == AF_INET6)
			fail = nla_put(skb, WGPEER_A_ENDPOINT, sizeof(struct sockaddr_in6), &peer->endpoint.addr6);
		read_unlock_bh(&peer->endpoint_lock);
		if (fail)
			goto err;
	}

	allowedips_nest = nla_nest_start(skb, WGPEER_A_ALLOWEDIPS);
	if (!allowedips_nest)
		goto err;
	if (routing_table_walk_ips_by_peer_sleepable(&peer->device->peer_routing_table, &ctx, peer, get_allowedips)) {
		*allowedips_idx_cursor = ctx.idx;
		nla_nest_end(skb, allowedips_nest);
		nla_nest_end(skb, peer_nest);
		return -EMSGSIZE;
	}
	*allowedips_idx_cursor = 0;
	nla_nest_end(skb, allowedips_nest);
	nla_nest_end(skb, peer_nest);
	return 0;
err:
	nla_nest_cancel(skb, peer_nest);
	return -EMSGSIZE;
}

static int get_start(struct netlink_callback *cb)
{
	struct wireguard_device *wg;
	struct nlattr **attrs = genl_family_attrbuf(&genl_family);
	int ret = nlmsg_parse(cb->nlh, GENL_HDRLEN + genl_family.hdrsize, attrs, genl_family.maxattr, device_policy, NULL);
	if (ret < 0)
		return ret;
	wg = lookup_interface(attrs, cb->skb);
	if (IS_ERR(wg))
		return PTR_ERR(wg);
	cb->args[0] = (long)wg;
	return 0;
}

static int get(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct wireguard_device *wg = (struct wireguard_device *)cb->args[0];
	struct wireguard_peer *peer, *next_peer_cursor = NULL, *last_peer_cursor = (struct wireguard_peer *)cb->args[1];
	unsigned int peer_idx = 0, allowedips_idx_cursor = (unsigned int)cb->args[2];
	struct nlattr *peers_nest;
	bool done = true;
	void *hdr;
	int ret = -EMSGSIZE;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
	if (!wg) {
		ret = get_start(cb);
		if (ret)
			return ret;
		return get(skb, cb);
	}
#endif

	rtnl_lock();
	mutex_lock(&wg->device_update_lock);
	cb->seq = wg->device_update_gen;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq, &genl_family, NLM_F_MULTI, WG_CMD_GET_DEVICE);
	if (!hdr)
		goto out;
	genl_dump_check_consistent(cb, hdr, &genl_family);

	if (!last_peer_cursor) {
		if (nla_put_u16(skb, WGDEVICE_A_LISTEN_PORT, wg->incoming_port) || nla_put_u32(skb, WGDEVICE_A_FWMARK, wg->fwmark) || nla_put_u32(skb, WGDEVICE_A_IFINDEX, wg->dev->ifindex) || nla_put_string(skb, WGDEVICE_A_IFNAME, wg->dev->name))
			goto out;

		down_read(&wg->static_identity.lock);
		if (wg->static_identity.has_identity) {
			if (nla_put(skb, WGDEVICE_A_PRIVATE_KEY, NOISE_PUBLIC_KEY_LEN, wg->static_identity.static_private) || nla_put(skb, WGDEVICE_A_PUBLIC_KEY, NOISE_PUBLIC_KEY_LEN, wg->static_identity.static_public)) {
				up_read(&wg->static_identity.lock);
				goto out;
			}
		}
		up_read(&wg->static_identity.lock);
	}

	peers_nest = nla_nest_start(skb, WGDEVICE_A_PEERS);
	if (!peers_nest)
		goto out;
	ret = 0;
	/* If the last cursor was removed via list_del_init in peer_remove, then we just treat
	 * this the same as there being no more peers left. The reason is that seq_nr should
	 * indicate to userspace that this isn't a coherent dump anyway, so they'll try again. */
	if (list_empty(&wg->peer_list) || (last_peer_cursor && list_empty(&last_peer_cursor->peer_list))) {
		nla_nest_cancel(skb, peers_nest);
		goto out;
	}
	lockdep_assert_held(&wg->device_update_lock);
	peer = list_prepare_entry(last_peer_cursor, &wg->peer_list, peer_list);
	list_for_each_entry_continue (peer, &wg->peer_list, peer_list) {
		if (get_peer(peer, peer_idx++, &allowedips_idx_cursor, skb)) {
			done = false;
			break;
		}
		next_peer_cursor = peer;
	}
	nla_nest_end(skb, peers_nest);

out:
	peer_put(last_peer_cursor);
	if (!ret && !done)
		next_peer_cursor = peer_rcu_get(next_peer_cursor);
	mutex_unlock(&wg->device_update_lock);
	rtnl_unlock();

	if (ret) {
		genlmsg_cancel(skb, hdr);
		return ret;
	}
	genlmsg_end(skb, hdr);
	if (done) {
		cb->args[1] = cb->args[2] = 0;
		return 0;
	}
	cb->args[1] = (long)next_peer_cursor;
	cb->args[2] = (long)allowedips_idx_cursor;
	return skb->len;

	/* At this point, we can't really deal ourselves with safely zeroing out
	 * the private key material after usage. This will need an additional API
	 * in the kernel for marking skbs as zero_on_free. */
}

static int get_done(struct netlink_callback *cb)
{
	struct wireguard_device *wg = (struct wireguard_device *)cb->args[0];
	struct wireguard_peer *peer = (struct wireguard_peer *)cb->args[1];
	if (wg)
		dev_put(wg->dev);
	peer_put(peer);
	return 0;
}

static int set_device_port(struct wireguard_device *wg, u16 port)
{
	struct wireguard_peer *peer, *temp;
	if (wg->incoming_port == port)
		return 0;
	socket_uninit(wg);
	wg->incoming_port = port;
	peer_for_each (wg, peer, temp, false)
		socket_clear_peer_endpoint_src(peer);
	if (!netif_running(wg->dev))
		return 0;
	return socket_init(wg);
}

static int set_allowedip(struct wireguard_peer *peer, struct nlattr **attrs)
{
	int ret = -EINVAL;
	u16 family;
	u8 cidr;

	if (!attrs[WGALLOWEDIP_A_FAMILY] || !attrs[WGALLOWEDIP_A_IPADDR] || !attrs[WGALLOWEDIP_A_CIDR_MASK])
		return ret;
	family = nla_get_u16(attrs[WGALLOWEDIP_A_FAMILY]);
	cidr = nla_get_u8(attrs[WGALLOWEDIP_A_CIDR_MASK]);

	if (family == AF_INET && cidr <= 32 && nla_len(attrs[WGALLOWEDIP_A_IPADDR]) == sizeof(struct in_addr))
		ret = routing_table_insert_v4(&peer->device->peer_routing_table, nla_data(attrs[WGALLOWEDIP_A_IPADDR]), cidr, peer);
	else if (family == AF_INET6 && cidr <= 128 && nla_len(attrs[WGALLOWEDIP_A_IPADDR]) == sizeof(struct in6_addr))
		ret = routing_table_insert_v6(&peer->device->peer_routing_table, nla_data(attrs[WGALLOWEDIP_A_IPADDR]), cidr, peer);

	return ret;
}

static int set_peer(struct wireguard_device *wg, struct nlattr **attrs)
{
	int ret;
	u32 flags = 0;
	struct wireguard_peer *peer = NULL;
	u8 *public_key = NULL, *preshared_key = NULL;

	ret = -EINVAL;
	if (attrs[WGPEER_A_PUBLIC_KEY] && nla_len(attrs[WGPEER_A_PUBLIC_KEY]) == NOISE_PUBLIC_KEY_LEN)
		public_key = nla_data(attrs[WGPEER_A_PUBLIC_KEY]);
	else
		goto out;
	if (attrs[WGPEER_A_PRESHARED_KEY] && nla_len(attrs[WGPEER_A_PRESHARED_KEY]) == NOISE_SYMMETRIC_KEY_LEN)
		preshared_key = nla_data(attrs[WGPEER_A_PRESHARED_KEY]);
	if (attrs[WGPEER_A_FLAGS])
		flags = nla_get_u32(attrs[WGPEER_A_FLAGS]);

	peer = pubkey_hashtable_lookup(&wg->peer_hashtable, nla_data(attrs[WGPEER_A_PUBLIC_KEY]));
	if (!peer) { /* Peer doesn't exist yet. Add a new one. */
		ret = -ENODEV;
		if (flags & WGPEER_F_REMOVE_ME)
			goto out; /* Tried to remove a non-existing peer. */

		down_read(&wg->static_identity.lock);
		if (wg->static_identity.has_identity && !memcmp(nla_data(attrs[WGPEER_A_PUBLIC_KEY]), wg->static_identity.static_public, NOISE_PUBLIC_KEY_LEN)) {
			/* We silently ignore peers that have the same public key as the device. The reason we do it silently
			 * is that we'd like for people to be able to reuse the same set of API calls across peers. */
			up_read(&wg->static_identity.lock);
			ret = 0;
			goto out;
		}
		up_read(&wg->static_identity.lock);

		ret = -ENOMEM;
		peer = peer_rcu_get(peer_create(wg, public_key, preshared_key));
		if (!peer)
			goto out;
	}

	ret = 0;
	if (flags & WGPEER_F_REMOVE_ME) {
		peer_remove(peer);
		goto out;
	}

	if (preshared_key) {
		down_write(&peer->handshake.lock);
		memcpy(&peer->handshake.preshared_key, preshared_key, NOISE_SYMMETRIC_KEY_LEN);
		up_write(&peer->handshake.lock);
	}

	if (attrs[WGPEER_A_ENDPOINT]) {
		struct sockaddr *addr = nla_data(attrs[WGPEER_A_ENDPOINT]);
		size_t len = nla_len(attrs[WGPEER_A_ENDPOINT]);
		if ((len == sizeof(struct sockaddr_in) && addr->sa_family == AF_INET) || (len == sizeof(struct sockaddr_in6) && addr->sa_family == AF_INET6)) {
			struct endpoint endpoint = { { { 0 } } };
			memcpy(&endpoint.addr, addr, len);
			socket_set_peer_endpoint(peer, &endpoint);
		}
	}

	if (flags & WGPEER_F_REPLACE_ALLOWEDIPS)
		routing_table_remove_by_peer(&wg->peer_routing_table, peer);

	if (attrs[WGPEER_A_ALLOWEDIPS]) {
		int rem;
		struct nlattr *attr, *allowedip[WGALLOWEDIP_A_MAX + 1];
		nla_for_each_nested (attr, attrs[WGPEER_A_ALLOWEDIPS], rem) {
			ret = nla_parse_nested(allowedip, WGALLOWEDIP_A_MAX, attr, allowedip_policy, NULL);
			if (ret < 0)
				goto out;
			ret = set_allowedip(peer, allowedip);
			if (ret < 0)
				goto out;
		}
	}

	if (attrs[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]) {
		const u16 persistent_keepalive_interval = nla_get_u16(attrs[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]);
		const bool send_keepalive = !peer->persistent_keepalive_interval && persistent_keepalive_interval && netif_running(wg->dev);
		peer->persistent_keepalive_interval = (unsigned long)persistent_keepalive_interval * HZ;
		if (send_keepalive)
			packet_send_keepalive(peer);
	}

	if (netif_running(wg->dev))
		packet_send_staged_packets(peer);

out:
	peer_put(peer);
	if (attrs[WGPEER_A_PRESHARED_KEY])
		memzero_explicit(nla_data(attrs[WGPEER_A_PRESHARED_KEY]), nla_len(attrs[WGPEER_A_PRESHARED_KEY]));
	return ret;
}

static int set(struct sk_buff *skb, struct genl_info *info)
{
	int ret;
	struct wireguard_device *wg = lookup_interface(info->attrs, skb);
	if (IS_ERR(wg)) {
		ret = PTR_ERR(wg);
		goto out_nodev;
	}

	rtnl_lock();
	mutex_lock(&wg->device_update_lock);
	++wg->device_update_gen;

	if (info->attrs[WGDEVICE_A_FWMARK]) {
		struct wireguard_peer *peer, *temp;
		wg->fwmark = nla_get_u32(info->attrs[WGDEVICE_A_FWMARK]);
		peer_for_each (wg, peer, temp, false)
			socket_clear_peer_endpoint_src(peer);
	}

	if (info->attrs[WGDEVICE_A_LISTEN_PORT]) {
		ret = set_device_port(wg, nla_get_u16(info->attrs[WGDEVICE_A_LISTEN_PORT]));
		if (ret)
			goto out;
	}

	if (info->attrs[WGDEVICE_A_FLAGS] && nla_get_u32(info->attrs[WGDEVICE_A_FLAGS]) & WGDEVICE_F_REPLACE_PEERS)
		peer_remove_all(wg);

	if (info->attrs[WGDEVICE_A_PRIVATE_KEY] && nla_len(info->attrs[WGDEVICE_A_PRIVATE_KEY]) == NOISE_PUBLIC_KEY_LEN) {
		struct wireguard_peer *peer, *temp;
		u8 public_key[NOISE_PUBLIC_KEY_LEN] = { 0 }, *private_key = nla_data(info->attrs[WGDEVICE_A_PRIVATE_KEY]);
		/* We remove before setting, to prevent race, which means doing two 25519-genpub ops. */
		bool unused __attribute((unused)) = curve25519_generate_public(public_key, private_key);
		peer = pubkey_hashtable_lookup(&wg->peer_hashtable, public_key);
		if (peer) {
			peer_put(peer);
			peer_remove(peer);
		}
		noise_set_static_identity_private_key(&wg->static_identity, private_key);
		peer_for_each (wg, peer, temp, false) {
			if (!noise_precompute_static_static(peer))
				peer_remove(peer);
		}
		cookie_checker_precompute_device_keys(&wg->cookie_checker);
	}

	if (info->attrs[WGDEVICE_A_PEERS]) {
		int rem;
		struct nlattr *attr, *peer[WGPEER_A_MAX + 1];
		nla_for_each_nested (attr, info->attrs[WGDEVICE_A_PEERS], rem) {
			ret = nla_parse_nested(peer, WGPEER_A_MAX, attr, peer_policy, NULL);
			if (ret < 0)
				goto out;
			ret = set_peer(wg, peer);
			if (ret < 0)
				goto out;
		}
	}
	ret = 0;

out:
	mutex_unlock(&wg->device_update_lock);
	rtnl_unlock();
	dev_put(wg->dev);
out_nodev:
	if (info->attrs[WGDEVICE_A_PRIVATE_KEY])
		memzero_explicit(nla_data(info->attrs[WGDEVICE_A_PRIVATE_KEY]), nla_len(info->attrs[WGDEVICE_A_PRIVATE_KEY]));
	return ret;
}

static const struct genl_ops genl_ops[] = {
	{
		.cmd = WG_CMD_GET_DEVICE,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)
		.start = get_start,
#endif
		.dumpit = get,
		.done = get_done,
		.policy = device_policy,
		.flags = GENL_UNS_ADMIN_PERM
	}, {
		.cmd = WG_CMD_SET_DEVICE,
		.doit = set,
		.policy = device_policy,
		.flags = GENL_UNS_ADMIN_PERM
	}
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
static struct genl_family genl_family __ro_after_init = {
	.ops = genl_ops,
	.n_ops = ARRAY_SIZE(genl_ops),
#else
static struct genl_family genl_family = {
#endif
	.name = WG_GENL_NAME,
	.version = WG_GENL_VERSION,
	.maxattr = WGDEVICE_A_MAX,
	.module = THIS_MODULE,
	.netnsok = true
};

int __init netlink_init(void)
{
	return genl_register_family(&genl_family);
}

void __exit netlink_uninit(void)
{
	genl_unregister_family(&genl_family);
}