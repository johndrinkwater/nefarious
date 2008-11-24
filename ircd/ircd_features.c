/*
 * IRC - Internet Relay Chat, ircd/features.c
 * Copyright (C) 2000 Kevin L. Mitchell <klmitch@mit.edu>
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

#include "ircd_features.h"
#include "channel.h"	/* list_set_default */
#include "class.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_alloc.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_string.h"
#include "match.h"
#include "motd.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "random.h"	/* random_seed_set */
#include "s_bsd.h"
#include "s_debug.h"
#include "s_misc.h"
#include "s_stats.h"
#include "s_user.h"
#include "send.h"
#include "ircd_struct.h"
#include "support.h"
#include "sys.h"    /* FALSE bleah */
#include "whowas.h"	/* whowas_realloc */

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdlib.h>
#include <string.h>

struct Client his;

/* List of log output types that can be set */
static struct LogTypes {
  char *type;
  int (*set)(const char *, const char *);
  char *(*get)(const char *);
} logTypes[] = {
  { "FILE", log_set_file, log_get_file },
  { "FACILITY", log_set_facility, log_get_facility },
  { "SNOMASK", log_set_snomask, log_get_snomask },
  { "LEVEL", log_set_level, log_get_level },
  { 0, 0, 0 }
};

char* itoa (int n){
  int i=0,j;
  char* s;
  char* u;

  s= (char*) malloc(17);
  u= (char*) malloc(17);
  
  do{
    s[i++]=(char)( n%10+48 );
    n-=n%10;
  }
  while((n/=10)>0);
  for (j=0;j<i;j++)
  u[i-1-j]=s[j];

  u[j]='\0';
  return u;
}

/** Handle an update to FEAT_HIS_SERVERNAME. */
static void
feature_notify_servername(void)
{
  ircd_strncpy(cli_name(&his), feature_str(FEAT_HIS_SERVERNAME), HOSTLEN);
}

/** Handle an update to FEAT_HIS_SERVERINFO. */
static void
feature_notify_serverinfo(void)
{
  ircd_strncpy(cli_info(&his), feature_str(FEAT_HIS_SERVERINFO), REALLEN);
}

/* Look up a struct LogType given the type string */
static struct LogTypes *
feature_log_desc(struct Client* from, const char *type)
{
  int i;

  assert(0 != type);

  for (i = 0; logTypes[i].type; i++) /* find appropriate descriptor */
    if (!ircd_strcmp(type, logTypes[i].type))
      return &logTypes[i];

  Debug((DEBUG_ERROR, "Unknown log feature type \"%s\"", type));
  if (from) /* send an error; if from is NULL, called from conf parser */
    send_reply(from, ERR_BADLOGTYPE, type);
  else
    log_write(LS_CONFIG, L_ERROR, 0, "Unknown log feature type \"%s\"", type);

  return 0; /* not found */
}

/* Set the value of a log output type for a log subsystem */
static int
feature_log_set(struct Client* from, const char* const* fields, int count)
{
  struct LogTypes *desc;
  char *subsys;

  if (count < 2) { /* set default facility */
    if (log_set_default(count < 1 ? 0 : fields[0])) {
      assert(count >= 1); /* should always accept default */

      if (from) /* send an error */
	send_reply(from, ERR_BADLOGVALUE, fields[0]);
      else
	log_write(LS_CONFIG, L_ERROR, 0,
		  "Bad value \"%s\" for default facility", fields[0]);
    } else
      return count < 1 ? -1 : 1; /* tell feature to set or clear mark */
  } else if (!(subsys = log_canon(fields[0]))) { /* no such subsystem */
    if (from) /* send an error */
      send_reply(from, ERR_BADLOGSYS, fields[0]);
    else
      log_write(LS_CONFIG, L_ERROR, 0,
		"No such logging subsystem \"%s\"", fields[0]);
  } else if ((desc = feature_log_desc(from, fields[1]))) { /* set value */
    if ((*desc->set)(fields[0], count < 3 ? 0 : fields[2])) {
      assert(count >= 3); /* should always accept default */

      if (from) /* send an error */
	send_reply(from, ERR_BADLOGVALUE, fields[2]);
      else
	log_write(LS_CONFIG, L_ERROR, 0,
		  "Bad value \"%s\" for log type %s (subsystem %s)",
		  fields[2], desc->type, subsys);
    }
  }

  return 0;
}

/* reset a log type for a subsystem to its default value */
static int
feature_log_reset(struct Client* from, const char* const* fields, int count)
{
  struct LogTypes *desc;
  char *subsys;

  assert(0 != from); /* Never called by the .conf parser */

  if (count < 1) { /* reset default facility */
    log_set_default(0);
    return -1; /* unmark this entry */
  } else if (count < 2)
    need_more_params(from, "RESET");
  else if (!(subsys = log_canon(fields[0]))) /* no such subsystem */
    send_reply(from, ERR_BADLOGSYS, fields[0]);
  else if ((desc = feature_log_desc(from, fields[1]))) /* reset value */
    (*desc->set)(fields[0], 0); /* default should always be accepted */

  return 0;
}

/* report the value of a log setting */
static void
feature_log_get(struct Client* from, const char* const* fields, int count)
{
  struct LogTypes *desc;
  char *value, *subsys;

  assert(0 != from); /* never called by .conf parser */

  if (count < 1) /* return default facility */
    send_reply(from, SND_EXPLICIT | RPL_FEATURE, ":Log facility: %s",
	       log_get_default());
  else if (count < 2)
    need_more_params(from, "GET");
  else if (!(subsys = log_canon(fields[0]))) { /* no such subsystem */
    send_reply(from, ERR_BADLOGSYS, fields[0]);
  } else if ((desc = feature_log_desc(from, fields[1]))) {
    if ((value = (*desc->get)(fields[0]))) /* send along value */
      send_reply(from, SND_EXPLICIT | RPL_FEATURE,
		 ":Log %s for subsystem %s: %s", desc->type, subsys,
		 (*desc->get)(subsys));
    else
      send_reply(from, SND_EXPLICIT | RPL_FEATURE,
		 ":No log %s is set for subsystem %s", desc->type, subsys);
  }
}

static void
set_isupport_halfops(void)
{
    add_isupport_s("PREFIX", feature_bool(FEAT_HALFOPS) ? "(ohv)@%+" : "(ov)@+");
    add_isupport_s("STATUSMSG", feature_bool(FEAT_HALFOPS) ? "@%+" : "@+");
}

static void
set_isupport_excepts(void)
{
    char imaxlist[BUFSIZE] = "";

    if (feature_bool(FEAT_EXCEPTS)) {
      add_isupport_s("EXCEPTS", "e");
      add_isupport_i("MAXEXCEPTS", feature_int(FEAT_MAXEXCEPTS));
    } else {
      del_isupport("EXCEPTS");
      del_isupport("MAXEXCEPTS");
    }

    add_isupport_s("CHANMODES", feature_bool(FEAT_EXCEPTS) ? "be,k,l,acimnprstzCLMNOQSTZ" : "b,k,l,acimnprstzCLMNOQSTZ");

    strcat(imaxlist, "b:");
    strcat(imaxlist, itoa(feature_int(FEAT_MAXBANS)));
    if (feature_bool(FEAT_EXCEPTS)) {
      strcat(imaxlist, "e:");
      strcat(imaxlist, itoa(feature_int(FEAT_MAXEXCEPTS)));
    }

    add_isupport_s("MAXLIST", imaxlist);
}

static void
set_isupport_watchs(void)
{
    add_isupport_i("WATCH", feature_int(FEAT_MAXWATCHS));
}

static void
set_isupport_maxsiles(void)
{
    add_isupport_i("SILENCE", feature_int(FEAT_MAXSILES));
}

static void
set_isupport_maxchannels(void)
{
    /* uint */
    add_isupport_i("MAXCHANNELS", feature_int(FEAT_MAXCHANNELSPERUSER));
}

static void
set_isupport_maxbans(void)
{
    add_isupport_i("MAXBANS", feature_int(FEAT_MAXBANS));
}

static void
set_isupport_nicklen(void)
{
    /* uint */
    add_isupport_i("NICKLEN", feature_int(FEAT_NICKLEN));
}

static void
set_isupport_channellen(void)
{
    /* uint */
    add_isupport_i("CHANNELLEN", feature_int(FEAT_CHANNELLEN));
}

static void
set_isupport_chantypes(void)
{
    add_isupport_s("CHANTYPES", feature_bool(FEAT_LOCAL_CHANNELS) ? "#&" : "#");
}

static void
set_isupport_network(void)
{
    add_isupport_s("NETWORK", feature_str(FEAT_NETWORK));
}

/* sets a feature to the given value */
typedef int  (*feat_set_call)(struct Client*, const char* const*, int);
/* gets the value of a feature */
typedef void (*feat_get_call)(struct Client*, const char* const*, int);
/* callback to notify of a feature's change */
typedef void (*feat_notify_call)(void);
/* unmarks all sub-feature values prior to reading .conf */
typedef void (*feat_unmark_call)(void);
/* resets to defaults all currently unmarked values */
typedef int  (*feat_mark_call)(int);
/* reports features as a /stats f list */
typedef void (*feat_report_call)(struct Client*, int);

#define FEAT_NONE   0x0000	/* no value */
#define FEAT_INT    0x0001	/* set if entry contains an integer value */
#define FEAT_BOOL   0x0002	/* set if entry contains a boolean value */
#define FEAT_STR    0x0003	/* set if entry contains a string value */
#define FEAT_ALIAS  0x0004      /**< set if entry is alias for another entry */
#define FEAT_DEP    0x0005      /**< set if entry is deprecated feature */
#define FEAT_UINT   0x0006      /**< set if entry contains an unsigned value */
#define FEAT_MASK   0x000f	/* possible value types */

/** Extract just the feature type from a feature descriptor. */
#define feat_type(feat)         ((feat)->flags & FEAT_MASK)

#define FEAT_MARK   0x0010	/* set if entry has been changed */
#define FEAT_NULL   0x0020	/* NULL string is permitted */
#define FEAT_CASE   0x0040	/* string is case-sensitive */

#define FEAT_OPER   0x0100	/* set to display only to opers */
#define FEAT_MYOPER 0x0200	/* set to display only to local opers */
#define FEAT_NODISP 0x0400	/* feature must never be displayed */

#define FEAT_READ   0x1000	/* feature is read-only (for now, perhaps?) */

static struct FeatureDesc {
  enum Feature	   feat;    /* feature identifier */
  char*		   type;    /* string describing type */
  unsigned int     flags;   /* flags for feature */
  int		   v_int;   /* integer value */
  int		   def_int; /* default value */
  char*		   v_str;   /* string value */
  char*		   def_str; /* default value */
  feat_set_call	   set;	    /* set feature values */
  feat_set_call	   reset;   /* reset feature values to defaults */
  feat_get_call	   get;	    /* get feature values */
  feat_notify_call notify;  /* notify of value change */
  feat_unmark_call unmark;  /* unmark all feature change values */
  feat_mark_call   mark;    /* reset to defaults all unchanged features */
  feat_report_call report;  /* report feature values */
} features[] = {
#define F_N(type, flags, set, reset, get, notify, unmark, mark, report)	      \
  { FEAT_ ## type, #type, FEAT_NONE | (flags), 0, 0, 0, 0,		      \
    (set), (reset), (get), (notify), (unmark), (mark), (report) }
#define F_I(type, flags, v_int, notify)					      \
  { FEAT_ ## type, #type, FEAT_INT | (flags), 0, (v_int), 0, 0,		      \
    0, 0, 0, (notify), 0, 0, 0 }
/** Declare a feature that takes unsigned integer values. */
#define F_U(type, flags, v_uint, notify)                                      \
  { FEAT_ ## type, #type, FEAT_UINT | (flags), 0, (v_uint), 0, 0,             \
    0, 0, 0, (notify), 0, 0, 0 }
#define F_B(type, flags, v_int, notify)					      \
  { FEAT_ ## type, #type, FEAT_BOOL | (flags), 0, (v_int), 0, 0,	      \
    0, 0, 0, (notify), 0, 0, 0 }
#define F_S(type, flags, v_str, notify)					      \
  { FEAT_ ## type, #type, FEAT_STR | (flags), 0, 0, 0, (v_str),		      \
    0, 0, 0, (notify), 0, 0, 0 }
/** Declare a feature as an alias for another feature. */
#define F_A(type, alias)                                                      \
  { FEAT_ ## type, #type, FEAT_ALIAS, 0, FEAT_ ## alias, 0, 0,                \
    0, 0, 0, 0, 0, 0, 0 }
/** Declare a feature as deprecated. */
#define F_D(type)                                                             \
  { FEAT_ ## type, #type, FEAT_DEP, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }

  /* Misc. features */
  F_N(LOG, FEAT_MYOPER, feature_log_set, feature_log_reset, feature_log_get,
      0, log_feature_unmark, log_feature_mark, log_feature_report),
  F_S(DOMAINNAME, 0, "Nefarious.IRC", 0),
  F_B(RELIABLE_CLOCK, 0, 0, 0),
  F_I(BUFFERPOOL, 0, 27000000, 0),
  F_B(HAS_FERGUSON_FLUSHER, 0, 0, 0),
  F_I(CLIENT_FLOOD, 0, 1024, 0),
  F_I(SERVER_PORT, FEAT_OPER, 4400, 0),
  F_B(NODEFAULTMOTD, 0, 1, 0),
  F_S(MOTD_BANNER, FEAT_NULL, 0, 0),
  F_S(PROVIDER, FEAT_NULL, 0, 0),
  F_B(KILL_IPMISMATCH, FEAT_OPER, 0, 0),
  F_B(IDLE_FROM_MSG, 0, 1, 0),
  F_B(HUB, 0, 0, 0),
  F_B(WALLOPS_OPER_ONLY, 0, 0, 0),
  F_B(NODNS, 0, 0, 0),
  F_N(RANDOM_SEED, FEAT_NODISP, random_seed_set, 0, 0, 0, 0, 0, 0),
  F_S(DEFAULT_LIST_PARAM, FEAT_NULL, 0, list_set_default),
  F_I(NICKNAMEHISTORYLENGTH, 0, 800, whowas_realloc),
  F_B(HOST_HIDING, 0, 1, 0),
  F_S(HIDDEN_HOST, FEAT_CASE, "Users.Nefarious", 0),
  F_S(HIDDEN_IP, 0, "127.0.0.1", 0),
  F_B(CONNEXIT_NOTICES, 0, 0, 0),

  /* features that probably should not be touched */
  F_I(KILLCHASETIMELIMIT, 0, 30, 0),
  F_I(MAXCHANNELSPERUSER, 0, 20, set_isupport_maxchannels),
  F_I(NICKLEN, 0, 15, set_isupport_nicklen),
  F_I(CHANNELLEN, 0, 200, set_isupport_channellen),
  F_I(AVBANLEN, 0, 40, 0),
  F_I(MAXBANS, 0, 45, set_isupport_maxbans),
  F_I(MAXSILES, 0, 15, set_isupport_maxsiles),
  F_I(MAXWATCHS, 0, 128, set_isupport_watchs),
  F_I(HANGONGOODLINK, 0, 300, 0),
  F_I(HANGONRETRYDELAY, 0, 10, 0),
  F_I(CONNECTTIMEOUT, 0, 60, 0),
  F_I(TIMESEC, 0, 60, 0),
  F_I(MAXIMUM_LINKS, 0, 1, init_class), /* reinit class 0 as needed */
  F_I(PINGFREQUENCY, 0, 120, init_class),
  F_I(CONNECTFREQUENCY, 0, 600, init_class),
  F_I(DEFAULTMAXSENDQLENGTH, 0, 40000, init_class),
  F_I(GLINEMAXUSERCOUNT, 0, 20, 0),
  F_I(SOCKSENDBUF, 0, 0, 0),
  F_I(SOCKRECVBUF, 0, 0, 0),
  F_I(IPCHECK_CLONE_LIMIT, 0, 4, 0),
  F_I(IPCHECK_CLONE_PERIOD, 0, 40, 0),
  F_I(IPCHECK_CLONE_DELAY, 0, 600, 0),  

  /* Some misc. default paths */
  F_S(MPATH, FEAT_CASE | FEAT_MYOPER, "ircd.motd", motd_init_local),
  F_S(RPATH, FEAT_CASE | FEAT_MYOPER, "remote.motd", motd_init_remote),
  F_S(PPATH, FEAT_CASE | FEAT_MYOPER | FEAT_READ, "ircd.pid", 0),

  /* Networking features */
  F_B(VIRTUAL_HOST, 0, 0, 0),
  F_I(TOS_SERVER, 0, 0x08, 0),
  F_I(TOS_CLIENT, 0, 0x08, 0),
  F_I(POLLS_PER_LOOP, 0, 200, 0),
  F_I(IRCD_RES_RETRIES, 0, 2, 0),
  F_I(IRCD_RES_TIMEOUT, 0, 4, 0),
  F_I(AUTH_TIMEOUT, 0, 9, 0),

  /* features that affect all operators */
  F_B(CONFIG_OPERCMDS, 0, 1, 0),

  /* HEAD_IN_SAND Features */
  F_B(HIS_SNOTICES, 0, 1, 0),
  F_B(HIS_SNOTICES_OPER_ONLY, 0, 1, 0),
  F_B(HIS_SNOTICES_OPER_AND_BOT, 0, 0, 0),
  F_B(HIS_DESYNCS, 0, 1, 0),
  F_B(HIS_DEBUG_OPER_ONLY, 0, 1, 0),
  F_B(HIS_WALLOPS, 0, 1, 0),
  F_B(HIS_MAP, 0, 1, 0),
  F_B(HIS_LINKS, 0, 1, 0),
  F_B(HIS_TRACE, 0, 1, 0),
  F_A(HIS_STATS_B, HIS_STATS_MAPPINGS),
  F_B(HIS_STATS_MAPPINGS, 0, 1, 0),
  F_A(HIS_STATS_c, HIS_STATS_CONNECT),
  F_B(HIS_STATS_CONNECT, 0, 1, 0),
  F_A(HIS_STATS_d, HIS_STATS_CRULES),
  F_B(HIS_STATS_CRULES, 0, 1, 0),
  F_A(HIS_STATS_e, HIS_STATS_ENGINE),
  F_B(HIS_STATS_ENGINE, 0, 1, 0),
  F_A(HIS_STATS_E, HIS_STATS_EXCEPTIONS),
  F_B(HIS_STATS_EXCEPTIONS, 0, 1, 0),
  F_A(HIS_STATS_f, HIS_STATS_FILTERS),
  F_B(HIS_STATS_FILTERS, 0, 1, 0),
  F_A(HIS_STATS_F, HIS_STATS_FEATURES),
  F_B(HIS_STATS_FEATURES, 0, 1, 0),
  F_A(HIS_STATS_g, HIS_STATS_GLINES),
  F_B(HIS_STATS_GLINES, 0, 1, 0),
  F_A(HIS_STATS_i, HIS_STATS_ACCESS),
  F_B(HIS_STATS_ACCESS, 0, 1, 0),
  F_A(HIS_STATS_j, HIS_STATS_HISTOGRAM),
  F_B(HIS_STATS_HISTOGRAM, 0, 1, 0),
  F_A(HIS_STATS_J, HIS_STATS_JUPES),
  F_B(HIS_STATS_JUPES, 0, 1, 0),
  F_A(HIS_STATS_k, HIS_STATS_KLINES),
  F_B(HIS_STATS_KLINES, 0, 1, 0),
  F_A(HIS_STATS_l, HIS_STATS_LINKS),
  F_B(HIS_STATS_LINKS, 0, 1, 0),
  F_A(HIS_STATS_L, HIS_STATS_MODULES),
  F_B(HIS_STATS_MODULES, 0, 1, 0),
  F_A(HIS_STATS_m, HIS_STATS_COMMANDS),
  F_B(HIS_STATS_COMMANDS, 0, 1, 0),
  F_A(HIS_STATS_o, HIS_STATS_OPERATORS),
  F_B(HIS_STATS_OPERATORS, 0, 1, 0),
  F_A(HIS_STATS_p, HIS_STATS_PORTS),
  F_B(HIS_STATS_PORTS, 0, 1, 0),
  F_A(HIS_STATS_q, HIS_STATS_QUARANTINES),
  F_B(HIS_STATS_QUARANTINES, 0, 1, 0),
  F_A(HIS_STATS_r, HIS_STATS_USAGE),
  F_B(HIS_STATS_USAGE, 0, 1, 0),
  F_A(HIS_STATS_R, HIS_STATS_REDIRECTIONS),
  F_B(HIS_STATS_REDIRECTIONS, 0, 1, 0),
  F_A(HIS_STATS_s, HIS_STATS_SPOOFHOSTS),
  F_B(HIS_STATS_SPOOFHOSTS, 0, 1, 0),
  F_A(HIS_STATS_S, HIS_STATS_SHUNS),
  F_B(HIS_STATS_SHUNS, 0, 1, 0),
  F_A(HIS_STATS_t, HIS_STATS_LOCALS),
  F_B(HIS_STATS_LOCALS, 0, 1, 0),
  F_A(HIS_STATS_T, HIS_STATS_MOTDS),
  F_B(HIS_STATS_MOTDS, 0, 1, 0),
  F_A(HIS_STATS_u, HIS_STATS_UPTIME),
  F_B(HIS_STATS_UPTIME, 0, 0, 0),
  F_A(HIS_STATS_U, HIS_STATS_UWORLD),
  F_B(HIS_STATS_UWORLD, 0, 1, 0),
  F_A(HIS_STATS_v, HIS_STATS_VSERVERS),
  F_B(HIS_STATS_VSERVERS, 0, 1, 0),
  F_A(HIS_STATS_w, HIS_STATS_USERLOAD),
  F_B(HIS_STATS_USERLOAD, 0, 0, 0),
  F_A(HIS_STATS_W, HIS_STATS_WEBIRCS),
  F_B(HIS_STATS_WEBIRCS, 0, 1, 0),
  F_A(HIS_STATS_x, HIS_STATS_MEMUSAGE),
  F_B(HIS_STATS_MEMUSAGE, 0, 1, 0),
  F_A(HIS_STATS_X, HIS_STATS_DNSBLS),
  F_B(HIS_STATS_DNSBLS, 0, 1, 0),
  F_A(HIS_STATS_y, HIS_STATS_CLASSES),
  F_B(HIS_STATS_CLASSES, 0, 1, 0),
  F_A(HIS_STATS_z, HIS_STATS_MEMORY),
  F_B(HIS_STATS_MEMORY, 0, 1, 0),
  F_A(HIS_STATS_Z, HIS_STATS_ZLINES),
  F_B(HIS_STATS_ZLINES, 0, 1, 0),
  F_B(HIS_WHOIS_SERVERNAME, 0, 1, 0),
  F_B(HIS_WHOIS_IDLETIME, 0, 1, 0),
  F_B(HIS_WHOIS_LOCALCHAN, 0, 1, 0),
  F_B(HIS_WHO_SERVERNAME, 0, 1, 0),
  F_B(HIS_WHO_HOPCOUNT, 0, 1, 0),
  F_B(HIS_BANWHO, 0, 1, 0),
  F_B(HIS_KILLWHO, 0, 0, 0),
  F_B(HIS_REWRITE, 0, 1, 0),
  F_I(HIS_REMOTE, 0, 1, 0),
  F_B(HIS_NETSPLIT, 0, 1, 0),
  F_S(HIS_SERVERNAME, 0, "*.Nefarious", feature_notify_servername),
  F_S(HIS_SERVERINFO, 0, "evilnet development", feature_notify_serverinfo),
  F_S(HIS_URLSERVERS, 0, "http://sourceforge.net/projects/evilnet/", 0),

  /* Misc. random stuff */
  F_S(NETWORK, 0, "Nefarious", set_isupport_network),
  F_S(URL_CLIENTS, 0, "http://www.ircreviews.org/clients/", 0),

  /* Nefarious features */
  F_B(NEFARIOUS, FEAT_NODISP, 0, 0),
  F_S(OMPATH, FEAT_CASE | FEAT_MYOPER, "ircd.opermotd", 0),
  F_S(QPATH, FEAT_CASE | FEAT_MYOPER, "ircd.quotes", 0),
  F_S(EPATH, FEAT_CASE | FEAT_MYOPER, "ircd.rules", 0),
  F_S(TPATH, FEAT_CASE | FEAT_MYOPER, "ircd.tune", 0),
  F_I(HOST_HIDING_STYLE, 0, 1, 0),
  F_S(HOST_HIDING_PREFIX, 0, "AfterNET", 0),
  F_S(HOST_HIDING_KEY1, 0, "aoAr1HnR6gl3sJ7hVz4Zb7x4YwpW", 0),
  F_S(HOST_HIDING_KEY2, 0, "sdfjkLJKHlkjdkfjsdklfjlkjKLJ", 0),
  F_S(HOST_HIDING_KEY3, 0, "KJklJSDFLkjLKDFJSLKjlKJFlkjS", 0),
  F_B(OPERHOST_HIDING, 0, 1, 0),
  F_S(HIDDEN_OPERHOST, FEAT_CASE, "Staff.Nefarious", 0),
  F_B(TOPIC_BURST, 0, 1, 0),
  F_B(REMOTE_OPER, 0, 1, 0),
  F_B(REMOTE_MOTD, 0, 0, 0),
  F_I(BOT_CLASS, 0, 0, 0),
  F_B(LOCAL_CHANNELS, 0, 1, set_isupport_chantypes),
  F_B(OPER_LOCAL_CHANNELS, 0, 1, 0),
  F_B(OPER_XTRAOP, 0, 0, 0),
  F_I(XTRAOP_CLASS, 0, 0, 0),
  F_B(OPER_HIDECHANS, 0, 0, 0),
  F_B(OPER_HIDEIDLE, 0, 0, 0),
  F_B(CHECK, 0, 1, 0),
  F_B(CHECK_EXTENDED, 0, 1, 0),
  F_B(OPER_SINGLELETTERNICK, 0, 0, 0),
  F_B(SETHOST, 0, 1, 0),
  F_B(SETHOST_FREEFORM, 0, 0, 0),
  F_B(SETHOST_USER, 0, 1, 0),
  F_B(SETHOST_AUTO, 0, 1, 0),
  F_B(FAKEHOST, 0, 1, 0),
  F_S(DEFAULT_FAKEHOST, FEAT_NULL | FEAT_CASE, 0, 0),
  F_B(HIS_GLINE, 0, 1, 0),
  F_B(HIS_USERGLINE, 0, 1, 0),
  F_B(HIS_USERIP, 0, 0, 0),
  F_B(AUTOJOIN_USER, 0, 0, 0),
  F_S(AUTOJOIN_USER_CHANNEL, 0, "#evilnet", 0),
  F_B(AUTOJOIN_USER_NOTICE, 0, 1, 0),
  F_S(AUTOJOIN_USER_NOTICE_VALUE, 0, "*** Notice -- You are now being autojoined into #evilnet", 0),
  F_B(AUTOJOIN_OPER, 0, 0, 0),
  F_S(AUTOJOIN_OPER_CHANNEL, 0, "#opers", 0),
  F_B(AUTOJOIN_OPER_NOTICE, 0, 1, 0),
  F_S(AUTOJOIN_OPER_NOTICE_VALUE, 0, "*** Notice -- You are now being autojoined into oper channel #opers", 0),
  F_B(AUTOJOIN_ADMIN, 0, 0, 0),
  F_S(AUTOJOIN_ADMIN_CHANNEL, 0, "#admin", 0),
  F_B(AUTOJOIN_ADMIN_NOTICE, 0, 1, 0),
  F_S(AUTOJOIN_ADMIN_NOTICE_VALUE, 0, "*** Notice -- You are now being autojoined into admin channel #admin", 0),
  F_B(QUOTES, 0, 0, 0),
  F_B(POLICY_NOTICE, 0, 1, 0),
  F_S(BADCHAN_REASON, 0, "This channel has been banned", 0),
  F_B(RULES, 0, 0, 0),
  F_B(OPERMOTD, 0, 0, 0),
  F_S(GEO_LOCATION, FEAT_NULL, 0, 0),
  F_S(DEFAULT_UMODE, 0, "+", 0),
  F_B(HOST_IN_TOPIC, 0, 1, 0),
  F_B(TIME_IN_TIMEOUT, 0, 0, 0),
  F_B(HALFOPS, FEAT_READ, 0, set_isupport_halfops),
  F_B(EXCEPTS, FEAT_READ, 0, set_isupport_excepts),
  F_B(BREAK_P10, FEAT_READ, 0, 0),
  F_I(AVEXCEPTLEN, 0, 40, 0),
  F_I(MAXEXCEPTS, 0, 45, 0),
  F_B(HIS_EXCEPTWHO, 0, 1, 0),
  F_B(TARGET_LIMITING, 0, 1, 0),
  F_S(BADUSER_URL, 0, "http://www.mirc.co.uk/help/servererrors.html", 0),
  F_B(STATS_C_IPS, 0, 0, 0),
  F_B(HIS_IRCOPS, 0, 1, 0),
  F_B(HIS_IRCOPS_SERVERS, 0, 1, 0),
  F_B(HIS_MAP_SCRAMBLED, 0, 1, 0),
  F_B(HIS_LINKS_SCRAMBLED, 0, 1, 0),
  F_I(HIS_SCRAMBLED_CACHE_TIME, 0, 604800, 0),
  F_B(FLEXABLEKEYS, 0, 0, 0),
  F_B(NOTHROTTLE, 0, 0, 0),
  F_B(CREATE_CHAN_OPER_ONLY, 0, 0, 0),
  F_I(MAX_CHECK_OUTPUT, 0, 1000, 0),
  F_S(RESTARTPASS, FEAT_NULL | FEAT_CASE | FEAT_NODISP | FEAT_READ, 0, 0),
  F_S(DIEPASS, FEAT_NULL | FEAT_CASE | FEAT_NODISP | FEAT_READ, 0, 0),
  F_B(DNSBL_CHECKS, 0, 0, 0),
  F_I(DNSBL_EXEMPT_CLASS, 0, 0, 0),
  F_B(ANNOUNCE_INVITES, 0, 0, 0),
  F_B(OPERFLAGS, 0, 0, 0),
  F_S(WHOIS_OPER, 0, "is an IRC Operator", 0),
  F_S(WHOIS_ADMIN, 0, "is an IRC Administrator", 0),
  F_S(WHOIS_SERVICE, 0, "is a Network Service", 0),
  F_B(AUTOCHANMODES, 0, 0, 0),
  F_S(AUTOCHANMODES_LIST, FEAT_CASE | FEAT_NULL, 0, 0),
  F_B(LOGIN_ON_CONNECT, 0, 0, 0),
  F_B(EXTENDED_ACCOUNTS, 0, 1, 0),
  F_S(LOC_DEFAULT_SERVICE, 0, "AuthServ", 0),
  F_B(DNSBL_LOC_EXEMPT, 0, 0, 0),
  F_S(DNSBL_LOC_EXEMPT_N_ONE, 0, "If you have an account with Nefarious services then you can bypass the DNSBL ban by logging in like this (where Account is your account name and Password is your password):", 0),
  F_S(DNSBL_LOC_EXEMPT_N_TWO, 0, "Type \002/QUOTE PASS AuthServ Account :Password\002 to connect", 0),
  F_B(DNSBL_WALLOPS_ONLY, 0, 0, 0),
  F_B(DNSBL_MARK_FAKEHOST, 0, 1, 0),
  F_B(OPER_WHOIS_SECRET, 0, 1, 0),
  F_B(AUTOINVISIBLE, 0, 0, 0),
  F_B(SWHOIS, 0, 0, 0),
  F_B(CHMODE_e_CHMODEEXCEPTION, 0, 0, 0),
  F_B(CHMODE_a, 0, 1, 0),
  F_B(CHMODE_c, 0, 1, 0),
  F_B(CHMODE_z, 0, 1, 0),
  F_B(CHMODE_C, 0, 1, 0),
  F_B(CHMODE_L, 0, 1, 0),
  F_B(CHMODE_M, 0, 1, 0),
  F_B(CHMODE_N, 0, 1, 0),
  F_B(CHMODE_O, 0, 1, 0),
  F_B(CHMODE_Q, 0, 1, 0),
  F_B(CHMODE_S, 0, 1, 0),
  F_B(CHMODE_T, 0, 1, 0),
  F_B(CHMODE_Z, 0, 1, 0),
  F_B(LUSERS_AUTHED, 0, 0, 0),
  F_B(OPER_WHOIS_PARANOIA, 0, 0, 0),
  F_I(SHUNMAXUSERCOUNT, 0, 20, 0),
  F_B(OPER_SHUN, 0, 1, 0),
  F_B(OPER_LSHUN, 0, 1, 0),
  F_B(OPER_WIDE_SHUN, 0, 1, 0),
  F_B(HIS_SHUN, 0, 1, 0),
  F_B(HIS_USERSHUN, 0, 1, 0),
  F_B(HIS_SHUN_REASON, 0, 1, 0),
  F_S(ERR_OPERONLYCHAN, 0, "Cannot join channel (+O)", 0),
  F_I(EXEMPT_EXPIRE, 0, 172800, 0),
  F_B(HIS_HIDEWHO, 0, 1, 0),
  F_B(STRICTUSERNAME, 0, 1, 0),
  F_B(SET_ACTIVE_ON_CREATE, 0, 1, 0),
  F_I(DEF_ALIST_LIMIT, 0, 30, 0),
  F_I(ALIST_SEND_FREQ, 0, 300, 0),
  F_I(ALIST_SEND_DIFF, 0, 600, 0),
  F_I(ZLINEMAXUSERCOUNT, 0, 20, 0),
  F_B(HIS_ZLINE, 0, 1, 0),
  F_B(HIS_USERZLINE, 0, 1, 0),
  F_B(HIS_ZLINE_REASON, 0, 1, 0),
  F_I(NICK_DELAY, 0, 30, 0),
  F_I(HELP_PACE, 0, 5, 0),
  F_B(OPER_LIST_CHAN, 0, 1, 0),
  F_B(LASTMOD_TWEAK, 0, 0, 0),

  F_B(CTCP_VERSIONING, 0, 0, 0), /* added by Vadtec 02/25/2008 */
  F_B(CTCP_VERSIONING_KILL, 0, 0, 0), /* added by Vadtec 02/27/2008 */
  F_B(CTCP_VERSIONING_CHAN, 0, 0, 0), /* added by Vadtec 02/27/2008 */
  F_S(CTCP_VERSIONING_CHANNAME, 0, "#opers", 0), /* added by Vadtec 02/27/2008 */
  F_B(CTCP_VERSIONING_USEMSG, 0, 0, 0), /* added by Vadtec 02/28/2008 */
  F_S(CTCP_VERSIONING_NOTICE, 0, "*** Checking your client version", 0),

  F_S(FILTER_DEFAULT_CHANNAME, 0, "#opers", 0),
  F_B(FILTER_ALERT_USEMSG, 0, 0, 0),
  F_I(FILTER_DEFAULT_LENGTH, 0, 3600, 0),

  /* Allows configuring sending host in LOC */
  F_B(LOC_SENDHOST, 0, 0, 0),

  /* Really special features (tm) */
  F_B(NETWORK_REHASH, 0, 0, 0),
  F_B(NETWORK_RESTART, 0, 0, 0),
  F_B(NETWORK_DIE, 0, 0, 0),

#undef F_S
#undef F_B
#undef F_I
#undef F_N
  { FEAT_LAST_F, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }
};

/* Given a feature's identifier, look up the feature descriptor */
static struct FeatureDesc *
feature_desc(struct Client* from, const char *feature)
{
  int i;

  assert(0 != feature);

  for (i = 0; features[i].type; i++) /* find appropriate descriptor */
    if (!strcmp(feature, features[i].type)) {
      if (feat_type(&features[i]) == FEAT_ALIAS) {
        Debug((DEBUG_NOTICE, "Deprecated feature \"%s\" referenced; replace "
               "with %s", feature, features[features[i].def_int].type));
        if (from) /* report a warning */
          send_reply(from, SND_EXPLICIT | ERR_NOFEATURE,
                     "%s :Feature deprecated, use %s", feature,
                     features[features[i].def_int].type);
        else
          log_write(LS_CONFIG, L_WARNING, 0, "Feature \"%s\" deprecated, "
                    "use \"%s\"", feature, features[features[i].def_int].type);

        return &features[features[i].def_int];
      } else if (feat_type(&features[i]) == FEAT_DEP) {
        Debug((DEBUG_NOTICE, "Deprecated feature \"%s\" referenced", feature));
        if (from) /* report a warning */
          send_reply(from, SND_EXPLICIT | ERR_NOFEATURE,
                     "%s :Feature deprecated", feature);
        else
          log_write(LS_CONFIG, L_WARNING, 0, "Feature \"%s\" deprecated",
                    feature);
      }
      return &features[i];
    }

  Debug((DEBUG_ERROR, "Unknown feature \"%s\"", feature));
  if (from) /* report an error */
    send_reply(from, ERR_NOFEATURE, feature);
  else
    log_write(LS_CONFIG, L_ERROR, 0, "Unknown feature \"%s\"", feature);

  return 0; /* not found */
}

/* Given a feature vector string, set the value of a feature */
int
feature_set(struct Client* from, const char* const* fields, int count)
{
  int i, change = 0, tmp;
  const char *t_str;
  struct FeatureDesc *feat;

  if (from && !HasPriv(from, PRIV_SET) && !IsServer(from))
    return send_reply(from, ERR_NOPRIVILEGES);

  if (count < 1) {
    if (from) /* report an error in the number of arguments */
      need_more_params(from, "SET");
    else
      log_write(LS_CONFIG, L_ERROR, 0, "Not enough fields in F line");
  } else if ((feat = feature_desc(from, fields[0]))) { /* find feature */
    if (from && feat->flags & FEAT_READ)
      return send_reply(from, ERR_NOFEATURE, fields[0]);

    switch (feat_type(feat)) {
    case FEAT_NONE:
      if (feat->set && (i = (*feat->set)(from, fields + 1, count - 1))) {
	change++; /* feature handler wants a change recorded */

	if (i > 0) /* call the set callback and do marking */
	  feat->flags |= FEAT_MARK;
	else /* i < 0 */
	  feat->flags &= ~FEAT_MARK;
	break;
      }

    case FEAT_INT: /* an integer value */
    case FEAT_UINT:
      tmp = feat->v_int; /* detect changes... */

      if (count < 2) { /* reset value */
	feat->v_int = feat->def_int;
	feat->flags &= ~FEAT_MARK;
      } else { /* ok, figure out the value and whether to mark it */
	feat->v_int = strtoul(fields[1], 0, 0);
	if (feat->v_int == feat->def_int)
	  feat->flags &= ~FEAT_MARK;
	else
	  feat->flags |= FEAT_MARK;
      }

      if (feat->v_int != tmp) /* check for change */
	change++;
      break;

    case FEAT_BOOL: /* it's a boolean value--true or false */
      tmp = feat->v_int; /* detect changes... */

      if (count < 2) { /* reset value */
	feat->v_int = feat->def_int;
	feat->flags &= ~FEAT_MARK;
      } else { /* figure out the value and whether to mark it */
	if (!ircd_strncmp(fields[1], "TRUE", strlen(fields[1])) ||
	    !ircd_strncmp(fields[1], "YES", strlen(fields[1])) ||
	    (strlen(fields[1]) >= 2 &&
	     !ircd_strncmp(fields[1], "ON", strlen(fields[1]))))
	  feat->v_int = 1;
	else if (!ircd_strncmp(fields[1], "FALSE", strlen(fields[1])) ||
		 !ircd_strncmp(fields[1], "NO", strlen(fields[1])) ||
		 (strlen(fields[1]) >= 2 &&
		  !ircd_strncmp(fields[1], "OFF", strlen(fields[1]))))
	  feat->v_int = 0;
	else if (from) /* report an error... */
	  return send_reply(from, ERR_BADFEATVALUE, fields[1], feat->type);
	else {
	  log_write(LS_CONFIG, L_ERROR, 0, "Bad value \"%s\" for feature %s",
		    fields[1], feat->type);
	  return 0;
	}

	if (feat->v_int == feat->def_int) /* figure out whether to mark it */
	  feat->flags &= ~FEAT_MARK;
	else
	  feat->flags |= FEAT_MARK;
      }

      if (feat->v_int != tmp) /* check for change */
	change++;
      break;

    case FEAT_STR: /* it's a string value */
      if (count < 2)
	t_str = feat->def_str; /* changing to default */
      else
	t_str = *fields[1] ? fields[1] : 0;

      if (!t_str && !(feat->flags & FEAT_NULL)) { /* NULL value permitted? */
	if (from)
	  return send_reply(from, ERR_BADFEATVALUE, "NULL", feat->type);
	else {
	  log_write(LS_CONFIG, L_ERROR, 0, "Bad value \"NULL\" for feature %s",
		    feat->type);
	  return 0;
	}
      }

      if (t_str == feat->def_str ||
	  (t_str && feat->def_str &&
	   !(feat->flags & FEAT_CASE ? strcmp(t_str, feat->def_str) :
	     ircd_strcmp(t_str, feat->def_str)))) { /* resetting to default */
	if (feat->v_str != feat->def_str) {
	  change++; /* change from previous value */

	  if (feat->v_str)
	    MyFree(feat->v_str); /* free old value */
	}

	feat->v_str = feat->def_str; /* very special... */

	feat->flags &= ~FEAT_MARK;
      } else if (!t_str) {
	if (feat->v_str) {
	  change++; /* change from previous value */

	  if (feat->v_str != feat->def_str)
	    MyFree(feat->v_str); /* free old value */
	}

	feat->v_str = 0; /* set it to NULL */

	feat->flags |= FEAT_MARK;
      } else if (!feat->v_str ||
		 (feat->flags & FEAT_CASE ? strcmp(t_str, feat->v_str) :
		  ircd_strcmp(t_str, feat->v_str))) { /* new value */
	change++; /* change from previous value */

	if (feat->v_str && feat->v_str != feat->def_str)
	  MyFree(feat->v_str); /* free old value */
	DupString(feat->v_str, t_str); /* store new value */

	feat->flags |= FEAT_MARK;
      } else /* they match, but don't match the default */
	feat->flags |= FEAT_MARK;
      break;
    }

    if (change && feat->notify) /* call change notify function */
      (*feat->notify)();

    if (change && from) {
      
      if(!IsServer(from) && !IsMe(from))
        send_reply(from, SND_EXPLICIT | RPL_FEATURE, ":Value of %s changed",
                   feat->type);

      switch (feat_type(feat)) {
        case FEAT_NONE:
          sendto_opmask_butone(0, SNO_OLDSNO, "%C changed %s",
                               from, feat->type);
          break;
        case FEAT_INT:
          sendto_opmask_butone(0, SNO_OLDSNO, "%C changed %s to %d",
                               from, feat->type, feat->v_int);
          break;
        case FEAT_BOOL:
          sendto_opmask_butone(0, SNO_OLDSNO, "%C changed %s to %s", from,
                               feat->type, (feat->v_int ? "TRUE" : "FALSE"));
          break;
        case FEAT_STR:
          if(feat->v_str)
            sendto_opmask_butone(0, SNO_OLDSNO, "%C changed %s to: %s",
                                 from, feat->type, feat->v_str);
          else
            sendto_opmask_butone(0, SNO_OLDSNO, "%C unset %s",
                                 from, feat->type);
          break;
      } /* switch */
    } /* if (change) */
  }

  return 0;
}

/* reset a feature to its default values */
int
feature_reset(struct Client* from, const char* const* fields, int count)
{
  int i, change = 0;
  struct FeatureDesc *feat;

  assert(0 != from);

  if (!HasPriv(from, PRIV_SET))
    return send_reply(from, ERR_NOPRIVILEGES);

  if (count < 1) /* check arguments */
    need_more_params(from, "RESET");
  else if ((feat = feature_desc(from, fields[0]))) { /* get descriptor */
    if (from && feat->flags & FEAT_READ)
      return send_reply(from, ERR_NOFEATURE, fields[0]);

    switch (feat_type(feat)) {
    case FEAT_NONE: /* None... */
      if (feat->reset && (i = (*feat->reset)(from, fields + 1, count - 1))) {
	change++; /* feature handler wants a change recorded */

	if (i > 0) /* call reset callback and parse mark return */
	  feat->flags |= FEAT_MARK;
	else /* i < 0 */
	  feat->flags &= ~FEAT_MARK;
      }
      break;

    case FEAT_INT:  /* Integer... */
    case FEAT_UINT:
    case FEAT_BOOL: /* Boolean... */
      if (feat->v_int != feat->def_int)
	change++; /* change will be made */

      feat->v_int = feat->def_int; /* set the default */
      feat->flags &= ~FEAT_MARK; /* unmark it */
      break;

    case FEAT_STR: /* string! */
      if (feat->v_str != feat->def_str) {
	change++; /* change has been made */
	if (feat->v_str)
	  MyFree(feat->v_str); /* free old value */
      }

      feat->v_str = feat->def_str; /* set it to default */
      feat->flags &= ~FEAT_MARK; /* unmark it */
      break;
    }

    if (change && feat->notify) /* call change notify function */
      (*feat->notify)();

    if(from && !IsServer(from) && !IsMe(from))
      send_reply(from, SND_EXPLICIT | RPL_FEATURE, ":Value of %s reset to "
                 "default", feat->type);
      
    if(change)
      sendto_opmask_butone(0, SNO_OLDSNO, "%C reset %s to its default",
                           from, feat->type);
  }

  return 0;
}

/* Gets the value of a specific feature and reports it to the user */
int
feature_get(struct Client* from, const char* const* fields, int count)
{
  struct FeatureDesc *feat;

  assert(0 != from);

  if (count < 1) /* check parameters */
    need_more_params(from, "GET");
  else if ((feat = feature_desc(from, fields[0]))) {
    if ((feat->flags & FEAT_NODISP) ||
	(feat->flags & FEAT_MYOPER && !MyOper(from)) ||
	(feat->flags & FEAT_OPER && !IsAnOper(from))) /* check privs */
      return send_reply(from, ERR_NOPRIVILEGES);

    switch (feat_type(feat)) {
    case FEAT_NONE: /* none, call the callback... */
      if (feat->get) /* if there's a callback, use it */
	(*feat->get)(from, fields + 1, count - 1);
      break;

    case FEAT_INT: /* integer, report integer value */
      send_reply(from, SND_EXPLICIT | RPL_FEATURE,
		 ":Integer value of %s: %d", feat->type, feat->v_int);
      break;

    case FEAT_UINT: /* unsigned integer, report its value */
      send_reply(from, SND_EXPLICIT | RPL_FEATURE,
                 ":Unsigned value of %s: %u", feat->type, feat->v_int);
      break;

    case FEAT_BOOL: /* boolean, report boolean value */
      send_reply(from, SND_EXPLICIT | RPL_FEATURE,
		 ":Boolean value of %s: %s", feat->type,
		 feat->v_int ? "TRUE" : "FALSE");
      break;

    case FEAT_STR: /* string, report string value */
      if (feat->v_str) /* deal with null case */
	send_reply(from, SND_EXPLICIT | RPL_FEATURE,
		   ":String value of %s: %s", feat->type, feat->v_str);
      else
	send_reply(from, SND_EXPLICIT | RPL_FEATURE,
		   ":String value for %s not set", feat->type);
      break;
    }
  }

  return 0;
}

/* called before reading the .conf to clear all marks */
void
feature_unmark(void)
{
  int i;

  for (i = 0; features[i].type; i++) {
    features[i].flags &= ~FEAT_MARK; /* clear the marks... */
    if (features[i].unmark) /* call the unmark callback if necessary */
      (*features[i].unmark)();
  }
}

/* Called after reading the .conf to reset unmodified values to defaults */
void
feature_mark(void)
{
  int i, change;

  for (i = 0; features[i].type; i++) {
    change = 0;

    switch (feat_type(&features[i])) {
    case FEAT_NONE:
      if (features[i].mark &&
	  (*features[i].mark)(features[i].flags & FEAT_MARK ? 1 : 0))
	change++; /* feature handler wants a change recorded */
      break;

    case FEAT_INT:  /* Integers or Booleans... */
    case FEAT_UINT:
    case FEAT_BOOL:
      if (!(features[i].flags & FEAT_MARK)) { /* not changed? */
	if (features[i].v_int != features[i].def_int)
	  change++; /* we're making a change */
	features[i].v_int = features[i].def_int;
      }
      break;

    case FEAT_STR: /* strings... */
      if (!(features[i].flags & FEAT_MARK)) { /* not changed? */
	if (features[i].v_str != features[i].def_str) {
	  change++; /* we're making a change */
	  if (features[i].v_str)
	    MyFree(features[i].v_str); /* free old value */
	}
	features[i].v_str = features[i].def_str;
      }
      break;
    }

    if (change && features[i].notify)
      (*features[i].notify)(); /* call change notify function */
  }
}

/* used to initialize the features subsystem */
void
feature_init(void)
{
  int i;

  for (i = 0; features[i].type; i++) {
    struct FeatureDesc *feat = &features[i];

    switch (feat_type(&features[i])) {
    case FEAT_NONE: /* you're on your own */
      break;

    case FEAT_INT:  /* Integers or Booleans... */
    case FEAT_UINT:
    case FEAT_BOOL:
      feat->v_int = feat->def_int;
      break;

    case FEAT_STR:  /* Strings */
      feat->v_str = feat->def_str;
      assert(feat->def_str || (feat->flags & FEAT_NULL));
      break;
    }

    if (feat->notify)
     (*feat->notify)();
 } 	  

  cli_magic(&his) = CLIENT_MAGIC;
  cli_status(&his) = STAT_SERVER;
}

/* report all F-lines */
void
feature_report(struct Client* to, const struct StatDesc* sd, char* param)
{
  int i;

  for (i = 0; features[i].type; i++) {
    if ((features[i].flags & FEAT_NODISP) ||
	(features[i].flags & FEAT_MYOPER && !MyOper(to)) ||
	(features[i].flags & FEAT_OPER && !IsAnOper(to)))
      continue; /* skip this one */

    switch (feat_type(&features[i])) {
    case FEAT_NONE:
      if (features[i].report) /* let the callback handle this */
	(*features[i].report)(to, features[i].flags & FEAT_MARK ? 1 : 0);
      break;


    case FEAT_INT: /* Report an F-line with integer values */
      if (features[i].flags & FEAT_MARK) /* it's been changed */
	send_reply(to, SND_EXPLICIT | RPL_STATSFLINE, "F %s %d",
		   features[i].type, features[i].v_int);
      break;

    case FEAT_BOOL: /* Report an F-line with boolean values */
      if (features[i].flags & FEAT_MARK) /* it's been changed */
	send_reply(to, SND_EXPLICIT | RPL_STATSFLINE, "F %s %s",
		   features[i].type, features[i].v_int ? "TRUE" : "FALSE");
      break;

    case FEAT_STR: /* Report an F-line with string values */
      if (features[i].flags & FEAT_MARK) { /* it's been changed */
	if (features[i].v_str)
	  send_reply(to, SND_EXPLICIT | RPL_STATSFLINE, "F %s %s",
		     features[i].type, features[i].v_str);
	else /* Actually, F:<type> would reset it; you want F:<type>: */
	  send_reply(to, SND_EXPLICIT | RPL_STATSFLINE, "F %s",
		     features[i].type);
      }
      break;
    }
  }
}

/* return a feature's integer value */
int
feature_int(enum Feature feat)
{
  assert(features[feat].feat == feat);
  assert(feat_type(&features[feat]) == FEAT_INT);

  return features[feat].v_int;
}

/** Return a feature's unsigned integer value.
 * @param[in] feat &Feature identifier.
 * @return Unsigned integer value of feature.
 */
unsigned int
feature_uint(enum Feature feat)
{
  assert(features[feat].feat == feat);
  assert(feat_type(&features[feat]) == FEAT_UINT);

  return features[feat].v_int;
}

/* return a feature's boolean value */
int
feature_bool(enum Feature feat)
{
  assert(features[feat].feat == feat);
  assert(feat_type(&features[feat]) == FEAT_BOOL);

  return features[feat].v_int;
}

/* return a feature's string value */
const char *
feature_str(enum Feature feat)
{
  assert(features[feat].feat == feat);
  assert(feat_type(&features[feat]) == FEAT_STR);

  return features[feat].v_str;
}
