/*
 * Copyright (C) 1996,1997 Michael R. Elkins <me@cs.hmc.edu>
 * Copyright (c) 1998,1999 Thomas Roessler <roessler@guug.de>
 * 
 *     This program is free software; you can redistribute it
 *     and/or modify it under the terms of the GNU General Public
 *     License as published by the Free Software Foundation; either
 *     version 2 of the License, or (at your option) any later
 *     version.
 * 
 *     This program is distributed in the hope that it will be
 *     useful, but WITHOUT ANY WARRANTY; without even the implied
 *     warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *     PURPOSE.  See the GNU General Public License for more
 *     details.
 * 
 *     You should have received a copy of the GNU General Public
 *     License along with this program; if not, write to the Free
 *     Software Foundation, Inc., 59 Temple Place - Suite 330,
 *     Boston, MA  02111, USA.
 */

#include "mutt.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mime.h"
#include "pgp.h"
#include "pager.h"
#include "sort.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <locale.h>

#ifdef HAVE_PGP

struct pgp_cache
{
  char *what;
  char *dflt;
  struct pgp_cache *next;
};

static struct pgp_cache *id_defaults = NULL;

static char trust_flags[] = "?- +";

static char *pgp_key_abilities (int flags)
{
  static char buff[3];

  if (!(flags & KEYFLAG_CANENCRYPT))
    buff[0] = '-';
  else if (flags & KEYFLAG_PREFER_SIGNING)
    buff[0] = '.';
  else
    buff[0] = 'e';

  if (!(flags & KEYFLAG_CANSIGN))
    buff[1] = '-';
  else if (flags & KEYFLAG_PREFER_ENCRYPTION)
    buff[1] = '.';
  else
    buff[1] = 's';

  buff[2] = '\0';

  return buff;
}

static char pgp_flags (int flags)
{
  if (flags & KEYFLAG_REVOKED)
    return 'R';
  else if (flags & KEYFLAG_EXPIRED)
    return 'X';
  else if (flags & KEYFLAG_DISABLED)
    return 'd';
  else if (flags & KEYFLAG_CRITICAL)
    return 'c';
  else 
    return ' ';
}

static pgp_key_t *pgp_principal_key (pgp_key_t *key)
{
  if (key->flags & KEYFLAG_SUBKEY && key->parent)
    return key->parent;
  else
    return key;
}

/*
 * Format an entry on the PGP key selection menu.
 * 
 * %n	number
 * %k	key id		%K 	key id of the principal key
 * %u	user id
 * %a	algorithm	%A      algorithm of the princ. key
 * %l	length		%L	length of the princ. key
 * %f	flags		%F 	flags of the princ. key
 * %c	capabilities	%C	capabilities of the princ. key
 * %t	trust/validity of the key-uid association
 * %[...] date of key using strftime(3)
 */

typedef struct pgp_entry
{
  size_t num;
  pgp_uid_t *uid;
} pgp_entry_t;

static const char *pgp_entry_fmt (char *dest,
				  size_t destlen,
				  char op,
				  const char *src,
				  const char *prefix,
				  const char *ifstring,
				  const char *elsestring,
				  unsigned long data,
				  format_flag flags)
{
  char fmt[16];
  pgp_entry_t *entry;
  pgp_uid_t *uid;
  pgp_key_t *key, *pkey;
  int kflags;
  int optional = (flags & M_FORMAT_OPTIONAL);

  entry = (pgp_entry_t *) data;
  uid   = entry->uid;
  key   = uid->parent;
  pkey  = pgp_principal_key (key);

  if (isupper (op))
  {
    key = pkey;
    kflags = pkey->flags;
  }
  else
    /* a subkey inherits the principal key's usage restrictions. */
    kflags = key->flags | (pkey->flags & KEYFLAG_RESTRICTIONS);
  
  switch (tolower (op))
  {
    case '[':

      {
	const char *cp;
	char buf2[SHORT_STRING], *p;
	int do_locales;
	struct tm *tm;
	size_t len;

	p = dest;

	cp = src;
	if (*cp == '!')
	{
	  do_locales = 0;
	  cp++;
	}
	else
	  do_locales = 1;

	len = destlen - 1;
	while (len > 0 && *cp != ']')
	{
	  if (*cp == '%')
	  {
	    cp++;
	    if (len >= 2)
	    {
	      *p++ = '%';
	      *p++ = *cp;
	      len -= 2;
	    }
	    else
	      break; /* not enough space */
	    cp++;
	  }
	  else
	  {
	    *p++ = *cp++;
	    len--;
	  }
	}
	*p = 0;

	if (do_locales && Locale)
	  setlocale (LC_TIME, Locale);

	tm = localtime (&key->gen_time);

	strftime (buf2, sizeof (buf2), dest, tm);

	if (do_locales)
	  setlocale (LC_TIME, "C");

	snprintf (fmt, sizeof (fmt), "%%%ss", prefix);
	snprintf (dest, destlen, fmt, buf2);
	if (len > 0)
	  src = cp + 1;
      }
      break;
    case 'n':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
	snprintf (dest, destlen, fmt, entry->num);
      }
      break;
    case 'k':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%ss", prefix);
	snprintf (dest, destlen, fmt, _pgp_keyid (key));
      }
      break;
    case 'u':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%ss", prefix);
	snprintf (dest, destlen, fmt, uid->addr);
      }
      break;
    case 'a':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%ss", prefix);
	snprintf (dest, destlen, fmt, key->algorithm);
      }
      break;
    case 'l':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%sd", prefix);
	snprintf (dest, destlen, fmt, key->keylen);
      }
      break;
    case 'f':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%sc", prefix);
	snprintf (dest, destlen, fmt, pgp_flags (kflags));
      }
      else if (!(kflags & (KEYFLAG_RESTRICTIONS)))
        optional = 0;
      break;
    case 'c':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%ss", prefix);
	snprintf (dest, destlen, fmt, pgp_key_abilities (kflags));
      }
      else if (!(kflags & (KEYFLAG_ABILITIES)))
        optional = 0;
      break;
    case 't':
      if (!optional)
      {
	snprintf (fmt, sizeof (fmt), "%%%sc", prefix);
	snprintf (dest, destlen, fmt, trust_flags[uid->trust & 0x03]);
      }
      else if (!(uid->trust & 0x03))
        /* undefined trust */
        optional = 0;
      break;
    default:
      *dest = '\0';
  }

  if (optional)
    mutt_FormatString (dest, destlen, ifstring, mutt_attach_fmt, data, 0);
  else if (flags & M_FORMAT_OPTIONAL)
    mutt_FormatString (dest, destlen, elsestring, mutt_attach_fmt, data, 0);
  return (src);
}
      
static void pgp_entry (char *s, size_t l, MUTTMENU * menu, int num)
{
  pgp_uid_t **KeyTable = (pgp_uid_t **) menu->data;
  pgp_entry_t entry;
  
  entry.uid = KeyTable[num];
  entry.num = num + 1;

  mutt_FormatString (s, l, NONULL (PgpEntryFormat), pgp_entry_fmt, 
		     (unsigned long) &entry, M_FORMAT_ARROWCURSOR);
}

static int _pgp_compare_address (const void *a, const void *b)
{
  int r;

  pgp_uid_t **s = (pgp_uid_t **) a;
  pgp_uid_t **t = (pgp_uid_t **) b;

  if ((r = mutt_strcasecmp ((*s)->addr, (*t)->addr)))
    return r > 0;
  else
    return (mutt_strcasecmp (pgp_keyid ((*s)->parent),
			     pgp_keyid ((*t)->parent)) > 0);
}

static int pgp_compare_address (const void *a, const void *b)
{
  return ((PgpSortKeys & SORT_REVERSE) ? !_pgp_compare_address (a, b)
				       : _pgp_compare_address (a, b));
}



static int _pgp_compare_keyid (const void *a, const void *b)
{
  int r;

  pgp_uid_t **s = (pgp_uid_t **) a;
  pgp_uid_t **t = (pgp_uid_t **) b;

  if ((r = mutt_strcasecmp (pgp_keyid ((*s)->parent), 
			    pgp_keyid ((*t)->parent))))
    return r > 0;
  else
    return (mutt_strcasecmp ((*s)->addr, (*t)->addr)) > 0;
}

static int pgp_compare_keyid (const void *a, const void *b)
{
  return ((PgpSortKeys & SORT_REVERSE) ? !_pgp_compare_keyid (a, b)
				       : _pgp_compare_keyid (a, b));
}

static int _pgp_compare_date (const void *a, const void *b)
{
  int r;
  pgp_uid_t **s = (pgp_uid_t **) a;
  pgp_uid_t **t = (pgp_uid_t **) b;

  if ((r = ((*s)->parent->gen_time - (*t)->parent->gen_time)))
    return r > 0;
  return (mutt_strcasecmp ((*s)->addr, (*t)->addr)) > 0;
}

static int pgp_compare_date (const void *a, const void *b)
{
  return ((PgpSortKeys & SORT_REVERSE) ? !_pgp_compare_date (a, b)
				       : _pgp_compare_date (a, b));
}

static int _pgp_compare_trust (const void *a, const void *b)
{
  int r;

  pgp_uid_t **s = (pgp_uid_t **) a;
  pgp_uid_t **t = (pgp_uid_t **) b;

  if ((r = (((*s)->parent->flags & (KEYFLAG_RESTRICTIONS))
	    - ((*t)->parent->flags & (KEYFLAG_RESTRICTIONS)))))
    return r > 0;
  if ((r = ((*s)->trust - (*t)->trust)))
    return r < 0;
  if ((r = ((*s)->parent->keylen - (*t)->parent->keylen)))
    return r < 0;
  if ((r = ((*s)->parent->gen_time - (*t)->parent->gen_time)))
    return r < 0;
  if ((r = mutt_strcasecmp ((*s)->addr, (*t)->addr)))
    return r > 0;
  return (mutt_strcasecmp (pgp_keyid ((*s)->parent), 
			   pgp_keyid ((*t)->parent))) > 0;
}

static int pgp_compare_trust (const void *a, const void *b)
{
  return ((PgpSortKeys & SORT_REVERSE) ? !_pgp_compare_trust (a, b)
				       : _pgp_compare_trust (a, b));
}

static pgp_key_t *pgp_select_key (pgp_key_t *keys,
				  ADDRESS * p, const char *s)
{
  int keymax;
  pgp_uid_t **KeyTable;
  MUTTMENU *menu;
  int i, done = 0;
  char helpstr[SHORT_STRING], buf[LONG_STRING], tmpbuf[STRING];
  char cmd[LONG_STRING], tempfile[_POSIX_PATH_MAX];
  FILE *fp, *devnull;
  pid_t thepid;
  pgp_key_t *kp;
  pgp_uid_t *a;
  int (*f) (const void *, const void *);

  for (i = 0, kp = keys; kp; kp = kp->next)
  {
    if (!option (OPTPGPSHOWUNUSABLE) && (kp->flags & KEYFLAG_CANTUSE))
      continue;
    
    for (a = kp->address; a; i++, a = a->next)
      ;
  }

  if (i == 0)
    return NULL;

  keymax = i;
  KeyTable = safe_malloc (sizeof (pgp_key_t *) * i);

  for (i = 0, kp = keys; kp; kp = kp->next)
  {
    if (!option (OPTPGPSHOWUNUSABLE) && (kp->flags & KEYFLAG_CANTUSE))
      continue;
	
    for (a = kp->address; a; i++, a = a->next)
      KeyTable[i] = a;
  }

  switch (PgpSortKeys & SORT_MASK)
  {
    case SORT_DATE:
      f = pgp_compare_date;
      break;
    case SORT_KEYID:
      f = pgp_compare_keyid;
      break;
    case SORT_ADDRESS:
      f = pgp_compare_address;
      break;
    case SORT_TRUST:
    default:
      f = pgp_compare_trust;
      break;
  }
  qsort (KeyTable, i, sizeof (pgp_key_t *), f);

  helpstr[0] = 0;
  mutt_make_help (buf, sizeof (buf), _("Exit  "), MENU_PGP, OP_EXIT);
  strcat (helpstr, buf);
  mutt_make_help (buf, sizeof (buf), _("Select  "), MENU_PGP,
		  OP_GENERIC_SELECT_ENTRY);
  strcat (helpstr, buf);
  mutt_make_help (buf, sizeof (buf), _("Check key  "), MENU_PGP, OP_VERIFY_KEY);
  strcat (helpstr, buf);
  mutt_make_help (buf, sizeof (buf), _("Help"), MENU_PGP, OP_HELP);
  strcat (helpstr, buf);

  menu = mutt_new_menu ();
  menu->max = keymax;
  menu->make_entry = pgp_entry;
  menu->menu = MENU_PGP;
  menu->help = helpstr;
  menu->data = KeyTable;

  if (p)
    snprintf (buf, sizeof (buf), _("PGP keys matching <%s>."), p->mailbox);
  else
    snprintf (buf, sizeof (buf), _("PGP keys matching \"%s\"."), s);
    
  
  menu->title = buf;

  kp = NULL;

  mutt_clear_error ();
  
  while (!done)
  {
    switch (mutt_menuLoop (menu))
    {

    case OP_VERIFY_KEY:

      mutt_mktemp (tempfile);
      if ((devnull = fopen ("/dev/null", "w")) == NULL)
      {
	mutt_perror _("Can't open /dev/null");
	break;
      }
      if ((fp = safe_fopen (tempfile, "w")) == NULL)
      {
	fclose (devnull);
	mutt_perror _("Can't create temporary file");
	break;
      }

      mutt_message _("Invoking PGP...");

      snprintf (tmpbuf, sizeof (tmpbuf), "0x%s", pgp_keyid (pgp_principal_key (KeyTable[menu->current]->parent)));

      if ((thepid = pgp_invoke_verify_key (NULL, NULL, NULL, -1,
		    fileno (fp), fileno (devnull), tmpbuf)) == -1)
      {
	mutt_perror _("Can't create filter");
	unlink (tempfile);
	fclose (fp);
	fclose (devnull);
      }

      mutt_wait_filter (thepid);
      fclose (fp);
      fclose (devnull);
      mutt_clear_error ();
      snprintf (cmd, sizeof (cmd), _("Key ID: 0x%s"), 
		pgp_keyid (pgp_principal_key (KeyTable[menu->current]->parent)));
      mutt_do_pager (cmd, tempfile, 0, NULL);
      menu->redraw = REDRAW_FULL;

      break;

    case OP_VIEW_ID:

      mutt_message ("%s", KeyTable[menu->current]->addr);
      break;

    case OP_GENERIC_SELECT_ENTRY:


      /* XXX make error reporting more verbose */
      
      if (option (OPTPGPCHECKTRUST))
      {
	pgp_key_t *key, *principal;
	
	key = KeyTable[menu->current]->parent;
	principal = pgp_principal_key (key);
	
	if ((key->flags | principal->flags) & KEYFLAG_CANTUSE)
	{
	  mutt_error _("This key can't be used: expired/disabled/revoked.");
	  break;
	}
      }
      
      if (option (OPTPGPCHECKTRUST) &&
	  (KeyTable[menu->current]->trust & 0x03) < 3)
      {
	char *s = "";
	char buff[LONG_STRING];

	switch (KeyTable[menu->current]->trust & 0x03)
	{
	case 0:
	  s = N_("This ID's trust level is undefined.");
	  break;
	case 1:
	  s = N_("This ID is not trusted.");
	  break;
	case 2:
	  s = N_("This ID is only marginally trusted.");
	  break;
	}

	snprintf (buff, sizeof (buff), _("%s Do you really want to use it?"),
		  _(s));

	if (mutt_yesorno (buff, 0) != 1)
	{
	  mutt_clear_error ();
	  break;
	}
      }

# if 0
      kp = pgp_principal_key (KeyTable[menu->current]->parent);
# else
      kp = KeyTable[menu->current]->parent;
# endif
      done = 1;
      break;

    case OP_EXIT:

      kp = NULL;
      done = 1;
      break;
    }
  }

  mutt_menuDestroy (&menu);
  safe_free ((void **) &KeyTable);

  set_option (OPTNEEDREDRAW);
  
  return (kp);
}

pgp_key_t *pgp_ask_for_key (char *tag, char *whatfor,
			    short abilities, pgp_ring_t keyring)
{
  pgp_key_t *key;
  char resp[SHORT_STRING];
  struct pgp_cache *l = NULL;

  resp[0] = 0;
  if (whatfor)
  {

    for (l = id_defaults; l; l = l->next)
      if (!mutt_strcasecmp (whatfor, l->what))
      {
	strcpy (resp, NONULL (l->dflt));
	break;
      }
  }


  FOREVER
  {
    resp[0] = 0;
    if (mutt_get_field (tag, resp, sizeof (resp), M_CLEAR) != 0)
      return NULL;

    if (whatfor)
    {
      if (l)
	mutt_str_replace (&l->dflt, resp);
      else
      {
	l = safe_malloc (sizeof (struct pgp_cache));
	l->next = id_defaults;
	id_defaults = l;
	l->what = safe_strdup (whatfor);
	l->dflt = safe_strdup (resp);
      }
    }

    if ((key = pgp_getkeybystr (resp, abilities, keyring)))
      return key;

    BEEP ();
  }
  /* not reached */
}

/* generate a public key attachment */

BODY *pgp_make_key_attachment (char *tempf)
{
  BODY *att;
  char buff[LONG_STRING];
  char tempfb[_POSIX_PATH_MAX], tmp[STRING];
  FILE *tempfp;
  FILE *devnull;
  struct stat sb;
  pid_t thepid;
  pgp_key_t *key;
  unset_option (OPTPGPCHECKTRUST);

  key = pgp_ask_for_key (_("Please enter the key ID: "), NULL, 0, PGP_PUBRING);

  if (!key)    return NULL;

  snprintf (tmp, sizeof (tmp), "0x%s", pgp_keyid (pgp_principal_key (key)));
  pgp_free_key (&key);
  
  if (!tempf)
  {
    mutt_mktemp (tempfb);
    tempf = tempfb;
  }

  if ((tempfp = safe_fopen (tempf, tempf == tempfb ? "w" : "a")) == NULL)
  {
    mutt_perror _("Can't create temporary file");
    return NULL;
  }

  if ((devnull = fopen ("/dev/null", "w")) == NULL)
  {
    mutt_perror _("Can't open /dev/null");
    fclose (tempfp);
    if (tempf == tempfb)
      unlink (tempf);
    return NULL;
  }

  mutt_message _("Invoking pgp...");

  
  if ((thepid = 
       pgp_invoke_export (NULL, NULL, NULL, -1,
			   fileno (tempfp), fileno (devnull), tmp)) == -1)
  {
    mutt_perror _("Can't create filter");
    unlink (tempf);
    fclose (tempfp);
    fclose (devnull);
    return NULL;
  }

  mutt_wait_filter (thepid);

  fclose (tempfp);
  fclose (devnull);

  att = mutt_new_body ();
  att->filename = safe_strdup (tempf);
  att->unlink = 1;
  att->use_disp = 0;
  att->type = TYPEAPPLICATION;
  att->subtype = safe_strdup ("pgp-keys");
  snprintf (buff, sizeof (buff), _("PGP Key %s."), tmp);
  att->description = safe_strdup (buff);
  mutt_update_encoding (att);

  stat (tempf, &sb);
  att->length = sb.st_size;

  return att;
}

static LIST *pgp_add_string_to_hints (LIST *hints, const char *str)
{
  char *scratch;
  char *t;

  if ((scratch = safe_strdup (str)) == NULL)
    return hints;

  for (t = strtok (scratch, " ,.:\"()<>\n"); t;
       		t = strtok (NULL, " ,.:\"()<>\n"))
  {
    if (strlen (t) > 3)
      hints = mutt_add_list (hints, t);
  }

  safe_free ((void **) &scratch);
  return hints;
}


pgp_key_t *pgp_getkeybyaddr (ADDRESS * a, short abilities, pgp_ring_t keyring)
{
  ADDRESS *r, *p;
  LIST *hints = NULL;
  int weak = 0;
  int weak_association, kflags;
  int match;
  pgp_uid_t *q;
  pgp_key_t *keys, *k, *kn, *pk;
  pgp_key_t *matches = NULL;
  pgp_key_t **last = &matches;
  
  if (a && a->mailbox)
    hints = pgp_add_string_to_hints (hints, a->mailbox);
  if (a && a->personal)
    hints = pgp_add_string_to_hints (hints, a->personal);

  mutt_message (_("Looking for keys matching \"%s\"..."), a->mailbox);
  keys = pgp_get_candidates (keyring, hints);

  mutt_free_list (&hints);
  
  if (!keys)
    return NULL;
  
  dprint (5, (debugfile, "pgp_getkeybyaddr: looking for %s <%s>.",
	      a->personal, a->mailbox));


  for (k = keys; k; k = kn)
  {
    kn = k->next;

    dprint (5, (debugfile, "  looking at key: %s\n",
		pgp_keyid (k)));

    if (abilities && !(k->flags & abilities))
    {
      dprint (5, (debugfile, "  insufficient abilities: Has %x, want %x\n",
		  k->flags, abilities));
      continue;
    }

    pk = pgp_principal_key (k);
    kflags = k->flags | pk->flags;
    
    q = k->address;
    weak_association = 1;
    match = 0;

    for (; q; q = q->next)
    {
      r = rfc822_parse_adrlist (NULL, q->addr);


      for (p = r; p && weak_association; p = p->next)
      {
	if ((p->mailbox && a->mailbox &&
	     mutt_strcasecmp (p->mailbox, a->mailbox) == 0) ||
	    (a->personal && p->personal &&
	     mutt_strcasecmp (a->personal, p->personal) == 0))
	{
	  match = 1;

	  if (((q->trust & 0x03) == 3) &&
	      (!(kflags & KEYFLAG_CANTUSE)) &&
	      (p->mailbox && a->mailbox &&
	       !mutt_strcasecmp (p->mailbox, a->mailbox)))
	  {
	    weak_association = 0;
	  }
	}
      }
      rfc822_free_address (&r);
    }

    if (match && weak_association)
	weak = 1;
    
    if (match)
    {
      pgp_key_t *_p, *_k;

      _k = pgp_principal_key (k);
      
      *last = _k;
      kn = pgp_remove_key (&keys, _k);

      /* start with k, not with _k: k is always a successor of _k. */
      
      for (_p = k; _p; _p = _p->next)
      {
	if (!_p->next)
	{
	  last = &_p->next;
	  break;
	}
      }
    }
  }

  pgp_free_key (&keys);
  
  if (matches)
  {
    if (matches->next || weak)
    {
      /* query for which key the user wants */
      k = pgp_select_key (matches, a, NULL);
      if (k) 
	pgp_remove_key (&matches, k);

      pgp_free_key (&matches);
    }
    else
      k = matches;
    
    return k;
  }

  return NULL;
}

pgp_key_t *pgp_getkeybystr (char *p, short abilities, pgp_ring_t keyring)
{
  LIST *hints = NULL;
  pgp_key_t *keys;
  pgp_key_t *matches = NULL;
  pgp_key_t **last = &matches;
  pgp_key_t *k, *kn;
  pgp_uid_t *a;
  short match;

  mutt_message (_("Looking for keys matching \"%s\"..."), p);
  
  hints = pgp_add_string_to_hints (hints, p);
  keys = pgp_get_candidates (keyring, hints);
  mutt_free_list (&hints);

  if (!keys)
    return NULL;
  
  
  for (k = keys; k; k = kn)
  {
    kn = k->next;
    if (abilities && !(k->flags & abilities))
      continue;

    match = 0;
    
    for (a = k->address; a; a = a->next)
    {
      dprint (5, (debugfile, "pgp_getkeybystr: matching \"%s\" against key %s, \"%s\": ",
		  p, pgp_keyid (k), a->addr));
      if (!*p || mutt_strcasecmp (p, pgp_keyid (k)) == 0 ||
	  (!mutt_strncasecmp (p, "0x", 2) && !mutt_strcasecmp (p + 2, pgp_keyid (k))) ||
	  (option (OPTPGPLONGIDS) && !mutt_strncasecmp (p, "0x", 2) &&
	   !mutt_strcasecmp (p + 2, k->keyid + 8)) ||
	  mutt_stristr (a->addr, p))
      {
	dprint (5, (debugfile, "match.\n"));
	match = 1;
	break;
      }
    }
    
    if (match)
    {
      pgp_key_t *_p, *_k;

      _k = pgp_principal_key (k);
      
      *last = _k;
      kn = pgp_remove_key (&keys, _k);

      /* start with k, not with _k: k is always a successor of _k. */
      
      for (_p = k; _p; _p = _p->next)
      {
	if (!_p->next)
	{
	  last = &_p->next;
	  break;
	}
      }
    }
  }

  pgp_free_key (&keys);

  if (matches)
  {
    k = pgp_select_key (matches, NULL, p);
    if (k) 
      pgp_remove_key (&matches, k);
    
    pgp_free_key (&matches);
    return k;
  }

  return NULL;
}



#endif /* HAVE_PGP */
