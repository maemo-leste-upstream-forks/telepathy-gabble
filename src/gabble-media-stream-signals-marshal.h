
#ifndef __gabble_media_stream_marshal_MARSHAL_H__
#define __gabble_media_stream_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* VOID:STRING,BOXED (gabble-media-stream-signals-marshal.list:1) */
extern void gabble_media_stream_marshal_VOID__STRING_BOXED (GClosure     *closure,
                                                            GValue       *return_value,
                                                            guint         n_param_values,
                                                            const GValue *param_values,
                                                            gpointer      invocation_hint,
                                                            gpointer      marshal_data);

/* VOID:STRING,STRING (gabble-media-stream-signals-marshal.list:2) */
extern void gabble_media_stream_marshal_VOID__STRING_STRING (GClosure     *closure,
                                                             GValue       *return_value,
                                                             guint         n_param_values,
                                                             const GValue *param_values,
                                                             gpointer      invocation_hint,
                                                             gpointer      marshal_data);

G_END_DECLS

#endif /* __gabble_media_stream_marshal_MARSHAL_H__ */

