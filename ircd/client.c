/*
 * IRC - Internet Relay Chat, ircd/client.c
 * Copyright (C) 1990 Darren Reed
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
 */
/** @file
 * @brief Implementation of functions for handling local clients.
 * @version $Id$
 */
#include "config.h"

#include "client.h"
#include "class.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "ircd_struct.h"
#include "list.h"
#include "msgq.h"
#include "msg.h"
#include "numeric.h"
#include "s_conf.h"
#include "s_debug.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <string.h>

#define BAD_PING                ((unsigned int)-2)
char privbufp[512] = "";


/** Find the shortest non-zero ping time attached to a client.
 * If all attached ping times are zero, return the value for
 * FEAT_PINGFREQUENCY.
 * @param[in] acptr Client to find ping time for.
 * @return Ping time in seconds.
 */
int client_get_ping(const struct Client* acptr)
{
  int     ping = 0;
  struct ConfItem* aconf;
  struct SLink*    link;

  assert(cli_verify(acptr));

  for (link = cli_confs(acptr); link; link = link->next) {
    aconf = link->value.aconf;
    if (aconf->status & (CONF_CLIENT | CONF_SERVER)) {
      int tmp = get_conf_ping(aconf);
      if (0 < tmp && (ping > tmp || !ping))
        ping = tmp;
    }
  }
  if (0 == ping)
    ping = feature_int(FEAT_PINGFREQUENCY);

  Debug((DEBUG_DEBUG, "Client %s Ping %d", cli_name(acptr), ping));

  return ping;
}

/*
 * client_get_default_umode
 * returns default usermode in attached client connection class
 */
const char* client_get_default_umode(const struct Client* sptr)
{
  struct ConfItem* aconf;
  struct SLink* link;

  assert(cli_verify(sptr));

  for (link = cli_confs(sptr); link; link = link->next) {
    aconf = link->value.aconf;
    if ((aconf->status & CONF_CLIENT) && ConfUmode(aconf))
      return ConfUmode(aconf);
  }
  return NULL;
}

/** Remove a connection from the list of connections with queued data.
 * @param[in] con Connection with no queued data.
 */
void client_drop_sendq(struct Connection* con)
{
  if (con_prev_p(con)) { /* on the queued data list... */
    if (con_next(con))
      con_prev_p(con_next(con)) = con_prev_p(con);
    *(con_prev_p(con)) = con_next(con);

    con_next(con) = 0;
    con_prev_p(con) = 0;
  }
}

/** Add a connection to the list of connections with queued data.
 * @param[in] con Connection with queued data.
 * @param[in,out] con_p Previous pointer to next connection.
 */
void client_add_sendq(struct Connection* con, struct Connection** con_p)
{
  if (!con_prev_p(con)) { /* not on the queued data list yet... */
    con_prev_p(con) = con_p;
    con_next(con) = *con_p;

    if (*con_p)
      con_prev_p(*con_p) = &(con_next(con));
    *con_p = con;
  }
}

/** Default privilege set for global operators. */
static struct Privs privs_global;
/** Default privilege set for local operators. */
static struct Privs privs_local;
/** Non-zero if #privs_global and #privs_local have been initialized. */
static int privs_defaults_set;

/** Array mapping privilege values to names and vice versa. */
static struct {
  char        *name;
  unsigned int priv;
} privtab[] = {
#define P(priv)         { #priv, PRIV_ ## priv }
  P(CHAN_LIMIT),     P(MODE_LCHAN),     P(WALK_LCHAN),    P(DEOP_LCHAN),
  P(SHOW_INVIS),     P(SHOW_ALL_INVIS), P(UNLIMIT_QUERY), P(KILL),
  P(LOCAL_KILL),     P(REHASH),         P(RESTART),       P(DIE),
  P(GLINE),          P(LOCAL_GLINE),    P(JUPE),          P(LOCAL_JUPE),
  P(OPMODE),         P(LOCAL_OPMODE),   P(SET),           P(WHOX),
  P(BADCHAN),        P(LOCAL_BADCHAN),  P(SEE_CHAN),      P(PROPAGATE),
  P(DISPLAY),        P(SEE_OPERS),      P(WIDE_GLINE),    P(FORCE_OPMODE),
  P(FORCE_LOCAL_OPMODE), P(REMOTEREHASH), P(CHECK), P(SEE_SECRET_CHAN),
  P(SHUN),           P(LOCAL_SHUN),     P(WIDE_SHUN),     P(ZLINE),
  P(LOCAL_ZLINE),    P(WIDE_ZLINE),     P(LIST_CHAN),     P(WHOIS_NOTICE),
  P(HIDE_IDLE),      P(XTRAOP),         P(HIDE_CHANNELS), P(DISPLAY_MODE),
  P(FREEFORM),       P(REMOVE),         P(SPAMFILTER),
#undef P
  { 0, 0 }
};

/* client_set_privs(struct Client* client)
 *
 * Sets the privileges for opers.
 */
/** Set the privileges for a client.
 * @param[in] client Client who has become an operator.
 * @param[in] oper Configuration item describing oper's privileges.
 */
void
client_set_privs(struct Client *client, struct ConfItem *oper)
{
  struct Privs *source, *defaults;
  enum Priv priv;
  char *privbuf;

/* 
 * This stopped remote OPER from working properly resulting in opers
 * not getting proper PRIVS
 *
  if (!MyConnect(client))
    return;
*/

  /* Clear out client's privileges. */
  memset(&cli_privs(client), 0, sizeof(struct Privs));

  if (!IsAnOper(client) || !oper)
      return;

  if (!privs_defaults_set)
  {
    memset(&privs_global, -1, sizeof(privs_global));
    memset(&privs_local, 0, sizeof(privs_local));
    FlagClr(&privs_global, PRIV_WALK_LCHAN);
    FlagClr(&privs_global, PRIV_UNLIMIT_QUERY);
    FlagClr(&privs_global, PRIV_SET);
    FlagClr(&privs_global, PRIV_BADCHAN);
    FlagClr(&privs_global, PRIV_LOCAL_BADCHAN);
    FlagClr(&privs_global, PRIV_WHOIS_NOTICE);
    FlagClr(&privs_global, PRIV_HIDE_IDLE);
    FlagClr(&privs_global, PRIV_XTRAOP);
    FlagClr(&privs_global, PRIV_HIDE_CHANNELS);
    FlagClr(&privs_global, PRIV_REMOVE);
    FlagClr(&privs_global, PRIV_DISPLAY_MODE);
    FlagClr(&privs_global, PRIV_SPAMFILTER);
    FlagClr(&privs_global, PRIV_FREEFORM);

    FlagSet(&privs_local, PRIV_CHAN_LIMIT);
    FlagSet(&privs_local, PRIV_MODE_LCHAN);
    FlagSet(&privs_local, PRIV_SHOW_INVIS);
    FlagSet(&privs_local, PRIV_SHOW_ALL_INVIS);
    FlagSet(&privs_local, PRIV_LOCAL_KILL);
    FlagSet(&privs_local, PRIV_REHASH);
    FlagSet(&privs_local, PRIV_LOCAL_GLINE);
    FlagSet(&privs_local, PRIV_LOCAL_ZLINE);
    FlagSet(&privs_local, PRIV_LOCAL_SHUN);
    FlagSet(&privs_local, PRIV_LOCAL_JUPE);
    FlagSet(&privs_local, PRIV_LOCAL_OPMODE);
    FlagSet(&privs_local, PRIV_WHOX);
    FlagSet(&privs_local, PRIV_DISPLAY);
    FlagSet(&privs_local, PRIV_FORCE_LOCAL_OPMODE);
    privs_defaults_set = 1;
  }

  /* Decide whether to use global or local oper defaults. */
  if (FlagHas(&oper->privs_dirty, PRIV_PROPAGATE))
    defaults = FlagHas(&oper->privs, PRIV_PROPAGATE) ? &privs_global : &privs_local;
  else if (FlagHas(&oper->conn_class->privs_dirty, PRIV_PROPAGATE))
    defaults = FlagHas(&oper->conn_class->privs, PRIV_PROPAGATE) ? &privs_global : &privs_local;
  else {
    assert(0 && "Oper has no propagation and neither does connection class");
    return;
  }

  /* For each feature, figure out whether it comes from the operator
   * conf, the connection class conf, or the defaults, then apply it.
   */
  for (priv = 0; priv < PRIV_LAST_PRIV; ++priv)
  {
    /* Figure out most applicable definition for the privilege. */
    if (FlagHas(&oper->privs_dirty, priv))
      source = &oper->privs;
    else if (FlagHas(&oper->conn_class->privs_dirty, priv))
      source = &oper->conn_class->privs;
    else
      source = defaults;

    /* Set it if necessary (privileges were already cleared). */
    if (FlagHas(source, priv))
      SetPriv(client, priv);
  }

  /* This should be handled in the config, but lets be sure... */
  if (HasPriv(client, PRIV_PROPAGATE))
  {
    /* force propagating opers to display */
    SetPriv(client, PRIV_DISPLAY);
  }
  else
  {
    /* if they don't propagate oper status, prevent desyncs */
    ClrPriv(client, PRIV_KILL);
    ClrPriv(client, PRIV_GLINE);
    ClrPriv(client, PRIV_SHUN);
    ClrPriv(client, PRIV_ZLINE);
    ClrPriv(client, PRIV_JUPE);
    ClrPriv(client, PRIV_OPMODE);
    ClrPriv(client, PRIV_BADCHAN);
  }

  if (OIsWhois(client)) {
    SetPriv(client, PRIV_WHOIS_NOTICE);
  }
  if (OIsIdle(client)) {
    SetPriv(client, PRIV_HIDE_IDLE);
  }
  if (OIsXtraop(client)) {
    SetPriv(client, PRIV_XTRAOP);
  }
  if (OIsHideChans(client)) {
    SetPriv(client, PRIV_HIDE_CHANNELS);
  }

  privbuf = client_print_privs(client);
  sendcmdto_serv_butone(&me, CMD_PRIVS, client, "%C %s", client, privbuf);
}

/** Report privileges of \a client to \a to.
 * @param[in] to Client requesting privilege list.
 * @param[in] client Client whos privileges should be listed.
 * @return Zero.
 */
int
client_report_privs(struct Client *to, struct Client *client)
{
  struct MsgBuf *mb;
  int found1 = 0;
  int i;

  mb = msgq_make(to, rpl_str(RPL_PRIVS), cli_name(&me), cli_name(to),
		 cli_name(client));

  for (i = 0; privtab[i].name; i++)
    if (HasPriv(client, privtab[i].priv))
      msgq_append(0, mb, "%s%s", found1++ ? " " : "", privtab[i].name);

  send_buffer(to, mb, 0); /* send response */
  msgq_clean(mb);

  return 0;
}


int
client_debug_privs(struct Client *client)
{
  int i;
  for (i = 0; privtab[i].name; i++) {
    if (HasPriv(client, privtab[i].priv))
      Debug((DEBUG_DEBUG, "PRIV Set: %s", privtab[i].name));
  }

  return 0;
}

char *client_print_privs(struct Client *client)
{
  int i;

  privbufp[0] = '\0';
  for (i = 0; privtab[i].name; i++) {
    if (HasPriv(client, privtab[i].priv)) {
      strcat(privbufp, privtab[i].name);
      strcat(privbufp, " ");
    }
  }
  privbufp[strlen(privbufp)] = 0;

  return privbufp;
}

char *client_check_privs(struct Client *client, struct Client *replyto)
{
  char outbuf[BUFSIZE];
  int i, p = 0;

  privbufp[0] = '\0';

  for (i = 0; privtab[i].name; i++) {
    if (HasPriv(client, privtab[i].priv)) {
      p++;
      if (p > 10) {
        ircd_snprintf(0, outbuf, sizeof(outbuf), "     Privileges:: %s", privbufp);
        send_reply(replyto, RPL_DATASTR, outbuf);

        p = 0;
        privbufp[strlen(privbufp)] = 0;
        privbufp[0] = '\0';
      }
      strcat(privbufp, privtab[i].name);
      strcat(privbufp, " ");
    }
  }

  if (privbufp[0] != '\0') {
    ircd_snprintf(0, outbuf, sizeof(outbuf), "     Privileges:: %s", privbufp);
    send_reply(replyto, RPL_DATASTR, outbuf);
  }

  privbufp[strlen(privbufp)] = 0;
}

int client_modify_priv_by_name(struct Client *who, char *priv, int what) {
  int i = 0;
  assert(0 != priv);
  assert(0 != who);

  for (i = 0; privtab[i].name; i++)
  if (0 == ircd_strcmp(privtab[i].name, priv)) {
    if (what == PRIV_ADD) {
      SetPriv(who, privtab[i].priv);
    } else if (what == PRIV_DEL) {
      ClrPriv(who, privtab[i].priv);
    }
  }
  return 0;
}

int clear_privs(struct Client *who) {
  int i = 0;
  assert(0 != who);

  for (i = 0; privtab[i].name; i++)
    ClrPriv(who, privtab[i].priv);

  return 0;
}

/*
 * A little spin-marking utility to tell us which clients we have already
 * processed and which not
 */
unsigned int get_client_marker(void)
{
  static unsigned int marker = 0;

  if (!++marker)
  {
    struct Client *cptr;
    for (cptr=GlobalClientList;cptr;cptr=cli_next(cptr))
    {
      cli_marker(cptr) = 0;
    }
    marker++;
  }

  return marker;
}

