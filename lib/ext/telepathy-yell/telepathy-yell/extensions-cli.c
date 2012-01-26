#include "extensions.h"

#include <telepathy-glib/channel.h>
#include <telepathy-glib/connection.h>
#include <telepathy-glib/proxy-subclass.h>

#include "_gen/signals-marshal.h"

/* include auto-generated stubs for client-specific code */
#include "_gen/cli-call-body.h"
#include "_gen/register-dbus-glib-marshallers-body.h"

static gpointer
tpy_cli_once (gpointer unused)
{
  _tpy_register_dbus_glib_marshallers ();

  tp_channel_init_known_interfaces ();
  tp_connection_init_known_interfaces ();

  tp_proxy_or_subclass_hook_on_interface_add (TP_TYPE_PROXY,
      tpy_cli_call_add_signals);

  return NULL;
}

void
tpy_cli_init (void)
{
  static GOnce once = G_ONCE_INIT;

  g_once (&once, tpy_cli_once, NULL);
}
