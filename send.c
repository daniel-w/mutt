/*
 * Copyright (C) 1996-2000 Michael R. Elkins <me@cs.hmc.edu>
 * 
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 * 
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 */ 

#include "mutt.h"
#include "mutt_curses.h"
#include "rfc2047.h"
#include "keymap.h"
#include "mime.h"
#include "mailbox.h"
#include "copy.h"
#include "mx.h"

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <sys/types.h>
#include <utime.h>

#ifdef HAVE_PGP
#include "pgp.h"
#endif

#ifdef MIXMASTER
#include "remailer.h"
#endif


static void append_signature (FILE *f)
{
  FILE *tmpfp;
  pid_t thepid;

  if (Signature && (tmpfp = mutt_open_read (Signature, &thepid)))
  {
    if (option (OPTSIGDASHES))
      fputs ("\n-- \n", f);
    mutt_copy_stream (tmpfp, f);
    fclose (tmpfp);
    if (thepid != -1)
      mutt_wait_filter (thepid);
  }
}

/* compare two e-mail addresses and return 1 if they are equivalent */
static int mutt_addrcmp (ADDRESS *a, ADDRESS *b)
{
  if (!a->mailbox || !b->mailbox)
    return 0;
  if (ascii_strcasecmp (a->mailbox, b->mailbox))
    return 0;
  return 1;
}

/* search an e-mail address in a list */
static int mutt_addrsrc (ADDRESS *a, ADDRESS *lst)
{
  for (; lst; lst = lst->next)
  {
    if (mutt_addrcmp (a, lst))
      return (1);
  }
  return (0);
}

/* removes addresses from "b" which are contained in "a" */
static ADDRESS *mutt_remove_xrefs (ADDRESS *a, ADDRESS *b)
{
  ADDRESS *top, *p, *prev = NULL;

  top = b;
  while (b)
  {
    for (p = a; p; p = p->next)
    {
      if (mutt_addrcmp (p, b))
	break;
    }
    if (p)
    {
      if (prev)
      {
	prev->next = b->next;
	b->next = NULL;
	rfc822_free_address (&b);
	b = prev;
      }
      else
      {
	top = top->next;
	b->next = NULL;
	rfc822_free_address (&b);
	b = top;
      }
    }
    else
    {
      prev = b;
      b = b->next;
    }
  }
  return top;
}

/* remove any address which matches the current user.  if `leave_only' is
 * nonzero, don't remove the user's address if it is the only one in the list
 */
static ADDRESS *remove_user (ADDRESS *a, int leave_only)
{
  ADDRESS *top = NULL, *last = NULL;

  while (a)
  {
    if (!mutt_addr_is_user (a))
    {
      if (top)
      {
        last->next = a;
        last = last->next;
      }
      else
        last = top = a;
      a = a->next;
      last->next = NULL;
    }
    else
    {
      ADDRESS *tmp = a;
      
      a = a->next;
      if (!leave_only || a || last)
      {
	tmp->next = NULL;
	rfc822_free_address (&tmp);
      }
      else
	last = top = tmp;
    }
  }
  return top;
}

static ADDRESS *find_mailing_lists (ADDRESS *t, ADDRESS *c)
{
  ADDRESS *top = NULL, *ptr = NULL;

  for (; t || c; t = c, c = NULL)
  {
    for (; t; t = t->next)
    {
      if (mutt_is_mail_list (t) && !t->group)
      {
	if (top)
	{
	  ptr->next = rfc822_cpy_adr_real (t);
	  ptr = ptr->next;
	}
	else
	  ptr = top = rfc822_cpy_adr_real (t);
      }
    }
  }
  return top;
}

static int edit_address (ADDRESS **a, /* const */ char *field)
{
  char buf[HUGE_STRING];

  buf[0] = 0;
  rfc822_write_address (buf, sizeof (buf), *a);
  if (mutt_get_field (field, buf, sizeof (buf), M_ALIAS) != 0)
    return (-1);
  rfc822_free_address (a);
  *a = mutt_expand_aliases (mutt_parse_adrlist (NULL, buf));
  return 0;
}

static int edit_envelope (ENVELOPE *en)
{
  char buf[HUGE_STRING];
  LIST *uh = UserHeader;

  if (edit_address (&en->to, "To: ") == -1 || en->to == NULL)
    return (-1);
  if (option (OPTASKCC) && edit_address (&en->cc, "Cc: ") == -1)
    return (-1);
  if (option (OPTASKBCC) && edit_address (&en->bcc, "Bcc: ") == -1)
    return (-1);

  if (en->subject)
  {
    if (option (OPTFASTREPLY))
      return (0);
    else
      strfcpy (buf, en->subject, sizeof (buf));
  }
  else
  {
    char *p;

    buf[0] = 0;
    for (; uh; uh = uh->next)
    {
      if (ascii_strncasecmp ("subject:", uh->data, 8) == 0)
      {
	p = uh->data + 8;
	SKIPWS (p);
	strncpy (buf, p, sizeof (buf));
      }
    }
  }
  
  if (mutt_get_field ("Subject: ", buf, sizeof (buf), 0) != 0 ||
      (!buf[0] && query_quadoption (OPT_SUBJECT, _("No subject, abort?")) != 0))
  {
    mutt_message _("No subject, aborting.");
    return (-1);
  }
  mutt_str_replace (&en->subject, buf);

  return 0;
}

static void process_user_recips (ENVELOPE *env)
{
  LIST *uh = UserHeader;

  for (; uh; uh = uh->next)
  {
    if (ascii_strncasecmp ("to:", uh->data, 3) == 0)
      env->to = rfc822_parse_adrlist (env->to, uh->data + 3);
    else if (ascii_strncasecmp ("cc:", uh->data, 3) == 0)
      env->cc = rfc822_parse_adrlist (env->cc, uh->data + 3);
    else if (ascii_strncasecmp ("bcc:", uh->data, 4) == 0)
      env->bcc = rfc822_parse_adrlist (env->bcc, uh->data + 4);
  }
}

static void process_user_header (ENVELOPE *env)
{
  LIST *uh = UserHeader;
  LIST *last = env->userhdrs;

  if (last)
    while (last->next)
      last = last->next;

  for (; uh; uh = uh->next)
  {
    if (ascii_strncasecmp ("from:", uh->data, 5) == 0)
    {
      /* User has specified a default From: address.  Remove default address */
      rfc822_free_address (&env->from);
      env->from = rfc822_parse_adrlist (env->from, uh->data + 5);
    }
    else if (ascii_strncasecmp ("reply-to:", uh->data, 9) == 0)
    {
      rfc822_free_address (&env->reply_to);
      env->reply_to = rfc822_parse_adrlist (env->reply_to, uh->data + 9);
    }
    else if (ascii_strncasecmp ("message-id:", uh->data, 11) == 0)
      mutt_str_replace (&env->message_id, uh->data + 11);
    else if (ascii_strncasecmp ("to:", uh->data, 3) != 0 &&
	     ascii_strncasecmp ("cc:", uh->data, 3) != 0 &&
	     ascii_strncasecmp ("bcc:", uh->data, 4) != 0 &&
	     ascii_strncasecmp ("subject:", uh->data, 8) != 0)
    {
      if (last)
      {
	last->next = mutt_new_list ();
	last = last->next;
      }
      else
	last = env->userhdrs = mutt_new_list ();
      last->data = safe_strdup (uh->data);
    }
  }
}

LIST *mutt_copy_list (LIST *p)
{
  LIST *t, *r=NULL, *l=NULL;

  for (; p; p = p->next)
  {
    t = (LIST *) safe_malloc (sizeof (LIST));
    t->data = safe_strdup (p->data);
    t->next = NULL;
    if (l)
    {
      r->next = t;
      r = r->next;
    }
    else
      l = r = t;
  }
  return (l);
}

void mutt_forward_intro (FILE *fp, HEADER *cur)
{
  char buffer[STRING];
  
  fputs ("----- Forwarded message from ", fp);
  buffer[0] = 0;
  rfc822_write_address (buffer, sizeof (buffer), cur->env->from);
  fputs (buffer, fp);
  fputs (" -----\n\n", fp);
}

void mutt_forward_trailer (FILE *fp)
{
  fputs ("\n----- End forwarded message -----\n", fp);
}


static int include_forward (CONTEXT *ctx, HEADER *cur, FILE *out)
{
  int chflags = CH_DECODE, cmflags = 0;
  
  mutt_parse_mime_message (ctx, cur);
  mutt_message_hook (ctx, cur, M_MESSAGEHOOK);

#ifdef HAVE_PGP
  if ((cur->pgp & PGPENCRYPT) && option (OPTFORWDECODE))
  {
    /* make sure we have the user's passphrase before proceeding... */
    pgp_valid_passphrase ();
  }
#endif /* HAVE_PGP */

  mutt_forward_intro (out, cur);

  if (option (OPTFORWDECODE))
  {
    cmflags |= M_CM_DECODE | M_CM_CHARCONV;
    if (option (OPTWEED))
    {
      chflags |= CH_WEED | CH_REORDER;
      cmflags |= M_CM_WEED;
    }
  }
  if (option (OPTFORWQUOTE))
    cmflags |= M_CM_PREFIX;

  mutt_copy_message (out, ctx, cur, cmflags, chflags);
  mutt_forward_trailer (out);
  return 0;
}

void mutt_make_attribution (CONTEXT *ctx, HEADER *cur, FILE *out)
{
  char buffer[STRING];
  if (Attribution)
  {
    mutt_make_string (buffer, sizeof (buffer), Attribution, ctx, cur);
    fputs (buffer, out);
    fputc ('\n', out);
  }
}

void mutt_make_post_indent (CONTEXT *ctx, HEADER *cur, FILE *out)
{
  char buffer[STRING];
  if (PostIndentString)
  {
    mutt_make_string (buffer, sizeof (buffer), PostIndentString, ctx, cur);
    fputs (buffer, out);
    fputc ('\n', out);
  }
}

static int include_reply (CONTEXT *ctx, HEADER *cur, FILE *out)
{
  int cmflags = M_CM_PREFIX | M_CM_DECODE | M_CM_CHARCONV;
  int chflags = CH_DECODE;

#ifdef HAVE_PGP
  if (cur->pgp)
  {
    if (cur->pgp & PGPENCRYPT)
    {
      /* make sure we have the user's passphrase before proceeding... */
      pgp_valid_passphrase ();
    }
  }
#endif /* HAVE_PGP */

  mutt_parse_mime_message (ctx, cur);
  mutt_message_hook (ctx, cur, M_MESSAGEHOOK);
  
  mutt_make_attribution (ctx, cur, out);
  
  if (!option (OPTHEADER))
    cmflags |= M_CM_NOHEADER;
  if (option (OPTWEED))
  {
    chflags |= CH_WEED;
    cmflags |= M_CM_WEED;
  }

  mutt_copy_message (out, ctx, cur, cmflags, chflags);

  mutt_make_post_indent (ctx, cur, out);
  
  return 0;
}

static int default_to (ADDRESS **to, ENVELOPE *env, int flags)
{
  char prompt[STRING];

  if (flags && env->mail_followup_to)
  {
    snprintf (prompt, sizeof (prompt), _("Follow-up to %s%s?"),
	      env->mail_followup_to->mailbox,
	      env->mail_followup_to->next ? ",..." : "");

    switch (query_quadoption (OPT_MFUPTO, prompt))
    {
    case M_YES:
      rfc822_append (to, env->mail_followup_to);
      return 0;

    case -1:
      return -1; /* abort */
    }
  }

  /* Exit now if we're setting up the default Cc list for list-reply
   * (only set if Mail-Followup-To is present and honoured).
   */
  if (flags & SENDLISTREPLY)
    return 0;

  if (!option(OPTREPLYSELF) && mutt_addr_is_user (env->from))
  {
    /* mail is from the user, assume replying to recipients */
    rfc822_append (to, env->to);
  }
  else if (env->reply_to)
  {
    if ((mutt_addrcmp (env->from, env->reply_to) && !env->reply_to->next) || 
	(option (OPTIGNORELISTREPLYTO) &&
	mutt_is_mail_list (env->reply_to) &&
	(mutt_addrsrc (env->reply_to, env->to) ||
	mutt_addrsrc (env->reply_to, env->cc))))
    {
      /* If the Reply-To: address is a mailing list, assume that it was
       * put there by the mailing list, and use the From: address
       * 
       * We also take the from header if our correspondant has a reply-to
       * header which is identical to the electronic mail address given
       * in his From header.
       * 
       */
      rfc822_append (to, env->from);
    }
    else if (!(mutt_addrcmp (env->from, env->reply_to) && 
	       !env->reply_to->next) &&
	     quadoption (OPT_REPLYTO) != M_YES)
    {
      /* There are quite a few mailing lists which set the Reply-To:
       * header field to the list address, which makes it quite impossible
       * to send a message to only the sender of the message.  This
       * provides a way to do that.
       */
      snprintf (prompt, sizeof (prompt), _("Reply to %s%s?"),
		env->reply_to->mailbox, 
		env->reply_to->next?",...":"");
      switch (query_quadoption (OPT_REPLYTO, prompt))
      {
      case M_YES:
	rfc822_append (to, env->reply_to);
	break;

      case M_NO:
	rfc822_append (to, env->from);
	break;

      default:
	return (-1); /* abort */
      }
    }
    else
      rfc822_append (to, env->reply_to);
  }
  else
    rfc822_append (to, env->from);

  return (0);
}

int mutt_fetch_recips (ENVELOPE *out, ENVELOPE *in, int flags)
{
  ADDRESS *tmp;
  if (flags & SENDLISTREPLY)
  {
    tmp = find_mailing_lists (in->to, in->cc);
    rfc822_append (&out->to, tmp);
    rfc822_free_address (&tmp);

    if (in->mail_followup_to &&
        default_to (&out->cc, in, flags & SENDLISTREPLY) == -1)
      return (-1); /* abort */
  }
  else
  {
    if (default_to (&out->to, in, flags & SENDGROUPREPLY) == -1)
      return (-1); /* abort */

    if ((flags & SENDGROUPREPLY) && !in->mail_followup_to)
    {
      /* if(!mutt_addr_is_user(in->to)) */
      rfc822_append (&out->cc, in->to);
      rfc822_append (&out->cc, in->cc);
    }
  }
  return 0;
}

LIST *mutt_make_references(ENVELOPE *e)
{
  LIST *t = NULL, *l = NULL;

  if (e->references)
    l = mutt_copy_list (e->references);
  else
    l = mutt_copy_list (e->in_reply_to);
  
  if (e->message_id)
  {
    t = mutt_new_list();
    t->data = safe_strdup(e->message_id);
    t->next = l;
    l = t;
  }
  
  return l;
}

void mutt_fix_reply_recipients (ENVELOPE *env)
{
  if (! option (OPTMETOO))
  {
    /* the order is important here.  do the CC: first so that if the
     * the user is the only recipient, it ends up on the TO: field
     */
    env->cc = remove_user (env->cc, (env->to == NULL));
    env->to = remove_user (env->to, (env->cc == NULL));
  }
  
  /* the CC field can get cluttered, especially with lists */
  env->to = mutt_remove_duplicates (env->to);
  env->cc = mutt_remove_duplicates (env->cc);
  env->cc = mutt_remove_xrefs (env->to, env->cc);
}

void mutt_make_forward_subject (ENVELOPE *env, CONTEXT *ctx, HEADER *cur)
{
  char buffer[STRING];

  /* set the default subject for the message. */
  mutt_make_string (buffer, sizeof (buffer), NONULL(ForwFmt), ctx, cur);
  env->subject = safe_strdup (buffer);
}

void mutt_make_misc_reply_headers (ENVELOPE *env, CONTEXT *ctx,
				    HEADER *cur, ENVELOPE *curenv)
{
  if (curenv->real_subj)
  {
    env->subject = safe_malloc (mutt_strlen (curenv->real_subj) + 5);
    sprintf (env->subject, "Re: %s", curenv->real_subj);	/* __SPRINTF_CHECKED__ */
  }
  else
    env->subject = safe_strdup ("Re: your mail");
  
}

void mutt_add_to_reference_headers (ENVELOPE *env, ENVELOPE *curenv, LIST ***pp, LIST ***qq)
{
  LIST **p = NULL, **q = NULL;

  if (pp) p = *pp;
  if (qq) q = *qq;
  
  if (!p) p = &env->references;
  if (!q) q = &env->in_reply_to;
  
  while (*p) p = &(*p)->next;
  while (*q) q = &(*q)->next;
  
  *p = mutt_make_references (curenv);
  
  if (curenv->message_id)
  {
    *q = mutt_new_list();
    (*q)->data = safe_strdup (curenv->message_id);
  }
  
  if (pp) *pp = p;
  if (qq) *qq = q;
  
}

static void 
mutt_make_reference_headers (ENVELOPE *curenv, ENVELOPE *env, CONTEXT *ctx)
{
  env->references = NULL;
  env->in_reply_to = NULL;
  
  if (!curenv)
  {
    HEADER *h;
    LIST **p = NULL, **q = NULL;
    int i;
    
    for(i = 0; i < ctx->vcount; i++)
    {
      h = ctx->hdrs[ctx->v2r[i]];
      if (h->tagged)
	mutt_add_to_reference_headers (env, h->env, &p, &q);
    }
  }
  else
    mutt_add_to_reference_headers (env, curenv, NULL, NULL);
}

static int
envelope_defaults (ENVELOPE *env, CONTEXT *ctx, HEADER *cur, int flags)
{
  ENVELOPE *curenv = NULL;
  int i = 0, tag = 0;

  if (!cur)
  {
    tag = 1;
    for (i = 0; i < ctx->vcount; i++)
      if (ctx->hdrs[ctx->v2r[i]]->tagged)
      {
	cur = ctx->hdrs[ctx->v2r[i]];
	curenv = cur->env;
	break;
      }

    if (!cur)
    {
      /* This could happen if the user tagged some messages and then did
       * a limit such that none of the tagged message are visible.
       */
      mutt_error _("No tagged messages are visible!");
      return (-1);
    }
  }
  else
    curenv = cur->env;

  if (flags & SENDREPLY)
  {
    if (tag)
    {
      HEADER *h;

      for (i = 0; i < ctx->vcount; i++)
      {
	h = ctx->hdrs[ctx->v2r[i]];
	if (h->tagged && mutt_fetch_recips (env, h->env, flags) == -1)
	  return -1;
      }
    }
    else if (mutt_fetch_recips (env, curenv, flags) == -1)
      return -1;

    if ((flags & SENDLISTREPLY) && !env->to)
    {
      mutt_error _("No mailing lists found!");
      return (-1);
    }

    mutt_fix_reply_recipients (env);
    mutt_make_misc_reply_headers (env, ctx, cur, curenv);
    mutt_make_reference_headers (tag ? NULL : curenv, env, ctx);
  }
  else if (flags & SENDFORWARD)
    mutt_make_forward_subject (env, ctx, cur);

  return (0);
}

static int
generate_body (FILE *tempfp,	/* stream for outgoing message */
	       HEADER *msg,	/* header for outgoing message */
	       int flags,	/* compose mode */
	       CONTEXT *ctx,	/* current mailbox */
	       HEADER *cur)	/* current message */
{
  int i;
  HEADER *h;
  BODY *tmp;

  if (flags & SENDREPLY)
  {
    if ((i = query_quadoption (OPT_INCLUDE, _("Include message in reply?"))) == -1)
      return (-1);

    if (i == M_YES)
    {
      mutt_message _("Including quoted message...");
      if (!cur)
      {
	for (i = 0; i < ctx->vcount; i++)
	{
	  h = ctx->hdrs[ctx->v2r[i]];
	  if (h->tagged)
	  {
	    if (include_reply (ctx, h, tempfp) == -1)
	    {
	      mutt_error _("Could not include all requested messages!");
	      return (-1);
	    }
	    fputc ('\n', tempfp);
	  }
	}
      }
      else
	include_reply (ctx, cur, tempfp);

    }
  }
  else if (flags & SENDFORWARD)
  {
    if ((i = query_quadoption (OPT_MIMEFWD, _("Forward as attachment?"))) == M_YES)
    {
      BODY *last = msg->content;

      mutt_message _("Preparing forwarded message...");
      
      while (last && last->next)
	last = last->next;

      if (cur)
      {
	tmp = mutt_make_message_attach (ctx, cur, 0);
	if (last)
	  last->next = tmp;
	else
	  msg->content = tmp;
      }
      else
      {
	for (i = 0; i < ctx->vcount; i++)
	{
	  if (ctx->hdrs[ctx->v2r[i]]->tagged)
	  {
	    tmp = mutt_make_message_attach (ctx, ctx->hdrs[ctx->v2r[i]], 0);
	    if (last)
	    {
	      last->next = tmp;
	      last = tmp;
	    }
	    else
	      last = msg->content = tmp;
	  }
	}
      }
    }
    else if (i != -1)
    {
      if (cur)
	include_forward (ctx, cur, tempfp);
      else
	for (i=0; i < ctx->vcount; i++)
	  if (ctx->hdrs[ctx->v2r[i]]->tagged)
	    include_forward (ctx, ctx->hdrs[ctx->v2r[i]], tempfp);
    }
    else if (i == -1)
      return -1;
  }



#ifdef HAVE_PGP
  else if (flags & SENDKEY) 
  {
    BODY *tmp;
    if ((tmp = pgp_make_key_attachment (NULL)) == NULL)
      return -1;

    tmp->next = msg->content;
    msg->content = tmp;
  }
#endif

  mutt_clear_error ();

  return (0);
}

void mutt_set_followup_to (ENVELOPE *e)
{
  ADDRESS *t = NULL;
  ADDRESS *from;

  /* 
   * Only generate the Mail-Followup-To if the user has requested it, and
   * it hasn't already been set
   */

  if (option (OPTFOLLOWUPTO) && !e->mail_followup_to)
  {
    if (mutt_is_list_cc (0, e->to, e->cc))
    {
      /* 
       * this message goes to known mailing lists, so create a proper
       * mail-followup-to header
       */

      t = rfc822_append (&e->mail_followup_to, e->to);
      rfc822_append (&t, e->cc);
    }

    /* remove ourselves from the mail-followup-to header */
    e->mail_followup_to = remove_user (e->mail_followup_to, 0);

    /*
     * If we are not subscribed to any of the lists in question,
     * re-add ourselves to the mail-followup-to header.  The 
     * mail-followup-to header generated is a no-op with group-reply,
     * but makes sure list-reply has the desired effect.
     */

    if (e->mail_followup_to && !mutt_is_list_recipient (0, e->to, e->cc))
    {
      if (e->from)
	from = rfc822_cpy_adr (e->from);
      else
	from = mutt_default_from ();
      
      if (from)
      {
	/* Normally, this loop will not even be entered. */
	for (t = from; t && t->next; t = t->next)
	  ;
	
	t->next = e->mail_followup_to; 	/* t cannot be NULL at this point. */
	e->mail_followup_to = from;
      }
    }
    
    e->mail_followup_to = mutt_remove_duplicates (e->mail_followup_to);
    
  }
}


/* look through the recipients of the message we are replying to, and if
   we find an address that matches $alternates, we use that as the default
   from field */
static ADDRESS *set_reverse_name (ENVELOPE *env)
{
  ADDRESS *tmp;

  for (tmp = env->to; tmp; tmp = tmp->next)
  {
    if (mutt_addr_is_user (tmp))
      break;
  }
  if (!tmp)
  {
    for (tmp = env->cc; tmp; tmp = tmp->next)
    {
      if (mutt_addr_is_user (tmp))
	break;
    }
  }
  if (!tmp && mutt_addr_is_user (env->from))
    tmp = env->from;
  if (tmp)
  {
    tmp = rfc822_cpy_adr_real (tmp);
    if (!option (OPTREVREAL))
      FREE (&tmp->personal);
    if (!tmp->personal)
      tmp->personal = safe_strdup (Realname);
  }
  return (tmp);
}

ADDRESS *mutt_default_from (void)
{
  ADDRESS *adr;
  const char *fqdn = mutt_fqdn(1);

  /* 
   * Note: We let $from override $realname here.  Is this the right
   * thing to do? 
   */

  if (From)
    adr = rfc822_cpy_adr_real (From);
  else if (option (OPTUSEDOMAIN))
  {
    adr = rfc822_new_address ();
    adr->mailbox = safe_malloc (mutt_strlen (Username) + mutt_strlen (fqdn) + 2);
    sprintf (adr->mailbox, "%s@%s", NONULL(Username), NONULL(fqdn));	/* __SPRINTF_CHECKED__ */
  }
  else
  {
    adr = rfc822_new_address ();
    adr->mailbox = safe_strdup (NONULL(Username));
  }
  
  return (adr);
}

static int send_message (HEADER *msg)
{  
  char tempfile[_POSIX_PATH_MAX];
  FILE *tempfp;
  int i;
  
  /* Write out the message in MIME form. */
  mutt_mktemp (tempfile);
  if ((tempfp = safe_fopen (tempfile, "w")) == NULL)
    return (-1);

#ifdef MIXMASTER
  mutt_write_rfc822_header (tempfp, msg->env, msg->content, 0, msg->chain ? 1 : 0);
#endif
#ifndef MIXMASTER
  mutt_write_rfc822_header (tempfp, msg->env, msg->content, 0, 0);
#endif
  
  fputc ('\n', tempfp); /* tie off the header. */

  if ((mutt_write_mime_body (msg->content, tempfp) == -1))
  {
    fclose(tempfp);
    unlink (tempfile);
    return (-1);
  }
  
  if (fclose (tempfp) != 0)
  {
    mutt_perror (tempfile);
    unlink (tempfile);
    return (-1);
  }

#ifdef MIXMASTER
  if (msg->chain)
    return mix_send_message (msg->chain, tempfile);
#endif

  i = mutt_invoke_sendmail (msg->env->from, msg->env->to, msg->env->cc, 
			    msg->env->bcc, tempfile, (msg->content->encoding == ENC8BIT));
  return (i);
}

/* rfc2047 encode the content-descriptions */
static void encode_descriptions (BODY *b, short recurse)
{
  BODY *t;

  for (t = b; t; t = t->next)
  {
    if (t->description)
    {
      rfc2047_encode_string (&t->description);
    }
    if (recurse && t->parts)
      encode_descriptions (t->parts, recurse);
  }
}

/* rfc2047 decode them in case of an error */
static void decode_descriptions (BODY *b)
{
  BODY *t;
  
  for (t = b; t; t = t->next)
  {
    if (t->description)
    {
      rfc2047_decode (&t->description);
    }
    if (t->parts)
      decode_descriptions (t->parts);
  }
}

int mutt_resend_message (FILE *fp, CONTEXT *ctx, HEADER *cur)
{
  HEADER *msg = mutt_new_header ();
  
  if (mutt_prepare_template (fp, ctx, msg, cur, 1) < 0)
    return -1;
  
  return ci_send_message (SENDRESEND, msg, NULL, ctx, cur);
}

#ifdef HAVE_PGP

static int _set_pgp_flags (HEADER *cur)
{
  int flags = 0;
  
  if (option (OPTPGPREPLYENCRYPT) && cur && cur->pgp & PGPENCRYPT)
    flags |= PGPENCRYPT;
  if (option (OPTPGPREPLYSIGN) && cur && cur->pgp & PGPSIGN)
    flags |= PGPSIGN;
  if (option (OPTPGPREPLYSIGNENCRYPTED) && cur && cur->pgp & PGPENCRYPT)
    flags |= PGPSIGN;

  return flags;

}

static int set_pgp_flags (HEADER *cur, CONTEXT *ctx)
{
  int i;
  int flags = 0;
  
  if (cur) 
    return _set_pgp_flags (cur);
  
  if (!ctx)
    return 0;
    
  for (i = 0; i < ctx->vcount; i++)
  {
    cur = ctx->hdrs[ctx->v2r[i]];
    if (cur->tagged)
      flags |= _set_pgp_flags (cur);
  }

  return flags;
}

#endif /* HAVE_PGP */

int
ci_send_message (int flags,		/* send mode */
		 HEADER *msg,		/* template to use for new message */
		 char *tempfile,	/* file specified by -i or -H */
		 CONTEXT *ctx,		/* current mailbox */
		 HEADER *cur)		/* current message */
{
  char buffer[LONG_STRING];
  char fcc[_POSIX_PATH_MAX] = ""; /* where to copy this message */
  FILE *tempfp = NULL;
  BODY *pbody;
  int i, killfrom = 0;

#ifdef HAVE_PGP
  BODY *save_content = NULL;
  BODY *clear_content = NULL;
  char *pgpkeylist = NULL;
  /* save current value of "pgp_sign_as" */
  char *signas = NULL;
#endif

  int rv = -1;
  
  if (!flags && !msg && quadoption (OPT_RECALL) != M_NO &&
      mutt_num_postponed (1))
  {
    /* If the user is composing a new message, check to see if there
     * are any postponed messages first.
     */
    if ((i = query_quadoption (OPT_RECALL, _("Recall postponed message?"))) == -1)
      return rv;

    if(i == M_YES)
      flags |= SENDPOSTPONED;
  }
  
  
#ifdef HAVE_PGP
  if (flags & SENDPOSTPONED)
    signas = safe_strdup(PgpSignAs);
#endif /* HAVE_PGP */

  if (msg)
  {
    mutt_expand_aliases_env (msg->env);
  }
  else
  {
    msg = mutt_new_header ();

    if (flags == SENDPOSTPONED)
    {
      if ((flags = mutt_get_postponed (ctx, msg, &cur, fcc, sizeof (fcc))) < 0)
	goto cleanup;
    }

    if (flags & (SENDPOSTPONED|SENDRESEND))
    {
      if ((tempfp = safe_fopen (msg->content->filename, "a+")) == NULL)
      {
	mutt_perror (msg->content->filename);
	goto cleanup;
      }
    }

    if (!msg->env)
      msg->env = mutt_new_envelope ();
  }

  if (! (flags & (SENDKEY | SENDPOSTPONED | SENDRESEND)))
  {
    pbody = mutt_new_body ();
    pbody->next = msg->content; /* don't kill command-line attachments */
    msg->content = pbody;
    
    msg->content->type = TYPETEXT;
    msg->content->subtype = safe_strdup ("plain");
    msg->content->unlink = 1;
    msg->content->use_disp = 0;
    msg->content->disposition = DISPINLINE;
    if (option (OPTTEXTFLOWED))
      mutt_set_parameter ("format", "flowed", &msg->content->parameter);
    
    if (!tempfile)
    {
      mutt_mktemp (buffer);
      tempfp = safe_fopen (buffer, "w+");
      msg->content->filename = safe_strdup (buffer);
    }
    else
    {
      tempfp = safe_fopen (tempfile, "a+");
      msg->content->filename = safe_strdup (tempfile);
    }

    if (!tempfp)
    {
      dprint(1,(debugfile, "newsend_message: can't create tempfile %s (errno=%d)\n", msg->content->filename, errno));
      mutt_perror (msg->content->filename);
      goto cleanup;
    }
  }

  /* this is handled here so that the user can match ~f in send-hook */
  if (cur && option (OPTREVNAME) && !(flags & (SENDPOSTPONED|SENDRESEND)))
  {
    /* we shouldn't have to worry about freeing `msg->env->from' before
     * setting it here since this code will only execute when doing some
     * sort of reply.  the pointer will only be set when using the -H command
     * line option */
    msg->env->from = set_reverse_name (cur->env);
  }

  if (!msg->env->from && option (OPTUSEFROM) && !(flags & (SENDPOSTPONED|SENDRESEND)))
    msg->env->from = mutt_default_from ();

  if (flags & SENDBATCH) 
  {
    mutt_copy_stream (stdin, tempfp);
    if (option (OPTHDRS))
    {
      process_user_recips (msg->env);
      process_user_header (msg->env);
    }
  }
  else if (! (flags & (SENDPOSTPONED|SENDRESEND)))
  {
    if ((flags & (SENDREPLY | SENDFORWARD)) && ctx &&
	envelope_defaults (msg->env, ctx, cur, flags) == -1)
      goto cleanup;

    if (option (OPTHDRS))
      process_user_recips (msg->env);

    if (! (flags & SENDMAILX) &&
	! (option (OPTAUTOEDIT) && option (OPTEDITHDRS)) &&
	! ((flags & SENDREPLY) && option (OPTFASTREPLY)))
    {
      if (edit_envelope (msg->env) == -1)
	goto cleanup;
    }

    /* the from address must be set here regardless of whether or not
     * $use_from is set so that the `~P' (from you) operator in send-hook
     * patterns will work.  if $use_from is unset, the from address is killed
     * after send-hooks are evaulated */

    if (!msg->env->from)
    {
      msg->env->from = mutt_default_from ();
      killfrom = 1;
    }

    /* change settings based upon recipients */
    
    mutt_message_hook (NULL, msg, M_SENDHOOK);

    if (killfrom)
    {
      rfc822_free_address (&msg->env->from);
      killfrom = 0;
    }

    if (option (OPTHDRS))
      process_user_header (msg->env);


    if (option (OPTSIGONTOP) && (! (flags & (SENDMAILX | SENDKEY)) && Editor && mutt_strcmp (Editor, "builtin") != 0))
      append_signature (tempfp);

    /* include replies/forwarded messages, unless we are given a template */
    if (!tempfile && (ctx || !(flags & (SENDREPLY|SENDFORWARD)))
	&& generate_body (tempfp, msg, flags, ctx, cur) == -1)
      goto cleanup;

    if (!option (OPTSIGONTOP) && (! (flags & (SENDMAILX | SENDKEY)) && Editor && mutt_strcmp (Editor, "builtin") != 0))
      append_signature (tempfp);

    /* 
     * this wants to be done _after_ generate_body, so message-hooks
     * can take effect.
     */

#ifdef HAVE_PGP
    if (! (flags & SENDMAILX))
    {
      if (option (OPTPGPAUTOSIGN))
	msg->pgp |= PGPSIGN;
      if (option (OPTPGPAUTOENCRYPT))
	msg->pgp |= PGPENCRYPT;
      
      msg->pgp |= set_pgp_flags (cur, ctx);
    }

#endif /* HAVE_PGP */
      



  }
  /* wait until now to set the real name portion of our return address so
     that $realname can be set in a send-hook */
  if (msg->env->from && !msg->env->from->personal && !(flags & (SENDRESEND|SENDPOSTPONED)))
    msg->env->from->personal = safe_strdup (Realname);



#ifdef HAVE_PGP
  if (! (flags & SENDKEY))
#endif
    safe_fclose (&tempfp);



  if (flags & SENDMAILX)
  {
    if (mutt_builtin_editor (msg->content->filename, msg, cur) == -1)
      goto cleanup;
  }
  else if (! (flags & SENDBATCH))
  {
    struct stat st;
    time_t mtime = mutt_decrease_mtime (msg->content->filename, NULL);
    
    mutt_update_encoding (msg->content);

    /* If the this isn't a text message, look for a mailcap edit command */
    if(! (flags & SENDKEY))
    {
      if (mutt_needs_mailcap (msg->content))
	mutt_edit_attachment (msg->content);
      else if (!Editor || mutt_strcmp ("builtin", Editor) == 0)
	mutt_builtin_editor (msg->content->filename, msg, cur);
      else if (option (OPTEDITHDRS))
	mutt_edit_headers (Editor, msg->content->filename, msg, fcc, sizeof (fcc));
      else
	mutt_edit_file (Editor, msg->content->filename);
    }

    if (! (flags & (SENDPOSTPONED | SENDFORWARD | SENDKEY | SENDRESEND)))
    {
      if (stat (msg->content->filename, &st) == 0)
      {
	/* if the file was not modified, bail out now */
	if (mtime == st.st_mtime &&
	    query_quadoption (OPT_ABORT, _("Abort unmodified message?")) == M_YES)
	{
	  mutt_message _("Aborted unmodified message.");
	  goto cleanup;
	}
      }
      else
	mutt_perror (msg->content->filename);
    }
  }

  /* specify a default fcc.  if we are in batchmode, only save a copy of
   * the message if the value of $copy is yes or ask-yes */

  if (!fcc[0] && !(flags & (SENDPOSTPONED)) && (!(flags & SENDBATCH) || (quadoption (OPT_COPY) & 0x1)))
  {
    /* set the default FCC */
    if (!msg->env->from)
    {
      msg->env->from = mutt_default_from ();
      killfrom = 1; /* no need to check $use_from because if the user specified
		       a from address it would have already been set by now */
    }
    mutt_select_fcc (fcc, sizeof (fcc), msg);
    if (killfrom)
    {
      rfc822_free_address (&msg->env->from);
      killfrom = 0;
    }
  }

  
  mutt_update_encoding (msg->content);

  if (! (flags & (SENDMAILX | SENDBATCH)))
  {
main_loop:

    mutt_pretty_mailbox (fcc);
    i = mutt_compose_menu (msg, fcc, sizeof (fcc), cur);
    if (i == -1)
    {
      /* abort */
      mutt_message _("Mail not sent.");
      goto cleanup;
    }
    else if (i == 1)
    {
      /* postpone the message until later. */
      if (msg->content->next)
	msg->content = mutt_make_multipart (msg->content);

      /*
       * make sure the message is written to the right part of a maildir 
       * postponed folder.
       */
      msg->read = 0; msg->old = 0;

      encode_descriptions (msg->content, 1);
      mutt_prepare_envelope (msg->env, 0);

      if (!Postponed || mutt_write_fcc (NONULL (Postponed), msg, (cur && (flags & SENDREPLY)) ? cur->env->message_id : NULL, 1, fcc) < 0)
      {
	msg->content = mutt_remove_multipart (msg->content);
	decode_descriptions (msg->content);
	mutt_unprepare_envelope (msg->env);
	goto main_loop;
      }
      mutt_update_num_postponed ();
      mutt_message _("Message postponed.");
      goto cleanup;
    }
  }

  if (!msg->env->to && !msg->env->cc && !msg->env->bcc)
  {
    if (! (flags & SENDBATCH))
    {
      mutt_error _("No recipients are specified!");
      goto main_loop;
    }
    else
    {
      puts _("No recipients were specified.");
      goto cleanup;
    }
  }

  if (!msg->env->subject && ! (flags & SENDBATCH) &&
      (i = query_quadoption (OPT_SUBJECT, _("No subject, abort sending?"))) != M_NO)
  {
    /* if the abort is automatic, print an error message */
    if (quadoption (OPT_SUBJECT) == M_YES)
      mutt_error _("No subject specified.");
    goto main_loop;
  }

  if (msg->content->next)
    msg->content = mutt_make_multipart (msg->content);

  /* 
   * Ok, we need to do it this way instead of handling all fcc stuff in
   * one place in order to avoid going to main_loop with encoded "env"
   * in case of error.  Ugh.
   */

  encode_descriptions (msg->content, 1);
  
#ifdef HAVE_PGP
  if (msg->pgp)
  {
    /* save the decrypted attachments */
    clear_content = msg->content;

    if ((pgp_get_keys (msg, &pgpkeylist) == -1) ||
	(pgp_protect (msg, pgpkeylist) == -1))
    {
      msg->content = mutt_remove_multipart (msg->content);
      
      if (pgpkeylist)
	FREE (&pgpkeylist);
      
      decode_descriptions (msg->content);
      goto main_loop;
    }
    encode_descriptions (msg->content, 0);
  }

  /* 
   * at this point, msg->content is one of the following three things:
   * - multipart/signed.  In this case, clear_content is a child.
   * - multipart/encrypted.  In this case, clear_content exists
   *   independently
   * - application/pgp.  In this case, clear_content exists independently.
   * - something else.  In this case, it's the same as clear_content.
   */

#endif /* HAVE_PGP */

  if (!option (OPTNOCURSES) && !(flags & SENDMAILX))
    mutt_message _("Sending message...");

  mutt_prepare_envelope (msg->env, 1);

  /* save a copy of the message, if necessary. */

  mutt_expand_path (fcc, sizeof (fcc));

  
  /* Don't save a copy when we are in batch-mode, and the FCC
   * folder is on an IMAP server: This would involve possibly lots
   * of user interaction, which is not available in batch mode. 
   * 
   * Note: A patch to fix the problems with the use of IMAP servers
   * from non-curses mode is available from Brendan Cully.  However, 
   * I'd like to think a bit more about this before including it.
   */

#ifdef USE_IMAP
  if ((flags & SENDBATCH) && fcc[0] && mx_is_imap (fcc))
    fcc[0] = '\0';
#endif

  if (*fcc && mutt_strcmp ("/dev/null", fcc) != 0)
  {
    BODY *tmpbody = msg->content;
#ifdef HAVE_PGP
    BODY *save_sig = NULL;
    BODY *save_parts = NULL;
#endif /* HAVE_PGP */

#ifdef HAVE_PGP
    if (msg->pgp && option (OPTFCCCLEAR))
      msg->content = clear_content;
#endif

    /* check to see if the user wants copies of all attachments */
    if (!option (OPTFCCATTACH) && msg->content->type == TYPEMULTIPART)
    {
#ifdef HAVE_PGP
      if (mutt_strcmp (msg->content->subtype, "encrypted") == 0 ||
	  mutt_strcmp (msg->content->subtype, "signed") == 0)
      {
	if (clear_content->type == TYPEMULTIPART)
	{
	  if (!(msg->pgp & PGPENCRYPT) && (msg->pgp & PGPSIGN))
	  {
	    /* save initial signature and attachments */
	    save_sig = msg->content->parts->next;
	    save_parts = clear_content->parts->next;
	  }

	  /* this means writing only the main part */
	  msg->content = clear_content->parts;

	  if (pgp_protect (msg, pgpkeylist) == -1)
	  {
	    /* we can't do much about it at this point, so
	     * fallback to saving the whole thing to fcc
	     */
	    msg->content = tmpbody;
	    save_sig = NULL;
	    goto full_fcc;
	  }

	  save_content = msg->content;
	}
      }
      else
#endif /* HAVE_PGP */
	msg->content = msg->content->parts;
    }

#ifdef HAVE_PGP
full_fcc:
#endif /* HAVE_PGP */
    if (msg->content)
    {
      /* update received time so that when storing to a mbox-style folder
       * the From_ line contains the current time instead of when the
       * message was first postponed.
       */
      msg->received = time (NULL);
      mutt_write_fcc (fcc, msg, NULL, 0, NULL);
    }

    msg->content = tmpbody;

#ifdef HAVE_PGP
    if (save_sig)
    {
      /* cleanup the second signature structures */
      if (save_content->parts)
      {
	mutt_free_body (&save_content->parts->next);
	save_content->parts = NULL;
      }
      mutt_free_body (&save_content);

      /* restore old signature and attachments */
      msg->content->parts->next = save_sig;
      msg->content->parts->parts->next = save_parts;
    }
    else if (save_content)
    {
      /* destroy the new encrypted body. */
      mutt_free_body (&save_content);
    }
      
#endif /* HAVE_PGP */
  }


  if ((i = send_message (msg)) == -1)
  {
    if (!(flags & SENDBATCH))
    {
#ifdef HAVE_PGP
      if ((msg->pgp & PGPENCRYPT) || 
	  ((msg->pgp & PGPSIGN) && msg->content->type == TYPEAPPLICATION))
      {
	mutt_free_body (&msg->content); /* destroy PGP data */
	msg->content = clear_content;	/* restore clear text. */
      }
      else if ((msg->pgp & PGPSIGN) && msg->content->type == TYPEMULTIPART)
      {
	mutt_free_body (&msg->content->parts->next);		/* destroy sig */
	msg->content = mutt_remove_multipart (msg->content);	/* remove multipart */
      }
#endif
      msg->content = mutt_remove_multipart (msg->content);
      decode_descriptions (msg->content);
      mutt_unprepare_envelope (msg->env);
      goto main_loop;
    }
    else
    {
      puts _("Could not send the message.");
      goto cleanup;
    }
  }
  else if (!option (OPTNOCURSES) && ! (flags & SENDMAILX))
    mutt_message (i == 0 ? _("Mail sent.") : _("Sending in background."));

#ifdef HAVE_PGP
  if (msg->pgp & PGPENCRYPT)
  {
    /* cleanup structures from the first encryption */
    mutt_free_body (&clear_content);
    FREE (&pgpkeylist);
  }
#endif /* HAVE_PGP */

  if (flags & SENDREPLY)
  {
    if (cur && ctx)
      mutt_set_flag (ctx, cur, M_REPLIED, 1);
    else if (!(flags & SENDPOSTPONED) && ctx && ctx->tagged)
    {
      for (i = 0; i < ctx->vcount; i++)
	if (ctx->hdrs[ctx->v2r[i]]->tagged)
	  mutt_set_flag (ctx, ctx->hdrs[ctx->v2r[i]], M_REPLIED, 1);
    }
  }


  rv = 0;
  
cleanup:



#ifdef HAVE_PGP
  if (flags & SENDPOSTPONED)
  {
    
    if(signas)
    {
      safe_free((void **) &PgpSignAs);
      PgpSignAs = signas;
    }
  }
#endif /* HAVE_PGP */
   
  safe_fclose (&tempfp);
  mutt_free_header (&msg);
  
  return rv;

}
