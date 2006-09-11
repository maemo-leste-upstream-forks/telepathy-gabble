
#ifndef __text_mixin_marshal_MARSHAL_H__
#define __text_mixin_marshal_MARSHAL_H__

#include	<glib-object.h>

G_BEGIN_DECLS

/* VOID:UINT,UINT,UINT,UINT,UINT,STRING (text-mixin-signals-marshal.list:1) */
extern void text_mixin_marshal_VOID__UINT_UINT_UINT_UINT_UINT_STRING (GClosure     *closure,
                                                                      GValue       *return_value,
                                                                      guint         n_param_values,
                                                                      const GValue *param_values,
                                                                      gpointer      invocation_hint,
                                                                      gpointer      marshal_data);

/* VOID:UINT,UINT,UINT,STRING (text-mixin-signals-marshal.list:2) */
extern void text_mixin_marshal_VOID__UINT_UINT_UINT_STRING (GClosure     *closure,
                                                            GValue       *return_value,
                                                            guint         n_param_values,
                                                            const GValue *param_values,
                                                            gpointer      invocation_hint,
                                                            gpointer      marshal_data);

/* VOID:UINT,UINT,STRING (text-mixin-signals-marshal.list:3) */
extern void text_mixin_marshal_VOID__UINT_UINT_STRING (GClosure     *closure,
                                                       GValue       *return_value,
                                                       guint         n_param_values,
                                                       const GValue *param_values,
                                                       gpointer      invocation_hint,
                                                       gpointer      marshal_data);

G_END_DECLS

#endif /* __text_mixin_marshal_MARSHAL_H__ */

