/* vim:set et ts=4: */
/* IBus - The Input Bus
 * Copyright (C) 2008-2009 Huang Peng <shawn.p.huang@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
#include <config.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <string.h>
#include <stdarg.h>
#include <glib/gstdio.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>

#ifdef HAVE_SYS_INOTIFY_H
#define HAVE_INOTIFY
#  include <sys/inotify.h>
#endif

#include "ibusimclient.h"

#define IBUS_NAME  "org.freedesktop.IBus"
#define IBUS_IFACE "org.freedesktop.IBus"
#define IBUS_PATH  "/org/freedesktop/IBus"

/* IBusIMClientPriv */
struct _IBusIMClientPrivate {
#if USE_DBUS_SESSION_BUS
    DBusConnection  *dbus;
#endif

#ifdef HAVE_INOTIFY
    /* inotify */
    gint            inotify_wd;
    GIOChannel     *inotify_channel;
    guint           inotify_source;
#endif

    DBusConnection *ibus;

    GHashTable     *ic_table;
    GList          *contexts;
};

/* functions prototype */
static void     ibus_im_client_class_init   (IBusIMClientClass  *klass);
static void     ibus_im_client_init         (IBusIMClient       *client);
static void     ibus_im_client_finalize     (GObject            *obj);

static const gchar *
                _ibus_im_client_create_input_context
                                            (IBusIMClient       *client);

static gboolean _ibus_call_with_reply_and_block
                                           (DBusConnection      *connection,
                                            const gchar         *method,
                                            int                 first_arg_type,
                                                                ...);
static gboolean _ibus_call_with_reply      (DBusConnection      *connection,
                                            const gchar         *method,
                                            DBusPendingCallNotifyFunction
                                                                function,
                                            void                *data,
                                            DBusFreeFunction    free_function,
                                            int                 first_arg_type,
                                                                ...);
static gboolean _dbus_call_with_reply_and_block
                                           (DBusConnection      *connection,
                                            const gchar         *dest,
                                            const gchar         *path,
                                            const gchar         *iface,
                                            const char          *method,
                                            int                 first_arg_type,
                                                                ...);
static GtkIMContext *
                _ibus_client_ic_to_context (IBusIMClient        *client,
                                            const gchar         *ic);

/* callback functions */
static DBusHandlerResult
                _ibus_im_client_message_filter_cb
                                           (DBusConnection      *connection,
                                            DBusMessage         *message,
                                            void                *user_data);

static void     _dbus_name_owner_changed_cb
                                           (DBusGProxy          *proxy,
                                            const gchar         *name,
                                            const gchar         *old_name,
                                            const gchar         *new_name,
                                            IBusIMClient        *client);

static GType ibus_type_im_client = 0;
static GtkObjectClass *parent_class = NULL;


GType
ibus_im_client_get_type (void)
{
    g_assert (ibus_type_im_client != 0);
    return ibus_type_im_client;
}

void
ibus_im_client_register_type (GTypeModule *type_module)
{
    static const GTypeInfo ibus_im_client_info = {
        sizeof (IBusIMClientClass),
        (GBaseInitFunc)        NULL,
        (GBaseFinalizeFunc)     NULL,
        (GClassInitFunc)     ibus_im_client_class_init,
        NULL,            /* class finialize */
        NULL,            /* class data */
        sizeof (IBusIMClient),
        0,
        (GInstanceInitFunc)    ibus_im_client_init,
    };

    if (! ibus_type_im_client ) {
        if (type_module) {
            ibus_type_im_client =
                g_type_module_register_type (type_module,
                    GTK_TYPE_OBJECT,
                    "IBusIMClient",
                    &ibus_im_client_info,
                    (GTypeFlags)0);
        }
        else {
            ibus_type_im_client =
                g_type_register_static (GTK_TYPE_OBJECT,
                    "IBusIMClient",
                    &ibus_im_client_info,
                    (GTypeFlags)0);
        }
    }
}

IBusIMClient *
ibus_im_client_new (void)
{
    IBusIMClient *client;

    client = IBUS_IM_CLIENT(g_object_new (IBUS_TYPE_IM_CLIENT, NULL));

    return client;
}

static void
ibus_im_client_class_init     (IBusIMClientClass *klass)
{
    GtkObjectClass *object_class = GTK_OBJECT_CLASS (klass);
    GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

    parent_class = (GtkObjectClass *) g_type_class_peek_parent (klass);

    g_type_class_add_private (klass, sizeof (IBusIMClientPrivate));

    gobject_class->finalize = ibus_im_client_finalize;
}

/*
 * open ibus connection
 */
static void
_ibus_im_client_ibus_open (IBusIMClient *client)
{
    gchar *ibus_addr = NULL;
    DBusError error;

    IBusIMClientPrivate *priv = client->priv;

    if (priv->ibus != NULL)
        return;

#if USE_DBUS_SESSION_BUS
    dbus_connection_setup_with_g_main (priv->dbus, NULL);
    if (!_dbus_call_with_reply_and_block (priv->dbus,
                        IBUS_NAME, IBUS_PATH, IBUS_IFACE,
                        "GetIBusAddress",
                        DBUS_TYPE_INVALID,
                        DBUS_TYPE_STRING, &ibus_addr,
                        DBUS_TYPE_INVALID
                        )) {
        g_warning ("Can not get ibus address");
        return;
    }
#endif
    if (ibus_addr == NULL) {
        gchar *display;
        gchar *hostname = "";
        gchar *displaynumber = "0";
        gchar *screennumber = "0";
        gchar *username = NULL;
        gchar *p;

        display = g_strdup (g_getenv ("DISPLAY"));
        if (display == NULL) {
            g_warning ("DISPLAY is empty! We use default DISPLAY (:0.0)");
        }
        else {
            p = display;
            hostname = display;
            for (; *p != ':' && *p != '\0'; p++);

            if (*p == ':') {
                *p = '\0';
                p++;
                displaynumber = p;
            }

            for (; *p != '.' && *p != '\0'; p++);

            if (*p == '.') {
                *p = '\0';
                p++;
                screennumber = p;
            }
        }

        username = getlogin();
        if (username == NULL)
            username = getenv("LOGNAME");
        if (username == NULL)
            username = getenv("USER");
        if (username == NULL)
            username = getenv("LNAME");
        if (username == NULL)
            username = getenv("USERNAME");

        ibus_addr = g_strdup_printf (
            "unix:path=/tmp/ibus-%s/ibus-%s-%s.%s",
            username, hostname, displaynumber, screennumber);

        g_free (display);
    }

    /*
     * Init ibus and proxy object
     */
    dbus_error_init (&error);
    priv->ibus = dbus_connection_open_private (ibus_addr, &error);
    g_free (ibus_addr);
    if (priv->ibus == NULL) {
        g_warning ("Error: %s", error.message);
        dbus_error_free (&error);
        return;
    }

    if (!dbus_connection_add_filter (priv->ibus,
            _ibus_im_client_message_filter_cb,
            client, NULL)) {
        g_warning ("Out of memory");
        return;
    }
    dbus_connection_setup_with_g_main (priv->ibus, NULL);

    GList *p;
    for (p = priv->contexts; p != NULL; p = g_list_next (p)) {
        IBusIMContext *context = IBUS_IM_CONTEXT (p->data);
        const gchar *ic = _ibus_im_client_create_input_context (client);
        g_hash_table_insert (priv->ic_table, g_strdup (ic), context);
        ibus_im_context_set_ic (context, ic);
    }

}

/*
 * close ibus connection
 */
static void
_ibus_im_client_ibus_close (IBusIMClient *client)
{
    IBusIMClientPrivate *priv = client->priv;

    GList *p;
    for (p = priv->contexts; p != NULL; p = g_list_next (p)) {
        IBusIMContext *ctx = IBUS_IM_CONTEXT (p->data);
        ibus_im_context_set_ic (ctx, NULL);
    }

    g_hash_table_remove_all (priv->ic_table);

    if (priv->ibus) {
        dbus_connection_close (priv->ibus);
        dbus_connection_unref (priv->ibus);
        priv->ibus = NULL;
    }
}

/*
 * create an im context
 */
IBusIMContext *
ibus_im_client_create_im_context (IBusIMClient *client)
{
    IBusIMContext *context;
    IBusIMClientPrivate *priv = client->priv;

    context = IBUS_IM_CONTEXT (ibus_im_context_new ());
    priv->contexts = g_list_append (priv->contexts, context);

    const gchar *ic = _ibus_im_client_create_input_context (client);
    ibus_im_context_set_ic (context, ic);
    if (ic) {
        g_hash_table_insert (priv->ic_table, (gpointer)g_strdup (ic), context);
    }
    return context;
}

/*
 * create a ibus input context
 */
static const gchar *
_ibus_im_client_create_input_context (IBusIMClient *client)
{
    IBusIMClientPrivate *priv = client->priv;

    if (priv->ibus == NULL)
        return NULL;

    const gchar *app_name = g_get_application_name ();
    gchar *ic = NULL;
    _ibus_call_with_reply_and_block (priv->ibus, "CreateInputContext",
                DBUS_TYPE_STRING, &app_name,
                DBUS_TYPE_INVALID,
                DBUS_TYPE_STRING, &ic,
                DBUS_TYPE_INVALID);
    return ic;
}

#ifdef HAVE_INOTIFY
static gboolean
_ibus_im_client_inotify_cb (GIOChannel *source, GIOCondition condition, IBusIMClient *client)
{
    struct inotify_event *p = NULL;
    gchar *name;
    gsize n;

    if (condition & G_IO_IN == 0)
        return TRUE;

    p = g_malloc0 (sizeof (struct inotify_event) + 1024);

    g_io_channel_read_chars (source, (gchar *) p, sizeof (struct inotify_event),  &n, NULL);
    g_io_channel_read_chars (source, ((gchar *)p) + sizeof (struct inotify_event), p->len,  &n, NULL);

    name = g_strdup_printf ("ibus-%s", g_getenv ("DISPLAY"));
    for (n = 0; name[n] != 0; n++) {
        if (name[n] != ':')
            continue;
        name[n] = '-';
        break;
    }

    if (g_strcmp0 (p->name, name) == 0) {
        if (p->mask & IN_CREATE) {
            g_usleep (1000);
            _ibus_im_client_ibus_open (client);
        }
    }
    g_free (name);
    g_free (p);

}
#endif

static void
ibus_im_client_init (IBusIMClient *obj)
{
    DEBUG_FUNCTION_IN;

    DBusError error;
    IBusIMClient *client = IBUS_IM_CLIENT (obj);
    IBusIMClientPrivate *priv;

    gchar *watch_path;
    struct stat stat_buf;

#ifdef HAVE_INOTIFY
    gint inotify_fd = inotify_init ();
#endif

    priv = G_TYPE_INSTANCE_GET_PRIVATE (client, IBUS_TYPE_IM_CLIENT, IBusIMClientPrivate);
    client->priv = priv;

    priv->ic_table = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    priv->contexts = NULL;

    watch_path = g_strdup_printf ("/tmp/ibus-%s", g_get_user_name ());

    if (g_stat (watch_path, &stat_buf) != 0) {
        g_mkdir (watch_path, 0750);
    }

#ifdef HAVE_INOTIFY
    /* init inotify */
    priv->inotify_wd = inotify_add_watch (inotify_fd, watch_path, IN_CREATE | IN_DELETE);
    priv->inotify_channel = g_io_channel_unix_new (inotify_fd);
    g_io_channel_set_close_on_unref (priv->inotify_channel, TRUE);
    priv->inotify_source = g_io_add_watch (priv->inotify_channel,
                                    G_IO_IN,
                                    (GIOFunc)_ibus_im_client_inotify_cb,
                                    (gpointer)client);
#endif
    g_free (watch_path);

#if USE_DBUS_SESSION_BUS
    /*
     * Init dbus
     */
    dbus_error_init (&error);
    priv->dbus = dbus_bus_get (DBUS_BUS_SESSION, &error);
    if (priv->dbus == NULL) {
        g_warning ("Error: %s", error.message);
        dbus_error_free (&error);
        return;
    }
#endif

    _ibus_im_client_ibus_open (client);

#if USE_DBUS_SESSION_BUS
    if (!dbus_connection_add_filter (priv->dbus,
            _ibus_im_client_message_filter_cb,
            client, NULL)) {
        g_warning ("Out of memory");
        return;
    }

    gchar *rule =
            "type='signal',"
            "sender='" DBUS_SERVICE_DBUS "',"
            "interface='" DBUS_INTERFACE_DBUS "',"
            "member='NameOwnerChanged',"
            "path='" DBUS_PATH_DBUS "',"
            "arg0='" IBUS_NAME "'";

    if (!_dbus_call_with_reply_and_block (priv->dbus,
                        DBUS_SERVICE_DBUS, DBUS_PATH_DBUS, DBUS_INTERFACE_DBUS,
                        "AddMatch",
                        DBUS_TYPE_STRING, &rule,
                        DBUS_TYPE_INVALID,
                        DBUS_TYPE_INVALID
                        )) {
        g_warning ("Can not get ibus address");
        return;
    }
#endif
#if 0
    /* get dbus proxy */
    priv->dbus = dbus_g_proxy_new_for_name (priv->ibus,
                                DBUS_SERVICE_DBUS,
                                DBUS_PATH_DBUS,
                                DBUS_INTERFACE_DBUS);
    g_assert (priv->dbus != NULL);

    /* connect NameOwnerChanged signal */
    dbus_g_proxy_add_signal (priv->dbus, "NameOwnerChanged",
                                G_TYPE_STRING,
                                G_TYPE_STRING,
                                G_TYPE_STRING,
                                G_TYPE_INVALID);

    dbus_g_proxy_connect_signal (priv->dbus, "NameOwnerChanged",
                                G_CALLBACK (_dbus_name_owner_changed_cb),
                                (gpointer)client, NULL);
    dbus_bus_add_match ((DBusConnection *)dbus_g_connection_get_connection (priv->ibus),
                        "type='signal',"
                        "sender='" DBUS_SERVICE_DBUS
                        "',interface='" DBUS_INTERFACE_DBUS
                        "',path='" DBUS_PATH_DBUS
                        "',member='NameOwnerChanged',"
                        "arg0='" IBUS_DBUS_SERVICE "'",
                        &dbus_error);

     _ibus_im_client_reinit_imm (client);
#endif

}


static void
ibus_im_client_finalize (GObject *obj)
{
    DEBUG_FUNCTION_IN;

    IBusIMClient *client = IBUS_IM_CLIENT (obj);
    IBusIMClientPrivate *priv = client->priv;

    g_assert (client == _client);

#ifdef HAVE_INOTIFY
    g_source_remove (priv->inotify_source);
    g_io_channel_unref (priv->inotify_channel);
#endif

#if USE_DBUS_SESSION_BUS
    if (priv->dbus) {
        dbus_connection_unref (priv->dbus);
    }
#endif
    _ibus_im_client_ibus_close (client);

    G_OBJECT_CLASS(parent_class)->finalize (obj);

    _client = NULL;
}


static void
ibus_im_client_commit_string (IBusIMClient *client, const gchar *ic, const gchar *string)
{
    IBusIMClientPrivate *priv = client->priv;
    IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);

    if (context == NULL) {
        g_debug ("Can not find context assocate with ic(%s)", ic);
        return;
    }
    g_signal_emit_by_name (G_OBJECT (context), "commit", string);
}

static void
ibus_im_client_update_preedit (IBusIMClient *client, const gchar *ic, const gchar *string,
        PangoAttrList *attrs, gint cursor_pos, gboolean visible)
{
    IBusIMClientPrivate *priv = client->priv;
    IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);

    if (context == NULL) {
        g_debug ("Can not find context assocate with ic(%s)", ic);
        return;
    }
    ibus_im_context_update_preedit (context, string, attrs, cursor_pos, visible);
}

static void
_ibus_signal_commit_string_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    /* Handle CommitString signal */
    IBusIMClientPrivate *priv = client->priv;
    DBusError error = {0};
    gchar *ic = NULL;
    gchar *string = NULL;

    if (!dbus_message_get_args (message, &error,
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_STRING, &string,
            DBUS_TYPE_INVALID)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
    }
    else {
        IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);
        if (context == NULL) {
            g_debug ("Can not find context assocate with ic(%s)", ic);
            return;
        }
        ibus_im_context_commit_string (context, string);
    }
}

static void
_ibus_signal_update_preedit_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    /* Handle UpdatePreedit signal */
    IBusIMClientPrivate *priv = client->priv;
    DBusError error = {0};
    DBusMessageIter iter, sub_iter;
    int type, sub_type;

    gchar *ic = NULL;
    gchar *string = NULL;
    PangoAttrList *attrs = NULL;
    int cursor = 0;
    gboolean visible = False;

    if (!dbus_message_iter_init (message, &iter)) {
        g_warning ("The UpdatePreedit signal does have args!");
        return;
    }

    type = dbus_message_iter_get_arg_type (&iter);
    if (type != DBUS_TYPE_STRING) {
        g_warning ("The 1st argument of UpdatePreedit signal must be a String");
        return;
    }
    dbus_message_iter_get_basic (&iter, &ic);
    dbus_message_iter_next (&iter);

    type = dbus_message_iter_get_arg_type (&iter);
    if (type != DBUS_TYPE_STRING) {
        g_warning ("The 2nd argument of UpdatePreedit signal must be a String");
        return;
    }
    dbus_message_iter_get_basic (&iter, &string);
    dbus_message_iter_next (&iter);


    type = dbus_message_iter_get_arg_type (&iter);
    if (type != DBUS_TYPE_ARRAY) {
        g_warning ("The 3rd argument of UpdatePreedit signal must be a Struct Array");
        return;
    }

    dbus_message_iter_recurse (&iter, &sub_iter);

    if (dbus_message_iter_get_arg_type (&sub_iter) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type (&sub_iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type (&sub_iter) != DBUS_TYPE_UINT32 ) {
            g_warning ("The 3rd argument of UpdatePreedit signal must be a Struct Array");
            return;
        }

        attrs = pango_attr_list_new ();

        while ((sub_type = dbus_message_iter_get_arg_type (&sub_iter) != DBUS_TYPE_INVALID)) {
            PangoAttribute *attr;
            DBusMessageIter sub_sub_iter;
            guint *values = NULL;
            gint length = 0;
            dbus_message_iter_recurse (&sub_iter, &sub_sub_iter);
            dbus_message_iter_get_fixed_array (&sub_sub_iter, &values, &length);

            if (length <= 0) {
                g_warning ("The element of the 3rd argument of UpdatePreedit should not be a empty array");
                continue;
            }

            switch (values[0]) {
            case 1: /* Underline */
                attr = pango_attr_underline_new (values[1]);
                attr->start_index = g_utf8_offset_to_pointer (string, values[2]) - string;
                attr->end_index = g_utf8_offset_to_pointer (string, values[3]) - string;
                pango_attr_list_insert (attrs, attr);
                break;

            case 2: /* Foreground Color */
                attr = pango_attr_foreground_new (
                                (values[1] & 0xff0000) >> 8,
                                (values[1] & 0x00ff00),
                                (values[1] & 0x0000ff) << 8
                                );
                attr->start_index = g_utf8_offset_to_pointer (string, values[2]) - string;
                attr->end_index = g_utf8_offset_to_pointer (string, values[3]) - string;
                pango_attr_list_insert (attrs, attr);
                break;
            case 3: /* Background Color */
                attr = pango_attr_background_new (
                                (values[1] & 0xff0000) >> 8,
                                (values[1] & 0x00ff00),
                                (values[1] & 0x0000ff) << 8
                                );
                attr->start_index = g_utf8_offset_to_pointer (string, values[2]) - string;
                attr->end_index = g_utf8_offset_to_pointer (string, values[3]) - string;
                pango_attr_list_insert (attrs, attr);
                break;
            default:
                g_warning ("Unkown type attribute type = %d", values[0]);

            }

            dbus_message_iter_next (&sub_iter);

        }
    }
    dbus_message_iter_next (&iter);

    type = dbus_message_iter_get_arg_type (&iter);
    if (type != DBUS_TYPE_INT32) {
        g_warning ("The 4th argument of UpdatePreedit signal must be an Int32 %c", type);
        pango_attr_list_unref (attrs);
        return;
    }
    dbus_message_iter_get_basic (&iter, &cursor);
    dbus_message_iter_next (&iter);

    type = dbus_message_iter_get_arg_type (&iter);
    if (type != DBUS_TYPE_BOOLEAN) {
        g_warning ("The 4th argument of UpdatePreedit signal must be an Int32 %c", type);
        pango_attr_list_unref (attrs);
        return;
    }
    dbus_message_iter_get_basic (&iter, &visible);
    dbus_message_iter_next (&iter);

    {
        IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);
        if (context == NULL) {
            g_debug ("Can not find context assocate with ic(%s)", ic);
            return;
        }
        ibus_im_context_update_preedit (context, string, attrs, cursor, visible);
    }
    pango_attr_list_unref (attrs);

}

static void
_ibus_signal_show_preedit_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    /* Handle CommitString signal */
    IBusIMClientPrivate *priv = client->priv;
    DBusError error = {0};
    gchar *ic = NULL;

    if (!dbus_message_get_args (message, &error,
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
    }
    else {
        IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);
        if (context == NULL) {
            g_debug ("Can not find context assocate with ic(%s)", ic);
            return;
        }
        ibus_im_context_show_preedit (context);
    }
}

static void
_ibus_signal_hide_preedit_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    /* Handle CommitString signal */
    IBusIMClientPrivate *priv = client->priv;
    DBusError error = {0};
    gchar *ic = NULL;

    if (!dbus_message_get_args (message, &error,
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
    }
    else {
        IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);
        if (context == NULL) {
            g_debug ("Can not find context assocate with ic(%s)", ic);
            return;
        }
        ibus_im_context_hide_preedit (context);
    }
}

#ifdef USE_DBUS_SESSION_BUS
static void
_ibus_signal_name_owner_changed_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    IBusIMClientPrivate *priv = client->priv;

    gchar *name = NULL;
    gchar *old_name = NULL;
    gchar *new_name = NULL;
    DBusError error = {0};

    if (!dbus_message_get_args (message, &error,
            DBUS_TYPE_STRING, &name,
            DBUS_TYPE_STRING, &old_name,
            DBUS_TYPE_STRING, &new_name,
            DBUS_TYPE_INVALID)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
    }

    g_return_if_fail (strcmp (name, IBUS_NAME) == 0);

    if (g_strcmp0 (new_name, "") == 0) {
        _ibus_im_client_ibus_close (client);
        priv->enable = FALSE;
    }
    else {
        _ibus_im_client_ibus_open (client);
        priv->enable = TRUE;
    }
}
#endif

static void
_ibus_signal_disconnected_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    _ibus_im_client_ibus_close (client);
}

static void
_ibus_signal_enabled_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    DEBUG_FUNCTION_IN;
    /* Handle CommitString signal */
    IBusIMClientPrivate *priv = client->priv;
    DBusError error = {0};
    gchar *ic = NULL;

    if (!dbus_message_get_args (message, &error,
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
    }
    else {
        IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);
        if (context == NULL) {
            g_debug ("Can not find context assocate with ic(%s)", ic);
            return;
        }
        ibus_im_context_enable (context);
    }
}


static void
_ibus_signal_disabled_handler (DBusConnection *connection, DBusMessage *message, IBusIMClient *client)
{
    DEBUG_FUNCTION_IN;
    /* Handle CommitString signal */
    IBusIMClientPrivate *priv = client->priv;
    DBusError error = {0};
    gchar *ic = NULL;

    if (!dbus_message_get_args (message, &error,
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
    }
    else {
        IBusIMContext *context = g_hash_table_lookup (priv->ic_table, (gpointer)ic);
        if (context == NULL) {
            g_debug ("Can not find context assocate with ic(%s)", ic);
            return;
        }
        ibus_im_context_disable (context);
    }
}


static DBusHandlerResult
_ibus_im_client_message_filter_cb (DBusConnection *connection, DBusMessage *message, void *user_data)
{
    IBusIMClient *client = (IBusIMClient *) user_data;

    static struct SIGNAL_HANDLER {
        const gchar *iface;
        const gchar *name;
        void (* handler) (DBusConnection *, DBusMessage *, IBusIMClient *);
    } handlers[] = {
#ifdef USE_DBUS_SESSION_BUS
        { DBUS_INTERFACE_DBUS, "NameOwnerChanged", _ibus_signal_name_owner_changed_handler },
#endif
        { DBUS_INTERFACE_LOCAL, "Disconnected", _ibus_signal_disconnected_handler },
        { IBUS_IFACE, "CommitString", _ibus_signal_commit_string_handler },
        { IBUS_IFACE, "UpdatePreedit", _ibus_signal_update_preedit_handler },
        { IBUS_IFACE, "ShowPreedit", _ibus_signal_show_preedit_handler },
        { IBUS_IFACE, "HidePreedit", _ibus_signal_hide_preedit_handler },
        { IBUS_IFACE, "Enabled", _ibus_signal_enabled_handler },
        { IBUS_IFACE, "Disabled", _ibus_signal_disabled_handler },
        {0},
    };

    gint i;
    for (i = 0; handlers[i].iface != NULL; i++) {
        if (dbus_message_is_signal (message, handlers[i].iface, handlers[i].name)) {
            handlers[i].handler (connection, message, client);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
    }
    g_debug ("Unknown message %s", dbus_message_get_member (message));
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

inline static gboolean
_dbus_call_with_reply_and_block_valist (DBusConnection *connection,
    const gchar *dest, const gchar *path, const gchar* iface, const char *method,
    int first_arg_type, va_list args)
{

    DBusMessage *message, *reply;
    DBusError error = {0};
    int type;
    va_list tmp;

    if (connection == NULL)
        return FALSE;

    message = dbus_message_new_method_call (dest,
                                    path, iface, method);
    if (!message) {
        g_warning ("Out of memory!");
        return FALSE;
    }

    va_copy (tmp, args);
    if (!dbus_message_append_args_valist (message, first_arg_type, tmp)) {
        dbus_message_unref (message);
        g_warning ("Can not create call message");
        return FALSE;
    }

    reply = dbus_connection_send_with_reply_and_block (connection,
                        message, -1, &error);

    dbus_message_unref (message);

    if (!reply) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
        return FALSE;
    }

    type = first_arg_type;
    while (type != DBUS_TYPE_INVALID) {
        if (type == DBUS_TYPE_ARRAY) {
            va_arg (args, int);
            va_arg (args, void *);
            va_arg (args, int);
        }
        else {
            va_arg (args, void *);
        }
        type = va_arg (args, int);
    }

    type = va_arg (args, int);
    if (!dbus_message_get_args_valist (reply, &error, type, args)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
        dbus_message_unref (reply);
        return FALSE;
    }
    dbus_message_unref (reply);

    return TRUE;

}

inline static gboolean
_dbus_call_with_reply_and_block (DBusConnection *connection,
    const gchar *dest, const gchar *path, const gchar* iface, const char *method,
    gint first_arg_type, ...)
{
    va_list args;
    gboolean retval;

    if (connection == NULL)
        return FALSE;

    va_start (args, first_arg_type);
    retval = _dbus_call_with_reply_and_block_valist (connection,
                    dest, path, iface, method, first_arg_type, args);
    va_end (args);

    return TRUE;

}

static gboolean
_ibus_call_with_reply_and_block (DBusConnection *connection, const gchar *method, int first_arg_type, ...)
{
    va_list args;
    gboolean retval;

    if (connection == NULL)
        return FALSE;

    va_start (args, first_arg_type);
    retval = _dbus_call_with_reply_and_block_valist (connection,
                    IBUS_NAME, IBUS_PATH, IBUS_IFACE, method, first_arg_type, args);
    va_end (args);

    return retval;

}


inline static gboolean
_dbus_call_with_reply_valist (DBusConnection *connection,
    const gchar *dest, const gchar *path, const gchar* iface, const char *method,
    DBusPendingCallNotifyFunction notify_function,
    void *user_data, DBusFreeFunction free_function,
    gint first_arg_type, va_list args)
{
    DBusMessage *message = NULL;
    DBusPendingCall *pendingcall = NULL;
    DBusError error = {0};
    int type;

    if (connection == NULL) {
        goto error;
    }

    message = dbus_message_new_method_call (dest,
                                    path, iface, method);
    if (!message) {
        g_warning ("Out of memory!");
        goto error;
    }

    if (!dbus_message_append_args_valist (message, first_arg_type, args)) {
        g_warning ("Can not create call message");
        goto error;
    }

    if (!dbus_connection_send_with_reply (connection,
                        message, &pendingcall, -1)) {
        g_warning ("Out of memory!");
        goto error;
    }

    if (!dbus_pending_call_set_notify (pendingcall, notify_function,
            user_data, free_function)) {
        g_warning ("Out of memory!");
        goto error;
    }

    dbus_message_unref (message);
    return TRUE;

error:
    if (message)
        dbus_message_unref (message);
    if (pendingcall)
        dbus_pending_call_cancel (pendingcall);
    if (user_data && free_function)
        free_function (user_data);
    return False;
}

inline static gboolean
_dbus_call_with_reply (DBusConnection *connection,
    const gchar *dest, const gchar *path, const gchar* iface, const char *method,
    DBusPendingCallNotifyFunction notify_function,
    void *user_data, DBusFreeFunction free_function,
    gint first_arg_type, ...)
{
    va_list args;
    gboolean retval;

    if (connection == NULL)
        return FALSE;

    va_start (args, first_arg_type);
    retval = _dbus_call_with_reply_valist (connection,
                    dest, path, iface, method,
                    notify_function,
                    user_data, free_function,
                    first_arg_type, args);
    va_end (args);

    return TRUE;

}



static gboolean
_ibus_call_with_reply (DBusConnection *connection, const gchar *method,
       DBusPendingCallNotifyFunction notify_function,
       void *user_data, DBusFreeFunction free_function,
       int first_arg_type, ...)
{
    va_list args;
    gboolean retval;

    if (connection == NULL)
        return FALSE;

    va_start (args, first_arg_type);
    retval = _dbus_call_with_reply_valist (connection,
                    IBUS_NAME, IBUS_PATH, IBUS_IFACE,
                    method, notify_function,
                    user_data, free_function,
                    first_arg_type, args);
    va_end (args);

    return retval;
}


static void
_ibus_filter_keypress_reply_cb (DBusPendingCall *pending, void *user_data)
{
    DBusMessage *reply;
    DBusError error = {0};
    GdkEvent *event = (GdkEvent *) user_data;
    gboolean retval;


    reply = dbus_pending_call_steal_reply (pending);
    dbus_pending_call_unref (pending);

    if (dbus_set_error_from_message (&error, reply)) {
        g_warning ("%s", error.message);
        dbus_error_free (&error);
        retval = FALSE;
    }
    else {
        if (!dbus_message_get_args (reply, &error,
                DBUS_TYPE_BOOLEAN, &retval, DBUS_TYPE_INVALID)) {
            g_warning ("%s", error.message);
            dbus_error_free (&error);
            retval = FALSE;
        }
    }

    if (!retval) {
        event->any.send_event = TRUE;
        gdk_event_put (event);
    }
}

gboolean
ibus_im_client_filter_keypress (IBusIMClient *client, IBusIMContext *context, GdkEventKey *event)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);

    if (ic == NULL)
        return FALSE;

    guint state = event->state & GDK_MODIFIER_MASK;
    gboolean is_press = event->type == GDK_KEY_PRESS;

    if (event->send_event) {
        return FALSE;
    }

    /* Call IBus ProcessKeyEvent method */
    if (!_ibus_call_with_reply (priv->ibus,
            "ProcessKeyEvent",
            _ibus_filter_keypress_reply_cb,
            gdk_event_copy ((GdkEvent *)event),
            (DBusFreeFunction)gdk_event_free,
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_UINT32, &event->keyval,
            DBUS_TYPE_BOOLEAN, &is_press,
            DBUS_TYPE_UINT32, &state,
            DBUS_TYPE_INVALID))
        return FALSE;

    return TRUE;
}


void
ibus_im_client_focus_in (IBusIMClient *client, IBusIMContext *context)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);

    if (ic == NULL)
        return;

    /* Call IBus FocusIn method */
     _ibus_call_with_reply_and_block (priv->ibus,
            "FocusIn",
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID,
            DBUS_TYPE_INVALID);
}

void
ibus_im_client_focus_out (IBusIMClient *client, IBusIMContext *context)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);

    if (ic == NULL)
        return;

    /* Call IBus FocusOut method */
    _ibus_call_with_reply_and_block (priv->ibus,
            "FocusOut",
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID,
            DBUS_TYPE_INVALID);

}

void
ibus_im_client_reset (IBusIMClient *client, IBusIMContext *context)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);

    if (ic == NULL)
        return;

    /* Call IBus Reset method */
    _ibus_call_with_reply_and_block (priv->ibus,
            "Reset",
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INVALID,
            DBUS_TYPE_INVALID);

}

void
ibus_im_client_set_cursor_location (IBusIMClient *client, IBusIMContext *context, GdkRectangle *area)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);

    if (ic == NULL)
        return;

    _ibus_call_with_reply_and_block (client->priv->ibus,
            "SetCursorLocation",
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INT32, &area->x,
            DBUS_TYPE_INT32, &area->y,
            DBUS_TYPE_INT32, &area->width,
            DBUS_TYPE_INT32, &area->height,
            DBUS_TYPE_INVALID,
            DBUS_TYPE_INVALID);
}

void
ibus_im_client_set_use_preedit (IBusIMClient *client, IBusIMContext *context, gboolean use_preedit)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);

    if (ic == NULL)
        return;

    _ibus_call_with_reply_and_block (client->priv->ibus,
            "SetCapabilities",
            DBUS_TYPE_STRING, &ic,
            DBUS_TYPE_INT32, &use_preedit,
            DBUS_TYPE_INVALID,
            DBUS_TYPE_INVALID);
}

void
ibus_im_client_release_im_context (IBusIMClient *client, IBusIMContext *context)
{
    IBusIMClientPrivate *priv = client->priv;
    gchar *ic = ibus_im_context_get_ic (context);
    priv->contexts = g_list_remove (priv->contexts, context);
    if (ic) {
        g_hash_table_remove (priv->ic_table, ic);
        _ibus_call_with_reply_and_block (priv->ibus, "ReleaseInputContext",
                DBUS_TYPE_STRING, &ic,
                DBUS_TYPE_INVALID,
                DBUS_TYPE_INVALID);

    }

}

