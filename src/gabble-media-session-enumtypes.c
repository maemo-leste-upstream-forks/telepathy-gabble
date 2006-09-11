
/* Generated data (by glib-mkenums) */

#include <gabble-media-session.h>

/* enumerations from "gabble-media-session.h" */
GType
gabble_media_session_mode_get_type (void)
{
  static GType etype = 0;
  if (etype == 0) {
    static const GEnumValue values[] = {
      { MODE_GOOGLE, "MODE_GOOGLE", "MODE_GOOGLE" },
      { MODE_JINGLE, "MODE_JINGLE", "MODE_JINGLE" },
      { 0, NULL, NULL }
    };
    etype = g_enum_register_static ("GabbleMediaSessionMode", values);
  }
  return etype;
}
GType
jingle_session_state_get_type (void)
{
  static GType etype = 0;
  if (etype == 0) {
    static const GEnumValue values[] = {
      { JS_STATE_INVALID, "JS_STATE_INVALID", "JS_STATE_INVALID" },
      { JS_STATE_PENDING_CREATED, "JS_STATE_PENDING_CREATED", "JS_STATE_PENDING_CREATED" },
      { JS_STATE_PENDING_INITIATED, "JS_STATE_PENDING_INITIATED", "JS_STATE_PENDING_INITIATED" },
      { JS_STATE_ACTIVE, "JS_STATE_ACTIVE", "JS_STATE_ACTIVE" },
      { JS_STATE_ENDED, "JS_STATE_ENDED", "JS_STATE_ENDED" },
      { 0, NULL, NULL }
    };
    etype = g_enum_register_static ("JingleSessionState", values);
  }
  return etype;
}
GType
debug_message_type_get_type (void)
{
  static GType etype = 0;
  if (etype == 0) {
    static const GEnumValue values[] = {
      { DEBUG_MSG_INFO, "DEBUG_MSG_INFO", "DEBUG_MSG_INFO" },
      { DEBUG_MSG_DUMP, "DEBUG_MSG_DUMP", "DEBUG_MSG_DUMP" },
      { DEBUG_MSG_WARNING, "DEBUG_MSG_WARNING", "DEBUG_MSG_WARNING" },
      { DEBUG_MSG_ERROR, "DEBUG_MSG_ERROR", "DEBUG_MSG_ERROR" },
      { DEBUG_MSG_EVENT, "DEBUG_MSG_EVENT", "DEBUG_MSG_EVENT" },
      { 0, NULL, NULL }
    };
    etype = g_enum_register_static ("DebugMessageType", values);
  }
  return etype;
}

/* Generated data ends here */

