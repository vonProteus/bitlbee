/***************************************************************************\
*                                                                           *
*  BitlBee - An IRC to IM gateway                                           *
*  Jabber module - Main file                                                *
*                                                                           *
*  Copyright 2006 Wilmer van der Gaast <wilmer@gaast.net>                   *
*                                                                           *
*  This program is free software; you can redistribute it and/or modify     *
*  it under the terms of the GNU General Public License as published by     *
*  the Free Software Foundation; either version 2 of the License, or        *
*  (at your option) any later version.                                      *
*                                                                           *
*  This program is distributed in the hope that it will be useful,          *
*  but WITHOUT ANY WARRANTY; without even the implied warranty of           *
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the            *
*  GNU General Public License for more details.                             *
*                                                                           *
*  You should have received a copy of the GNU General Public License along  *
*  with this program; if not, write to the Free Software Foundation, Inc.,  *
*  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.              *
*                                                                           *
\***************************************************************************/

#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdio.h>

#include "ssl_client.h"
#include "xmltree.h"
#include "bitlbee.h"
#include "jabber.h"

static void jabber_acc_init( account_t *acc )
{
	set_t *s;
	
	s = set_add( &acc->set, "port", "5222", set_eval_int, acc );
	s->flags |= ACC_SET_OFFLINE_ONLY;
	
	s = set_add( &acc->set, "priority", "0", set_eval_priority, acc );
	
	s = set_add( &acc->set, "resource", "BitlBee", NULL, acc );
	s->flags |= ACC_SET_OFFLINE_ONLY;
	
	s = set_add( &acc->set, "resource_select", "priority", NULL, acc );
	
	s = set_add( &acc->set, "server", NULL, set_eval_account, acc );
	s->flags |= ACC_SET_NOSAVE | ACC_SET_OFFLINE_ONLY;
	
	s = set_add( &acc->set, "ssl", "false", set_eval_bool, acc );
	s->flags |= ACC_SET_OFFLINE_ONLY;
	
	s = set_add( &acc->set, "tls", "try", set_eval_tls, acc );
	s->flags |= ACC_SET_OFFLINE_ONLY;
}

static void jabber_login( account_t *acc )
{
	struct gaim_connection *gc = new_gaim_conn( acc );
	struct jabber_data *jd = g_new0( struct jabber_data, 1 );
	struct ns_srv_reply *srv = NULL;
	char *connect_to;
	
	jd->gc = gc;
	gc->proto_data = jd;
	
	jd->username = g_strdup( acc->user );
	jd->server = strchr( jd->username, '@' );
	
	if( jd->server == NULL )
	{
		hide_login_progress( gc, "Incomplete account name (format it like <username@jabberserver.name>)" );
		signoff( gc );
		return;
	}
	
	/* So don't think of free()ing jd->server.. :-) */
	*jd->server = 0;
	jd->server ++;
	
	jd->node_cache = g_hash_table_new_full( g_str_hash, g_str_equal, NULL, jabber_cache_entry_free );
	jd->buddies = g_hash_table_new( g_str_hash, g_str_equal );
	
	/* Figure out the hostname to connect to. */
	if( acc->server )
		connect_to = acc->server;
	else if( ( srv = srv_lookup( "xmpp-client", "tcp", jd->server ) ) ||
		 ( srv = srv_lookup( "jabber-client", "tcp", jd->server ) ) )
		connect_to = srv->name;
	else
		connect_to = jd->server;
	
	/* For non-SSL connections we can try to use the port # from the SRV
	   reply, but let's not do that when using SSL, SSL usually runs on
	   non-standard ports... */
	if( set_getbool( &acc->set, "ssl" ) )
	{
		jd->ssl = ssl_connect( connect_to, set_getint( &acc->set, "port" ), jabber_connected_ssl, gc );
		jd->fd = ssl_getfd( jd->ssl );
	}
	else
	{
		jd->fd = proxy_connect( connect_to, srv ? srv->port : set_getint( &acc->set, "port" ), jabber_connected_plain, gc );
	}
	
	g_free( srv );
}

static void jabber_close( struct gaim_connection *gc )
{
	struct jabber_data *jd = gc->proto_data;
	
	jabber_end_stream( gc );
	
	if( jd->r_inpa >= 0 )
		b_event_remove( jd->r_inpa );
	if( jd->w_inpa >= 0 )
		b_event_remove( jd->w_inpa );
	
	if( jd->ssl )
		ssl_disconnect( jd->ssl );
	if( jd->fd >= 0 )
		closesocket( jd->fd );
	
	if( jd->tx_len )
		g_free( jd->txq );
	
	g_hash_table_destroy( jd->node_cache );
	
	xt_free( jd->xt );
	
	g_free( jd->away_message );
	g_free( jd->username );
	g_free( jd );
}

static int jabber_send_im( struct gaim_connection *gc, char *who, char *message, int len, int away )
{
	struct jabber_data *jd = gc->proto_data;
	struct jabber_buddy *bud;
	struct xt_node *node;
	int st;
	
	bud = jabber_buddy_by_jid( gc, who );
	
	node = xt_new_node( "body", message, NULL );
	node = jabber_make_packet( "message", "chat", bud->full_jid, node );
	
	if( ( jd->flags & JFLAG_WANT_TYPING ) &&
	    ( ( bud->flags & JBFLAG_DOES_JEP85 ) ||
	     !( bud->flags & JBFLAG_PROBED_JEP85 ) ) )
	{
		struct xt_node *act;
		
		/* If the user likes typing notification and if we don't know
		   (and didn't probe before) if this resource supports JEP85,
		   include a probe in this packet now. */
		act = xt_new_node( "active", NULL, NULL );
		xt_add_attr( act, "xmlns", "http://jabber.org/protocol/chatstates" );
		xt_add_child( node, act );
		
		/* Just make sure we do this only once. */
		bud->flags |= JBFLAG_PROBED_JEP85;
	}
	
	st = jabber_write_packet( gc, node );
	xt_free_node( node );
	
	return st;
}

static GList *jabber_away_states( struct gaim_connection *gc )
{
	static GList *l = NULL;
	int i;
	
	if( l == NULL )
		for( i = 0; jabber_away_state_list[i].full_name; i ++ )
			l = g_list_append( l, (void*) jabber_away_state_list[i].full_name );
	
	return l;
}

static void jabber_get_info( struct gaim_connection *gc, char *who )
{
	struct jabber_data *jd = gc->proto_data;
	struct jabber_buddy *bud;
	
	if( strchr( who, '/' ) )
		bud = jabber_buddy_by_jid( gc, who );
	else
		bud = g_hash_table_lookup( jd->buddies, who );
	
	while( bud )
	{
		serv_got_crap( gc, "Buddy %s/%s (%d) information:\nAway state: %s\nAway message: %s",
		                   bud->handle, bud->resource, bud->priority,
		                   bud->away_state ? bud->away_state->full_name : "(none)",
		                   bud->away_message ? : "(none)" );
		bud = bud->next;
	}
}

static void jabber_set_away( struct gaim_connection *gc, char *state_txt, char *message )
{
	struct jabber_data *jd = gc->proto_data;
	struct jabber_away_state *state;
	
	/* Save all this info. We need it, for example, when changing the priority setting. */
	state = (void *) jabber_away_state_by_name( state_txt );
	jd->away_state = state ? state : (void *) jabber_away_state_list; /* Fall back to "Away" if necessary. */
	g_free( jd->away_message );
	jd->away_message = ( message && *message ) ? g_strdup( message ) : NULL;
	
	presence_send_update( gc );
}

static void jabber_add_buddy( struct gaim_connection *gc, char *who )
{
	if( jabber_add_to_roster( gc, who, NULL ) )
		presence_send_request( gc, who, "subscribe" );
}

static void jabber_remove_buddy( struct gaim_connection *gc, char *who, char *group )
{
	if( jabber_remove_from_roster( gc, who ) )
		presence_send_request( gc, who, "unsubscribe" );
}

static void jabber_keepalive( struct gaim_connection *gc )
{
	/* Just any whitespace character is enough as a keepalive for XMPP sessions. */
	jabber_write( gc, "\n", 1 );
	
	/* This runs the garbage collection every minute, which means every packet
	   is in the cache for about a minute (which should be enough AFAIK). */
	jabber_cache_clean( gc );
}

static int jabber_send_typing( struct gaim_connection *gc, char *who, int typing )
{
	struct jabber_data *jd = gc->proto_data;
	struct jabber_buddy *bud;
	
	/* Enable typing notification related code from now. */
	jd->flags |= JFLAG_WANT_TYPING;
	
	bud = jabber_buddy_by_jid( gc, who );
	if( bud->flags & JBFLAG_DOES_JEP85 )
	{
		/* We're only allowed to send this stuff if we know the other
		   side supports it. */
		
		struct xt_node *node;
		char *type;
		int st;
		
		if( typing == 0 )
			type = "active";
		else if( typing == 2 )
			type = "paused";
		else /* if( typing == 1 ) */
			type = "composing";
		
		node = xt_new_node( type, NULL, NULL );
		xt_add_attr( node, "xmlns", "http://jabber.org/protocol/chatstates" );
		node = jabber_make_packet( "message", "chat", bud->full_jid, node );
		
		st = jabber_write_packet( gc, node );
		xt_free_node( node );
		
		return st;
	}
	
	return 1;
}

void jabber_init()
{
	struct prpl *ret = g_new0( struct prpl, 1 );
	
	ret->name = "jabber";
	ret->login = jabber_login;
	ret->acc_init = jabber_acc_init;
	ret->close = jabber_close;
	ret->send_im = jabber_send_im;
	ret->away_states = jabber_away_states;
//	ret->get_status_string = jabber_get_status_string;
	ret->set_away = jabber_set_away;
//	ret->set_info = jabber_set_info;
	ret->get_info = jabber_get_info;
	ret->add_buddy = jabber_add_buddy;
	ret->remove_buddy = jabber_remove_buddy;
//	ret->chat_send = jabber_chat_send;
//	ret->chat_invite = jabber_chat_invite;
//	ret->chat_leave = jabber_chat_leave;
//	ret->chat_open = jabber_chat_open;
	ret->keepalive = jabber_keepalive;
	ret->send_typing = jabber_send_typing;
	ret->handle_cmp = g_strcasecmp;

	register_protocol( ret );
}
