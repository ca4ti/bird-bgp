/*
 *	BIRD -- Simple Network Management Protocol (SNMP)
 *
 *      (c) 2022 Vojtech Vilimek <vojtech.vilimek@nic.cz>
 *      (c) 2022 CZ.NIC z.s.p.o
 *
 *	Can be freely distributed and used under the terms of the GNU GPL.
 *
 */

#include "lib/unaligned.h"
#include "subagent.h"
#include "bgp_mib.h"

/* =============================================================
 *  Problems
 *  ------------------------------------------------------------
 *
 *    change of remote ip -> no notification, no update 
 *    same ip, different ports
 *    distinct VRF (two interfaces with overlapping private addrs)
 *    posible link-local addresses in LOCAL_IP
 *
 */

static int parse_response(struct snmp_proto *p, byte *buf, uint size);
static void header_update_len(byte *buf, u32 len);
static inline uint oid_size(struct oid *o);
static inline uint vb_size(struct agentx_varbind *vb);
static int snmp_stop_ack(sock *sk, uint size);
static void do_response(struct snmp_proto *p, byte *buf, uint size);
static int parse_get_pdu(struct snmp_proto *p, byte *buf, uint size);
static int parse_gets_pdu(struct snmp_proto *p, byte *buf, uint size);
//static void response_fail(struct snmp_proto *p, u16 err_no, u16 index);
static byte *prepare_response(struct snmp_proto *p, byte *buf, uint size);
static void response_err_ind(byte *buf, uint err, uint ind);
static struct oid *bgp_search(struct oid *o_start, struct oid *o_end, struct oid *o_curr);
static struct oid *prefixize(struct snmp_proto *p, struct oid *o, int byte_ord);
static inline byte *find_n_fill(struct snmp_proto *p, struct oid *o, byte *buf, uint size, uint contid, int byte_ord);

static const char * const snmp_errs[] = {
  #define SNMP_ERR_SHIFT 256
  [AGENTX_RES_OPEN_FAILED - SNMP_ERR_SHIFT] = "Open failed",
  [AGENTX_RES_NOT_OPEN - SNMP_ERR_SHIFT] = "Not open",
  [AGENTX_RES_INDEX_WRONG_TYPE - SNMP_ERR_SHIFT] = "Index wrong type",
  [AGENTX_RES_INDEX_ALREADY_ALLOC - SNMP_ERR_SHIFT] = "Index already allocated",
  [AGENTX_RES_INDEX_NONE_AVAIL - SNMP_ERR_SHIFT] = "Index none availlable",
  [AGENTX_RES_NOT_ALLOCATED - SNMP_ERR_SHIFT] = "Not allocated",
  [AGENTX_RES_UNSUPPORTED_CONTEXT - SNMP_ERR_SHIFT] = "Unsupported contex",
  [AGENTX_RES_DUPLICATE_REGISTR - SNMP_ERR_SHIFT] = "Duplicate registration",
  [AGENTX_RES_UNKNOWN_REGISTR - SNMP_ERR_SHIFT] = "Unknown registration",
  [AGENTX_RES_UNKNOWN_AGENT_CAPS - SNMP_ERR_SHIFT] = "Unknown agent caps",
  [AGENTX_RES_PARSE_ERROR - SNMP_ERR_SHIFT] = "Parse error",
  [AGENTX_RES_REQUEST_DENIED - SNMP_ERR_SHIFT] = "Request denied",
  [AGENTX_RES_PROCESSING_ERR - SNMP_ERR_SHIFT] = "Processing error",
};

/* payload length in bytes */
static inline size_t
pkt_len(byte *buf, byte *pkt)
{
  return (pkt - buf) - AGENTX_HEADER_SIZE;
}

static inline size_t
str_size(const char *str)
{
  return 4 + BIRD_ALIGN(strlen(str), 4);
}

static byte *
put_str(byte *buf, const char *str)
{
  uint len = strlen(str);
  uint slen = BIRD_ALIGN(len, 4);

  if (len > MAX_STR)
    return NULL;

  STORE_PTR(buf, len);

  memcpy(buf + 4, str, len);

  for (uint i = 0; i < slen - len; i++)
    buf[len + i] = 0x00;  // PADDING

  return buf + str_size(str);
}

static byte *
put_blank(byte *buf)
{
  STORE_PTR(buf, 0);
  return buf + 4;
}

static byte *
put_oid(byte *buf, struct oid *oid)
{
  log(L_INFO "testing oid");
  for (uint i = 0; i < oid->n_subid; i++)
    log(L_INFO "oid id %d:  %u", i, oid->ids[i]);
  log(L_INFO "put_oid()");
  put_u8(buf, oid->n_subid);
  log(L_INFO "data %p: %02X", buf, *buf);
  put_u8(++buf, oid->prefix);
  log(L_INFO "data %p: %02X", buf, *buf);
  put_u8(++buf, oid->include);
  log(L_INFO "data %p: %02X", buf, *buf);
  put_u8(++buf, 0);  // PADDING
  /* last increment */
  ++buf;
  log(L_INFO "oid head end %p", buf);

  /* copy OID data */
#ifdef SNMP_NATIVE
  for (uint i = 0; i < oid->n_subid; i++)
    *(((u32 *) buf) + i) = oid->ids[i];
#else
  put_u32s(buf, oid->ids, oid->n_subid << 2);
#endif
/*
  for (uint i = 0; i <= (oid->n_subid << 2) +4 ; i += 4)
    log(L_INFO "OID % 3u: %02X %02X %02X %02X", i,
      *(buf - 4 + i),
      *(buf - 4 + i + 1),
      *(buf - 4 + i + 2),
      *(buf - 4  + i + 3)
  );
*/

  return buf + (oid->n_subid << 2);
}


/* paste data at first byte in message 
 *   with 3B of padding
 */
static byte *
paste_fbyte(byte *buf, u8 data)
{
  log(L_INFO "paste_fbyte()");
  put_u8(buf, data);
  put_u24(++buf, 0);  // PADDING
  return buf + 3;
}

static void
open_pdu(struct snmp_proto *p, struct oid *oid)
{
  sock *sk = p->sock;
  byte *buf, *pkt;
  buf = pkt = sk->tbuf;
  uint size = sk->tbsize;

  // should be configurable
  const char *str = "bird";

  //uint pkt_size = 0;

  if (size > AGENTX_HEADER_SIZE + oid_size(oid) + str_size(str))
  {
    log(L_INFO "open_pdu()");

    struct agentx_header *h;
    SNMP_CREATE(pkt, struct agentx_header, h)
    SNMP_B_HEADER(h, AGENTX_OPEN_PDU)

    STORE(h->session_id, 1);
    STORE(h->transaction_id, 1);
    STORE(h->packet_id, 1);

    pkt = paste_fbyte(pkt, p->timeout);
    pkt = put_oid(pkt, oid);
    pkt = put_str(pkt, str);

    SNMP_UPDATE(h, pkt_len(buf, pkt));

    int ret = sk_send(sk, pkt - buf);

    if (ret == 0)
      log(L_INFO "sleep");
    else if (ret < 0)
      log(L_INFO "err %d", ret);
    else
      log(L_INFO "ok !!! ");
  }

  else
    log(L_INFO "open_pdu() insufficient size, %u <= %u ",
	size, AGENTX_HEADER_SIZE + oid_size(oid) + str_size(str));
}

/* index allocate / deallocate pdu * /
static void
de_allocate_pdu(struct snmp_proto *p, struct oid *oid, u8 type)
{
  sock *sk = p->sock;
  byte *buf, *pkt;
  buf = pkt = sk->tbuf;
  uint size = sk->tbsize;

  if (size > AGENTX_HEADER_SIZE + )
  {
    log(L_INFO "de_allocate_pdu()");

    struct agentx_header *h;
    SNMP_CREATE(pkt, struct agentx_header, h);
    SNMP_B_HEADER(h, type);
    SNMP_SESSION(h,p);

    struct agentx_varbind *vb = (struct agentx_varbind *) pkt;
    STORE_16(vb->type, AGENTX_OBJECT_ID);
    STORE(vb->oid,
  }

  else
    log(L_INFO "de_allocate_pdu(): insufficient size");
}
*/

/* register / unregister pdu */
static void
un_register_pdu(struct snmp_proto *p, struct oid *oid, uint index, uint len, u8 type)
{
  sock *sk = p->sock;
  byte *buf, *pkt;
  buf = pkt = sk->tbuf;
  uint size = sk->tbsize;

  /* conditional +4 for upper-bound */
  if (size > AGENTX_HEADER_SIZE + oid_size(oid) + ((len > 1) ? 4 : 0))
  {
    log(L_INFO "un_register_pdu()");
    struct agentx_un_register_pdu *ur;
    SNMP_CREATE(pkt, struct agentx_un_register_pdu, ur);
    struct agentx_header *h = &ur->h;

    // FIXME correctly set INSTANCE REGISTRATION bit
    SNMP_HEADER(h, type, AGENTX_FLAG_INSTANCE_REGISTRATION);
    SNMP_SESSION(h, p);

    /* do not override timeout */
    STORE(ur->timeout, 0);
    /* default priority */
    STORE(ur->priority, AGENTX_PRIORITY);
    STORE(ur->range_subid, (len > 1) ? index : 0);

    pkt = put_oid(pkt, oid);
    log(L_INFO "pkt - buf : %lu sizeof %u", pkt -buf, AGENTX_HEADER_SIZE);

    /* place upper-bound if needed */
    if (len > 1)
    {
      STORE_PTR(pkt, len);
      pkt += 4;
    }

    log("size of pkt: %u", pkt_len(buf,pkt));
    SNMP_UPDATE(h, pkt_len(buf, pkt));

    for (uint i = 0; i < pkt - buf; i++)
      log(L_INFO "%p:  %02X", buf+i, *(buf + i));

    log(L_INFO "sending (un)register %d", type);
    int ret = sk_send(sk, pkt - buf);

    if (ret == 0)
      log(L_INFO "sleep");
    else if (ret < 0)
      log(L_INFO "err %d", ret);
    else
      log(L_INFO "ok !!");
  }

  else
    log(L_INFO "un_register_pdu() insufficient size");
}

/* register pdu */
static void
snmp_register(struct snmp_proto *p, struct oid *oid, uint index, uint len)
{
  un_register_pdu(p, oid, index, len, AGENTX_REGISTER_PDU);
}


/* unregister pdu */
static void
snmp_unregister(struct snmp_proto *p, struct oid *oid, uint index, uint len)
{
  un_register_pdu(p, oid, index, len, AGENTX_UNREGISTER_PDU);
}

static void
close_pdu(struct snmp_proto *p, u8 reason)
{
  sock *sk = p->sock;
  byte *buf, *pkt;
  buf = pkt = sk->tbuf;
  uint size = sk->tbsize;
  log(L_INFO "close_pdu() size: %u %c %u", size, (size > AGENTX_HEADER_SIZE + 4)
? '>':'<', AGENTX_HEADER_SIZE);

  /* +4B for reason */
  if (size > AGENTX_HEADER_SIZE + 4)
  {
    struct agentx_header *h;
    SNMP_CREATE(pkt, struct agentx_header, h)
    SNMP_B_HEADER(h, AGENTX_CLOSE_PDU)

    SNMP_SESSION(h, p)

    pkt = paste_fbyte(pkt, reason);

    SNMP_UPDATE(h, pkt_len(buf, pkt));

    log(L_INFO "preparing to sk_send()");
    int ret = sk_send(sk, pkt - buf);

    if (ret == 0)
      log(L_INFO "sleep");
    else if (ret < 0)
      log(L_INFO "err");
    else 
      log(L_INFO, "ok !!");
  }
}

static inline void
refresh_ids(struct snmp_proto *p, struct agentx_header *h)
{
  int byte_ord = h->flags & AGENTX_NETWORK_BYTE_ORDER;
  p->transaction_id = LOAD(h->transaction_id, byte_ord);
  p->packet_id = LOAD(h->packet_id, byte_ord);
}

static int
parse_pkt(struct snmp_proto *p, byte *buf, uint size)
{
  if (size < AGENTX_HEADER_SIZE)
    return 0;

  struct agentx_header *h = (void *) buf;
  log(L_INFO "parse_pkt got type %u", h->type);
  switch (h->type)
  {
    case AGENTX_RESPONSE_PDU:
      return parse_response(p, buf, size);

    /*
    case AGENTX_GET_PDU:
      refresh_ids(p, h);
      return parse_get_pdu(p, buf, size);
    */

    case AGENTX_GET_PDU:
    case AGENTX_GET_NEXT_PDU:
      refresh_ids(p, h);
      return parse_gets_pdu(p, buf, size);

    /* should not happen */
    default:
      die("unknown packet type %u", h->type);
  }
}

static int
parse_response(struct snmp_proto *p, byte *buf, uint size)
{
  if (size < sizeof(struct agentx_response))
    return 0;

  struct agentx_response *r = (void *) buf;
  struct agentx_header *h = &r->h;

  log(L_INFO "endianity: %s, session %u, transaction: %u", (h->flags & AGENTX_NETWORK_BYTE_ORDER) ? "big end":
"little end", h->session_id, h->transaction_id);
  log(L_INFO "sid: %3u\ttid: %3u\tpid: %3u\t", p->session_id, p->transaction_id,
p->packet_id);

  log(L_INFO "size %u", h->payload);
  log(L_INFO "uptime: %u s", r->uptime);

  if (r->err == AGENTX_RES_NO_ERROR)
    do_response(p, buf, size);
  else
    log(L_INFO "an error occured '%s'", snmp_errs[get_u16(&r->err) -
SNMP_ERR_SHIFT]);

  return 1;
}

static void
do_response(struct snmp_proto *p, byte *buf, uint size UNUSED)
{
  struct agentx_response *r = (void *) buf;
  struct agentx_header *h = &r->h;

  /* TODO make it asynchronous for better speed */
  switch (p->state)
  {
    case SNMP_INIT:
      if (h->flags & AGENTX_NETWORK_BYTE_ORDER)
      {
	p->session_id = get_u32(&h->session_id);
	p->transaction_id = get_u32(&h->transaction_id);
	p->packet_id = get_u32(&h->packet_id);
      }
      else
      {
	memcpy(&p->session_id, &h->session_id, 12);
      }

      p->transaction_id++;

      log(L_INFO "sending register-pdu");

      // register whole BGP4-MIB
      u32 arr_bgp[] = {1, 15, 1};
      struct oid *o = mb_allocz(p->p.pool, 4 * 4);
      put_u8(&o->n_subid, 2);
      put_u8(&o->prefix, 2);

      memcpy(o->ids, arr_bgp, 2 * 4);

      snmp_register(p, o, 0, 1);

      put_u8(&o->n_subid, 3);
      STORE(o->ids[2], arr_bgp[2]);
      snmp_register(p, o, 0, 1);


      STORE(o->ids[2], 2);
      snmp_register(p, o, 0, 1);

      mb_free(o);

      u32 arr_with_prefix[] = {1, 15, 3, 1, 1};
      struct oid *o2 = mb_allocz(p->p.pool, 10 * 4);

      put_u8(&o2->n_subid, 9);
      memcpy(o2->ids, arr_with_prefix, 5 * 4);
      u32 remote_addr[] = {10, 0, 0, 0};
      memcpy(o2->ids + 5, remote_addr, 4 * 4);
      STORE(o2->prefix, 2);

      // register first line in BGP4-MIB bgpPeerTable
      // TODO register all bind bgp connections
      snmp_register(p, o2, 9, 24);
      mb_free(o2);

      p->state = SNMP_REGISTR;
      //proto_notify_state(&p->p, PS_UP);
      break;

    case SNMP_REGISTR:
      p->state = SNMP_CONN;
      break;

    case SNMP_CONN:
      break;

    default:
      die("unkonwn SNMP state");
  }
}

static int
parse_get_pdu(struct snmp_proto *p, byte *buf, uint size)
{
  log(L_INFO "parse_get_pdu()");

  sock *sk = p->sock;
  byte *res_pkt, *res = sk->tbuf;
  uint rsize = sk->tbsize;

  if (size < AGENTX_HEADER_SIZE)
    return 0;

  log(L_INFO "Get-PDU enough room %p", buf);

  struct agentx_header *h = (void *) buf;
  ADVANCE(buf, size, AGENTX_HEADER_SIZE);
  log(L_INFO "advancing %p cause %u", buf, AGENTX_HEADER_SIZE);

  int byte_ord = h->flags & AGENTX_NETWORK_BYTE_ORDER;

  byte *pkt = buf;
  uint pkt_size = LOAD(h->payload, byte_ord);
  log(L_INFO "RX packet size is %u", pkt_size);

  uint clen;	      /* context len */
  char *context = NULL;
  SNMP_LOAD_CONTEXT(p, h, pkt, context, clen)
  log(L_INFO "after context load %p, pkt == buf %d", pkt, pkt == buf);

  res_pkt = prepare_response(p, res, rsize);
  log(L_INFO "response header created: %p (%u)", res_pkt, res_pkt - res);
  /* parsing one search range */
  uint ind = 1;
  int err = 0;
               /* parsed  */
  while (!err && pkt - buf < pkt_size)
  {
    struct oid *o_start, *o_end;
    o_start = (struct oid *) pkt;
    pkt += oid_size(o_start);
    o_end = (struct oid *) pkt;  // for Get-PDU always null
    pkt += oid_size(o_end);

    log(L_INFO "sizes o_start %lu o_end %lu", oid_size(o_start),
	oid_size(o_end));

    log(L_INFO "o_subid: %u o_prefix %u o_include %u ---",
	o_start->n_subid, o_start->prefix, o_start->include);


    /* currently unsupported non-default context */
    res_pkt = find_n_fill(p, o_start, res_pkt, rsize, 0, byte_ord);

    /*
    struct agentx_varbind *vb_start;
    vb_start = (void *) res_pkt;

    memcpy(&vb_start->name, o_start, oid_size(o_start));
    STORE_16(vb_start->type, AGENTX_INTEGER);
    STORE_16(vb_start->pad, 0);  // padding zeroing
    res_pkt += vb_size(vb_start);

    log(L_INFO " vb_size() rpkt %p %u", res_pkt, res_pkt - res);

    STORE_PTR(res_pkt, 0x1234ABCD);

    log(L_INFO " STORE_PTR int-value rpkt %p %u", res_pkt, res_pkt - res);
    res_pkt += 4;
    log(L_INFO " shift rpkt %p %u", res_pkt, res_pkt - res);
    */
    ind++;
  }

  struct agentx_header *rh = (void *) res;
  SNMP_UPDATE(rh, pkt_len(res, res_pkt));

  if (err)
    response_err_ind(res, err, ind);

  log(L_INFO "res->payload %u (loaded) %u, trying to send: %u",
    rh->payload, LOAD(rh->payload, rh->flags & AGENTX_NETWORK_BYTE_ORDER),
    res_pkt - res + 4);
   
  int ret = sk_send(sk, res_pkt - res);
  log(L_INFO "message sent");

  if (ret == 0)
    log(L_INFO "sleep");
  else if (ret < 0)
    log(L_INFO "err no: %d", ret);
  else
    log(L_INFO "OK !!");

  return 1;
}


/* req is request */
static int
parse_gets_pdu(struct snmp_proto *p, byte *req, uint size)
{
  log(L_INFO "parse_gets_pdu");

  sock *sk = p->sock;
  byte *res_pkt, *res = sk->tbuf;
  uint rsize = sk->tbsize;

  if (size < AGENTX_HEADER_SIZE)
    return 0;

  struct agentx_header *h = (void *) req;
  ADVANCE(req, size, AGENTX_HEADER_SIZE);
  log(L_INFO "advancing %p cause header", req);

  byte *pkt = req;

  int byte_ord = h->flags & AGENTX_NETWORK_BYTE_ORDER;
  uint pkt_size = LOAD(h->payload, byte_ord);

  uint clen;
  char *context;
  SNMP_LOAD_CONTEXT(p, h, req, context, clen);

  res_pkt = prepare_response(p, res, rsize);
  uint ind = 1;
  int err = 0;
  while (!err && pkt - req < pkt_size)
  {
    struct oid *o_start, *o_end;
    o_start = (struct oid *) pkt;
    pkt += oid_size(o_start);
    o_end = (struct oid *) pkt;
    pkt += oid_size(o_end);

    // TODO normalize OID to prefix form

    /* advertised size of oid is greater then size of message */
    if (oid_size(o_start) > size || oid_size(o_end) > size)
    {
      err = -1;  /* parse error too big n_subid */
      continue;
    }

    switch (h->type)
    {
      case AGENTX_GET_PDU:
	res_pkt = find_n_fill(p, o_start, res_pkt, rsize, 0, byte_ord);
	break;

      case AGENTX_GET_NEXT_PDU:
	o_start = bgp_search(o_start, o_end, NULL);
	if (o_start)
	  res_pkt = find_n_fill(p, o_start, res_pkt, rsize, 0, byte_ord);
	else
	{
	  err = -2;
	  continue;
	}
	break;

      case AGENTX_GET_BULK_PDU:
      {
	struct oid  *o_curr = NULL;
	/* TODO add res packet size limiting logic */
	while ((o_curr = bgp_search(o_start, o_end, o_curr)) != NULL)
	{
	  res_pkt = find_n_fill(p, o_start, res_pkt, rsize, 0, byte_ord);
	}

	/* no item found */
	if (res_pkt == res + sizeof(struct agentx_response))
	{
	  err = -2;
	  continue;
	}

	break;
      }
    }

    ind++;
  }

  switch (err)
  {
    case 0:
      response_err_ind(req, AGENTX_RES_NO_ERROR, 0);
      err = 1;
      break;
    case -1:
      response_err_ind(req, AGENTX_RES_PARSE_ERROR, ind);
      break;

    /* no item found - could it happen? */
    case -2:
      response_err_ind(req, AGENTX_RES_GEN_ERROR, ind);
      break;
  }

  struct agentx_header *rh = (void *) res;
  SNMP_UPDATE(rh, pkt_len(res, res_pkt));

  int ret = sk_send(sk, res_pkt - res);
  log(L_INFO "message sent");

  if (ret == 0)
    log(L_INFO "sleep");
  else if (ret < 0)
    log(L_INFO "err no: %d", ret);
  else
    log("OK ! !!");

  return err;
}


static void
header_update_len(byte *buf, u32 len)
{
  struct agentx_header *h = (void *) buf;
  put_u32(&h->payload, len);
  log(L_INFO "header_update_len() %d 0x%02X 0x%02X 0x%02X 0x%02X", len, *((unsigned char
*) &h->payload), *(((unsigned char *) &h->payload) + 1), *(((unsigned char *)
&h->payload) + 2), *(((unsigned char *) &h->payload) + 3));

}

void
snmp_start_subagent(struct snmp_proto *p)
{
  log(L_INFO "snmp_start_subagent() starting subagent");

  /* blank oid means unsupported */
  struct oid *o = mb_allocz(p->p.pool, sizeof(struct oid));
  open_pdu(p, o);
  mb_free(o);

}

void
snmp_stop_subagent(struct snmp_proto *p)
{
  log(L_INFO "snmp_stop_subagent()");
  sock *sk = p->sock;

  if (p->state == SNMP_CONN)
  {
    close_pdu(p, AGENTX_CLOSE_SHUTDOWN);

    sk->rx_hook = snmp_stop_ack;
  }
}

static inline uint
oid_size(struct oid *o)
{
  /* faster multipication by 4 */
  return 4 + (o->n_subid << 2);
}

static inline uint
vb_size(struct agentx_varbind *vb)
{
  /* +4B for type and pad */
  return oid_size(&vb->name) + 4;
}

int
snmp_rx(sock *sk, uint size)
{ 
  log(L_INFO "snmp_rx()");
  struct snmp_proto *p = sk->data;
  byte *pkt = sk->rbuf;

  // 1 means all done; 0 means to be continued
  return parse_pkt(p, pkt, size);
  /* 
  while (end >= pkt + AGENTX_HEADER_SIZE)
  {
    parse_header(p);
    parse_pkt(p, );
  }
  */ 
}

/* ping pdu */
void
snmp_ping(struct snmp_proto *p)
{
  /* this does not support non-default context */ 
  sock *sk = p->sock;
  byte *pkt = sk->tbuf;
  uint size = sk->tbsize;

  if (size > AGENTX_HEADER_SIZE)
  {
    log(L_INFO "ping_pdu()");
    struct agentx_header *h;
    log("before dead %p", pkt );
    SNMP_CREATE(pkt, struct agentx_header, h);
    SNMP_B_HEADER(h, AGENTX_PING_PDU);
    SNMP_SESSION(h, p);

    /* sending only header => pkt - buf */
    int ret = sk_send(sk, AGENTX_HEADER_SIZE);
    if (ret == 0)
      log(L_INFO "sleep");
    else if (ret < 0)
      log(L_INFO "err %d", ret);
    else
      log("ok ! !");
  }

  else
    log(L_INFO "ping_pdu() insufficient size");
}


static int
snmp_stop_ack(sock *sk, uint size)
{
  struct snmp_proto *p = sk->data;
  byte *buf = sk->rbuf;

  if (size < AGENTX_HEADER_SIZE)
    return 0;

  if (parse_response(p, buf, size))
  {
    p->p.disabled = 1;
    proto_notify_state(&p->p, PS_DOWN);

    sk->tx_hook = NULL;
    sk->rx_hook = NULL;
  }

  /* all done */
  return 0;
}

/*
void
snmp_agent_reconfigure(void)
{

}
*/

static int
compare(struct oid *left, struct oid *right)
{
  if (left->prefix < right->prefix)
    return -1;

  else if (left->prefix > right->prefix)
    return 1;

  for (int i = 0; i < MIN(left->n_subid, right->n_subid); i++)
  {
    if (left->ids[i] < right->ids[i])
      return -1;
    else if (left->ids[i] > right->ids[i])
      return 1;
  }
  return 0;
}

static inline int
is_bgp4_mib_prefix(struct oid *o)
{
  if (o->prefix == 2 && o->ids[0] == 15)
    return 1; 
  else
    return 0;
}

static struct oid *
bgp_search(struct oid *o_start, struct oid *e_end, struct oid *o_curr)
{
  if (o_start->include);
    // if valid o_start

  // search o_start.0 (resp. o_start.1)
  return o_start;
}

static byte *
find_bgp_one(struct bgp_proto *bp, struct oid *o, byte *pkt,  uint size, uint contid)
{
  struct bgp_conn *b_conn = bp->conn;
  struct bgp_conn *b_in = &bp->incoming_conn;
  struct bgp_conn *b_out = &bp->outgoing_conn;

  struct bgp_stats *b_stats = &bp->stats;
  const struct bgp_config *b_conf = bp->cf;

  uint b_state;

  if (b_conn)
    b_state = b_conn->state;
  else if (MAX(b_in->state, b_out->state) == BS_CLOSE &&
    MIN(b_in->state, b_out->state) != BS_CLOSE)
    b_state = MIN(b_in->state, b_out->state); 
  /* BS_CLOSE is unsupported by BGP4-MIB */
  else if (MIN(b_in->state, b_out->state) == BS_CLOSE)
    b_state = BS_IDLE;
  else
    b_state = MAX(b_in->state, b_out->state);
  
  struct agentx_varbind *vb = (void *) pkt;
  pkt += vb_size(vb);
  
  switch (o->ids[4])
  {
    case SNMP_BGP_IDENTIFIER:
      if (b_state == BS_OPENCONFIRM || b_state == BS_ESTABLISHED)
      {
	STORE_PTR(pkt, ipa_to_u32(bp->remote_ip));
	BGP_DATA(vb, AGENTX_IP_ADDRESS, pkt);
      }
      else
      {
	STORE_PTR(pkt, ipa_to_u32(IPA_NONE));
	BGP_DATA(vb, AGENTX_IP_ADDRESS, pkt);
      }
      break;
      
    case SNMP_BGP_STATE: 
      STORE_PTR(pkt, b_state);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_ADMIN_STATUS:
      if (((struct proto *) bp)->disabled)
	STORE_PTR(pkt, AGENTX_ADMIN_STOP);
      else
	STORE_PTR(pkt, AGENTX_ADMIN_START);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_VERSION:
      if (b_state == BS_OPENCONFIRM || b_state == BS_ESTABLISHED)
	STORE_PTR(pkt, 4);
      else
	STORE_PTR(pkt, 0);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_LOCAL_ADDR:
      // TODO XXX bp->link_addr & zero local_ip
      STORE_PTR(pkt, ipa_to_u32(bp->local_ip));
      BGP_DATA(vb, AGENTX_IP_ADDRESS, pkt);
      break;

    case SNMP_BGP_LOCAL_PORT:
      STORE_PTR(pkt, b_conf->local_port);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_REMOTE_ADDR:
      STORE_PTR(pkt, ipa_to_u32(bp->remote_ip));
      BGP_DATA(vb, AGENTX_IP_ADDRESS, pkt);
      break;

    case SNMP_BGP_REMOTE_PORT:
      STORE_PTR(pkt, b_conf->remote_port);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_REMOTE_AS:
      STORE_PTR(pkt, bp->remote_as);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    /* IN UPDATES */
    case SNMP_BGP_RX_UPDATES:
      STORE_PTR(pkt, b_stats->rx_updates);
      BGP_DATA(vb, AGENTX_COUNTER_32, pkt);
      break;

    /* OUT UPDATES */
    case SNMP_BGP_TX_UPDATES:
      STORE_PTR(pkt, b_stats->tx_updates);
      BGP_DATA(vb, AGENTX_COUNTER_32, pkt);
      break;

    /* IN MESSAGES */
    case SNMP_BGP_RX_MESSAGES:
      STORE_PTR(pkt, b_stats->rx_messages);
      BGP_DATA(vb, AGENTX_COUNTER_32, pkt);
      break;

    /* OUT MESSAGES */
    case SNMP_BGP_TX_MESSAGES:
      STORE_PTR(pkt, b_stats->tx_messages);
      BGP_DATA(vb, AGENTX_COUNTER_32, pkt);
      break;

    case SNMP_BGP_LAST_ERROR:
      STORE_PTR(pkt, 2);
      pkt += 4;
      /* force network order */
      put_u32(pkt,
	(bp->last_error_code << 8 | bp->last_error_code << 48) & 0xFFFF0000);
      /* real size is 8 but we already shifted the pkt by 4 */
      BGP_DATA(vb, AGENTX_OCTET_STRING, pkt);
      break;

    case SNMP_BGP_FSM_TRANSITIONS:
      STORE_PTR(pkt, b_stats->fsm_established_transitions);
      BGP_DATA(vb, AGENTX_COUNTER_32, pkt);
      break;

    case SNMP_BGP_RETRY_INTERVAL:
      STORE_PTR(pkt, b_conf->connect_retry_time);
      BGP_DATA(vb, AGENTX_COUNTER_32, pkt);
      break;

    case SNMP_BGP_HOLD_TIME:
      if (b_conn && b_conf->hold_time)
	STORE_PTR(pkt, b_conn->hold_time);
      else
	STORE_PTR(pkt, 0);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_KEEPALIVE:
      if (b_conn && b_conf->keepalive_time)
	STORE_PTR(pkt, b_conn->keepalive_time);
      else
	STORE_PTR(pkt, 0);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_HOLD_TIME_CONFIGURED:
      STORE_PTR(pkt, b_conf->hold_time);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_KEEPALIVE_CONFIGURED:
      STORE_PTR(pkt, b_conf->keepalive_time);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    /* UNSUPPORTED */
    /* TODO XXX forbiden value 0 */
    case SNMP_BGP_ORIGINATION_INTERVAL:
    case SNMP_BGP_MIN_ROUTE_ADVERTISEMENT:
      STORE_PTR(pkt, 0);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case SNMP_BGP_FSM_ESTABLISHED_TIME:
    case SNMP_BGP_IN_UPDATE_ELAPSED_TIME:
      vb->type = AGENTX_NO_SUCH_OBJECT;
      /* pkt += 0;  no data */
      break;

    /* no default */
  }

  return pkt;
}

/* contid - context identification number */
static byte *
snmp_bgp_record(struct snmp_proto *p, struct oid *o, byte *buf, uint size, uint contid)
{
  struct agentx_varbind *vb = (void *) buf;
  byte *pkt = buf + vb_size(vb);

  switch (o->ids[2])
  {
    case BGP4_MIB_VERSION:
      STORE_PTR(pkt, 1);   // string len
      pkt += 4;
      STORE_PTR(pkt, BGP4_VERSIONS);
      /* real size is 8 but we already shifted the pkt by 4 */
      BGP_DATA(vb, AGENTX_OCTET_STRING, pkt);
      break;
      
    case BGP4_MIB_LOCAL_AS:
      // XXX local as to use
      STORE_PTR(pkt, p->local_as);
      BGP_DATA(vb, AGENTX_INTEGER, pkt);
      break;

    case BGP4_PEER_TABLE:
      /* end part of .1.3.6.1.2.1.15.3.1.x.a.b.c.d */ 
      if (o->n_subid < 9 || o->ids[3] != BGP4_PEER_ENTRY
	  || o->ids[4] == 0 || o->ids[4] > 24)
      {
	vb->type = AGENTX_NO_SUCH_OBJECT;
	return buf + vb_size(vb);
      }

      // TODO enumerate range requests
      ip_addr addr = ipa_build4(o->ids[5], o->ids[6], o->ids[7], o->ids[8]);  
      struct snmp_bgp_peer *pe =
        HASH_FIND(p->bgp_hash, SNMP_HASH, addr);

      struct bgp_proto *bp = NULL;
      if (pe && pe->bond->proto->proto && 
	  ipa_equal(addr,
	    ((struct bgp_proto *) pe->bond->proto->proto)->remote_ip))
      {
	bp = (void *) pe->bond->proto->proto;
      }

      /* IF WE CONSIDER CHANGES OF REMOTE IP
      else 
      {
	struct snmp_bond *b;
	WALK_LIST(b, p->bgp_entries)
	  if (b->proto->proto &&
	      ipa_equal(((struct bgp_proto *) b->proto->proto)->remote_ip, addr))
	    bp = (struct bgp_proto *) b->proto->proto;
      }
      */

      if (!bp)
      {
	vb->type = AGENTX_NO_SUCH_OBJECT;
	/* pkt += 0; no data */
	return pkt;	
      }
      
      return find_bgp_one(bp, o, buf, size, contid); 
      break;

    default:
      vb->type = AGENTX_NO_SUCH_OBJECT;
      /* pkt += 0; no data */ 
      break;
  }

  return pkt;
}

static byte *
find_ospf_record(struct snmp_proto *p, struct oid *o, byte *buf, uint size)
{
  // TODO XXX
  return NULL;
}

static byte *
unsupported_oid(struct snmp_proto *p, struct oid *o, byte *buf, uint size, uint contid)
{
  struct agentx_varbind *vb = (void *) buf;
  vb->type = AGENTX_NO_SUCH_OBJECT;
  return buf + vb_size(vb);
}

static inline byte *
find_prefixed(struct snmp_proto *p, struct oid *o, byte *buf, uint size, uint contid)
{
  struct agentx_varbind *vb = (void *) buf;

  memcpy(&vb->name, o, oid_size(o));
  
                       /* SNMPv2   mgmt		     mib-2 */
  if (o->n_subid < 2 || (o->prefix != 2 && o->ids[0] != 1))
  {
    vb->type = AGENTX_NO_SUCH_OBJECT;
    return buf + vb_size(vb);
  }
 
  switch (o->ids[1])
  {
    case SNMP_BGP4_MIB:
      if (snmp_bgp_is_supported(o))
	return snmp_bgp_record(p, o, buf, size, contid);
      else
	return unsupported_oid(p, o, buf, size, contid);

    case SNMP_OSPFv3_MIB:
      return unsupported_oid(p, o, buf, size, contid);
      //return find_ospf_record(p, o, buf, size);

    default:
      return unsupported_oid(p, o, buf, size, contid);
      log(L_INFO "unsupported oid");
      break;
  }
}

/* tests if there is present canonical "internet" prefix .1.3.6.1
  and if so it shortens the oid to the ``prefix'' form */
static int
prefixize_in_place(struct oid *o, int byte_ord)
{
  const u32 prefix[] = {1, 3, 6, 1};

  /* NETWORK_BYTE_ORDER */
  if (byte_ord)
    /* prefix len == 4 */
    for (uint i = 0; i < 4; i++)
      if (get_u32(&o->ids[i]) != prefix[i]) return 0;

  else
    /* prefix len == 4 */
    for (uint i = 0; i < 4; i++)
      if (o->ids[i] != prefix[i]) return 0;

  o->n_subid -= 5;
  o->prefix = o->ids[4];
  /* n_subid contains number of elements, not bytes */
  memmove(&o->ids, &o->ids[5], o->n_subid << 2);
  return 1;
}

static struct oid *
prefixize(struct snmp_proto *p, struct oid *o, int byte_ord)
{
  const u32 prefix[] = {1, 3, 6, 1};

  if (o->n_subid < 5)
    return NULL;

  for (int i = 0; i < 4; i++)
    if (LOAD(o->ids[i], byte_ord) != prefix[i])
      return NULL;

  struct oid *new = mb_alloc(p->p.pool, sizeof(struct oid) + MAX((o->n_subid - 5) * 4, 0));

  memcpy(new, o, sizeof(struct oid));
  new->n_subid = o->n_subid - 5;

  if (o->ids[4] < 256)
    new->prefix = o->ids[4];
  else return NULL;

  memcpy(&new->ids, &o->ids[5], new->n_subid * 4);
  return new;
}

static inline byte *
find_n_fill(struct snmp_proto *p, struct oid *o, byte *buf, uint size, uint contid, int byte_ord)
{
  struct oid *new;
  if (!o->prefix && (new = prefixize(p, o, byte_ord)) != NULL)
    find_prefixed(p, new, buf, size, contid);
  else if (o->prefix)
    find_prefixed(p, o, buf, size, contid);
  else
    // TODO handle unprefixable oids
    return NULL;
}

static byte *
prepare_response(struct snmp_proto *p, byte *buf, uint size)
{
  log(L_INFO "prepare_response()");

  if (size < sizeof(struct agentx_response))
    return NULL;
  struct agentx_response *r = (void *) buf;
  struct agentx_header *h = &r->h;

  SNMP_B_HEADER(h, AGENTX_RESPONSE_PDU)
  SNMP_SESSION(h, p)

  /* protocol doesn't care about subagent upTime */
  STORE(r->uptime, 0);
  STORE_16(r->err, AGENTX_RES_NO_ERROR);
  STORE_16(r->index, 0);

  buf += sizeof(struct agentx_response);
  return buf;
}

static void
response_err_ind(byte *buf, uint err, uint ind)
{
  log(L_INFO "reponse_err_ind()");
  struct agentx_response *res = (void *) buf;

  res->err = err;
  res->index = ind;
}

#undef SNMP_ERR_SHIFT