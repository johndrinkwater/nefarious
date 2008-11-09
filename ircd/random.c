/*
 * IRC - Internet Relay Chat, ircd/random.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id$
 */
#include "config.h"

#include "random.h"
#include "client.h"
#include "ircd_log.h"
#include "ircd_md5.h"
#include "ircd_reply.h"
#include "send.h"

#include <string.h>
#include <sys/time.h>
#include <stdio.h>

/** Pseudo-random number generator state. */
static struct MD5Context localkey;
/** Next byte position in #localkey to insert at. */
static unsigned int localkey_pos;

/** Add bytes to #localkey.
 * This should be fairly resistant to adding non-random bytes, but the
 * more random the bytes are, the harder it is for an attacker to
 * guess the internal state.
 * @param[in] buf Buffer of bytes to add.
 * @param[in] count Number of bytes to add.
 */
static void
random_add_entropy(const char *buf, unsigned int count)
{
  while (count--) {
    localkey.in[localkey_pos++] ^= *buf++;
    if (localkey_pos >= sizeof(localkey.in))
      localkey_pos = 0;
  }
}

/* this routine is intended to be called by the feature subsystem; it takes
 * a key as found in the .conf and mashes it up for the seed for the random
 * number generator.
 */
int
random_seed_set(struct Client* from, const char* const* fields, int count)
{
  if (count < 1) {
    if (from) /* send an error */
      return need_more_params(from, "SET");
    else {
      log_write(LS_CONFIG, L_ERROR, 0, "Not enough fields in F line");
      return 0;
    }
  }

  random_add_entropy(fields[0], strlen(fields[0]));

  return 1;
}

unsigned int ircrandom(void)
{
  struct timeval tv;
  char usec[3];

  /* Add some randomness to the pool. */
  gettimeofday(&tv, 0);

  usec[0] = tv.tv_usec;
  usec[1] = tv.tv_usec >> 8;
  usec[2] = tv.tv_usec >> 16;
  random_add_entropy(usec, 3);

  /* Perform MD5 step. */
  localkey.buf[0] = 0x67452301;
  localkey.buf[1] = 0xefcdab89;
  localkey.buf[2] = 0x98badcfe;
  localkey.buf[3] = 0x10325476;
  MD5Transform(localkey.buf, (uint32*)localkey.in);

  /* Feed back 12 bytes of hash value into randomness pool. */
  random_add_entropy((char*)localkey.buf, 12);

  /* Return the final word of hash, which should not provide any
   * useful insight into current pool contents. */
  return localkey.buf[3];
}
