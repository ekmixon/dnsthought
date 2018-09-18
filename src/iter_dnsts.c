#include "config.h"
#include "dnst.h"
#include "rr-iter.h"
#include <arpa/inet.h>
#include <assert.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <getdns/getdns_extra.h>

static int quiet = 0;

void dnst_iter_done(dnst_iter *i)
{
	if (i->buf)
		munmap(i->buf, i->end_of_buf - i->buf);
	i->buf = NULL;
	if (i->fd >= 0)
		close(i->fd);
	i->fd = -1;
	i->cur = NULL;
}

dnst *dnst_iter_open(dnst_iter *i)
{
	char fn[4096 + 32 ];
	int r;
	struct stat st;

	r = snprintf( fn, sizeof(fn), "%s/%.4d-%.2d-%.2d.dnst"
	            , i->msm_dir
	            , i->start.tm_year + 1900
	            , i->start.tm_mon  + 1
	            , i->start.tm_mday);

	if (r < 0 || r > sizeof(fn) - 1)
		fprintf(stderr, "Filename parse error\n");

	else if ((i->fd = open(fn, O_RDONLY)) < 0)
		; /* fprintf(stderr, "Could not open \"%s\" (should pass silently)\n", fn); */

	else if (fstat(i->fd, &st) < 0)
		fprintf(stderr, "Could not fstat \"%s\"\n", fn);

	else if (st.st_size < 16)
		fprintf(stderr, "\"%s\" too small\n", fn);
	
	else if ((i->buf = mmap( NULL, st.st_size
	                       , PROT_READ, MAP_PRIVATE, i->fd, 0)) == MAP_FAILED)
		fprintf(stderr, "Could not mmap \"%s\"\n", fn);
	else if (st.st_size >= 16
	     &&  dnst_fits( (i->cur = (void *)i->buf)
	                  , (i->end_of_buf = i->buf + st.st_size)))
		return i->cur;
	else
		i->cur = NULL;
	if (i->buf)
		munmap(i->buf, i->end_of_buf - i->buf);
	if (i->fd >= 0)
		close(i->fd);

	i->start.tm_mday += 1;
	return (i->cur = NULL);
}

void dnst_iter_next(dnst_iter *i)
{
	i->cur = dnst_next(i->cur);
	if ((uint8_t *)i->cur + 16 < i->end_of_buf
	&&  dnst_fits(i->cur, i->end_of_buf))
		return;
	dnst_iter_done(i);
	i->start.tm_mday += 1;
	while (!i->cur && mktime(&i->start) < mktime(&i->stop))
		dnst_iter_open(i);
}

void dnst_iter_init(dnst_iter *i, struct tm *start, struct tm *stop, const char *path)
{
	const char *slash;
	if (mktime(start) >= mktime(stop))
		return;

	i->fd = -1;
	i->start = *start;
	i->stop  = *stop;
	if (!(slash = strrchr(path, '/')))
		i->msm_id = atoi(path);
	else	i->msm_id = atoi(slash + 1);
	(void)strlcpy(i->msm_dir, path, sizeof(i->msm_dir));
	while (!i->cur && mktime(&i->start) < mktime(stop))
		dnst_iter_open(i);
}

static int dnst_cmp(const void *x, const void *y)
{
	return memcmp(x, y, sizeof(dnst_rec_key));
}


static const uint8_t ipv4_mapped_ipv6_prefix[] =
    "\x00\x00" "\x00\x00" "\x00\x00" "\x00\x00" "\x00\x00" "\xFF\xFF";

void rec_debug(FILE *f, const char *msg, unsigned int msm_id, uint32_t time, dnst_rec *rec)
{
	char addrstr[80];
	char timestr[80];
	struct tm tm;
	time_t t = time;

	gmtime_r(&t, &tm);
	strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M.%S", &tm);

	if (memcmp(rec->key.addr, ipv4_mapped_ipv6_prefix, 12) == 0)
		inet_ntop(AF_INET, &rec->key.addr[12], addrstr, sizeof(addrstr));
	else
		inet_ntop(AF_INET6, rec->key.addr    , addrstr, sizeof(addrstr));

	if (msg)
		fprintf(f, "%s ", msg);
	if (msm_id)
		fprintf(f, "%5u ", msm_id);
	fprintf( f, "%s %5" PRIu32 "_%s\n"
	       , timestr, rec->key.prb_id, addrstr);
}
static FILE *out = NULL;
void log_rec(dnst_rec *rec)
{
	size_t i;
	char addrstr[80];
	char timestr[80];
	struct tm tm;
	time_t t;

	if (!out)
		return;

	t = rec->updated;
	gmtime_r(&t, &tm);
	strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S+00", &tm);

	if (memcmp(rec->key.addr, ipv4_mapped_ipv6_prefix, 12) == 0)
		inet_ntop(AF_INET, &rec->key.addr[12], addrstr, sizeof(addrstr));
	else
		inet_ntop(AF_INET6, rec->key.addr    , addrstr, sizeof(addrstr));

	fprintf(out, "INSERT INTO dnsthought VALUES('%s', %" PRIu32 ", '%s'"
	       , timestr, rec->key.prb_id, addrstr);
	if (memcmp(rec->whoami_g, "\x00\x00\x00\x00", 4) == 0)
		fprintf(out, ",NULL");
	else	fprintf(out, ",'%s'", inet_ntop( AF_INET, rec->whoami_g
		                               , addrstr, sizeof(addrstr)));

	if (memcmp(rec->whoami_a, "\x00\x00\x00\x00", 4) == 0)
		fprintf(out, ",NULL");
	else	fprintf(out, ",'%s'", inet_ntop( AF_INET, rec->whoami_a
		                               , addrstr, sizeof(addrstr)));
	if (memcmp(rec->whoami_6, "\x00\x00\x00\x00\x00\x00\x00\x00"
	                          "\x00\x00\x00\x00\x00\x00\x00\x00", 16) == 0)
		fprintf(out, ",NULL");
	else	fprintf(out, ",'%s'", inet_ntop( AF_INET6, rec->whoami_6
		                               , addrstr , sizeof(addrstr)));

	for (i = 0; i < 12; i++)
		fprintf(out, ",%" PRIu8, rec->dnskey_alg[i]);
	for (i = 0; i < 2; i++)
		fprintf(out, ",%" PRIu8, rec->ds_alg[i]);
	fprintf(out, ",%" PRIu8, rec->ecs_mask);
	fprintf(out, ",%d", (int)rec->qnamemin);
	fprintf(out, ",%d", (int)rec->tcp_ipv4);
	fprintf(out, ",%d", (int)rec->tcp_ipv6);
	fprintf(out, ",%d", (int)rec->nxdomain);
	fprintf(out, ",%d", (int)rec->has_ta_19036);
	fprintf(out, ",%d", (int)rec->has_ta_20326);
	fprintf(out, ") ON CONFLICT DO NOTHING;\n");
}

void process_secure(uint8_t *msg, size_t msg_len,
    uint8_t *secure, uint8_t *bogus, uint8_t *result)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_A
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&& (rr->rr_i.rr_type + 14 <= rr->rr_i.pkt_end)
	&&  rr->rr_i.rr_type[10] == 145 &&  rr->rr_i.rr_type[11] ==  97
	&&  rr->rr_i.rr_type[12] ==  20 &&  rr->rr_i.rr_type[13] ==  17) {

		*secure = CAP_DOES;
		if (*bogus == CAP_DOESNT)
			*result  = CAP_DOES;
		else if (*bogus == CAP_DOES)
			*result  = CAP_DOESNT;
	} else {
		*secure = CAP_DOESNT;
		if (*bogus != CAP_UNKNOWN)
			*result = CAP_BROKEN;
	}
}

void process_bogus(uint8_t *msg, size_t msg_len,
    uint8_t *bogus, uint8_t *secure, uint8_t *result)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_A
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&& (rr->rr_i.rr_type + 14 <= rr->rr_i.pkt_end)
	&&  rr->rr_i.rr_type[10] == 145 &&  rr->rr_i.rr_type[11] ==  97
	&&  rr->rr_i.rr_type[12] ==  20 &&  rr->rr_i.rr_type[13] ==  17) {

		*bogus = CAP_DOES;
		if (*secure == CAP_DOES)
			*result = CAP_DOESNT;
	} else {
		*bogus = CAP_DOESNT;
		if (*secure == CAP_DOES)
			*result = CAP_DOES;
	}
}

#define PROCESS_DNSKEY_ALG_SECURE(ALG) \
	process_secure( dnst_msg(d), d->len \
	              , &rec->secure_reply[(ALG)] \
	              , &rec->bogus_reply[(ALG)] \
	              , &rec->dnskey_alg[(ALG)] )

#define PROCESS_DNSKEY_ALG_BOGUS(ALG) \
	process_bogus( dnst_msg(d), d->len \
	             , &rec->bogus_reply[(ALG)] \
	             , &rec->secure_reply[(ALG)] \
	             , &rec->dnskey_alg[(ALG)] )

void process_nxdomain(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset = NULL;
	rrtype_iter rr_spc, *rr = NULL;

	if ((  RCODE_WIRE(msg) == RCODE_NXDOMAIN
	    || RCODE_WIRE(msg) == RCODE_NOERROR)
	&&  DNS_MSG_ANCOUNT(msg) == 0)
		rec->nxdomain = CAP_DOESNT; /* No hijack, good! */

	else if (RCODE_WIRE(msg) != RCODE_NOERROR
	    || !(rrset = rrset_answer(&rrset_spc, msg, msg_len))
	    ||   rrset->rr_type != RRTYPE_A
	    || !(rr = rrtype_iter_init(&rr_spc, rrset))) {
		rec->nxdomain = CAP_BROKEN;
	} else {
		rec->nxdomain = CAP_DOES;
#if 0
		char addr_str[80] = "hijacked ";

		inet_ntop(AF_INET, rr->rr_i.rr_type + 10, addr_str + 9, 70);
		rec_debug( stdout, addr_str, 0, rec->updated, rec);
#endif
	}
}

void process_whoami_g(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;

	if (RCODE_WIRE(msg) != RCODE_NOERROR
	|| !(rrset = rrset_answer(&rrset_spc, msg, msg_len))
	||   rrset->rr_type != RRTYPE_TXT)
		; /* pass */
	else for ( rr = rrtype_iter_init(&rr_spc, rrset)
	         ; rr ; rr = rrtype_iter_next(rr)) {

		uint16_t rd_len = READ_U16(rr->rr_i.rr_type + 8);
		const uint8_t *rdata = rr->rr_i.rr_type + 10;
		const uint8_t *eord  = rdata + rd_len;
		uint8_t  txt_len;

		if (eord > rr->rr_i.pkt_end)
			break;
		for (; rdata < eord; rdata += txt_len) {
			char strbuf[80];

			txt_len = *rdata;
			rdata += 1;
			if (txt_len > 20 &&
			    memcmp(rdata, "edns0-client-subnet ", 20) == 0) {
				const uint8_t *slash = rdata + txt_len - 1;
				uint8_t numlen;

				while (slash > rdata && *slash != '/')
					slash--;
				if (*slash == '/')
					slash++;
				numlen = txt_len - (slash - rdata);
				if (numlen > sizeof(strbuf) - 1)
					continue;
				memcpy(strbuf, slash, numlen);
				strbuf[numlen] = '\0';

				rec->ecs_mask = atoi(strbuf);
				continue;
			}
			if (txt_len > sizeof(strbuf) - 1)
				continue;
			memcpy(strbuf, rdata, txt_len);
			strbuf[txt_len] = '\0';
			inet_pton(AF_INET, strbuf, rec->whoami_g);
		}
	}
}

void process_whoami_a(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;
	const uint8_t *rdata;
	uint16_t rdlen;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_A
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&&  rr->rr_i.rr_type + 14 <= rr->rr_i.pkt_end
	&&  READ_U16(rr->rr_i.rr_type + 8) == 4)
		memcpy(rec->whoami_a, rr->rr_i.rr_type + 10, 4);
}

void process_whoami_6(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;
	const uint8_t *rdata;
	uint16_t rdlen;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_AAAA
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&&  rr->rr_i.rr_type + 26 <= rr->rr_i.pkt_end
	&&  READ_U16(rr->rr_i.rr_type + 8) == 16)
		memcpy(rec->whoami_6, rr->rr_i.rr_type + 10, 16);
}

void process_qnamemin(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;

	if (RCODE_WIRE(msg) != RCODE_NOERROR
	|| !(rrset = rrset_answer(&rrset_spc, msg, msg_len))
	||   rrset->rr_type != RRTYPE_TXT)
		; /* pass */
	else for ( rr = rrtype_iter_init(&rr_spc, rrset)
	         ; rr ; rr = rrtype_iter_next(rr)) {

		uint16_t rd_len = READ_U16(rr->rr_i.rr_type + 8);
		const uint8_t *rdata = rr->rr_i.rr_type + 10;
		const uint8_t *eord  = rdata + rd_len;
		uint8_t  txt_len;

		if (eord > rr->rr_i.pkt_end)
			break;
		for (; rdata < eord; rdata += txt_len) {
			txt_len = *rdata;
			rdata += 1;

			if (txt_len >= 7 && memcmp(rdata, "HOORAY ", 7) == 0) {
				rec->qnamemin = CAP_DOES;
				break;
			}
			if (txt_len >= 3 && memcmp(rdata, "NO ", 3) == 0) {
				rec->qnamemin = CAP_DOESNT;
				break;
			}
		}
	}
}

void process_tcp4(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;
	const uint8_t *rdata;
	uint16_t rdlen;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_A
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&&  rr->rr_i.rr_type + 14 <= rr->rr_i.pkt_end
	&&  READ_U16(rr->rr_i.rr_type + 8) == 4)
		rec->tcp_ipv4 = CAP_CAN;
	else	rec->tcp_ipv4 = CAP_CANNOT;
}

void process_tcp6(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;
	const uint8_t *rdata;
	uint16_t rdlen;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_AAAA
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&&  rr->rr_i.rr_type + 26 <= rr->rr_i.pkt_end
	&&  READ_U16(rr->rr_i.rr_type + 8) == 16)
		rec->tcp_ipv6 = CAP_CAN;
	else	rec->tcp_ipv6 = CAP_CANNOT;
}


void process_not_ta_19036(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_A
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&& (rr->rr_i.rr_type + 14 <= rr->rr_i.pkt_end)
	&&  rr->rr_i.rr_type[10] == 145 &&  rr->rr_i.rr_type[11] ==  97
	&&  rr->rr_i.rr_type[12] ==  20 &&  rr->rr_i.rr_type[13] ==  17) {

		rec->not_ta_19036 = CAP_DOES;
	} else {
		rec->not_ta_19036 = CAP_DOESNT;
		if (rec->dnskey_alg[5] == CAP_DOES)
			rec->has_ta_19036 = CAP_DOES;
	}
}

void process_not_ta_20326(dnst_rec *rec, uint8_t *msg, size_t msg_len)
{
	rrset_spc   rrset_spc;
	rrset      *rrset;
	rrtype_iter rr_spc, *rr;

	if (RCODE_WIRE(msg) == RCODE_NOERROR
	&& (rrset = rrset_answer(&rrset_spc, msg, msg_len))
	&&  rrset->rr_type == RRTYPE_A
	&& (rr = rrtype_iter_init(&rr_spc, rrset))
	&& (rr->rr_i.rr_type + 14 <= rr->rr_i.pkt_end)
	&&  rr->rr_i.rr_type[10] == 145 &&  rr->rr_i.rr_type[11] ==  97
	&&  rr->rr_i.rr_type[12] ==  20 &&  rr->rr_i.rr_type[13] ==  17) {

		rec->not_ta_20326 = CAP_DOES;
		if (rec->dnskey_alg[5] == CAP_DOES
		&&  rec->has_ta_19036  == CAP_DOES)
			rec->has_ta_20326 = CAP_DOESNT;
	} else {
		rec->not_ta_20326 = CAP_DOESNT;
		if (rec->dnskey_alg[5] == CAP_DOES)
			rec->has_ta_20326 = CAP_DOES;
	}
}

static rbtree_type recs = { RBTREE_NULL, 0, dnst_cmp };
void process_dnst(dnst *d, unsigned int msm_id)
{
	dnst_rec_key k;
	dnst_rec *rec;

	k.prb_id = d->prb_id;
	if (d->af == AF_INET6)
		memcpy(k.addr, dnst_addr(d), 16);
	else if (d->af == AF_INET) {
		memcpy( k.addr    , ipv4_mapped_ipv6_prefix, 12);
		memcpy(&k.addr[12], dnst_addr(d), 4);
	} else
		return;

	if (!(rec = (dnst_rec *)rbtree_search(&recs, &k))) {
		rec = calloc(1, sizeof(dnst_rec));
		rec->key = k;
		rec->node.key = &rec->key;
		(void)rbtree_insert(&recs, &rec->node);
	}
	if (d->error) {
		/* TODO: log error; */
	} else switch (msm_id) {
	case  8310237: /* o-o.myaddr.l.google.com TXT */
		process_whoami_g(rec, dnst_msg(d), d->len);
		break;
	case  8310245: /* whoami.akamai.net A */
		process_whoami_a(rec, dnst_msg(d), d->len);
		break;
	case  8310366: /* <prb_id>.<time>.ripe-hackathon6.nlnetlabs.nl AAAA (ipv6 cap) */
		process_whoami_6(rec, dnst_msg(d), d->len);
		break;
	case  8926853: /*  secure.d2a1n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 0); break;
	case  8926855: /*  secure.d2a3n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 1); break;
	case  8926857: /*  secure.d2a5n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 2); break;
	case  8926859: /*  secure.d2a6n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 3); break;
	case  8926861: /*  secure.d2a7n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 4); break;
	case  8926863: /*  secure.d2a8n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 5); break;
	case  8926865: /* secure.d2a10n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 6); break;
	case  8926867: /* secure.d2a12n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 7); break;
	case  8926869: /* secure.d2a13n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 8); break;
	case  8926871: /* secure.d2a14n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE( 9); break;
	case  8926873: /* secure.d2a15n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE(10); break;
	case  8926875: /* secure.d2a16n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_SECURE(11); break;
	case  8926854: /*   bogus.d2a1n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 0); break;
	case  8926856: /*   bogus.d2a3n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 1); break;
	case  8926858: /*   bogus.d2a5n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 2); break;
	case  8926860: /*   bogus.d2a6n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 3); break;
	case  8926862: /*   bogus.d2a7n1.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 4); break;
	case  8926864: /*   bogus.d2a8n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 5); break;
	case  8926866: /*  bogus.d2a10n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 6); break;
	case  8926868: /*  bogus.d2a12n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 7); break;
	case  8926870: /*  bogus.d2a13n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 8); break;
	case  8926872: /*  bogus.d2a14n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS( 9); break;
	case  8926874: /*  bogus.d2a15n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS(10); break;
	case  8926876: /*  bogus.d2a16n3.rootcanary.net A */
		PROCESS_DNSKEY_ALG_BOGUS(11); break;
	case  8926887: /*  secure.d3a8n3.rootcanary.net A */
		process_secure( dnst_msg(d), d->len
		              , &rec->ds_secure_reply[0]
			      , &rec->ds_bogus_reply[0]
			      , &rec->ds_alg[0]
			      );
		break;
	case  8926911: /*  secure.d4a8n3.rootcanary.net A */
		process_secure( dnst_msg(d), d->len
		              , &rec->ds_secure_reply[1]
			      , &rec->ds_bogus_reply[1]
			      , &rec->ds_alg[1]
			      );
		break;
	case  8926888: /*   bogus.d3a8n3.rootcanary.net A */
		process_bogus( dnst_msg(d), d->len
			     , &rec->ds_bogus_reply[0]
		             , &rec->ds_secure_reply[0]
			     , &rec->ds_alg[0]
			     );
		break;
	case  8926912: /*   bogus.d4a8n3.rootcanary.net A */
		process_bogus( dnst_msg(d), d->len
			     , &rec->ds_bogus_reply[0]
		             , &rec->ds_secure_reply[0]
			     , &rec->ds_alg[0]
			     );
		break;
	case  8310250: /* qnamemintest.internet.nl TXT */
		process_qnamemin(rec,  dnst_msg(d), d->len);
		break;
	case  8310360: /* <prb_id>.<time>.tc.ripe-hackathon4.nlnetlabs.nl A    (tcp4 cap) */
		process_tcp4(rec, dnst_msg(d), d->len);
		break;
	case  8310364: /* <prb_id>.<time>.tc.ripe-hackathon6.nlnetlabs.nl AAAA (tcp6 cap) */
		process_tcp6(rec, dnst_msg(d), d->len);
		break;
	case  8311777: /* nxdomain.ripe-hackathon2.nlnetlabs.nl A */
		process_nxdomain(rec, dnst_msg(d), d->len);
		break;
	case 15283670: /* root-key-sentinel-not-ta-19036.d2a8n3.rootcanary.net A */
		process_not_ta_19036(rec, dnst_msg(d), d->len);
		break;
	case 15283671: /* root-key-sentinel-not-ta-20326.d2a8n3.rootcanary.net A */
		process_not_ta_20326(rec, dnst_msg(d), d->len);
		break;
	default:
		fprintf(stderr, "Unknown msm_id: %u\n", msm_id);
		return;
	}
	if (rec->updated == 0) {
		rec->updated = d->time;
		rec->logged = d->time;

	} else if (d->time < rec->updated && rec->updated - d->time > 3600) {
		rec_debug( stderr, "Discard > 1 hour back leap", msm_id, d->time, rec);
		return;

	} else if (d->time > rec->updated)
		rec->updated = d->time;

	if (rec->updated - rec->logged > 3600) {
		log_rec(rec);
		rec->logged = d->time;
	}
}

int main(int argc, const char **argv)
{
	const char *endptr;
	struct tm   start;
	struct tm   stop;
	dnst_iter  *iters;
	size_t    n_iters, i;
	dnst_rec   *rec = NULL;
	size_t      rec_data_sz = ((uint8_t *) rec) + sizeof(*rec)
	                        - ((uint8_t *)&rec->key);

	memset((void *)&start, 0, sizeof(struct tm));
	memset((void *)&stop, 0, sizeof(struct tm));
	if (argc > 1 && strcmp(argv[1], "-q") == 0) {
		quiet = 1;
		argc--;
		argv++;
	}
	if (argc < 4)
		printf("usage: [-q] %s <start-date> <stop-date> <msm_dir> [ ... ]\n", argv[0]);

	else if (!(endptr = strptime(argv[1], "%Y-%m-%d", &start)) || *endptr)
		fprintf(stderr, "Could not parse <start-date>\n");

	else if (!(endptr = strptime(argv[2], "%Y-%m-%d", &stop)) || *endptr)
		fprintf(stderr, "Could not parse <stop-date>\n");

	else if (mktime(&start) >= mktime(&stop)) 
		fprintf(stderr, "<start-date> should be < <stop-date> (%d >= %d)\n"
		       , (int)mktime(&start), (int)mktime(&stop));

	else if (!(iters = calloc((n_iters = argc - 3), sizeof(dnst_iter))))
		fprintf(stderr, "Could not allocate dnst_iterators\n");

	else {
		char out_fn_tmp[40];
		char out_fn[40];
		char res_fn[40];
		uint32_t prev_t;
		int diff_t = 0;
		dnst_iter *first;
		int res_fd = -1;
		struct stat st;
		uint8_t *nodes;
		size_t n_nodes = 0;
		time_t forget = mktime(&start);

		forget -= 864000; /* Forget resolvers more than 10 days old */

		snprintf(res_fn, sizeof(res_fn), "%s.res", argv[1]);
		if ((res_fd = open(res_fn, O_RDONLY)) < 0)
			;
		else if (fstat(res_fd, &st) < 0)
			fprintf(stderr, "Could not fstat \"%s\"\n", res_fn);

		else if (!(nodes = malloc((n_nodes = (st.st_size / rec_data_sz))
		                         * sizeof(*rec))))
			fprintf(stderr, "Could not allocate space for nodes\n");

		else for (; n_nodes > 0; n_nodes--, nodes += sizeof(*rec)) {
			rec = (void *)nodes;
			if (read(res_fd, &rec->key, rec_data_sz) < 0)
				perror("Error reading resolvers");
			if ((time_t)rec->updated < forget)
				continue;
			rec->node.key = &rec->key;
			(void)rbtree_insert(&recs, &rec->node);
		}
		if (res_fd >= 0) {
			close(res_fd);
			res_fd = -1;
			fprintf(stderr, "Starting with %zu resolvers\n", recs.count);
		}
		if (!quiet && snprintf(out_fn_tmp, sizeof(out_fn_tmp),
		    "%s_%s.psql.tmp", argv[1], argv[2]) < sizeof(out_fn_tmp)) {
			snprintf( out_fn, sizeof(out_fn)
			        , "%s_%s.psql", argv[1], argv[2]);
			out = fopen(out_fn_tmp, "w");
		}
		for (i = 0; i < n_iters; i++)
			dnst_iter_init(&iters[i], &start, &stop, argv[i+3]);

		do {
			first = NULL;
			for (i = 0; i < n_iters; i++) {
				if (iters[i].cur
				&& (!first || iters[i].cur->time < first->cur->time))
					first = &iters[i];
			}
			if (first) {
				process_dnst(first->cur, first->msm_id);
				dnst_iter_next(first);
			}
		} while (first);
		for (i = 0; i < n_iters; i++)
			dnst_iter_done(&iters[i]);
		if (out) {
			fclose(out);
			rename(out_fn_tmp, out_fn);
		}
		snprintf(res_fn, sizeof(res_fn), "%s.res", argv[2]);
		if ((res_fd = open(res_fn, O_WRONLY | O_CREAT, 0644)) == -1)
			fprintf(stderr, "Could not open '%s'\n", res_fn);

		else RBTREE_FOR(rec, dnst_rec *, &recs) {
			write(res_fd, &rec->key, rec_data_sz);
		}
		if (res_fd != -1)
			close(res_fd);
		fprintf(stderr, "%zu resolvers on exit\n", recs.count);
		return 0;
	}
	return 1;
}
