/*
 *	BIRD -- Simple Network Management Protocol (SNMP)
 *
 *      (c) 2022 Vojtech Vilimek <vojtech.vilimek@nic.cz>
 *      (c) 2022 CZ.NIC z.s.p.o
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 */

#ifndef _BIRD_SNMP_H_
#define _BIRD_SNMP_H_

#include "lib/ip.h"
#include "lib/socket.h"
#include "lib/timer.h"
#include "nest/bird.h"
#include "nest/protocol.h"
#include "proto/bgp/bgp.h"


#define SNMP_UNDEFINED	0
#define SNMP_BGP	1
#define SNMP_OSPF	2
#define SNMP_INVALID  255

#define SNMP_PORT 705

#define SNMP_RX_BUFFER_SIZE 2048
#define SNMP_TX_BUFFER_SIZE 2048

#define SNMP_INIT 1
#define SNMP_REGISTR 2
#define SNMP_CONN 3
#define SNMP_ERR 4

#define SNMP_OFF 0


/* hash table macros */
#define SNMP_HASH_KEY(n)  n->peer_ip
#define SNMP_HASH_NEXT(n) n->next
#define SNMP_HASH_EQ(ip1, ip2) ipa_equal(ip1, ip2)
#define SNMP_HASH_FN(ip)  ipa_hash(ip)

struct snmp_bond {
  node n;
  struct proto_config *proto;
  u8 type;
};

struct snmp_config {
  struct proto_config cf;
  ip_addr local_ip;
  ip_addr remote_ip;
  u16 local_port;
  u16 remote_port;
  u32 local_as;
  u8 timeout;
  //struct iface *iface;
  list bgp_entries;
};

struct snmp_bgp_peer {
  struct snmp_bond *bond;
  ip_addr peer_ip;
  struct snmp_bgp_peer *next;
};

struct snmp_proto {
  struct proto p;
  struct object_lock *lock;
 
  ip_addr local_ip;
  ip_addr remote_ip;
  u16 local_port;
  u16 remote_port;
  u32 local_as;

  sock *sock;
  u8 timeout;

  u32 session_id;
  u32 transaction_id;
  u32 packet_id;

  //struct iface *iface;
  // map goes here
  HASH(struct snmp_bgp_peer) bgp_hash;
  struct tbf rl_gen;

  timer *ping_timer;
  timer *retry_timer;
  u8 state;

  list bgp_entries;
};

/*
struct snmp_channel_config {
  struct channel_config c;
  struct bgp_config *bgp;
  u8 type;
};

struct snmp_channel {
  struct channel c;
};
*/


#endif