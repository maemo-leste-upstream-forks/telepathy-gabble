/*
 * gabble-error.h - Header for Gabble's error handling API
 * Copyright (C) 2006-2007 Collabora Ltd.
 * Copyright (C) 2006 Nokia Corporation
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

#ifndef __GABBLE_ERROR_H__
#define __GABBLE_ERROR_H__

#include <gabble/gabble.h>

#include <glib.h>
#include <loudmouth/loudmouth.h>

typedef enum {
    XMPP_ERROR_TYPE_UNDEFINED = 0,
    XMPP_ERROR_TYPE_CANCEL,
    XMPP_ERROR_TYPE_CONTINUE,
    XMPP_ERROR_TYPE_MODIFY,
    XMPP_ERROR_TYPE_AUTH,
    XMPP_ERROR_TYPE_WAIT,
} GabbleXmppErrorType;

typedef enum {
    XMPP_ERROR_UNDEFINED_CONDITION = 0, /* 500 */
    XMPP_ERROR_REDIRECT,                /* 302 */
    XMPP_ERROR_GONE,                    /* 302 */

    XMPP_ERROR_BAD_REQUEST,             /* 400 */
    XMPP_ERROR_UNEXPECTED_REQUEST,      /* 400 */
    XMPP_ERROR_JID_MALFORMED,           /* 400 */

    XMPP_ERROR_NOT_AUTHORIZED,          /* 401 */

    XMPP_ERROR_PAYMENT_REQUIRED,        /* 402 */

    XMPP_ERROR_FORBIDDEN,               /* 403 */

    XMPP_ERROR_ITEM_NOT_FOUND,          /* 404 */
    XMPP_ERROR_RECIPIENT_UNAVAILABLE,   /* 404 */
    XMPP_ERROR_REMOTE_SERVER_NOT_FOUND, /* 404 */

    XMPP_ERROR_NOT_ALLOWED,             /* 405 */

    XMPP_ERROR_NOT_ACCEPTABLE,          /* 406 */

    XMPP_ERROR_REGISTRATION_REQUIRED,   /* 407 */
    XMPP_ERROR_SUBSCRIPTION_REQUIRED,   /* 407 */

    XMPP_ERROR_REMOTE_SERVER_TIMEOUT,   /* 408, 504 */

    XMPP_ERROR_CONFLICT,                /* 409 */

    XMPP_ERROR_INTERNAL_SERVER_ERROR,   /* 500 */
    XMPP_ERROR_RESOURCE_CONSTRAINT,     /* 500 */

    XMPP_ERROR_FEATURE_NOT_IMPLEMENTED, /* 501 */

    XMPP_ERROR_SERVICE_UNAVAILABLE,     /* 502, 503, 510 */

    XMPP_ERROR_JINGLE_OUT_OF_ORDER,
    XMPP_ERROR_JINGLE_UNKNOWN_SESSION,
    XMPP_ERROR_JINGLE_TIE_BREAK,
    XMPP_ERROR_JINGLE_UNSUPPORTED_INFO,

    XMPP_ERROR_SI_NO_VALID_STREAMS,
    XMPP_ERROR_SI_BAD_PROFILE,

    NUM_XMPP_ERRORS,
} GabbleXmppError;

GQuark gabble_xmpp_error_quark (void);
#define GABBLE_XMPP_ERROR (gabble_xmpp_error_quark ())

GabbleXmppError gabble_xmpp_error_from_node (LmMessageNode *error_node,
    GabbleXmppErrorType *type_out);
LmMessageNode *gabble_xmpp_error_to_node (GabbleXmppError error,
    LmMessageNode *parent_node, const gchar *errmsg);
const gchar *gabble_xmpp_error_string (GabbleXmppError error);
const gchar *gabble_xmpp_error_description (GabbleXmppError error);
GError *gabble_message_get_xmpp_error (LmMessage *msg);

GabbleXmppErrorType gabble_xmpp_error_type_to_enum (const gchar *error_type);

#endif /* __GABBLE_ERROR_H__ */
