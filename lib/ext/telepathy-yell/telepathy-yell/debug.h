/*
 * debug.h - debugging utilities for telepathy-yell
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

#ifndef __TPY_DEBUG_H__
#define __TPY_DEBUG_H__

#include "config.h"
#include <glib.h>

G_BEGIN_DECLS

typedef enum
{
  TPY_DEBUG_CALL = 1 << 0
} TpyDebugFlags;

void tpy_debug_set_flags (const char *flags_string);

void tpy_log (GLogLevelFlags level, TpyDebugFlags flag,
                const gchar *format, ...) G_GNUC_PRINTF(3, 4);

#ifdef DEBUG_FLAG

#define ERROR(format, ...) \
  tpy_log (G_LOG_LEVEL_ERROR, DEBUG_FLAG, "%s: " format, \
             G_STRFUNC, ##__VA_ARGS__)
#define CRITICAL(format, ...) \
  tpy_log (G_LOG_LEVEL_CRITICAL, DEBUG_FLAG, "%s: " format, \
             G_STRFUNC, ##__VA_ARGS__)
#define WARNING(format, ...) \
  tpy_log (G_LOG_LEVEL_WARNING, DEBUG_FLAG, "%s: " format, \
             G_STRFUNC, ##__VA_ARGS__)
#define MESSAGE(format, ...) \
  tpy_log (G_LOG_LEVEL_MESSAGE, DEBUG_FLAG, "%s: " format, \
             G_STRFUNC, ##__VA_ARGS__)
#define INFO(format, ...) \
  tpy_log (G_LOG_LEVEL_INFO, DEBUG_FLAG, "%s: " format, \
             G_STRFUNC, ##__VA_ARGS__)
#define DEBUG(format, ...) \
  tpy_log (G_LOG_LEVEL_DEBUG, DEBUG_FLAG, "%s: " format, \
             G_STRFUNC, ##__VA_ARGS__)

#endif /* DEBUG_FLAG */

G_END_DECLS

#endif /* __TPY_DEBUG_H__ */
