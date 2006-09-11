
/* Generated data (by glib-mkenums) */

#include <telepathy-errors.h>

/* enumerations from "telepathy-errors.h" */
GType
telepathy_errors_get_type (void)
{
  static GType etype = 0;
  if (etype == 0) {
    static const GEnumValue values[] = {
      { ChannelBanned, "ChannelBanned", "ChannelBanned" },
      { ChannelFull, "ChannelFull", "ChannelFull" },
      { ChannelInviteOnly, "ChannelInviteOnly", "ChannelInviteOnly" },
      { Disconnected, "Disconnected", "Disconnected" },
      { InvalidArgument, "InvalidArgument", "InvalidArgument" },
      { InvalidHandle, "InvalidHandle", "InvalidHandle" },
      { NetworkError, "NetworkError", "NetworkError" },
      { NotAvailable, "NotAvailable", "NotAvailable" },
      { NotImplemented, "NotImplemented", "NotImplemented" },
      { PermissionDenied, "PermissionDenied", "PermissionDenied" },
      { 0, NULL, NULL }
    };
    etype = g_enum_register_static ("TelepathyErrors", values);
  }
  return etype;
}

/* Generated data ends here */

