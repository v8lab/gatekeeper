/*
 * Gatekeeper - DDoS protection system.
 * Copyright (C) 2016 Digirati LTDA.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stddef.h>
#include <arpa/inet.h>
#include <linux/rtnetlink.h>

#include "gatekeeper_config.h"
#include "gatekeeper_fib.h"
#include "gatekeeper_gk.h"
#include "gatekeeper_l2.h"
#include "gatekeeper_lls.h"
#include "gatekeeper_main.h"

void
destroy_neigh_hash_table(struct neighbor_hash_table *neigh)
{
	if (neigh->cache_tbl != NULL) {
		rte_free(neigh->cache_tbl);
		neigh->cache_tbl = NULL;
	}

	if (neigh->hash_table != NULL) {
		rte_hash_free(neigh->hash_table);
		neigh->hash_table = NULL;
	}

	neigh->tbl_size = 0;
}

static int
gk_lpm_add_ipv4_route(uint32_t ip, uint8_t depth, uint32_t nexthop,
	struct gk_lpm *ltbl)
{
	int ret = rib_add(&ltbl->rib, (uint8_t *)&ip, depth, nexthop);
	if (ret < 0)
		return ret;

	ret = rte_lpm_add(ltbl->lpm, ntohl(ip), depth, nexthop);
	if (unlikely(ret < 0)) {
		int ret2 = rib_delete(&ltbl->rib, (uint8_t *)&ip, depth);
		if (unlikely(ret2 < 0)) {
			G_LOG(CRIT, "%s(): bug: failed to remove a prefix just added (errno=%i): %s\n",
				__func__, -ret2, strerror(-ret2));
		}
		return ret;
	}

	return 0;
}

static int
gk_lpm_add_ipv6_route(const uint8_t *ip, uint8_t depth, uint32_t nexthop,
	struct gk_lpm *ltbl)
{
	int ret = rib_add(&ltbl->rib6, ip, depth, nexthop);
	if (ret < 0)
		return ret;

	ret = rte_lpm6_add(ltbl->lpm6, ip, depth, nexthop);
	if (unlikely(ret < 0)) {
		int ret2 = rib_delete(&ltbl->rib6, ip, depth);
		if (unlikely(ret2 < 0)) {
			G_LOG(CRIT, "%s(): bug: failed to remove a prefix just added (errno=%i): %s\n",
				__func__, -ret2, strerror(-ret2));
		}
		return ret;
	}

	return 0;
}

static int
gk_lpm_del_ipv4_route(struct gk_lpm *ltbl, uint32_t ip, uint8_t depth)
{
	int ret2, ret = rib_delete(&ltbl->rib, (uint8_t *)&ip, depth);
	if (unlikely(ret != 0 && ret != -ENOENT)) {
		G_LOG(CRIT, "%s(): bug: unexpected return (errno=%i): %s\n",
			__func__, ret, strerror(-ret));
	}

	ret2 = rte_lpm_delete(ltbl->lpm, ntohl(ip), depth);
	if (unlikely(ret != ret2)) {
		G_LOG(CRIT, "%s(): bug: unexpected mismatch, ret == %i and ret2 == %i: %s\n",
			__func__, ret, ret2, strerror(-ret2));
	}

	return ret2;
}

static int
gk_lpm_del_ipv6_route(struct gk_lpm *ltbl, const uint8_t *ip, uint8_t depth)
{
	int ret2, ret = rib_delete(&ltbl->rib6, ip, depth);
	if (unlikely(ret != 0 && ret != -ENOENT)) {
		G_LOG(CRIT, "%s(): bug: unexpected return (errno=%i): %s\n",
			__func__, ret, strerror(-ret));
	}

	ret2 = rte_lpm6_delete(ltbl->lpm6, ip, depth);
	if (unlikely(ret != ret2)) {
		G_LOG(CRIT, "%s(): bug: unexpected mismatch, ret == %i and ret2 == %i: %s\n",
			__func__, ret, ret2, strerror(-ret2));
	}

	return ret2;
}

/*
 * This function is only called on cache entries that are not being used,
 * so we don't need a concurrencty mechanism here. However,
 * callers must ensure that the entry is not being used.
 */
int
clear_ether_cache(struct ether_cache *eth_cache)
{
	int ref_cnt;

	memset(eth_cache->fields_to_clear, 0, sizeof(*eth_cache) -
		offsetof(struct ether_cache, fields_to_clear));

	if ((ref_cnt = rte_atomic32_read(&eth_cache->ref_cnt)) != 1) {
		G_LOG(WARNING, "%s() the value of ref_cnt field in Ethernet cache entry is %d rather than 1\n",
			__func__, ref_cnt);
	}

	rte_atomic32_init(&eth_cache->ref_cnt);

	return 0;
}

static void
gk_arp_and_nd_req_cb(const struct lls_map *map, void *arg,
	__attribute__((unused))enum lls_reply_ty ty, int *pcall_again)
{
	struct ether_cache *eth_cache = arg;

	if (pcall_again == NULL) {
		clear_ether_cache(eth_cache);
		return;
	}

	/*
	 * Deal with concurrency control by sequential lock
	 * on the nexthop entry.
	 */
	write_seqlock(&eth_cache->lock);
	rte_ether_addr_copy(&map->ha, &eth_cache->l2_hdr.eth_hdr.d_addr);
	eth_cache->stale = map->stale;
	write_sequnlock(&eth_cache->lock);

	*pcall_again = true;
}

/* Get a new Ethernet cached header, and fill up the header accordingly. */
static struct ether_cache *
get_new_ether_cache_locked(struct neighbor_hash_table *neigh,
	struct ipaddr *addr, struct gatekeeper_if *iface)
{
	int i;
	struct ether_cache *eth_cache = NULL;

	for (i = 0; i < neigh->tbl_size; i++) {
		if (rte_atomic32_read(&neigh->cache_tbl[i].ref_cnt) == 0) {
			eth_cache = &neigh->cache_tbl[i];
			break;
		}
	}

	if (eth_cache == NULL)
		return NULL;

	/*
	 * We are initializing @eth_cache, no one but us should be
	 * reading/writing to @eth_cache, so it doesn't need a sequential lock
	 * to protect the operations here.
	 */
	eth_cache->stale = true;
	rte_memcpy(&eth_cache->ip_addr, addr, sizeof(eth_cache->ip_addr));
	if (iface->vlan_insert) {
		uint16_t vlan_tag_be = addr->proto == RTE_ETHER_TYPE_IPV4 ?
			iface->ipv4_vlan_tag_be : iface->ipv6_vlan_tag_be;
		fill_vlan_hdr(&eth_cache->l2_hdr.eth_hdr, vlan_tag_be, addr->proto);
	} else {
		eth_cache->l2_hdr.eth_hdr.ether_type =
			rte_cpu_to_be_16(addr->proto);
	}
	rte_ether_addr_copy(&iface->eth_addr,
		&eth_cache->l2_hdr.eth_hdr.s_addr);
	rte_atomic32_set(&eth_cache->ref_cnt, 1);

	return eth_cache;
}

static struct ether_cache *
neigh_get_ether_cache_locked(struct neighbor_hash_table *neigh,
	struct ipaddr *addr, struct gatekeeper_if *iface, int lcore_id)
{
	int ret;
	struct ether_cache *eth_cache = lookup_ether_cache(neigh, &addr->ip);
	if (eth_cache != NULL) {
		rte_atomic32_inc(&eth_cache->ref_cnt);
		return eth_cache;
	}

	eth_cache = get_new_ether_cache_locked(neigh, addr, iface);
	if (eth_cache == NULL)
		return NULL;

	if (addr->proto == RTE_ETHER_TYPE_IPV4) {
		ret = hold_arp(gk_arp_and_nd_req_cb,
			eth_cache, &addr->ip.v4, lcore_id);
	} else if (likely(addr->proto == RTE_ETHER_TYPE_IPV6)) {
		ret = hold_nd(gk_arp_and_nd_req_cb,
			eth_cache, &addr->ip.v6, lcore_id);
	} else {
		G_LOG(CRIT, "%s(): bug: unknown IP type %hu\n",
			__func__, addr->proto);
		ret = -1;
	}

	if (ret < 0)
		goto eth_cache_cleanup;

	ret = rte_hash_add_key_data(neigh->hash_table, &addr->ip, eth_cache);
	if (ret == 0) {
		/*
		 * Function get_new_ether_cache_locked() already
		 * sets @ref_cnt to 1.
		 */
		return eth_cache;
	}

	G_LOG(ERR, "%s(): failed to add a cache entry to the neighbor hash table\n",
		__func__);

	if (addr->proto == RTE_ETHER_TYPE_IPV4)
		put_arp(&addr->ip.v4, lcore_id);
	else
		put_nd(&addr->ip.v6, lcore_id);

	/*
	 * By calling put_xxx(), the LLS block will call
	 * gk_arp_and_nd_req_cb(), which, in turn, will call
	 * clear_ether_cache(), so we can return directly here.
	 */
	return NULL;

eth_cache_cleanup:
	clear_ether_cache(eth_cache);
	return NULL;
}

int
parse_ip_prefix(const char *ip_prefix, struct ipaddr *res)
{
	/* Need to make copy to tokenize. */
	size_t ip_prefix_len = ip_prefix != NULL ? strlen(ip_prefix) : 0;
	char ip_prefix_copy[ip_prefix_len + 1];
	char *ip_addr;

	char *saveptr;
	char *prefix_len_str;
	char *end;
	long prefix_len;
	int ip_type;

	if (ip_prefix == NULL)
		return -1;

	strcpy(ip_prefix_copy, ip_prefix);

	ip_addr = strtok_r(ip_prefix_copy, "/", &saveptr);
	if (ip_addr == NULL) {
		G_LOG(ERR, "%s(%s): failed to parse IP address in prefix\n",
			__func__, ip_prefix);
		return -1;
	}

	ip_type = get_ip_type(ip_addr);
	if (ip_type != AF_INET && ip_type != AF_INET6)
		return -1;

	prefix_len_str = strtok_r(NULL, "\0", &saveptr);
	if (prefix_len_str == NULL) {
		G_LOG(ERR, "%s(%s): failed to parse prefix length in prefix\n",
			__func__, ip_prefix);
		return -1;
	}

	prefix_len = strtol(prefix_len_str, &end, 10);
	if (prefix_len_str == end || !*prefix_len_str || *end) {
		G_LOG(ERR, "%s(%s): prefix length \"%s\" is not a number\n",
			__func__, ip_prefix, prefix_len_str);
		return -1;
	}

	if ((prefix_len == LONG_MAX || prefix_len == LONG_MIN) &&
			errno == ERANGE) {
		G_LOG(ERR, "%s(%s): prefix length \"%s\" caused underflow or overflow\n",
			__func__, ip_prefix, prefix_len_str);
		return -1;
	}

	if (prefix_len < 0 || prefix_len > max_prefix_len(ip_type)) {
		G_LOG(ERR, "%s(%s): prefix length \"%s\" is out of range\n",
			__func__, ip_prefix, prefix_len_str);
		return -1;
	}

	if (convert_str_to_ip(ip_addr, res) < 0) {
		G_LOG(ERR, "%s(%s): the IP address of the prefix is not valid\n",
			__func__, ip_prefix);
		return -1;
	}

	RTE_VERIFY((ip_type == AF_INET && res->proto == RTE_ETHER_TYPE_IPV4) ||
		(ip_type == AF_INET6 && res->proto == RTE_ETHER_TYPE_IPV6));

	return prefix_len;
}

/* WARNING: do NOT call this function directly, call get_empty_fib_id(). */
static int
__get_empty_fib_id(struct gk_fib *fib_tbl, uint32_t *plast_index,
	unsigned int num_fib_entries)
{
	uint32_t last_index = *plast_index;
	unsigned int i = last_index;

	/*
	 * @gk_conf->lpm_tbl.fib_tbl or @gk_conf->lpm_tbl.fib_tbl6 is NULL
	 * when IPv4 or IPv6 is disabled, respectively.
	 * But @fib_tbl must not be NULL if the code reached here.
	 */
	RTE_VERIFY(fib_tbl != NULL);

	do {
		/* Next index. */
		i++;
		if (unlikely(i >= num_fib_entries))
			i = 0;

		if (likely(fib_tbl[i].action == GK_FIB_MAX)) {
			*plast_index = i;
			return i;
		}
	} while (likely(i != last_index));
	return -ENOENT;
}

/* This function will return an empty FIB entry. */
static int
get_empty_fib_id(uint16_t ip_proto, struct gk_config *gk_conf,
	struct gk_fib **p_fib)
{
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	int ret;

	/* Find an empty FIB entry. */
	if (ip_proto == RTE_ETHER_TYPE_IPV4) {
		ret = __get_empty_fib_id(ltbl->fib_tbl, &ltbl->last_ipv4_index,
			gk_conf->max_num_ipv4_rules);
		if (unlikely(ret < 0)) {
			G_LOG(WARNING, "%s(): cannot find an empty fib entry in the IPv4 FIB table\n",
				__func__);
		} else {
			*p_fib = &ltbl->fib_tbl[ret];
		}
		return ret;
	}

	if (likely(ip_proto == RTE_ETHER_TYPE_IPV6)) {
		ret = __get_empty_fib_id(ltbl->fib_tbl6, &ltbl->last_ipv6_index,
			gk_conf->max_num_ipv6_rules);
		if (unlikely(ret < 0)) {
			G_LOG(WARNING, "%s(): cannot find an empty fib entry in the IPv6 FIB table\n",
				__func__);
		} else {
			*p_fib = &ltbl->fib_tbl6[ret];
		}
		return ret;
	}

	G_LOG(CRIT, "%s(): bug: unknown Ethernet type %hu\n",
		__func__, ip_proto);
	return -EINVAL;
}

/* Add a prefix into the LPM table. */
static int
lpm_add_route(const struct ipaddr *ip_addr, int prefix_len, int fib_id,
	struct gk_lpm *ltbl)
{
	if (ip_addr->proto == RTE_ETHER_TYPE_IPV4) {
		return gk_lpm_add_ipv4_route(
			ip_addr->ip.v4.s_addr, prefix_len, fib_id, ltbl);
	}

	if (likely(ip_addr->proto == RTE_ETHER_TYPE_IPV6)) {
		return gk_lpm_add_ipv6_route(
			ip_addr->ip.v6.s6_addr, prefix_len, fib_id, ltbl);
	}

	G_LOG(CRIT, "%s(): bug: unknown IP type %hu\n",
		__func__, ip_addr->proto);
	return -EINVAL;
}

/* Delete a prefix from the LPM table. */
static int
lpm_del_route(const struct ipaddr *ip_addr, int prefix_len, struct gk_lpm *ltbl)
{
	if (ip_addr->proto == RTE_ETHER_TYPE_IPV4) {
		return gk_lpm_del_ipv4_route(ltbl, ip_addr->ip.v4.s_addr,
			prefix_len);
	}

	if (likely(ip_addr->proto == RTE_ETHER_TYPE_IPV6)) {
		return gk_lpm_del_ipv6_route(ltbl, ip_addr->ip.v6.s6_addr,
			prefix_len);
	}

	G_LOG(CRIT, "%s(): bug: unknown IP type %hu\n",
		__func__, ip_addr->proto);
	return -EINVAL;
}

/*
 * For IPv4, the hash table key (i.e., IPv4 address) used is
 * in network byte order. Moreover, the DPDK's hash table
 * implementation takes a mod over the hash.
 * We convert the key to host order to make sure
 * that the most important bits of the hash function are
 * the least significant bits of the IP address.
 */
uint32_t
custom_ipv4_hash_func(const void *key,
	__attribute__((unused)) uint32_t length,
	__attribute__((unused)) uint32_t initval)
{
	return ntohl(*(const uint32_t *)key);
}

int
setup_neighbor_tbl(unsigned int socket_id, int identifier,
	int ip_ver, int ht_size, struct neighbor_hash_table *neigh,
	rte_hash_function hash_func)
{
	int  i, ret;
	char ht_name[64];
	int key_len = ip_ver == RTE_ETHER_TYPE_IPV4 ?
		sizeof(struct in_addr) : sizeof(struct in6_addr);

	struct rte_hash_parameters neigh_hash_params = {
		.entries = ht_size < HASH_TBL_MIN_SIZE
			? HASH_TBL_MIN_SIZE
			: ht_size,
		.key_len = key_len,
		.hash_func = hash_func,
		.hash_func_init_val = 0,
		.socket_id = socket_id,
	};

	ret = snprintf(ht_name, sizeof(ht_name),
		"neighbor_hash_%u", identifier);
	RTE_VERIFY(ret > 0 && ret < (int)sizeof(ht_name));

	/* Setup the neighbor hash table. */
	neigh_hash_params.name = ht_name;
	neigh->hash_table = rte_hash_create(&neigh_hash_params);
	if (neigh->hash_table == NULL) {
		G_LOG(ERR, "%s(): cannot create hash table for neighbor FIB\n",
			__func__);
		ret = -1;
		goto out;
	}

	/* Setup the Ethernet header cache table. */
	neigh->cache_tbl = rte_calloc_socket(NULL,
		ht_size, sizeof(struct ether_cache), 0, socket_id);
	if (neigh->cache_tbl == NULL) {
		G_LOG(ERR, "%s(): cannot create Ethernet header cache table\n",
			__func__);
		ret = -1;
		goto neigh_hash;
	}

	/* Initialize the sequential lock for each Ethernet cache entry. */
	for (i = 0; i < ht_size; i++)
		seqlock_init(&neigh->cache_tbl[i].lock);

	neigh->tbl_size = ht_size;

	ret = 0;
	goto out;

neigh_hash:
	rte_hash_free(neigh->hash_table);
	neigh->hash_table = NULL;
out:
	return ret;
}

/*
 * The caller is responsible for releasing any resource associated to @fib.
 * For example, if the FIB entry has action GK_FWD_NEIGHBOR_*_NET,
 * then the caller needs to first destroy the neighbor hash table before
 * calling this function.
 */
static inline void
initialize_fib_entry(struct gk_fib *fib)
{
	/* Reset the fields of the deleted FIB entry. */
	fib->action = GK_FIB_MAX;
	memset(&fib->u, 0, sizeof(fib->u));
}

/*
 * Setup the FIB entries for the network prefixes, for which @iface
 * is responsible.
 * These prefixes are configured when the Gatekeeper server starts.
 */
static int
setup_net_prefix_fib(int identifier,
	struct gk_fib **neigh_fib, struct gk_fib **neigh6_fib,
	struct gatekeeper_if *iface, struct gk_config *gk_conf)
{
	int ret;
	int fib_id;
	unsigned int socket_id = rte_lcore_to_socket_id(gk_conf->lcores[0]);
	struct net_config *net_conf = gk_conf->net;
	struct gk_fib *neigh_fib_ipv4 = NULL;
	struct gk_fib *neigh_fib_ipv6 = NULL;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	/* Set up the FIB entry for the IPv4 network prefix. */
	if (ipv4_if_configured(iface)) {
		fib_id = get_empty_fib_id(RTE_ETHER_TYPE_IPV4, gk_conf,
			&neigh_fib_ipv4);
		if (fib_id < 0)
			goto out;

		ret = setup_neighbor_tbl(socket_id, (identifier * 2),
			RTE_ETHER_TYPE_IPV4, (1 << (32 - iface->ip4_addr_plen)),
			&neigh_fib_ipv4->u.neigh, custom_ipv4_hash_func);
		if (ret < 0)
			goto init_fib_ipv4;

		if (iface == &net_conf->front)
			neigh_fib_ipv4->action = GK_FWD_NEIGHBOR_FRONT_NET;
		else if (likely(iface == &net_conf->back))
			neigh_fib_ipv4->action = GK_FWD_NEIGHBOR_BACK_NET;
		else {
			G_LOG(CRIT, "%s(): bug: invalid interface %s\n",
				__func__, iface->name);
			goto free_fib_ipv4_ht;
		}

		ret = gk_lpm_add_ipv4_route(iface->ip4_addr.s_addr,
			iface->ip4_addr_plen, fib_id, ltbl);
		if (ret < 0)
			goto free_fib_ipv4_ht;

		*neigh_fib = neigh_fib_ipv4;
	}

	/* Set up the FIB entry for the IPv6 network prefix. */
	if (ipv6_if_configured(iface)) {
		fib_id = get_empty_fib_id(RTE_ETHER_TYPE_IPV6, gk_conf,
			&neigh_fib_ipv6);
		if (fib_id < 0)
			goto free_fib_ipv4;

		ret = setup_neighbor_tbl(socket_id, (identifier * 2 + 1),
			RTE_ETHER_TYPE_IPV6, gk_conf->max_num_ipv6_neighbors,
			&neigh_fib_ipv6->u.neigh, DEFAULT_HASH_FUNC);
		if (ret < 0)
			goto init_fib_ipv6;

		if (iface == &net_conf->front)
			neigh_fib_ipv6->action = GK_FWD_NEIGHBOR_FRONT_NET;
		else if (likely(iface == &net_conf->back))
			neigh_fib_ipv6->action = GK_FWD_NEIGHBOR_BACK_NET;
		else {
			G_LOG(CRIT, "%s(): bug: invalid interface %s\n",
				__func__, iface->name);
			goto free_fib_ipv6_ht;
		}

		ret = gk_lpm_add_ipv6_route(iface->ip6_addr.s6_addr,
			iface->ip6_addr_plen, fib_id, ltbl);
		if (ret < 0)
			goto free_fib_ipv6_ht;

		*neigh6_fib = neigh_fib_ipv6;
	}

	return 0;

free_fib_ipv6_ht:
	destroy_neigh_hash_table(&neigh_fib_ipv6->u.neigh);

init_fib_ipv6:
	initialize_fib_entry(neigh_fib_ipv6);

free_fib_ipv4:
	if (neigh_fib_ipv4 == NULL)
		return -1;

	*neigh_fib = NULL;

	RTE_VERIFY(gk_lpm_del_ipv4_route(ltbl, iface->ip4_addr.s_addr,
		iface->ip4_addr_plen) == 0);

free_fib_ipv4_ht:
	destroy_neigh_hash_table(&neigh_fib_ipv4->u.neigh);

init_fib_ipv4:
	initialize_fib_entry(neigh_fib_ipv4);

out:
	return -1;
}

static int
init_fib_tbl(struct gk_config *gk_conf)
{
	int ret;
	unsigned int i;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gk_fib *neigh_fib_front = NULL, *neigh6_fib_front = NULL;
	struct gk_fib *neigh_fib_back = NULL, *neigh6_fib_back = NULL;

	rte_spinlock_init(&ltbl->lock);

	if (ltbl->fib_tbl != NULL) {
		for (i = 0; i < gk_conf->max_num_ipv4_rules; i++)
			ltbl->fib_tbl[i].action = GK_FIB_MAX;
	}

	if (ltbl->fib_tbl6 != NULL) {
		for (i = 0; i < gk_conf->max_num_ipv6_rules; i++)
			ltbl->fib_tbl6[i].action = GK_FIB_MAX;
	}

	/* Set up the FIB entry for the front network prefixes. */
	ret = setup_net_prefix_fib(0, &neigh_fib_front,
		&neigh6_fib_front, &gk_conf->net->front, gk_conf);
	if (ret < 0) {
		G_LOG(ERR, "%s(): failed to setup the FIB entry for the front network prefixes\n",
			__func__);
		goto out;
	}

	/* Set up the FIB entry for the back network prefixes. */
	RTE_VERIFY(gk_conf->net->back_iface_enabled);
	ret = setup_net_prefix_fib(1, &neigh_fib_back,
		&neigh6_fib_back, &gk_conf->net->back, gk_conf);
	if (ret < 0) {
		G_LOG(ERR, "%s(): failed to setup the FIB entry for the back network prefixes\n",
			__func__);
		goto free_front_fibs;
	}

	return 0;

free_front_fibs:
	if (neigh_fib_front != NULL) {
		struct gatekeeper_if *iface = &gk_conf->net->front;
		RTE_VERIFY(gk_lpm_del_ipv4_route(&gk_conf->lpm_tbl,
			iface->ip4_addr.s_addr, iface->ip4_addr_plen) == 0);
		destroy_neigh_hash_table(&neigh_fib_front->u.neigh);
		initialize_fib_entry(neigh_fib_front);
		neigh_fib_front = NULL;
	}
	if (neigh6_fib_front != NULL) {
		struct gatekeeper_if *iface = &gk_conf->net->front;
		RTE_VERIFY(gk_lpm_del_ipv6_route(&gk_conf->lpm_tbl,
			iface->ip6_addr.s6_addr, iface->ip6_addr_plen) == 0);
		destroy_neigh_hash_table(&neigh6_fib_front->u.neigh);
		initialize_fib_entry(neigh6_fib_front);
		neigh6_fib_front = NULL;
	}

out:
	return ret;
}

int
setup_gk_lpm(struct gk_config *gk_conf, unsigned int socket_id)
{
	int ret;
	struct rte_lpm_config ipv4_lpm_config;
	struct rte_lpm6_config ipv6_lpm_config;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	if (ipv4_configured(gk_conf->net)) {
		ret = rib_create(&ltbl->rib, "IPv4-RIB", socket_id, 32,
			gk_conf->max_num_ipv4_rules);
		if (unlikely(ret < 0)) {
			G_LOG(ERR, "%s(): failed to create the IPv4 RIB\n",
				__func__);
			goto out;
		}

		ipv4_lpm_config.max_rules = gk_conf->max_num_ipv4_rules;
		ipv4_lpm_config.number_tbl8s = gk_conf->num_ipv4_tbl8s;

		/*
		 * The GK blocks only need to create one single
		 * IPv4 LPM table on the @socket_id, so the
		 * @lcore and @identifier are set to 0.
		 */
		ltbl->lpm = init_ipv4_lpm("gk", &ipv4_lpm_config, socket_id,
			0, 0);
		if (unlikely(ltbl->lpm == NULL)) {
			G_LOG(ERR, "%s(): failed to create the IPv4 FIB\n",
				__func__);
			ret = -ENOMEM;
			goto free_rib;
		}

		ltbl->fib_tbl = rte_calloc_socket(NULL,
			gk_conf->max_num_ipv4_rules, sizeof(struct gk_fib),
			0, socket_id);
		if (unlikely(ltbl->fib_tbl == NULL)) {
			G_LOG(ERR, "%s(): failed to create the IPv4 FIB table\n",
				__func__);
			ret = -ENOMEM;
			goto free_fib;
		}
		ltbl->last_ipv4_index = gk_conf->max_num_ipv4_rules - 1;
	} else if (gk_conf->max_num_ipv4_rules != 0 ||
			gk_conf->num_ipv4_tbl8s != 0) {
		G_LOG(WARNING, "%s(): IPv4 is not configured, but the parameters max_num_ipv4_rules=%u and num_ipv4_tbl8s=%u are not both zero\n",
			__func__, gk_conf->max_num_ipv4_rules,
			gk_conf->num_ipv4_tbl8s);
	}

	if (ipv6_configured(gk_conf->net)) {
		ret = rib_create(&ltbl->rib6, "IPv6-RIB", socket_id, 128,
			gk_conf->max_num_ipv6_rules);
		if (unlikely(ret < 0)) {
			G_LOG(ERR, "%s(): failed to create the IPv6 RIB\n",
				__func__);
			goto free_lpm_tbl;
		}

		ipv6_lpm_config.max_rules = gk_conf->max_num_ipv6_rules;
		ipv6_lpm_config.number_tbl8s = gk_conf->num_ipv6_tbl8s;

		/*
		 * The GK blocks only need to create one single
		 * IPv6 LPM table on the @socket_id, so the
		 * @lcore and @identifier are set to 0.
		 */
		ltbl->lpm6 = init_ipv6_lpm("gk", &ipv6_lpm_config, socket_id,
			0, 0);
		if (unlikely(ltbl->lpm6 == NULL)) {
			G_LOG(ERR, "%s(): failed to create the IPv6 FIB\n",
				__func__);
			ret = -ENOMEM;
			goto free_rib6;
		}

		ltbl->fib_tbl6 = rte_calloc_socket(NULL,
			gk_conf->max_num_ipv6_rules, sizeof(struct gk_fib),
			0, socket_id);
		if (unlikely(ltbl->fib_tbl6 == NULL)) {
			G_LOG(ERR, "%s(): failed to create the IPv6 FIB table\n",
				__func__);
			ret = -ENOMEM;
			goto free_fib6;
		}
		ltbl->last_ipv6_index = gk_conf->max_num_ipv6_rules - 1;
	} else if (gk_conf->max_num_ipv6_rules != 0 ||
			gk_conf->num_ipv6_tbl8s != 0) {
		G_LOG(WARNING, "%s(): IPv6 is not configured, but the parameters max_num_ipv6_rules=%u and num_ipv6_tbl8s=%u are not both zero\n",
			__func__, gk_conf->max_num_ipv6_rules,
			gk_conf->num_ipv6_tbl8s);
	}

	ret = init_fib_tbl(gk_conf);
	if (unlikely(ret < 0)) {
		G_LOG(ERR, "%s(): failed to initialize the FIB table\n",
			__func__);
		goto free_lpm_tbl6;
	}

	ret = 0;
	goto out;

free_lpm_tbl6:
	if (!ipv6_configured(gk_conf->net))
		goto free_lpm_tbl;

	rte_free(ltbl->fib_tbl6);
	ltbl->fib_tbl6 = NULL;
free_fib6:
	destroy_ipv6_lpm(ltbl->lpm6);
	ltbl->lpm6 = NULL;
free_rib6:
	rib_free(&ltbl->rib6);
free_lpm_tbl:
	if (!ipv4_configured(gk_conf->net))
		goto out;

	rte_free(ltbl->fib_tbl);
	ltbl->fib_tbl = NULL;
free_fib:
	destroy_ipv4_lpm(ltbl->lpm);
	ltbl->lpm = NULL;
free_rib:
	rib_free(&ltbl->rib);
out:
	return ret;
}

static void
fill_in_cmd_entry(struct gk_cmd_entry *entry, rte_atomic32_t *done_counter,
	void *arg)
{
	struct gk_synch_request *req_template = arg;
	entry->op = GK_SYNCH_WITH_LPM;
	entry->u.synch = *req_template;
	entry->u.synch.done_counter = done_counter;
}

static void
synchronize_gk_instances_with_fib(struct gk_config *gk_conf,
	struct gk_fib *fib, bool update_only)
{
	struct gk_synch_request req_template = {
		.fib = fib,
		.update_only = update_only,
		.done_counter = NULL,
	};
	synchronize_gk_instances(gk_conf, fill_in_cmd_entry, &req_template);
}

/*
 * Note that, @action should be either GK_FWD_GATEWAY_FRONT_NET
 * or GK_FWD_GATEWAY_BACK_NET.
 */
static struct gk_fib *
find_fib_entry_for_neighbor_locked(const struct ipaddr *gw_addr,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	int ret;
	uint32_t fib_id;
	struct gk_fib *neigh_fib;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gatekeeper_if *iface;

	if (action == GK_FWD_GATEWAY_FRONT_NET)
		iface = &gk_conf->net->front;
	else if (likely(action == GK_FWD_GATEWAY_BACK_NET))
		iface = &gk_conf->net->back;
	else {
		G_LOG(ERR, "%s(): action = %d is not expected\n",
			__func__, action);
		return NULL;
	}

	if (gw_addr->proto == RTE_ETHER_TYPE_IPV4 &&
			ipv4_if_configured(iface)) {
		ret = rib_lookup(&ltbl->rib, (uint8_t *)&gw_addr->ip.v4.s_addr,
			&fib_id);
		/*
		 * Invalid gateway entry, since at least we should
		 * obtain the FIB entry for the neighbor table.
		 */
		if (unlikely(ret < 0))
			return NULL;

		neigh_fib = &ltbl->fib_tbl[fib_id];
	} else if (likely(gw_addr->proto == RTE_ETHER_TYPE_IPV6)
			&& ipv6_if_configured(iface)) {
		ret = rib_lookup(&ltbl->rib6, gw_addr->ip.v6.s6_addr, &fib_id);
		/*
		 * Invalid gateway entry, since at least we should
		 * obtain the FIB entry for the neighbor table.
		 */
		if (unlikely(ret < 0))
			return NULL;

		neigh_fib = &ltbl->fib_tbl6[fib_id];
	} else {
		G_LOG(ERR, "%s(): Unconfigued IP type %hu at interface %s\n",
			__func__, gw_addr->proto, iface->name);
		return NULL;
	}

	/*
	 * Invalid gateway entry, since the neighbor entry
	 * and the gateway entry should be in the same network.
	 */
	if ((action == GK_FWD_GATEWAY_FRONT_NET &&
			neigh_fib->action != GK_FWD_NEIGHBOR_FRONT_NET)
			|| (action == GK_FWD_GATEWAY_BACK_NET &&
			neigh_fib->action != GK_FWD_NEIGHBOR_BACK_NET))
		return NULL;

	return neigh_fib;
}

static int
ether_cache_put(struct gk_fib *neigh_fib,
	enum gk_fib_action action, struct ether_cache *eth_cache,
	struct gk_config *gk_conf)
{
	int ret, ref_cnt;
	struct gk_fib *neighbor_fib = neigh_fib;
	struct ipaddr addr;

	while ((ref_cnt = rte_atomic32_read(&eth_cache->ref_cnt)) >= 2) {
		if (likely(rte_atomic32_cmpset((volatile uint32_t *)
				&eth_cache->ref_cnt.cnt,
				ref_cnt, ref_cnt - 1) != 0))
			return 0;
	}
	if (ref_cnt < 1) {
		rte_panic("%s(): bug: the ref_cnt of the ether cache should be 1, but it is %d\n",
			__func__, ref_cnt);
	}

	/*
	 * We need a copy of the IP address of the nexthop,
	 * because after calling put_xxx(), it's possible that
	 * gk_arp_and_nd_req_cb() is called before rte_hash_del_key().
	 * In this case, the 'eth_cache->ip_addr' (hash key) will be reset,
	 * so that the hash key becomes invalid.
	 */
	addr = eth_cache->ip_addr;

	/*
	 * Find the FIB entry for the @addr.
	 * We need to release the @eth_cache
	 * Ethernet header entry from the neighbor hash table.
	 */
	if (neighbor_fib == NULL) {
		neighbor_fib = find_fib_entry_for_neighbor_locked(
			&addr, action, gk_conf);
		if (neighbor_fib == NULL) {
			G_LOG(ERR, "%s(): could not find neighbor FIB to release Ethernet header entry\n",
				__func__);
			return -1;
		}
	}

	if (addr.proto == RTE_ETHER_TYPE_IPV4) {
		ret = put_arp(&addr.ip.v4, gk_conf->lcores[0]);
		if (ret < 0)
			return ret;

		ret = rte_hash_del_key(neighbor_fib->u.neigh.hash_table,
			&addr.ip.v4.s_addr);
		if (ret < 0) {
			G_LOG(CRIT, "%s(): failed to delete an Ethernet cache entry from the IPv4 neighbor table; we are NOT trying to recover from this failure\n",
				__func__);
		}
		return ret;
	}

	if (likely(addr.proto == RTE_ETHER_TYPE_IPV6)) {
		ret = put_nd(&addr.ip.v6, gk_conf->lcores[0]);
		if (ret < 0)
			return ret;

		ret = rte_hash_del_key(neighbor_fib->u.neigh.hash_table,
			addr.ip.v6.s6_addr);
		if (ret < 0) {
			G_LOG(CRIT, "%s(): failed to delete an Ethernet cache entry from the IPv6 neighbor table; we are NOT trying to recover from this failure\n",
				__func__);
		}
		return ret;
	}

	G_LOG(ERR, "%s(): remove an invalid FIB entry with IP type %hu\n",
		__func__, addr.proto);
	return -1;
}

/*
 * This function is called by del_fib_entry_numerical_locked().
 * Notice that, it doesn't stand on its own, and it's only
 * a construct to make del_fib_entry_numerical_locked() readable.
 */
static int
del_gateway_from_neigh_table_locked(const struct ip_prefix *ip_prefix,
	enum gk_fib_action action, struct ether_cache *eth_cache,
	struct gk_config *gk_conf)
{
	int ret = ether_cache_put(NULL, action, eth_cache, gk_conf);
	if (ret < 0) {
		G_LOG(ERR, "%s(%s): failed to release the Ethernet cached header of the Grantor FIB entry\n",
			__func__, ip_prefix->str);
		return -1;
	}

	return 0;
}

static int
clear_grantor_set(const struct ip_prefix *ip_prefix, struct grantor_set *set,
	struct gk_config *gk_conf)
{
	bool failed_one = false;
	unsigned int i;

	for (i = 0; i < set->num_entries; i++) {
		int ret = del_gateway_from_neigh_table_locked(ip_prefix,
			GK_FWD_GATEWAY_BACK_NET,
			set->entries[i].eth_cache, gk_conf);
		if (ret < 0)
			failed_one = true;
	}

	rte_free(set);

	return failed_one ? -1 : 0;
}

/*
 * Returns:
 *    >= 0 if the prefix already exists, the return is the FIB ID.
 * -ENOENT if the prefix does not exist.
 *     < 0 if an error occurred.
 */
static int
check_prefix_exists_locked(const struct ip_prefix *prefix,
	struct gk_config *gk_conf, struct gk_fib **p_fib)
{
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	uint32_t fib_id;
	int ret;

	if (prefix->addr.proto == RTE_ETHER_TYPE_IPV4) {
		ret = rib_is_rule_present(&ltbl->rib,
			(uint8_t *)&prefix->addr.ip.v4.s_addr,
			prefix->len, &fib_id);
		if (ret == 1 && p_fib != NULL)
			*p_fib = &ltbl->fib_tbl[fib_id];
	} else if (likely(prefix->addr.proto == RTE_ETHER_TYPE_IPV6)) {
		ret = rib_is_rule_present(&ltbl->rib6,
			prefix->addr.ip.v6.s6_addr, prefix->len, &fib_id);
		if (ret == 1 && p_fib != NULL)
			*p_fib = &ltbl->fib_tbl6[fib_id];
	} else {
		G_LOG(WARNING, "%s(%s): Unknown IP type %hu\n",
			__func__, prefix->str, prefix->addr.proto);
		if (p_fib != NULL)
			*p_fib = NULL;
		return -EINVAL;
	}

	if (ret == 1)
	       return fib_id;
	if (p_fib != NULL)
		*p_fib = NULL;
	if (likely(ret == 0))
		return -ENOENT;
	RTE_VERIFY(ret < 0 && ret != -ENOENT);
	return ret;
}

static int
check_prefix(const struct ip_prefix *prefix_info)
{
	if (unlikely(prefix_info->len < 0))
		return -EINVAL;

	if (unlikely(prefix_info->len == 0)) {
		G_LOG(WARNING, "%s(%s): Gatekeeper currently does not support default routes\n",
			__func__, prefix_info->str);
		return -EPERM;
	}

	return 0;
}

/*
 * For removing FIB entries, it needs to notify the GK instances
 * about the removal of the FIB entry.
 */
int
del_fib_entry_numerical_locked(const struct ip_prefix *prefix_info,
	struct gk_config *gk_conf)
{
	struct gk_fib *prefix_fib;
	int ret = check_prefix(prefix_info);

	if (unlikely(ret < 0))
		return ret;

	ret = check_prefix_exists_locked(prefix_info, gk_conf, &prefix_fib);
	if (unlikely(ret == -ENOENT)) {
		G_LOG(WARNING, "%s(%s): tried to delete a non-existent IP prefix\n",
			__func__, prefix_info->str);
		return -ENOENT;
	}

	if (unlikely(ret < 0)) {
		G_LOG(ERR, "%s(%s): check_prefix_exists_locked() failed (errno=%i): %s\n",
			__func__, prefix_info->str, -ret, strerror(-ret));
		return ret;
	}

	RTE_VERIFY(prefix_fib != NULL);

	/*
	 * GK_FWD_NEIGHBOR_*_NET FIB entries are initialized when
	 * Gatekeeper starts. These FIB entries are only reserved
	 * for the network prefixes which Gatekeeper is responsible.
	 * Changing these network prefixes requires restarting Gatekeeper,
	 * so one can ignore the deletion of these FIB entries.
	 */
	if (unlikely(prefix_fib->action == GK_FWD_NEIGHBOR_FRONT_NET ||
			prefix_fib->action == GK_FWD_NEIGHBOR_BACK_NET)) {
		G_LOG(WARNING, "%s(%s) cannot delete a LAN prefix of Gatekeeper\n",
			__func__, prefix_info->str);
		return -EPERM;
	}

	ret = lpm_del_route(&prefix_info->addr, prefix_info->len,
		&gk_conf->lpm_tbl);
	if (ret < 0) {
		G_LOG(ERR, "%s(%s) failed to remove the IP prefix (errno=%i): %s\n",
			__func__, prefix_info->str, -ret, strerror(-ret));
		return ret;
	}

	/*
	 * We need to notify the GK blocks whenever we remove
	 * a FIB entry that is accessible through a prefix.
	 */
	synchronize_gk_instances_with_fib(gk_conf, prefix_fib, false);

	/*
	 * From now on, GK blocks must not have a reference
	 * to @prefix_fib.
	 */

	switch (prefix_fib->action) {
	case GK_FWD_GRANTOR:
		ret = clear_grantor_set(prefix_info,
			prefix_fib->u.grantor.set, gk_conf);
		break;

	case GK_FWD_GATEWAY_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_GATEWAY_BACK_NET:
		ret = del_gateway_from_neigh_table_locked(
			prefix_info, prefix_fib->action,
			prefix_fib->u.gateway.eth_cache, gk_conf);
		break;

	case GK_DROP:
		break;

	case GK_FWD_NEIGHBOR_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_NEIGHBOR_BACK_NET:
		rte_panic("%s(%s): GK_FWD_NEIGHBOR_FRONT_NET and GK_FWD_NEIGHBOR_BACK_NET (action = %u) should have been handled above\n",
			__func__, prefix_info->str, prefix_fib->action);
		ret = -1;
		break;

	default:
		rte_panic("%s(%s): bug: unsupported action %u\n",
			__func__, prefix_info->str, prefix_fib->action);
		ret = -1;
		break;
	}

	/* Reset the fields of the deleted FIB entry. */
	initialize_fib_entry(prefix_fib);

	return ret;
}

/*
 * Initialize a gateway FIB entry.
 *
 * @gw_addr the gateway address information.
 * @ip_prefix the IP prefix for which the gateway is responsible.
 *
 * add_fib_entry_numerical() already ensured that the gateway
 * and the prefix have the same IP version.
 */
static int
init_gateway_fib_locked(const struct ip_prefix *ip_prefix,
	enum gk_fib_action action, const struct route_properties *props,
	struct ipaddr *gw_addr, struct gk_config *gk_conf)
{
	int ret, fib_id;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct gk_fib *gw_fib, *neigh_fib;
	struct ether_cache *eth_cache;
	struct neighbor_hash_table *neigh_ht;
	struct gatekeeper_if *iface;

	if (action == GK_FWD_GATEWAY_FRONT_NET)
		iface = &gk_conf->net->front;
	else if (likely(action == GK_FWD_GATEWAY_BACK_NET))
		iface = &gk_conf->net->back;
	else {
		G_LOG(ERR, "%s(%s): failed to initialize a fib entry for gateway because it has invalid action %d\n",
			__func__, ip_prefix->str, action);
		return -1;
	}

	/* Find the neighbor FIB entry for this gateway. */
	neigh_fib = find_fib_entry_for_neighbor_locked(
		gw_addr, action, gk_conf);
	if (neigh_fib == NULL) {
		G_LOG(ERR, "%s(%s): invalid gateway entry; could not find neighbor FIB\n",
			__func__, ip_prefix->str);
		return -1;
	}

	/* Find the Ethernet cached header entry for this gateway. */
	neigh_ht = &neigh_fib->u.neigh;
	eth_cache = neigh_get_ether_cache_locked(
		neigh_ht, gw_addr, iface, gk_conf->lcores[0]);
	if (eth_cache == NULL)
		return -1;

	/* Find an empty FIB entry for the Gateway. */
	fib_id = get_empty_fib_id(ip_prefix->addr.proto, gk_conf, &gw_fib);
	if (fib_id < 0)
		goto put_ether_cache;

	/* Fills up the Gateway FIB entry for the IP prefix. */
	gw_fib->action = action;
	gw_fib->u.gateway.eth_cache = eth_cache;
	gw_fib->u.gateway.props = *props;

	ret = lpm_add_route(&ip_prefix->addr, ip_prefix->len, fib_id, ltbl);
	if (ret < 0)
		goto init_fib;

	return 0;

init_fib:
	initialize_fib_entry(gw_fib);
put_ether_cache:
	ether_cache_put(neigh_fib, action, eth_cache, gk_conf);
	return -1;
}

#define MAX_NUM_GRANTORS_PER_ENTRY \
	((1 << (RTE_SIZEOF_FIELD(struct gk_fib, u.grantor.set->num_entries) * 8)) - 1)

/*
 * Initialize a Grantor FIB entry.
 *
 * @gt_addr the Grantor address information.
 * @gw_addr the gateway address information.
 * @ip_prefix the IP prefix for which the gateway is responsible.
 *
 * add_fib_entry_numerical() already ensured that the gateway
 * and the prefix have the same IP version.
 */
static int
init_grantor_fib_locked(const struct ip_prefix *ip_prefix,
	struct ipaddr *gt_addrs, struct ipaddr *gw_addrs,
	unsigned int num_addrs, struct gk_config *gk_conf,
	struct gk_fib *gt_fib)
{
	int ret;
	struct gk_fib *neigh_fibs[num_addrs];
	struct ether_cache *eth_caches[num_addrs];
	struct gatekeeper_if *iface = &gk_conf->net->back;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;
	struct grantor_set *new_set;
	unsigned int i, num_cache_holds = 0;
	int fib_id = -1;

	if (num_addrs > MAX_NUM_GRANTORS_PER_ENTRY) {
		G_LOG(ERR, "%s(%s): number of Grantor/gateway address pairs (%u) is greater than the max number of entries allowed (%d)\n",
			__func__, ip_prefix->str, num_addrs,
			MAX_NUM_GRANTORS_PER_ENTRY);
		return -1;
	}

	for (i = 0; i < num_addrs; i++) {
		struct neighbor_hash_table *neigh_ht;

		if (gt_addrs[i].proto != ip_prefix->addr.proto) {
			G_LOG(ERR, "%s(%s): failed to initialize a Grantor FIB entry, since the Grantor IP and the given IP prefix have different IP versions\n",
				__func__, ip_prefix->str);
			goto put_ether_cache;
		}

		/* Find the neighbor FIB entry for this gateway. */
		neigh_fibs[i] = find_fib_entry_for_neighbor_locked(
			&gw_addrs[i], GK_FWD_GATEWAY_BACK_NET, gk_conf);
		if (neigh_fibs[i]== NULL) {
			G_LOG(ERR, "%s(%s): invalid gateway entry; could not find neighbor FIB\n",
				__func__, ip_prefix->str);
			goto put_ether_cache;
		}

		/* Find the Ethernet cached header entry for this gateway. */
		neigh_ht = &neigh_fibs[i]->u.neigh;
		eth_caches[i] = neigh_get_ether_cache_locked(
			neigh_ht, &gw_addrs[i], iface, gk_conf->lcores[0]);
		if (eth_caches[i] == NULL)
			goto put_ether_cache;
		num_cache_holds++;
	}

	if (gt_fib == NULL) {
		fib_id = get_empty_fib_id(ip_prefix->addr.proto, gk_conf,
			&gt_fib);
		if (fib_id < 0)
			goto put_ether_cache;
	}

	new_set = rte_malloc_socket("gk_fib.grantor.set",
		sizeof(*new_set) + num_addrs * sizeof(*(new_set->entries)),
		0, rte_lcore_to_socket_id(gk_conf->lcores[0]));
	if (unlikely(new_set == NULL)) {
		G_LOG(ERR, "%s(%s): could not allocate set of Grantor entries\n",
			__func__, ip_prefix->str);
		goto put_ether_cache;
	}
	new_set->proto = ip_prefix->addr.proto;
	new_set->num_entries = num_addrs;
	for (i = 0; i < num_addrs; i++) {
		new_set->entries[i].gt_addr = gt_addrs[i];
		new_set->entries[i].eth_cache = eth_caches[i];
	}

	if (fib_id < 0) {
		/* Replace old set of Grantors in existing entry. */
		struct grantor_set *old_set = gt_fib->u.grantor.set;
		gt_fib->u.grantor.set = new_set;
		synchronize_gk_instances_with_fib(gk_conf, gt_fib, true);
		clear_grantor_set(ip_prefix, old_set, gk_conf);
	} else {
		/* Add new entry. */
		gt_fib->action = GK_FWD_GRANTOR;
		gt_fib->u.grantor.set = new_set;
		ret = lpm_add_route(&ip_prefix->addr,
			ip_prefix->len, fib_id, ltbl);
		if (ret < 0)
			goto init_fib;
	}

	return 0;

init_fib:
	initialize_fib_entry(gt_fib);
	rte_free(new_set);
put_ether_cache:
	for (i = 0; i < num_cache_holds; i++) {
		ether_cache_put(neigh_fibs[i], GK_FWD_GATEWAY_BACK_NET,
			eth_caches[i], gk_conf);
	}
	return -1;
}

static int
init_drop_fib_locked(const struct ip_prefix *ip_prefix,
	const struct route_properties *props, struct gk_config *gk_conf)
{
	int ret;
	struct gk_fib *ip_prefix_fib;
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	/* Initialize the fib entry for the IP prefix. */
	int fib_id = get_empty_fib_id(ip_prefix->addr.proto, gk_conf,
		&ip_prefix_fib);
	if (fib_id < 0)
		return -1;

	ip_prefix_fib->action = GK_DROP;
	ip_prefix_fib->u.drop.props = *props;

	ret = lpm_add_route(&ip_prefix->addr, ip_prefix->len, fib_id, ltbl);
	if (ret < 0) {
		initialize_fib_entry(ip_prefix_fib);
		return -1;
	}

	return 0;
}

/*
 * If a FIB entry already exists for @prefix, then @cur_fib points to it.
 * Otherwise, @cur_fib is NULL.
 */
static int
add_fib_entry_locked(const struct ip_prefix *prefix,
	struct ipaddr *gt_addrs, struct ipaddr *gw_addrs,
	unsigned int num_addrs, enum gk_fib_action action,
	const struct route_properties *props, struct gk_config *gk_conf,
	struct gk_fib *cur_fib)
{
	int ret;

	if (cur_fib != NULL && cur_fib->action != action) {
		G_LOG(ERR, "%s(%s): attempt to overwrite prefix whose action is %u with a new FIB entry of action %u; delete current FIB entry and add the new one\n",
			__func__, prefix->str, cur_fib->action, action);
		return -EINVAL;
	}

	switch (action) {
	case GK_FWD_GRANTOR:
		if (num_addrs < 1 || gt_addrs == NULL || gw_addrs == NULL)
			return -EINVAL;

		ret = init_grantor_fib_locked(prefix, gt_addrs, gw_addrs,
			num_addrs, gk_conf, cur_fib);
		if (ret < 0)
			return ret;

		break;
	case GK_FWD_GATEWAY_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_GATEWAY_BACK_NET:
		if (num_addrs != 1 || gt_addrs != NULL || gw_addrs == NULL ||
				cur_fib != NULL)
			return -EINVAL;

		ret = init_gateway_fib_locked(prefix, action, props,
			&gw_addrs[0], gk_conf);
		if (ret < 0)
			return ret;

		break;
	case GK_DROP:
		if (num_addrs != 0 || gt_addrs != NULL || gw_addrs != NULL ||
				cur_fib != NULL)
			return -EINVAL;

		ret = init_drop_fib_locked(prefix, props, gk_conf);
		if (ret < 0)
			return ret;

		break;
	case GK_FWD_NEIGHBOR_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_NEIGHBOR_BACK_NET:
		/* FALLTHROUGH */
	default:
		G_LOG(ERR, "%s(%s): invalid FIB action %u\n",
			__func__, prefix->str, action);
		return -EINVAL;
	}

	return 0;
}

/*
 * Return 0 when @gw_addr is not included in @prefix.
 * If not, or if there is an error, return a negative number.
 *
 * Issue #267 discusses the assumptions behind this verification.
 */
static int
check_gateway_prefix(const struct ip_prefix *prefix, struct ipaddr *gw_addr)
{
	if (unlikely(prefix->addr.proto != gw_addr->proto)) {
		G_LOG(ERR, "%s(%s): IP prefix protocol (%hu) does not match the gateway address protocol (%hu)\n",
			__func__, prefix->str, prefix->addr.proto,
			gw_addr->proto);
		return -EINVAL;
	}

	if (gw_addr->proto == RTE_ETHER_TYPE_IPV4) {
		uint32_t ip4_mask =
			rte_cpu_to_be_32(~0ULL << (32 - prefix->len));
		if ((prefix->addr.ip.v4.s_addr ^
				gw_addr->ip.v4.s_addr) & ip4_mask)
			return 0;
	} else if (likely(gw_addr->proto == RTE_ETHER_TYPE_IPV6)) {
		uint64_t ip6_mask;
		uint64_t *pf = (uint64_t *)prefix->addr.ip.v6.s6_addr;
		uint64_t *gw = (uint64_t *)gw_addr->ip.v6.s6_addr;

		if (prefix->len == 0) {
			/* Do nothing. */
		} else if (prefix->len <= 64) {
			ip6_mask = rte_cpu_to_be_64(
				~0ULL << (64 - prefix->len));
			if ((pf[0] ^ gw[0]) & ip6_mask)
				return 0;
		} else {
			ip6_mask = rte_cpu_to_be_64(
				~0ULL << (128 - prefix->len));
			if ((pf[0] != gw[0]) ||
					((pf[1] ^ gw[1]) & ip6_mask))
				return 0;
		}
	} else {
		G_LOG(CRIT, "%s(%s): bug: unknown IP type %hu\n",
			__func__, prefix->str, gw_addr->proto);
		return -EINVAL;
	}

	G_LOG(ERR, "%s(%s): gateway address is in prefix, so gateway is not a neighbor\n",
		__func__, prefix->str);
	return -EPERM;
}

/*
 * Verify that the IP addresses of gateway FIB entries are not included in
 * the prefix.
 */
static int
check_gateway_prefixes(const struct ip_prefix *prefix_info,
	struct ipaddr *gw_addrs, unsigned int num_addrs)
{
	unsigned int i;

	for (i = 0; i < num_addrs; i++) {
		int ret = check_gateway_prefix(prefix_info, &gw_addrs[i]);
		if (unlikely(ret < 0))
			return ret;
	}

	return 0;
}

static int
check_longer_prefixes(const char *context, const struct rib_head *rib,
	const void *ip, uint8_t depth, const struct gk_fib *fib_table,
	const char *prefix_str, enum gk_fib_action prefix_action)
{
	struct rib_longer_iterator_state state;
	int ret = rib_longer_iterator_state_init(&state, rib, ip, depth);
	if (unlikely(ret < 0)) {
		G_LOG(ERR, "%s(%s): failed to initialize the %s RIB iterator (errno=%i): %s\n",
			__func__, prefix_str, context, -ret, strerror(-ret));
		return ret;
	}

	while (true) {
		struct rib_iterator_rule rule;
		const struct gk_fib *fib;

		ret = rib_longer_iterator_next(&state, &rule);
		if (unlikely(ret < 0)) {
			if (unlikely(ret != -ENOENT)) {
				G_LOG(ERR, "%s(%s): %s RIB iterator failed (errno=%i): %s\n",
					__func__, prefix_str, context,
					-ret, strerror(-ret));
				goto out;
			}
			ret = 0;
			goto out;
		}

		fib = &fib_table[rule.next_hop];
		if (fib->action != GK_FWD_GRANTOR && fib->action != GK_DROP) {
			G_LOG(WARNING, "%s(%s): adding the %s rule with action %u would add a security hole since there already exists an entry of %u length with action %u\n",
				__func__, prefix_str, context, prefix_action,
				rule.depth, fib->action);
			ret = -EPERM;
			goto out;
		}
	}

out:
	rib_longer_iterator_end(&state);
	return ret;
}

static int
check_shorter_prefixes(const char *context, const struct rib_head *rib,
	const void *ip, uint8_t depth, const struct gk_fib *fib_table,
	const char *prefix_str, enum gk_fib_action prefix_action)
{
	struct rib_shorter_iterator_state state;
	int ret = rib_shorter_iterator_state_init(&state, rib, ip, depth);
	if (unlikely(ret < 0)) {
		G_LOG(ERR, "%s(%s): failed to initialize the %s RIB iterator (errno=%i): %s\n",
			__func__, prefix_str, context, -ret, strerror(-ret));
		return ret;
	}

	while (true) {
		struct rib_iterator_rule rule;
		const struct gk_fib *fib;

		ret = rib_shorter_iterator_next(&state, &rule);
		if (unlikely(ret < 0)) {
			if (unlikely(ret != -ENOENT)) {
				G_LOG(ERR, "%s(%s): %s RIB iterator failed (errno=%i): %s\n",
					__func__, prefix_str, context,
					-ret, strerror(-ret));
				goto out;
			}
			ret = 0;
			goto out;
		}

		fib = &fib_table[rule.next_hop];
		if (fib->action == GK_FWD_GRANTOR || fib->action == GK_DROP) {
			G_LOG(WARNING, "%s(%s): adding the %s rule with action %u would add a security hole since there already exists an entry of %u length with action %u\n",
				__func__, prefix_str, context, prefix_action,
				rule.depth, fib->action);
			ret = -EPERM;
			goto out;
		}
	}

out:
	rib_shorter_iterator_end(&state);
	return ret;
}

/*
 * This function makes sure that only a drop or another Grantor entry
 * can have a longer prefix than a drop or Grantor entry.
 *
 * The importance of this sanity check is illustrated in the following example:
 * assume that the prefix 10.1.1.0/24 forwards to a gateway and
 * the prefix 10.1.0.0/16 being added forwards to a Grantor.
 * Although the prefix 10.1.0.0/16 is intended to protect every host in that
 * destination, the prefix 10.1.1.0/24 is a longer match and leaves some of
 * those hosts unprotected. Without this sanity check, variations of this
 * example could go unnoticed until it is too late.
 */
static int
check_prefix_security_hole_locked(const struct ip_prefix *prefix,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	struct gk_lpm *ltbl = &gk_conf->lpm_tbl;

	if (action == GK_DROP || action == GK_FWD_GRANTOR) {
		/* Ensure that all prefixes longer than @prefix are safe. */

		if (prefix->addr.proto == RTE_ETHER_TYPE_IPV4) {
			return check_longer_prefixes("IPv4", &ltbl->rib,
				&prefix->addr.ip.v4.s_addr, prefix->len,
				ltbl->fib_tbl, prefix->str, action);
		}

		if (likely(prefix->addr.proto == RTE_ETHER_TYPE_IPV6)) {
			return check_longer_prefixes("IPv6", &ltbl->rib6,
				prefix->addr.ip.v6.s6_addr, prefix->len,
				ltbl->fib_tbl6, prefix->str, action);
		}

		goto unknown;
	}

	/* Ensure that all prefixer shorter than @prefix are safe. */

	if (prefix->addr.proto == RTE_ETHER_TYPE_IPV4) {
		return check_shorter_prefixes("IPv4", &ltbl->rib,
			&prefix->addr.ip.v4.s_addr, prefix->len,
			ltbl->fib_tbl, prefix->str, action);
	}

	if (likely(prefix->addr.proto == RTE_ETHER_TYPE_IPV6)) {
		return check_shorter_prefixes("IPv6", &ltbl->rib6,
			prefix->addr.ip.v6.s6_addr, prefix->len,
			ltbl->fib_tbl6, prefix->str, action);
	}

unknown:
	G_LOG(WARNING, "%s(%s): unknown IP type %hu with action %u\n",
		__func__, prefix->str, prefix->addr.proto, action);
	return -EINVAL;
}

/*
 * Add a FIB entry for a binary IP address prefix.
 *
 * GK_FWD_GRANTOR entries use both @gt_addrs and @gw_addrs,
 * and @num_addrs represents the number of such Grantor and
 * gateway pairs for the FIB entry.
 *
 * GK_DROP uses neither @gt_addrs nor @gw_addrs.
 *
 * All other entry types only use @gw_addrs, and should only
 * have one gateway (@num_addrs == 1).
 */
int
add_fib_entry_numerical_locked(const struct ip_prefix *prefix_info,
	struct ipaddr *gt_addrs, struct ipaddr *gw_addrs,
	unsigned int num_addrs, enum gk_fib_action action,
	const struct route_properties *props, struct gk_config *gk_conf)
{
	struct gk_fib *neigh_fib;
	int ret = check_prefix(prefix_info);

	if (unlikely(ret < 0))
		return ret;

	neigh_fib = find_fib_entry_for_neighbor_locked(
		&prefix_info->addr, GK_FWD_GATEWAY_FRONT_NET, gk_conf);
	if (neigh_fib != NULL) {
		G_LOG(ERR, "%s(%s): invalid prefix; prefix lookup found existing neighbor FIB on front interface\n",
			__func__, prefix_info->str);
		return -1;
	} else {
		/* Clarify LPM lookup miss that will occur in log. */
		G_LOG(INFO, "%s(%s): prefix lookup did not find existing neighbor FIB on front interface, as expected\n",
			__func__, prefix_info->str);
	}

	neigh_fib = find_fib_entry_for_neighbor_locked(
		&prefix_info->addr, GK_FWD_GATEWAY_BACK_NET, gk_conf);
	if (neigh_fib != NULL) {
		G_LOG(ERR, "%s(%s): invalid prefix; prefix lookup found existing neighbor FIB on back interface\n",
			__func__, prefix_info->str);
		return -1;
	} else {
		/* Clarify LPM lookup miss that will occur in log. */
		G_LOG(INFO, "%s(%s): prefix lookup did not find existing neighbor FIB on back interface, as expected\n",
			__func__, prefix_info->str);
	}

	ret = check_gateway_prefixes(prefix_info, gw_addrs, num_addrs);
	if (unlikely(ret < 0))
		return ret;

	ret = check_prefix_exists_locked(prefix_info, gk_conf, NULL);
	if (ret != -ENOENT) {
		G_LOG(ERR, "%s(%s): prefix already exists or error occurred\n",
			__func__, prefix_info->str);
		if (ret >= 0)
			return -EEXIST;
		return ret;
	}

	ret = check_prefix_security_hole_locked(prefix_info, action, gk_conf);
	if (ret < 0)
		return ret;

	return add_fib_entry_locked(prefix_info, gt_addrs, gw_addrs, num_addrs,
		action, props, gk_conf, NULL);
}

int
add_fib_entry_numerical(const struct ip_prefix *prefix_info,
	struct ipaddr *gt_addrs, struct ipaddr *gw_addrs,
	unsigned int num_addrs, enum gk_fib_action action,
	const struct route_properties *props, struct gk_config *gk_conf)
{
	int ret;

	rte_spinlock_lock_tm(&gk_conf->lpm_tbl.lock);
	ret = add_fib_entry_numerical_locked(prefix_info, gt_addrs, gw_addrs,
		num_addrs, action, props, gk_conf);
	rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);

	return ret;
}

static int
update_fib_entry_numerical(const struct ip_prefix *prefix_info,
	struct ipaddr *gt_addrs, struct ipaddr *gw_addrs,
	unsigned int num_addrs, enum gk_fib_action action,
	const struct route_properties *props, struct gk_config *gk_conf)
{
	int fib_id;
	struct gk_fib *cur_fib;
	int ret = check_prefix(prefix_info);

	if (unlikely(ret < 0))
		return ret;

	ret = check_gateway_prefixes(prefix_info, gw_addrs, num_addrs);
	if (unlikely(ret < 0))
		return ret;

	rte_spinlock_lock_tm(&gk_conf->lpm_tbl.lock);
	fib_id = check_prefix_exists_locked(prefix_info, gk_conf, &cur_fib);
	if (fib_id < 0) {
		G_LOG(ERR, "%s(%s): cannot update set of Grantors; prefix does not already exist or error occurred\n",
			__func__, prefix_info->str);
		rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);
		return -1;
	}

	ret = add_fib_entry_locked(prefix_info, gt_addrs, gw_addrs, num_addrs,
		action, props, gk_conf, cur_fib);
	rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);

	return ret;
}

static const struct route_properties default_route_properties = {
	.rt_proto = RTPROT_STATIC,
	.priority = 0,
};

int
add_fib_entry(const char *prefix, const char *gt_ip, const char *gw_ip,
	enum gk_fib_action action, struct gk_config *gk_conf)
{
	int ret;
	struct ip_prefix prefix_info;
	struct ipaddr gt_addr, gw_addr;
	struct ipaddr *gt_para = NULL, *gw_para = NULL;

	if (gt_ip != NULL) {
		ret = convert_str_to_ip(gt_ip, &gt_addr);
		if (ret < 0)
			return -1;
		gt_para = &gt_addr;
	}

	if (gw_ip != NULL) {
		ret = convert_str_to_ip(gw_ip, &gw_addr);
		if (ret < 0)
			return -1;
		gw_para = &gw_addr;
	}

	prefix_info.str = prefix;
	prefix_info.len = parse_ip_prefix(prefix, &prefix_info.addr);

	return add_fib_entry_numerical(&prefix_info,
		gt_para, gw_para,
		gt_ip != NULL || gw_ip != NULL ? 1 : 0,
		action, &default_route_properties, gk_conf);
}

int
del_fib_entry_numerical(const struct ip_prefix *prefix_info,
	struct gk_config *gk_conf)
{
	int ret;

	rte_spinlock_lock_tm(&gk_conf->lpm_tbl.lock);
	ret = del_fib_entry_numerical_locked(prefix_info, gk_conf);
	rte_spinlock_unlock_tm(&gk_conf->lpm_tbl.lock);

	return ret;
}

int
del_fib_entry(const char *ip_prefix, struct gk_config *gk_conf)
{
	struct ip_prefix prefix_info;

	prefix_info.str = ip_prefix;
	prefix_info.len = parse_ip_prefix(ip_prefix, &prefix_info.addr);

	return del_fib_entry_numerical(&prefix_info, gk_conf);
}

/*
 * Stack when function starts:
 *
 * 5 |  gw_addrs  | (passed as parameter)
 * 4 |  gt_addrs  | (passed as parameter)
 * 3 |   gk_conf  | (unused in this function)
 * 2 |    table   |
 * 1 | prefix_str | (unused in this function)
 *   |____________|
 */
static void
read_grantor_lb_entries(lua_State *l, lua_Integer tbl_size,
	struct ipaddr *gt_addrs, struct ipaddr *gw_addrs)
{
	lua_Integer i;

	/* Iterate over a table of tables. */
	for (i = 1; i <= tbl_size; i++) {
		const char *gt_ip, *gw_ip;
		int ret;

		/* Get the table at index i. */
		lua_rawgeti(l, 2, i);

		/*
		 * Make sure that the item inside
		 * the table is a table itself.
		 */
		if (!lua_istable(l, 6))
			luaL_error(l, "%s(): Grantor entry %ld is not a table",
				__func__, i);

		lua_getfield(l, 6, "gt_ip");
		lua_getfield(l, 6, "gw_ip");

		gt_ip = luaL_checkstring(l, 7);
		gw_ip = luaL_checkstring(l, 8);

		ret = convert_str_to_ip(gt_ip, &gt_addrs[i - 1]);
		if (ret < 0) {
			luaL_error(l, "%s(): cannot convert Grantor IP %s to bytes",
				__func__, gt_ip);
		}

		ret = convert_str_to_ip(gw_ip, &gw_addrs[i - 1]);
		if (ret < 0) {
			luaL_error(l, "%s(): cannot convert gateway IP %s to bytes",
				__func__, gw_ip);
		}

		/* Pop the Grantor/gateway and their table from Lua stack. */
		lua_pop(l, 3);
	}
}

static void
add_grantor_entry_lb_verify_params(lua_State *l, const char **prefix,
	lua_Integer *tbl_size, struct gk_config **gk_conf)
{
	uint32_t ctypeid;
	uint32_t correct_ctypeid_gk_config = luaL_get_ctypeid(l,
		CTYPE_STRUCT_GK_CONFIG_PTR);
	void *cdata;
	size_t len;

	if (lua_gettop(l) != 3) {
		luaL_error(l, "%s(): expected three arguments, however it received %d arguments",
		      __func__, lua_gettop(l));
	}

	/* First argument must be a prefix string. */
	*prefix = lua_tolstring(l, 1, &len);
	if (*prefix == NULL || len == 0)
		luaL_error(l, "%s(): could not read prefix for adding load balanced Grantor set",
			__func__);

	/* Second argument must be a table. */
	luaL_checktype(l, 2, LUA_TTABLE);
	*tbl_size = lua_objlen(l, 2);
	if (*tbl_size <= 0)
		luaL_error(l, "%s(): table must have a positive number of Grantor entries",
			__func__);

	/* Third argument must be of type CTYPE_STRUCT_GK_CONFIG_PTR. */
	cdata = luaL_checkcdata(l, 3,
		&ctypeid, CTYPE_STRUCT_GK_CONFIG_PTR);
	if (ctypeid != correct_ctypeid_gk_config) {
		luaL_error(l, "%s(): expected '%s' as the third argument",
			__func__, CTYPE_STRUCT_GK_CONFIG_PTR);
	}
	*gk_conf = *(struct gk_config **)cdata;
}

static int
__add_grantor_entry_lb(lua_State *l, int overwrite)
{
	const char *prefix;
	struct ip_prefix prefix_info;
	lua_Integer tbl_size;
	struct gk_config *gk_conf;
	struct ipaddr *gt_addrs;
	struct ipaddr *gw_addrs;
	int ret;

	/* Verify presence and types of parameters and read them in. */
	add_grantor_entry_lb_verify_params(l, &prefix,
		&tbl_size, &gk_conf);

	gt_addrs = lua_newuserdata(l, tbl_size * sizeof(*gt_addrs));
	gw_addrs = lua_newuserdata(l, tbl_size * sizeof(*gw_addrs));

	read_grantor_lb_entries(l, tbl_size, gt_addrs, gw_addrs);

	/* Set up prefix info. */
	prefix_info.str = prefix;
	prefix_info.len = parse_ip_prefix(prefix, &prefix_info.addr);

	if (overwrite) {
		ret = update_fib_entry_numerical(&prefix_info,
			gt_addrs, gw_addrs, tbl_size, GK_FWD_GRANTOR,
			&default_route_properties, gk_conf);
	} else {
		ret = add_fib_entry_numerical(&prefix_info,
			gt_addrs, gw_addrs, tbl_size, GK_FWD_GRANTOR,
			&default_route_properties, gk_conf);
	}
	if (ret < 0)
		luaL_error(l, "%s(): could not add or update FIB entry; check Gatekeeper log",
			__func__);

	return 0;
}

int
l_add_grantor_entry_lb(lua_State *l)
{
	return __add_grantor_entry_lb(l, false);
}

int
l_update_grantor_entry_lb(lua_State *l)
{
	return __add_grantor_entry_lb(l, true);
}

static void
fillup_gk_fib_dump_entry_ether(struct fib_dump_addr_set *addr_set,
	struct ether_cache *eth_cache)
{
	addr_set->stale = eth_cache->stale;
	addr_set->nexthop_ip = eth_cache->ip_addr;
	rte_ether_addr_copy(&eth_cache->l2_hdr.eth_hdr.d_addr,
		&addr_set->d_addr);
}

/*
 * CAUTION: fields @dentry->addr and @dentry->prefix_len must be filled in
 * before calling this function.
 */
static void
fillup_gk_fib_dump_entry(struct gk_fib_dump_entry *dentry,
	const struct gk_fib *fib)
{
	dentry->action = fib->action;
	switch (dentry->action) {
	case GK_FWD_GRANTOR: {
		unsigned int i;
		for (i = 0; i < dentry->num_addr_sets; i++) {
			dentry->addr_sets[i].grantor_ip =
				fib->u.grantor.set->entries[i].gt_addr;
			fillup_gk_fib_dump_entry_ether(&dentry->addr_sets[i],
				fib->u.grantor.set->entries[i].eth_cache);
		}
		break;
	}
	case GK_FWD_GATEWAY_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_GATEWAY_BACK_NET:
		fillup_gk_fib_dump_entry_ether(&dentry->addr_sets[0],
			fib->u.gateway.eth_cache);
		break;

	case GK_FWD_NEIGHBOR_FRONT_NET:
		/* FALLTHROUGH */
	case GK_FWD_NEIGHBOR_BACK_NET:
		/* FALLTHROUGH */
	case GK_DROP:
		break;

	default: {
		/*
		 * Things went bad, but keep going.
		 */

		char str_prefix[INET6_ADDRSTRLEN];

		RTE_BUILD_BUG_ON(INET6_ADDRSTRLEN < INET_ADDRSTRLEN);

		if (unlikely(convert_ip_to_str(&dentry->addr, str_prefix,
				sizeof(str_prefix)) < 0))
			strcpy(str_prefix, "<ERROR>");
		G_LOG(CRIT, "%s(%s/%i): invalid FIB action (%u) in FIB",
			__func__, str_prefix, dentry->prefix_len, fib->action);
		break;
	}
	}
}

#define CTYPE_STRUCT_FIB_DUMP_ENTRY_PTR "struct gk_fib_dump_entry *"

static inline unsigned int
num_addrs_entry_type(const struct gk_fib *fib)
{
	switch (fib->action) {
	case GK_FWD_GRANTOR:
		return fib->u.grantor.set->num_entries;
	case GK_DROP:
		return 0;
	default:
		/* All other entry types have a single Gateway. */
		return 1;
	}
}

typedef void (*set_addr_t)(struct ipaddr *addr, rib_address_t address_no);

static void
list_fib_entries(lua_State *l, const char *context, const struct rib_head *rib,
	const struct gk_fib *fib_table, rte_spinlock_t *lock, set_addr_t setf,
	uint8_t batch_size)
{
	struct gk_fib_dump_entry *dentry = NULL;
	size_t dentry_size = 0;
	uint32_t correct_ctypeid_fib_dump_entry = luaL_get_ctypeid(l,
		CTYPE_STRUCT_FIB_DUMP_ENTRY_PTR);
	uint8_t current_batch_size = 0;
	struct rib_longer_iterator_state state;
	int ret;

	rte_spinlock_lock_tm(lock);
	ret = rib_longer_iterator_state_init(&state, rib, NULL, 0);
	if (unlikely(ret < 0)) {
		rte_spinlock_unlock_tm(lock);
		luaL_error(l, "%s(): failed to initialize the %s RIB iterator (errno=%d): %s",
			__func__, context, -ret, strerror(-ret));
	}

	while (true) {
		struct rib_iterator_rule rule;
		const struct gk_fib *fib;
		unsigned int num_addrs;
		size_t new_dentry_size;
		int done;
		void *cdata;

		ret = rib_longer_iterator_next(&state, &rule);
		if (unlikely(ret < 0)) {
			rte_free(dentry);
			rib_longer_iterator_end(&state);
			rte_spinlock_unlock_tm(lock);
			if (unlikely(ret != -ENOENT)) {
				luaL_error(l, "%s(): %s RIB iterator failed (errno=%d): %s",
					__func__, context,
					-ret, strerror(-ret));
			}
			return;
		}

		fib = &fib_table[rule.next_hop];
		if (unlikely(fib->action == GK_FWD_NEIGHBOR_FRONT_NET ||
				fib->action == GK_FWD_NEIGHBOR_BACK_NET))
			continue;

		num_addrs = num_addrs_entry_type(fib);
		new_dentry_size = sizeof(*dentry) +
			num_addrs * sizeof(*dentry->addr_sets);

		if (new_dentry_size > dentry_size) {
			dentry_size = new_dentry_size;
			rte_free(dentry);
			/*
			 * We don't need rte_zmalloc_socket() here because
			 * the memory is not being used by the GK block.
			 */
			dentry = rte_zmalloc("fib_dump", dentry_size, 0);
			if (unlikely(dentry == NULL)) {
				rib_longer_iterator_end(&state);
				rte_spinlock_unlock_tm(lock);
				luaL_error(l, "%s(): failed to allocate memory for the %s FIB dump",
					__func__, context);
			}
		} else
			memset(dentry, 0, new_dentry_size);

		setf(&dentry->addr, rule.address_no);
		dentry->prefix_len = rule.depth;
		dentry->fib_id = rule.next_hop;
		dentry->num_addr_sets = num_addrs;
		fillup_gk_fib_dump_entry(dentry, fib);

		lua_pushvalue(l, 2);
		lua_insert(l, 3);
		cdata = luaL_pushcdata(l, correct_ctypeid_fib_dump_entry,
			sizeof(struct gk_fib_dump_entry *));
		*(struct gk_fib_dump_entry **)cdata = dentry;
		lua_insert(l, 4);

		if (lua_pcall(l, 2, 2, 0) != 0) {
			rte_free(dentry);
			rib_longer_iterator_end(&state);
			rte_spinlock_unlock_tm(lock);
			lua_error(l);
		}

		done = lua_toboolean(l, -2);
		lua_remove(l, -2);
		if (unlikely(done)) {
			rte_free(dentry);
			rib_longer_iterator_end(&state);
			rte_spinlock_unlock_tm(lock);
			return;
		}

		if (++current_batch_size >= batch_size) {
			/* Release the lock after dumping the full batch. */
			rte_spinlock_unlock_tm(lock);

			current_batch_size = 0;

			/* Give other lcores a chance to acquire the lock. */
			rte_pause();

			/*
			 * Obtain the lock when starting a new dumping batch.
			 * For the last batch, the lock will be released at
			 * the end.
			 */
			rte_spinlock_lock_tm(lock);
		}
	}
}

static void
set_addr4(struct ipaddr *addr, rib_address_t address_no)
{
	addr->proto = RTE_ETHER_TYPE_IPV4;
	addr->ip.v4.s_addr = ipv4_from_rib_addr(address_no);
}

static void
set_addr6(struct ipaddr *addr, rib_address_t address_no)
{
	addr->proto = RTE_ETHER_TYPE_IPV6;
	rte_memcpy(&addr->ip.v6, &address_no, sizeof(addr->ip.v6));
}

#define CTYPE_STRUCT_GK_CONFIG_PTR "struct gk_config *"

static int
list_fib_for_lua(lua_State *l, bool list_ipv4)
{
	struct gk_config *gk_conf;
	uint32_t ctypeid;
	uint32_t correct_ctypeid_gk_config = luaL_get_ctypeid(l,
		CTYPE_STRUCT_GK_CONFIG_PTR);
	struct gk_lpm *ltbl;

	/* First argument must be of type CTYPE_STRUCT_GK_CONFIG_PTR. */
	void *cdata = luaL_checkcdata(l, 1,
		&ctypeid, CTYPE_STRUCT_GK_CONFIG_PTR);
	if (ctypeid != correct_ctypeid_gk_config)
		luaL_error(l, "%s(): expected `%s' as first argument",
			__func__, CTYPE_STRUCT_GK_CONFIG_PTR);

	/* Second argument must be a Lua function. */
	luaL_checktype(l, 2, LUA_TFUNCTION);

	/* Third argument should be a Lua value. */
	if (lua_gettop(l) != 3)
		luaL_error(l, "%s(): expected three arguments, however it got %d arguments",
			__func__, lua_gettop(l));

	gk_conf = *(struct gk_config **)cdata;
	ltbl = &gk_conf->lpm_tbl;

	if (list_ipv4) {
		list_fib_entries(l, "IPv4", &ltbl->rib, ltbl->fib_tbl,
			&ltbl->lock, set_addr4, gk_conf->fib_dump_batch_size);
	} else {
		list_fib_entries(l, "IPv6", &ltbl->rib6, ltbl->fib_tbl6,
			&ltbl->lock, set_addr6, gk_conf->fib_dump_batch_size);
	}

	lua_remove(l, 1);
	lua_remove(l, 1);
	return 1;
}

int
l_list_gk_fib4(lua_State *l)
{
	return list_fib_for_lua(l, true);
}

int
l_list_gk_fib6(lua_State *l)
{
	return list_fib_for_lua(l, false);
}

static void
fillup_gk_neighbor_dump_entry(struct gk_neighbor_dump_entry *dentry,
	struct ether_cache *eth_cache)
{
	dentry->stale = eth_cache->stale;
	rte_memcpy(&dentry->neigh_ip, &eth_cache->ip_addr,
		sizeof(dentry->neigh_ip));
	rte_memcpy(&dentry->d_addr, &eth_cache->l2_hdr.eth_hdr.d_addr,
		sizeof(dentry->d_addr));
}

#define CTYPE_STRUCT_NEIGHBOR_DUMP_ENTRY_PTR "struct gk_neighbor_dump_entry *"

static void
list_hash_table_neighbors_unlock(lua_State *l, enum gk_fib_action action,
	struct neighbor_hash_table *neigh_ht, struct gk_lpm *ltbl)
{
	uint32_t next = 0;
	const void *key;
	void *data;
	void *cdata;
	uint32_t correct_ctypeid_neighbor_dentry = luaL_get_ctypeid(l,
		CTYPE_STRUCT_NEIGHBOR_DUMP_ENTRY_PTR);

	int32_t index = rte_hash_iterate(neigh_ht->hash_table,
		(void *)&key, &data, &next);
	while (index >= 0) {
		struct gk_neighbor_dump_entry dentry;
		struct ether_cache *eth_cache = data;

		dentry.action = action;
		fillup_gk_neighbor_dump_entry(&dentry, eth_cache);

		lua_pushvalue(l, 2);
		lua_insert(l, 3);
		cdata = luaL_pushcdata(l, correct_ctypeid_neighbor_dentry,
			sizeof(struct gk_neighbor_dump_entry *));
		*(struct gk_neighbor_dump_entry **)cdata = &dentry;
		lua_insert(l, 4);

		if (lua_pcall(l, 2, 1, 0) != 0) {
			rte_spinlock_unlock_tm(&ltbl->lock);
			lua_error(l);
		}

		index = rte_hash_iterate(neigh_ht->hash_table,
			(void *)&key, &data, &next);
	}

	rte_spinlock_unlock_tm(&ltbl->lock);
}

static void
list_ipv4_if_neighbors(lua_State *l, struct gatekeeper_if *iface,
	enum gk_fib_action action, struct gk_lpm *ltbl)
{
	int ret;
	uint32_t fib_id;
	struct gk_fib *neigh_fib;

	rte_spinlock_lock_tm(&ltbl->lock);
	ret = rib_lookup(&ltbl->rib, (uint8_t *)&iface->ip4_addr.s_addr,
		&fib_id);
	/*
	 * Invalid gateway entry, since at least we should
	 * obtain the FIB entry for the neighbor table.
	 */
	if (unlikely(ret < 0)) {
		rte_spinlock_unlock_tm(&ltbl->lock);
		luaL_error(l, "%s(): failed to lookup the lpm table (errno=%d): %s",
			__func__, -ret, strerror(-ret));
	}

	neigh_fib = &ltbl->fib_tbl[fib_id];
	RTE_VERIFY(neigh_fib->action == action);

	list_hash_table_neighbors_unlock(l, action, &neigh_fib->u.neigh, ltbl);
}

static void
list_ipv6_if_neighbors(lua_State *l, struct gatekeeper_if *iface,
	enum gk_fib_action action, struct gk_lpm *ltbl)
{
	int ret;
	uint32_t fib_id;
	struct gk_fib *neigh_fib;

	rte_spinlock_lock_tm(&ltbl->lock);
	ret = rib_lookup(&ltbl->rib6, iface->ip6_addr.s6_addr, &fib_id);
	/*
	 * Invalid gateway entry, since at least we should
	 * obtain the FIB entry for the neighbor table.
	 */
	if (unlikely(ret < 0)) {
		rte_spinlock_unlock_tm(&ltbl->lock);
		luaL_error(l, "%s(): failed to lookup the lpm6 table (errno=%d): %s",
			__func__, -ret, strerror(-ret));
	}

	neigh_fib = &ltbl->fib_tbl6[fib_id];
	RTE_VERIFY(neigh_fib->action == action);

	list_hash_table_neighbors_unlock(l, action, &neigh_fib->u.neigh, ltbl);
}

static void
list_ipv4_neighbors(lua_State *l,
	struct net_config *net_conf, struct gk_lpm *ltbl)
{
	if (!ipv4_configured(net_conf))
		return;

	list_ipv4_if_neighbors(l, &net_conf->front,
		GK_FWD_NEIGHBOR_FRONT_NET, ltbl);

	if (net_conf->back_iface_enabled)
		list_ipv4_if_neighbors(l, &net_conf->back,
			GK_FWD_NEIGHBOR_BACK_NET, ltbl);
}

static void
list_ipv6_neighbors(lua_State *l,
	struct net_config *net_conf, struct gk_lpm *ltbl)
{
	if (!ipv6_configured(net_conf))
		return;

	list_ipv6_if_neighbors(l, &net_conf->front,
		GK_FWD_NEIGHBOR_FRONT_NET, ltbl);

	if (net_conf->back_iface_enabled)
		list_ipv6_if_neighbors(l, &net_conf->back,
			GK_FWD_NEIGHBOR_BACK_NET, ltbl);
}

typedef void (*list_neighbors)(lua_State *l,
	struct net_config *net_conf, struct gk_lpm *ltbl);

static void
list_neighbors_for_lua(lua_State *l, list_neighbors f)
{
	struct gk_config *gk_conf;
	uint32_t ctypeid;
	uint32_t correct_ctypeid_gk_config = luaL_get_ctypeid(l,
		CTYPE_STRUCT_GK_CONFIG_PTR);

	/* First argument must be of type CTYPE_STRUCT_GK_CONFIG_PTR. */
	void *cdata = luaL_checkcdata(l, 1,
		&ctypeid, CTYPE_STRUCT_GK_CONFIG_PTR);
	if (ctypeid != correct_ctypeid_gk_config)
		luaL_error(l, "%s(): expected `%s' as first argument",
			__func__, CTYPE_STRUCT_GK_CONFIG_PTR);

	/* Second argument must be a Lua function. */
	luaL_checktype(l, 2, LUA_TFUNCTION);

	/* Third argument should be a Lua value. */
	if (lua_gettop(l) != 3)
		luaL_error(l, "%s(): expected three arguments, however it got %d arguments",
			__func__, lua_gettop(l));

	gk_conf = *(struct gk_config **)cdata;

	f(l, gk_conf->net, &gk_conf->lpm_tbl);

	lua_remove(l, 1);
	lua_remove(l, 1);
}

int
l_list_gk_neighbors4(lua_State *l)
{
	list_neighbors_for_lua(l, list_ipv4_neighbors);
	return 1;
}

int
l_list_gk_neighbors6(lua_State *l)
{
	list_neighbors_for_lua(l, list_ipv6_neighbors);
	return 1;
}

#define CTYPE_STRUCT_ETHER_ADDR_REF "struct rte_ether_addr &"

int
l_ether_format_addr(lua_State *l)
{
	struct rte_ether_addr *d_addr;
	char d_buf[RTE_ETHER_ADDR_FMT_SIZE];
	uint32_t ctypeid;
	uint32_t correct_ctypeid_ether_addr = luaL_get_ctypeid(l,
		CTYPE_STRUCT_ETHER_ADDR_REF);

	/* First argument must be of type CTYPE_STRUCT_ETHER_ADDR_REF. */
	void *cdata = luaL_checkcdata(l, 1,
		&ctypeid, CTYPE_STRUCT_ETHER_ADDR_REF);
	if (ctypeid != correct_ctypeid_ether_addr)
		luaL_error(l, "%s(): expected `%s' as first argument",
			__func__, CTYPE_STRUCT_ETHER_ADDR_REF);

	d_addr = *(struct rte_ether_addr **)cdata;

	rte_ether_format_addr(d_buf, sizeof(d_buf), d_addr);

	lua_pushstring(l, d_buf);

	return 1;
}

#define CTYPE_STRUCT_IP_ADDR_REF "struct ipaddr &"

int
l_ip_format_addr(lua_State *l)
{
	struct ipaddr *ip_addr;
	char ip[MAX_INET_ADDRSTRLEN];
	int ret;
	uint32_t ctypeid;
	uint32_t correct_ctypeid_ip_addr = luaL_get_ctypeid(l,
		CTYPE_STRUCT_IP_ADDR_REF);

	/* First argument must be of type CTYPE_STRUCT_IP_ADDR_REF. */
	void *cdata = luaL_checkcdata(l, 1,
		&ctypeid, CTYPE_STRUCT_IP_ADDR_REF);
	if (ctypeid != correct_ctypeid_ip_addr)
		luaL_error(l, "%s(): expected `%s' as first argument",
			__func__, CTYPE_STRUCT_IP_ADDR_REF);

	ip_addr = *(struct ipaddr **)cdata;

	ret = convert_ip_to_str(ip_addr, ip, sizeof(ip));
	if (ret < 0)
		luaL_error(l, "%s(): failed to convert an IP address to string",
			__func__);

	lua_pushstring(l, ip);

	return 1;
}
