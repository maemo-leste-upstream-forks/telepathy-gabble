#ifndef __TELEPATHY_YELL_EXTENSIONS_H__
#define __TELEPATHY_YELL_EXTENSIONS_H__

#include <glib-object.h>
#include <telepathy-glib/channel.h>
#include <telepathy-glib/connection.h>

#include <telepathy-yell/enums.h>
#include <telepathy-yell/cli-call.h>
#include <telepathy-yell/svc-call.h>

G_BEGIN_DECLS

#include <telepathy-yell/gtypes.h>
#include <telepathy-yell/interfaces.h>

void tpy_cli_init (void);

G_END_DECLS

#endif
