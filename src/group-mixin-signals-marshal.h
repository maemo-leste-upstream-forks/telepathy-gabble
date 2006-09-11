
#ifndef __group_mixin_marshal_MARSHAL_H__
#define __group_mixin_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* VOID:UINT,UINT (group-mixin-signals-marshal.list:1) */
extern void group_mixin_marshal_VOID__UINT_UINT (GClosure     *closure,
                                                 GValue       *return_value,
                                                 guint         n_param_values,
                                                 const GValue *param_values,
                                                 gpointer      invocation_hint,
                                                 gpointer      marshal_data);

/* VOID:STRING,BOXED,BOXED,BOXED,BOXED,UINT,UINT (group-mixin-signals-marshal.list:2) */
extern void group_mixin_marshal_VOID__STRING_BOXED_BOXED_BOXED_BOXED_UINT_UINT (GClosure     *closure,
                                                                                GValue       *return_value,
                                                                                guint         n_param_values,
                                                                                const GValue *param_values,
                                                                                gpointer      invocation_hint,
                                                                                gpointer      marshal_data);

G_END_DECLS

#endif /* __group_mixin_marshal_MARSHAL_H__ */

