/*
 * debug.c - debugging utilities for telepathy-yell
 * Copyright (C) 2010 Collabora Ltd.
 * @author Jonathon Jongsma <jonathon.jongsma@collabora.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include <config.h>

#include <telepathy-glib/debug.h>
#include <telepathy-glib/debug-sender.h>
#include "debug.h"

static TpyDebugFlags flags = 0;

static GDebugKey debug_keys[] = {
  {"call", TPY_DEBUG_CALL},
  {NULL, 0}
};

void
tpy_debug_set_flags (const char *flags_string)
{
  guint nkeys;

  for (nkeys = 0; debug_keys[nkeys].value; nkeys++);

  flags |= g_parse_debug_string (flags_string, debug_keys, nkeys);
}

static const char *
debug_flag_to_domain (TpyDebugFlags flag)
{
  static GHashTable *flag_to_domains = NULL;

  if (G_UNLIKELY (flag_to_domains == NULL))
    {
      guint i;

      flag_to_domains = g_hash_table_new_full (g_direct_hash, g_direct_equal,
          NULL, g_free);

      for (i = 0; debug_keys[i].value; i++)
        {
          GDebugKey key = debug_keys[i];
          char *val = g_strdup_printf ("%s/%s", G_LOG_DOMAIN, key.key);

          g_hash_table_insert (flag_to_domains,
              GUINT_TO_POINTER (key.value), val);
        }
    }

  return g_hash_table_lookup (flag_to_domains, GUINT_TO_POINTER (flag));
}

void tpy_log (GLogLevelFlags level,
                TpyDebugFlags flag,
                const gchar *format,
                ...)
{
  TpDebugSender *debug_sender = tp_debug_sender_dup ();
  char *message;
  va_list args;
  GTimeVal now;

  va_start (args, format);
  message = g_strdup_vprintf (format, args);
  va_end (args);

  if (flag & flags)
    g_log (G_LOG_DOMAIN, level, "%s", message);

  g_get_current_time (&now);

  tp_debug_sender_add_message (debug_sender, &now, debug_flag_to_domain (flag),
      level, message);

  g_free (message);
  g_object_unref (debug_sender);
}
