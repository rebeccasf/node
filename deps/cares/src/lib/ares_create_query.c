
/* Copyright 1998 by the Massachusetts Institute of Technology.
 *
 * Permission to use, copy, modify, and distribute this
 * software and its documentation for any purpose and without
 * fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting
 * documentation, and that the name of M.I.T. not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is"
 * without express or implied warranty.
 */

#include "ares_setup.h"

#ifndef _WIN32
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warray-bounds"
#endif

#ifdef HAVE_NETINET_IN_H
#  include <netinet/in.h>
#endif

#include "ares_nameser.h"

#include "ares.h"
#include "ares_dns.h"
#include "ares_private.h"


/* Header format, from RFC 1035:
 *                                  1  1  1  1  1  1
 *    0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                      ID                       |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |QR|   Opcode  |AA|TC|RD|RA|   Z    |   RCODE   |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    QDCOUNT                    |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    ANCOUNT                    |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    NSCOUNT                    |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                    ARCOUNT                    |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *
 * AA, TC, RA, and RCODE are only set in responses.  Brief description
 * of the remaining fields:
 *      ID      Identifier to match responses with queries
 *      QR      Query (0) or response (1)
 *      Opcode  For our purposes, always O_QUERY
 *      RD      Recursion desired
 *      Z       Reserved (zero)
 *      QDCOUNT Number of queries
 *      ANCOUNT Number of answers
 *      NSCOUNT Number of name server records
 *      ARCOUNT Number of additional records
 *
 * Question format, from RFC 1035:
 *                                  1  1  1  1  1  1
 *    0  1  2  3  4  5  6  7  8  9  0  1  2  3  4  5
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                                               |
 *  /                     QNAME                     /
 *  /                                               /
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                     QTYPE                     |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *  |                     QCLASS                    |
 *  +--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
 *
 * The query name is encoded as a series of labels, each represented
 * as a one-byte length (maximum 63) followed by the text of the
 * label.  The list is terminated by a label of length zero (which can
 * be thought of as the root domain).
 */

int ares_create_query(const char *name, int dnsclass, int type,
                      unsigned short id, int rd, unsigned char **bufp,
                      int *buflenp, int max_udp_size)
{
  size_t len;
  unsigned char *q;
  const char *p;
  size_t buflen;
  unsigned char *buf;

  /* Set our results early, in case we bail out early with an error. */
  *buflenp = 0;
  *bufp = NULL;

  /* Per RFC 7686, reject queries for ".onion" domain names with NXDOMAIN. */
  if (ares__is_onion_domain(name))
    return ARES_ENOTFOUND;

  /* Allocate a memory area for the maximum size this packet might need. +2
   * is for the length byte and zero termination if no dots or ecscaping is
   * used.
   */
  len = strlen(name) + 2 + HFIXEDSZ + QFIXEDSZ +
    (max_udp_size ? EDNSFIXEDSZ : 0);
  buf = ares_malloc(len);
  if (!buf)
    return ARES_ENOMEM;

  /* Set up the header. */
  q = buf;
  memset(q, 0, HFIXEDSZ);
  DNS_HEADER_SET_QID(q, id);
  DNS_HEADER_SET_OPCODE(q, O_QUERY);
  if (rd) {
    DNS_HEADER_SET_RD(q, 1);
  }
  else {
    DNS_HEADER_SET_RD(q, 0);
  }
  DNS_HEADER_SET_QDCOUNT(q, 1);

  if (max_udp_size) {
      DNS_HEADER_SET_ARCOUNT(q, 1);
  }

  /* A name of "." is a screw case for the loop below, so adjust it. */
  if (strcmp(name, ".") == 0)
    name++;

  /* Start writing out the name after the header. */
  q += HFIXEDSZ;
  while (*name)
    {
      if (*name == '.') {
        ares_free (buf);
        return ARES_EBADNAME;
      }

      /* Count the number of bytes in this label. */
      len = 0;
      for (p = name; *p && *p != '.'; p++)
        {
          if (*p == '\\' && *(p + 1) != 0)
            p++;
          len++;
        }
      if (len > MAXLABEL) {
        ares_free (buf);
        return ARES_EBADNAME;
      }

      /* Encode the length and copy the data. */
      *q++ = (unsigned char)len;
      for (p = name; *p && *p != '.'; p++)
        {
          if (*p == '\\' && *(p + 1) != 0)
            p++;
          *q++ = *p;
        }

      /* Go to the next label and repeat, unless we hit the end. */
      if (!*p)
        break;
      name = p + 1;
    }

  /* Add the zero-length label at the end. */
  *q++ = 0;

  /* Finish off the question with the type and class. */
  DNS_QUESTION_SET_TYPE(q, type);
  DNS_QUESTION_SET_CLASS(q, dnsclass);

  q += QFIXEDSZ;
  if (max_udp_size)
  {
      memset(q, 0, EDNSFIXEDSZ);
      q++;
      DNS_RR_SET_TYPE(q, T_OPT);
      DNS_RR_SET_CLASS(q, max_udp_size);
      q += (EDNSFIXEDSZ-1);
  }
  buflen = (q - buf);

  /* Reject names that are longer than the maximum of 255 bytes that's
   * specified in RFC 1035 ("To simplify implementations, the total length of
   * a domain name (i.e., label octets and label length octets) is restricted
   * to 255 octets or less."). */
  if (buflen > (size_t)(MAXCDNAME + HFIXEDSZ + QFIXEDSZ +
                (max_udp_size ? EDNSFIXEDSZ : 0))) {
    ares_free (buf);
    return ARES_EBADNAME;
  }

  /* we know this fits in an int at this point */
  *buflenp = (int) buflen;
  *bufp = buf;

  return ARES_SUCCESS;
}

#ifndef _WIN32
#pragma clang diagnostic pop
#endif
