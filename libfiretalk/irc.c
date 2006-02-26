/*
irc.c - FireTalk IRC protocol definitions
Copyright (C) 2000 Ian Gulliver
Copyright 2002-2006 Daniel Reed <n@ml.org>

This program is free software; you can redistribute it and/or modify
it under the terms of version 2 of the GNU General Public License as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include <assert.h>
#include <ctype.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#define ROOMSTARTS "#+&"
#define DEFAULT_SERVER	"irc.n.ml.org"
#define DEFAULT_PORT	6667


static char irc_tolower(const char c) {
	if ((c >= 'A') && (c <= 'Z'))
		return((c - 'A') + 'a');
	if (c == '[')
		return('{');
	if (c == ']')
		return('{');
	if (c == '\\')
		return('|');
	return(c);
}

static int irc_compare_nicks_int(const char *const nick1, const char *const nick2) {
	int i = 0;

	while (nick1[i] != '\0') {
		if (irc_tolower(nick1[i]) != irc_tolower(nick2[i]))
			return(1);
		i++;
	}
	if (nick2[i] != '\0')
		return(1);

	return(0);
}

struct s_irc_whois {
	struct s_irc_whois *next;
	char	*nickname,
		*info;
	int	flags;
	long	online,
		idle;
};

typedef struct irc_conn_t *client_t;
#define _HAVE_CLIENT_T
#include "firetalk-int.h"

typedef struct irc_conn_t {
	char	*nickname;
	struct s_irc_whois
		*whois_head;
	firetalk_sock_t sock;
	unsigned char
		 usesilence:1,	/* are we on a network that understands SILENCE */
		 identified:1;	/* are we we identified */
} irc_conn_t;

#if 0
static const char *const irc_normalize_user_nick(const char *const name) {
	static char
		buf[512];
	int	i;

	if (strchr(name, '!') == NULL)
		return(name);

	for (i = 0; (i < sizeof(buf)) && (name[i] != '!'); i++)
		buf[i] = name[i];
	buf[i] = 0;
	return(buf);
}

static const char *const irc_normalize_user_mask(const char *const name) {
	static char
		buf[512];

	if (strchr(name, '!') != NULL)
		return(name);

	snprintf(buf, sizeof(buf), "%s!*@*", name);
	return(buf);
}
#endif

static void irc_disc_user_rem(firetalk_t conn, irc_conn_t *c, const char *disc, const char *name) {
	if (firetalk_user_visible_but(conn, disc, name) == FE_NOMATCH)
		conn->PI->im_buddyonline(conn, c, name, 0);
}

static void irc_disc_rem(firetalk_t conn, irc_conn_t *c, const char *disc) {
	struct s_firetalk_room *iter;
	struct s_firetalk_member *mem;

	iter = firetalk_find_room(conn, disc);
	assert(iter != NULL);

	for (mem = iter->member_head; mem != NULL; mem = mem->next)
		irc_disc_user_rem(conn, c, disc, mem->nickname);
}

static const char *irc_normalize_room_name(firetalk_t conn, const char *const name) {
	static char	newname[2048];

	if (strchr(ROOMSTARTS, *name))
		return(name);
	snprintf(newname, sizeof(newname), "#%s", name);
	return(newname);
}



#include "firetalk.h"



#ifdef DEBUG_ECHO
static void irc_echof(firetalk_t conn, irc_conn_t *c, const char *const where, const char *const format, ...) {
	va_list	ap;
	char	buf[sizeof(c->sock.buffer)+1];
	void	statrefresh(void);

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	while (buf[strlen(buf)-1] == '\n')
		buf[strlen(buf)-1] = 0;
	if (*buf != 0)
		conn->PI->chat_getmessage(conn, c, ":DEBUG", where, 0, buf);

	statrefresh();
}
#endif

static fte_t irc_compare_nicks(firetalk_t conn, const char *const nick1, const char *const nick2) {
	if (irc_compare_nicks_int(nick1, nick2) == 0)
		return(FE_SUCCESS);
	return(FE_NOMATCH);
}

static fte_t irc_isprint(firetalk_t conn, const int c) {
	if (isprint(c))
		return(FE_SUCCESS);
	return(FE_INVALIDFORMAT);
}

static fte_t irc_isnickfirst(const int c) {
	return(isalpha(c) || (c == '[') || (c == ']') || (c == '\\') || (c == '`') || (c == '^') || (c == '{') || (c == '}'));
}

static fte_t irc_isnick(const int c) {
	return(irc_isnickfirst(c) || isdigit(c) || (c == '-'));
}

static char *irc_html_to_irc(const char *const string) {
	static char *output = NULL;
	int	o = 0;
	size_t l, i=0;

	l = strlen(string);

	output = realloc(output, (l * 4) + 1);
	if (output == NULL)
		abort();

	while (i < l) {
		assert(o < ((l * 4) + 1));
		switch(string[i]) {
			case '&':
				if (!strncasecmp(&string[i], "&amp;", 5)) {
					output[o++] = '&';
					i += 5;
				} else if (!strncasecmp(&string[i], "&gt;", 4)) {
					output[o++] = '>';
					i += 4;
				} else if (!strncasecmp(&string[i], "&lt;", 4)) {
					output[o++] = '<';
					i += 4;
				} else if (!strncasecmp(&string[i], "&nbsp;", 6)) {
					output[o++] = ' ';
					i += 6;
				} else
					output[o++] = string[i++];
				break;
			case '<':
				if (!strncasecmp(&string[i], "<b>", 3)) {
					output[o++] = (char) 2;
					i += 3;
				} else if (!strncasecmp(&string[i], "</b>", 4)) {
					output[o++] = (char) 2;
					i += 4;
				} else if (!strncasecmp(&string[i], "<i>", 3)) {
					output[o++] = (char) 22;
					i += 3;
				} else if (!strncasecmp(&string[i], "</i>", 4)) {
					output[o++] = (char) 22;
					i += 4;
				} else if (!strncasecmp(&string[i], "<u>", 3)) {
					output[o++] = (char) 31;
					i += 3;
				} else if (!strncasecmp(&string[i], "</u>", 4)) {
					output[o++] = (char) 31;
					i += 4;
				} else if (!strncasecmp(&string[i], "<br>", 4)) {
					output[o++] = '\020';
					output[o++] = 'r';
					output[o++] = '\020';
					output[o++] = 'n';
					i += 4;
				} else
					output[o++] = string[i++];
				break;
			case '\r':
				output[o++] = '\020';
				output[o++] = 'r';
				i++;
				break;
			case '\n':
				output[o++] = '\020';
				output[o++] = 'n';
				i++;
				break;
			case '\020':
				output[o++] = '\020';
				output[o++] = '\020';
				i++;
				break;
			default:
				output[o++] = string[i++];
				break;
		}
	}
	assert(o <= ((l * 4) + 1));
	output[o] = '\0';
	return(output);
}

static const char *mIRCar[] = {
	"#FFFFFF",	// 0 white
	"#000000",	// 1 black
	"#0000FF",	// 2 blue     (navy)
	"#00FF00",	// 3 green
	"#FF0000",	// 4 red
	"#4E2F2F",	// 5 brown    (maroon)
	"#AA00FF",	// 6 purple
	"#FF7700",	// 7 orange   (olive)
	"#FFFF00",	// 8 yellow
	"#32CD32",	// 9 lt.green (lime)
	"#349F79",	// 10 teal    (a kinda green/blue cyan)
	"#70DB93",	// 11 lt.cyan (cyan ?) (aqua)
	"#3333FF",	// 12 lt.blue (royal)
	"#FF00AA",	// 13 pink    (light purple) (fuchsia)
	"#A8A8A8",	// 14 grey
	"#E6E8FA"	// 15 lt.grey (silver)
};

static const char *ANSIar[] = {
	"#000000",	// 30 black
	"#FF0000",	// 31 red
	"#00FF00",	// 32 green
	"#FFFF00",	// 33 yellow
	"#0000FF",	// 34 blue
	"#FF00FF",	// 35 magenta (purple)
	"#00FFFF",	// 36 cyan (aqua)
	"#FFFFFF",	// 37 white
};

static const char *irc_mIRC_to_html(const char *const string, size_t *pos) {
	int	i, col = 0;

	for (i = 0; (i < 2) && isdigit(string[*pos]); i++, (*pos)++) {
		col *= 10;
		col += string[*pos] - '0';
	}
	if (string[*pos] == ',') {
		(*pos)++;
		for (i = 0; (i < 2) && isdigit(string[*pos]); i++, (*pos)++)
			;
	}
	if ((col >= 0) && (col <= 15))
		return(mIRCar[col]);
	else
		return("#000000");
}

static char *irc_irc_to_html(const char *const string) {
	static char *output = NULL;
	int	o = 0;
	size_t	l, i = 0, s;
	int	infont = 0, inbold = 0, initalics = 0, inunderline = 0;

	assert(string != NULL);

	l = strlen(string);

	s = l*(sizeof("<font color=\"#RRGGBB\">")-1) + 1;
	output = realloc(output, s);
	if (output == NULL)
		abort();

	while (i < l) {
		switch(string[i]) {
		  case 2:
			if (inbold == 1) {
				memcpy(&output[o], "</B>", 4);
				o += 4;
				inbold = 0;
			} else {
				memcpy(&output[o], "<B>", 3);
				o += 3;
				inbold = 1;
			}
			break;
		  case 3:
			if (isdigit(string[i+1])) {
				i++;
				sprintf(output+o, "<font color=\"%s\">", irc_mIRC_to_html(string, &i));
				o += sizeof("<font color=\"#RRGGBB\">")-1;
				infont = 1;
				i--;
			} else if (infont == 1) {
				strcpy(output+o, "</font>");
				o += sizeof("</font>")-1;
				infont = 0;
			}
			break;
		  case 15:
			if (infont == 1) {
				strcpy(output+o, "</font>");
				o += sizeof("</font>")-1;
				infont = 0;
			}
			if (inbold == 1) {
				strcpy(output+o, "</B>");
				o += sizeof("</B>")-1;
				inbold = 0;
			}
			if (initalics == 1) {
				strcpy(output+o, "</I>");
				o += sizeof("</I>")-1;
				initalics = 0;
			}
			if (inunderline == 1) {
				strcpy(output+o, "</U>");
				o += sizeof("</U>")-1;
				inunderline = 0;
			}
			break;
		  case 27:
			if (string[i+1] == '[') {
				i += 2;
				while ((string[i] != 0) && (string[i] != 'm')) {
					int	num = 0;

					if (!isdigit(string[i]))
						break;
					while (isdigit(string[i])) {
						num *= 10;
						num += string[i] - '0';
						i++;
					}
					if (string[i] == ';')
						i++;
					switch (num) {
					  case 0:
						if (infont == 1) {
							strcpy(output+o, "</font>");
							o += sizeof("</font>")-1;
							infont = 0;
						}
						if (inbold == 1) {
							strcpy(output+o, "</B>");
							o += sizeof("</B>")-1;
							inbold = 0;
						}
						if (initalics == 1) {
							strcpy(output+o, "</I>");
							o += sizeof("</I>")-1;
							initalics = 0;
						}
						if (inunderline == 1) {
							strcpy(output+o, "</U>");
							o += sizeof("</U>")-1;
							inunderline = 0;
						}
						break;
					  case 1:
						if (inbold == 0) {
							strcpy(output+o, "<B>");
							o += sizeof("<B>")-1;
							inbold = 1;
						}
						break;
					  case 2:
						if (inbold == 1) {
							strcpy(output+o, "</B>");
							o += sizeof("</B>")-1;
							inbold = 0;
						}
						break;
					  case 3:
						if (inunderline == 0) {
							strcpy(output+o, "<U>");
							o += sizeof("<U>")-1;
							inunderline = 1;
						}
						break;
					  case 7:
						if (initalics == 0) {
							strcpy(output+o, "<I>");
							o += sizeof("<I>")-1;
							initalics = 1;
						}
						break;
					  case 30: case 31: case 32: case 33:
					  case 34: case 35: case 36: case 37:
						sprintf(output+o, "<font color=\"%s\">", ANSIar[num-30]);
						o += sizeof("<font color=\"#RRGGBB\">")-1;
						infont = 1;
						break;
					}
				}
				if (string[i] == 0)
					i--;
			}
			break;
		  case 22:
			if (initalics == 1) {
				memcpy(&output[o], "</I>", 4);
				o += 4;
				initalics = 0;
			} else {
				memcpy(&output[o], "<I>", 3);
				o += 3;
				initalics = 1;
			}
			break;
		  case 31:
			if (inunderline == 1) {
				memcpy(&output[o], "</U>", 4);
				o += 4;
				inunderline = 0;
			} else {
				memcpy(&output[o], "<U>", 3);
				o += 3;
				inunderline = 1;
			}
			break;
		  case '&':
			memcpy(&output[o], "&amp;", 5);
			o += 5;
			break;
		  case '<':
			memcpy(&output[o], "&lt;", 4);
			o += 4;
			break;
		  case '>':
			memcpy(&output[o], "&gt;", 4);
			o += 4;
			break;
		  case 16:
			switch(string[++i]) {
			  case 16:
				output[o++] = '\020';
				break;
			  case 'r':
				if (string[i+1] == '\020' && string[i+2] == 'n') {
					i += 2;
					memcpy(&output[o], "<br>", 4);
					o += 4;
				} else
					output[o++] = '\r';
				break;
			  case 'n':
				output[o++] = '\n';
				break;
			  default:
				output[o++] = string[i];
				break;
			}
			break;
		  case ' ':
			if (string[i+1] == ' ') {
				memcpy(&output[o], "&nbsp;", 6);
				o += 6;
				break;
			}
		  default:
			output[o++] = string[i];
			break;
		}
		i++;
	}

	output[o] = '\0';

	return(output);
}

static int irc_internal_disconnect(firetalk_t conn, irc_conn_t *c, const int error) {
	struct s_irc_whois *whois_iter, *whois_iter2;

	firetalk_sock_close(&(c->sock));

	if (c->nickname != NULL) {
		free(c->nickname);
		c->nickname = NULL;
	}
	for (whois_iter = c->whois_head; whois_iter != NULL; whois_iter = whois_iter2) {
		whois_iter2 = whois_iter->next;
		if (whois_iter->nickname != NULL) {
			free(whois_iter->nickname);
			whois_iter->nickname = NULL;
		}
		if (whois_iter->info != NULL) {
			free(whois_iter->info);
			whois_iter->info = NULL;
		}
		free(whois_iter);
	}
	c->whois_head = NULL;

	c->usesilence = 1;
	c->identified = 0;

	conn->PI->disconnect(conn, c, error);

	return(FE_SUCCESS);
}

static int irc_send_printf(firetalk_t conn, irc_conn_t *c, const char *const format, ...) {
	va_list	ap;
	size_t	i,
		datai = 0;
	char	data[513];

	va_start(ap, format);
	for (i = 0; format[i] != 0; i++) {
		if (format[i] == '%') {
			switch (format[++i]) {








				case 's': {
						const char
							*s = irc_html_to_irc(va_arg(ap, char *));
						size_t	slen = strlen(s);

						if ((datai+slen) > (sizeof(data)-2-1))
							return(FE_PACKETSIZE);
						strcpy(data+datai, s);
						datai += slen;
						break;
					}
				case '%':
					data[datai++] = '%';
					break;
			}
		} else {
			data[datai++] = format[i];
			if (datai > (sizeof(data)-2-1))
				return(FE_PACKETSIZE);
		}
	}
	va_end(ap);
	data[datai] = 0;

#ifdef DEBUG_ECHO
	irc_echof(conn, c, "send_printf", "%s", data);
#endif

	strcpy(data+datai, "\r\n");
	datai += 2;

	firetalk_sock_send(&(c->sock), data, datai);

	return(FE_SUCCESS);
}

static char **irc_recv_parse(firetalk_t conn, irc_conn_t *c, unsigned char *buffer, unsigned short *bufferpos) {
	static char *args[256];
	static char data[sizeof(c->sock.buffer)+1];
	size_t curarg;
	char *tempchr;
	char *tempchr2;

	args[0] = NULL;

	assert(*bufferpos < sizeof(data));
	memcpy(data, buffer, *bufferpos);
	data[*bufferpos] = '\0';

	tempchr = strchr(data, '\n');
	if (tempchr == NULL)
		return(NULL);
	if ((tempchr > data) && (tempchr[-1] == '\r'))
		tempchr[-1] = 0;
	else
		tempchr[0] = 0;
	*bufferpos -= (tempchr - data + 1);
	memmove(buffer, &buffer[tempchr - data + 1], *bufferpos);

#ifdef DEBUG_ECHO
	irc_echof(conn, c, "recv_parse", "%s", data);
#endif

	curarg = 0;
	tempchr = data;
	if (*tempchr == ':')
		tempchr++;
	else
		args[curarg++] = ":SERVER";

	while ((curarg < sizeof(args)/sizeof(*args)) && ((tempchr2 = strchr(tempchr, ' ')) != NULL)) {
		args[curarg++] = tempchr;
		*tempchr2 = 0;
		tempchr = tempchr2 + 1;
		if (*tempchr == ':') {
			tempchr++;
			break;
		}
	}
	args[curarg++] = tempchr;
	args[curarg] = NULL;
	return(args);
}

static char *irc_get_nickname(const char *const hostmask) {
	static char data[512];
	char	*tempchr;

	strncpy(data, hostmask, sizeof(data)-1);
	data[sizeof(data)-1] = 0;

	if ((tempchr = strchr(data, '!')) != NULL)
		*tempchr = 0;
	return(data);
}

static fte_t irc_set_nickname(firetalk_t conn, irc_conn_t *c, const char *const nickname) {
	return(irc_send_printf(conn, c, "NICK %s", nickname));
}

static fte_t irc_set_password(firetalk_t conn, irc_conn_t *c, const char *const oldpass, const char *const newpass) {
	return(FE_SUCCESS);
}

static void irc_destroy_handle(firetalk_t conn, irc_conn_t *c) {
fprintf(stderr, "destruction is nice\r\n");
fprintf(stderr, "c->sock.state = %i\r\n", c->sock.state);
	if (c->sock.state != FCS_NOTCONNECTED)
		irc_send_printf(conn, c, "QUIT :Handle destroyed");
fprintf(stderr, "internal_disconnect\r\n");
	irc_internal_disconnect(conn, c, FE_USERDISCONNECT);
	memset(c, 0, sizeof(*c));
	free(c);
fprintf(stderr, "destruction is done\r\n");
}

static fte_t irc_disconnect(firetalk_t conn, irc_conn_t *c) {
	if (c->sock.state != FCS_NOTCONNECTED)
		irc_send_printf(conn, c, "QUIT :User disconnected");
	return(irc_internal_disconnect(conn, c, FE_USERDISCONNECT));
}

static irc_conn_t *irc_create_handle(firetalk_t conn) {
	irc_conn_t *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		abort();
	c->usesilence = 1;
	firetalk_sock_init(&(c->sock));

	return(c);
}

#if defined(HAVE_GETPWUID) && defined(HAVE_GETUID)
# include <pwd.h>
#endif

static fte_t irc_signon(firetalk_t conn, irc_conn_t *c, const char *const nickname) {
#if defined(HAVE_GETPWUID) && defined(HAVE_GETUID)
	struct passwd	*pw = getpwuid(getuid());
	char	buf[1024];
	int	i;

	for (i = 0; (pw->pw_gecos[i] != 0) && (pw->pw_gecos[i] != ',') && (i < sizeof(buf)-1); i++)
		buf[i] = pw->pw_gecos[i];
	if (i == 0)
		snprintf(buf, sizeof(buf), "http://naim.n.ml.org/ user");
	else
		buf[i] = 0;

	if (irc_send_printf(conn, c, "USER %s %s %s :%s", pw->pw_name, nickname, nickname, buf) != FE_SUCCESS)
		return(FE_PACKET);
#else
	if (irc_send_printf(conn, c, "USER %s %s %s :%s", nickname, nickname, nickname, nickname) != FE_SUCCESS)
		return(FE_PACKET);
#endif

	if (irc_send_printf(conn, c, "NICK %s", nickname) != FE_SUCCESS)
		return(FE_PACKET);

	return(FE_SUCCESS);
}

static fte_t irc_save_config(firetalk_t conn, irc_conn_t *c) {
	return(FE_SUCCESS);
}

static fte_t irc_preselect(firetalk_t conn, irc_conn_t *c, fd_set *my_read, fd_set *my_write, fd_set *my_except, int *n) {
	if (c->sock.state == FCS_NOTCONNECTED)
		return(FE_SUCCESS);

	firetalk_sock_preselect(&(c->sock), my_read, my_write, my_except, n);

	return(FE_SUCCESS);
}

static void irc_addwhois(irc_conn_t *c, const char *const name, const char *const format, ...) {
	struct s_irc_whois *whoisiter;
	char	buf[1024];
	va_list	msg;

	va_start(msg, format);
	vsnprintf(buf, sizeof(buf), format, msg);
	va_end(msg);

	for (whoisiter = c->whois_head; whoisiter != NULL; whoisiter = whoisiter->next)
		if (irc_compare_nicks_int(name, whoisiter->nickname) == 0) {
			int	len = whoisiter->info?strlen(whoisiter->info):0;

			whoisiter->info = realloc(whoisiter->info, len+strlen(buf)+1);
			if (whoisiter->info == NULL)
				abort();
			strcpy(whoisiter->info+len, buf);
			break;
		}
}

static fte_t irc_got_data_parse(firetalk_t conn, irc_conn_t *c, char **args) {
	struct s_irc_whois
		*whoisiter,
		*whoisiter2;
	char	*tempchr;

	{
		static unsigned int inwhois = 0;
		int	handled = 0,
			numeric;

		handled = 1;
		if (strcmp(args[1], "PING") == 0) {
			if (args[2] != NULL) {
				if (irc_send_printf(conn, c, "PONG :%s", args[2]) != 0) {
					irc_internal_disconnect(conn, c, FE_PACKET);
					return(FE_PACKET);
				}
			} else {
				if (irc_send_printf(conn, c, "PONG") != 0) {
					irc_internal_disconnect(conn, c, FE_PACKET);
					return(FE_PACKET);
				}
			}
		} else if (strcmp(args[1], "QUIT") == 0) {
			const char *name = irc_get_nickname(args[0]);

			conn->PI->im_buddyonline(conn, c, name, 0);
			if (irc_compare_nicks_int(c->nickname, name) == 0)
				irc_internal_disconnect(conn, c, FE_DISCONNECT);
			else
				conn->PI->chat_user_quit(conn, c, name, irc_irc_to_html(args[2]));
		} else
			handled = 0;

		if (handled)
			return(FE_SUCCESS);

		numeric = atoi(args[1]);

		if (args[2] == NULL)
			goto unhandled;

		handled = 1;
		if (strcmp(args[1], "JOIN") == 0) {
			const char	*name = irc_get_nickname(args[0]);

			conn->PI->im_buddyonline(conn, c, name, 1);
			if (irc_compare_nicks_int(c->nickname, name) == 0) {
				conn->PI->chat_joined(conn, c, args[2]);

				if (c->identified == 1) {
					if (irc_send_printf(conn, c, "PRIVMSG ChanServ :OP %s %s", args[2], c->nickname) != FE_SUCCESS) {
						irc_internal_disconnect(conn, c, FE_PACKET);
						return(FE_PACKET);
					}
				}
			} else {
				char	*extra = strchr(args[0], '!');

				conn->PI->chat_user_joined(conn, c, args[2], name, (extra != NULL)?(extra+1):NULL);
			}
		} else if (strcmp(args[1], "PART") == 0) {
			const char	*name = irc_get_nickname(args[0]);

			if (irc_compare_nicks_int(c->nickname, name) == 0) {
				irc_disc_rem(conn, c, args[2]);
				conn->PI->chat_left(conn, c, args[2]);
			} else {
				irc_disc_user_rem(conn, c, args[2], name);
				conn->PI->chat_user_left(conn, c, args[2], name, (args[3] != NULL)?irc_irc_to_html(args[3]):NULL);
			}
		} else if (strcmp(args[1], "NICK") == 0) {
			const char *name = irc_get_nickname(args[0]);

			if (irc_compare_nicks_int(c->nickname, name) == 0) {
				free(c->nickname);
				c->nickname = strdup(args[2]);
				if (c->nickname == NULL)
					abort();
				conn->PI->newnick(conn, c, c->nickname);
			}
			conn->PI->user_nickchanged(conn, c, name, args[2]);
		} else
			handled = 0;

		if (handled)
			return(FE_SUCCESS);

		if (args[3] == NULL)
			goto unhandled;

		handled = 1;
		if (strcmp(args[1], "PRIVMSG") == 0) {
			/* scan for CTCPs */
			while ((tempchr = strchr(args[3], 1))) {
				char	*sp;

				if ((sp = strchr(tempchr+1, 1))) {
					*sp = 0;

					/* we have a ctcp */
					if (strchr(ROOMSTARTS, args[2][0])) {
						/* chat room subcode */
						if (strncasecmp(&tempchr[1], "ACTION ", 7) == 0)
							conn->PI->chat_getaction(conn, c, args[2], irc_get_nickname(args[0]), 0, irc_irc_to_html(tempchr+8));
					} else {
						char *endcommand;

						endcommand = strchr(&tempchr[1], ' ');
						if (endcommand) {
							*endcommand = '\0';
							endcommand++;
							conn->PI->subcode_request(conn, c, irc_get_nickname(args[0]), &tempchr[1], endcommand);
						} else
							conn->PI->subcode_request(conn, c, irc_get_nickname(args[0]), &tempchr[1], NULL);
					}
					memmove(tempchr, sp+1, strlen(sp+1) + 1);
				} else
					break;
			}
			if (args[3][0] != '\0') {
				if (strchr(ROOMSTARTS, args[2][0]))
					conn->PI->chat_getmessage(conn, c, args[2], irc_get_nickname(args[0]), 0, irc_irc_to_html(args[3]));
				else
					conn->PI->im_getmessage(conn, c, irc_get_nickname(args[0]), 0, irc_irc_to_html(args[3]));
			}
		} else if (strcmp(args[1], "NOTICE") == 0) {
			const char	*name = irc_get_nickname(args[0]);

			/* scan for CTCP's */
			while ((tempchr = strchr(args[3], 1))) {
				char	*sp;

				if ((sp = strchr(tempchr+1, 1)))
					*sp = 0;
				/* we have a ctcp */
				if (strchr(ROOMSTARTS, args[2][0]) == NULL) {
					char *endcommand;
					endcommand = strchr(&tempchr[1], ' ');
					if (endcommand) {
						*endcommand = '\0';
						endcommand++;
						conn->PI->subcode_reply(conn, c, name, &tempchr[1], endcommand);
					} else
						conn->PI->subcode_reply(conn, c, name, &tempchr[1], NULL);
				}
				if (sp)
					memcpy(tempchr, sp+1, strlen(sp+1) + 1);
				else
					break;
			}
			if (strchr(name, '.') != NULL) {
				if (strncmp(args[3], "*** Notice -- ", sizeof("*** Notice -- ")-1) == 0)
					conn->PI->chat_getmessage(conn, c, ":RAW", name, 1, irc_irc_to_html(args[3]+sizeof("*** Notice -- ")-1));
				else
					conn->PI->chat_getmessage(conn, c, ":RAW", name, 1, irc_irc_to_html(args[3]));
			} else if (args[3][0] != '\0') {
				if (strchr(ROOMSTARTS, args[2][0]))
					conn->PI->chat_getmessage(conn, c, args[2], name, 1, irc_irc_to_html(args[3]));
				else
					conn->PI->im_getmessage(conn, c, name, 1, irc_irc_to_html(args[3]));
			}
		} else if (strcmp(args[1], "TOPIC") == 0) {
			conn->PI->chat_gottopic(conn, c, args[2], irc_irc_to_html(args[3]), irc_get_nickname(args[0]));
		} else if (strcmp(args[1], "KICK") == 0) {
			const char	*name = irc_get_nickname(args[3]);

			if (irc_compare_nicks_int(c->nickname, name) == 0) {
				irc_disc_rem(conn, c, args[2]);
				conn->PI->chat_kicked(conn, c, args[2], irc_get_nickname(args[0]), irc_irc_to_html(args[4]));
			} else {
				irc_disc_user_rem(conn, c, args[2], name);

				tempchr = strdup(name);
				if (tempchr == NULL)
					abort();
				conn->PI->chat_user_kicked(conn, c, args[2], tempchr, irc_get_nickname(args[0]), irc_irc_to_html(args[4]));
				free(tempchr);
				tempchr = NULL;
			}
		} else
			handled = 0;

		if (handled)
			return(FE_SUCCESS);

		if (numeric == 311)     // RPL_WHOISUSER
			inwhois = 1;
		else if (numeric == 318)// RPL_ENDOFWHOIS
			inwhois = 0;

		if (!inwhois)
			switch (numeric) {
			  case 333: /* :PREFIX 333 sn channel topicsetuser topicsettime */
			  case 306: /* :PREFIX 306 sn :You have been marked as being away */
			  case 305: /* :PREFIX 305 sn :You are no longer marked as being away */
			  case 396: /* :PREFIX 396 sn whoishost :is now your hidden host */
				return(FE_SUCCESS);
			}

		handled = 1;
		switch (numeric) {
			case 366: /* :PREFIX 366 sn channel :End of /NAMES list. */
				conn->PI->chat_user_joined(conn, c, args[3], NULL, NULL);
				break;
			case 301: /* RPL_AWAY */
				break;
			case 313: /* RPL_WHOISOPER */
				for (whoisiter = c->whois_head; whoisiter != NULL; whoisiter = whoisiter->next)
					if (irc_compare_nicks_int(args[3], whoisiter->nickname) == 0) {
						whoisiter->flags |= FF_ADMIN;
						break;
					}
				break;
			case 318: /* RPL_ENDOFWHOIS */
				whoisiter2 = NULL;
				for (whoisiter = c->whois_head; whoisiter != NULL; whoisiter = whoisiter->next) {
					if (irc_compare_nicks_int(args[3], whoisiter->nickname) == 0) {
						/* manual whois */
						conn->PI->gotinfo(conn, c, whoisiter->nickname, whoisiter->info, 0, whoisiter->online, whoisiter->idle, whoisiter->flags);
						free(whoisiter->nickname);
						whoisiter->nickname = NULL;
						if (whoisiter->info != NULL) {
							free(whoisiter->info);
							whoisiter->info = NULL;
						}
						if (whoisiter2)
							whoisiter2->next = whoisiter->next;
						else
							c->whois_head = whoisiter->next;
						free(whoisiter);
						whoisiter = NULL;
						break;
					}
					whoisiter2 = whoisiter;
				}
				break;
			case 401: /* ERR_NOSUCHNICK */
				conn->PI->im_buddyonline(conn, c, args[3], 0);
			case 441: /* ERR_USERNOTINCHANNEL */
			case 443: /* ERR_USERONCHANNEL */
				whoisiter2 = NULL;
				for (whoisiter = c->whois_head; whoisiter != NULL; whoisiter = whoisiter->next) {
					if (irc_compare_nicks_int(args[3], whoisiter->nickname) == 0) {
						free(whoisiter->nickname);
						whoisiter->nickname = NULL;
						if (whoisiter->info != NULL) {
							free(whoisiter->info);
							whoisiter->info = NULL;
						}
						if (whoisiter2)
							whoisiter2->next = whoisiter->next;
						else
							c->whois_head = whoisiter->next;
						free(whoisiter);
						whoisiter = NULL;
						conn->PI->error(conn, c, FE_BADUSER, args[3], args[4]);
						break;
					}
					whoisiter2 = whoisiter;
				}
				break;
			case 403: /* ERR_NOSUCHCHANNEL */
			case 442: /* ERR_NOTONCHANNEL */
				conn->PI->error(conn, c, FE_BADROOM, args[3], args[4]);
				break;
			case 404: /* ERR_CANNOTSENDTOCHAN */
			case 405: /* ERR_TOOMANYCHANNELS */
			case 471: /* ERR_CHANNELISFULL */
			case 473: /* ERR_INVITEONLYCHAN */
			case 474: /* ERR_BANNEDFROMCHAN */
			case 475: /* ERR_BADCHANNELKEY */
				conn->PI->error(conn, c, FE_ROOMUNAVAILABLE, args[3], args[4]);
				break;
			case 412: /* ERR_NOTEXTTOSEND */
				conn->PI->error(conn, c, FE_BADMESSAGE, NULL, args[4]);
				break;
			case 421: /* ERR_UNKNOWNCOMMAND */
				if (strcmp(args[3], "SILENCE") == 0)
					c->usesilence = 0;
				goto unhandled;
				break;
			case 433: /* ERR_NICKNAMEINUSE */
				conn->PI->error(conn, c, FE_BADUSER, NULL, "Nickname in use.");
				break;
			case 482: /* ERR_CHANOPRIVSNEEDED */
				conn->PI->error(conn, c, FE_NOPERMS, args[3], "You need to be a channel operator.");
				break;
			default:
				handled = 0;
		}

		if (handled)
			return(FE_SUCCESS);

		if (args[4] == NULL)
			goto unhandled;

		if (strcmp(args[1], "MODE") == 0) {
			const char
				*source = irc_get_nickname(args[0]);
			int	loc = 0,
				arg = 4,
				dir = 1;
#ifdef RAWIRCMODES
			int     i;
			char	buf[512];

			strcpy(buf, args[3]);
			for (i = 4; args[i] != NULL; i++) {
				strcat(buf, " ");
				strcat(buf, args[i]);
			}
			conn->PI->chat_modechanged(conn, c, args[2], buf, source);
#endif

			while ((args[arg] != NULL) && (args[3][loc] != '\0')) {
				switch (args[3][loc++]) {
					case '+':
						dir = 1;
						break;
					case '-':
						dir = -1;
						break;
					case 'o':
						if (dir == 1) {
							conn->PI->chat_user_opped(conn, c, args[2], args[arg++], source);
							if (irc_compare_nicks_int(args[arg-1], c->nickname) == 0)
								conn->PI->chat_opped(conn, c, args[2], source);
						} else if (dir == -1) {
							conn->PI->chat_user_deopped(conn, c, args[2], args[arg++], source);
							if (irc_compare_nicks_int(args[arg-1], c->nickname) == 0) {
								conn->PI->chat_deopped(conn, c, args[2], source);
								if (c->identified == 1) {
									/* this is us, and we're identified, so we can request a reop */
									if (irc_send_printf(conn, c, "PRIVMSG ChanServ :OP %s %s", args[2], c->nickname) != FE_SUCCESS) {
										irc_internal_disconnect(conn, c, FE_PACKET);
										return(FE_PACKET);
									}
								}
							}
						}
						break;
					case 'k':
						if (dir == 1)
							conn->PI->chat_keychanged(conn, c, args[2], args[arg], source);
						else
							conn->PI->chat_keychanged(conn, c, args[2], NULL, source);
						arg++;
						break;
					case 'v':
					case 'b':
					case 'l':
						arg++;
						break;
					default:
						break;
				}
			}
			return(FE_SUCCESS);
		}

		handled = 1;
		switch (numeric) {
			case 317: /* RPL_WHOISIDLE */
				for (whoisiter = c->whois_head; whoisiter != NULL; whoisiter = whoisiter->next)
					if (irc_compare_nicks_int(args[3], whoisiter->nickname) == 0) {
						whoisiter->online = atol(args[5]);
						whoisiter->idle = atol(args[4])/60;
					}
				break;
			case 312: /* RPL_WHOISSERVER */
				irc_addwhois(c, args[3], "<br>On server %s (%s)", args[4], irc_irc_to_html(args[5]));
				break;
			case 319: /* RPL_WHOISCHANNELS */
				irc_addwhois(c, args[3], "<br>On channels %s", irc_irc_to_html(args[4]));
				break;
			case 332: /* RPL_TOPIC */
				conn->PI->chat_gottopic(conn, c, args[3], irc_irc_to_html(args[4]), NULL);
				break;
			default:
				handled = 0;
		}

		if (handled)
			return(FE_SUCCESS);

		if (args[5] == NULL)
			goto unhandled;

		if (numeric == 353) { /* RPL_NAMREPLY */
			char	*str = args[5];

			while ((str != NULL) && (*str != 0)) {
				int	oped = 0,
					voiced = 0;
				char	*sp;

				if ((sp = strchr(str, ' ')) != NULL)
					*sp = 0;

				while ((*str != 0) && !irc_isnickfirst(*str)) {
					if (*str == '@')
						oped = 1;
					else if (*str == '+')
						voiced = 1;
					str++;
				}

				conn->PI->chat_user_joined(conn, c, args[4], str, NULL);
				conn->PI->im_buddyonline(conn, c, str, 1);
				if (oped) {
					conn->PI->chat_user_opped(conn, c, args[4], str, NULL);
					if (irc_compare_nicks_int(str, c->nickname) == 0)
						conn->PI->chat_opped(conn, c, args[4], NULL);
				}

				if (sp != NULL)
					str = sp+1;
				else
					str = NULL;
			}
			return(FE_SUCCESS);
		}

		if ((args[6] == NULL) || (args[7] == NULL))
			goto unhandled;

		if (numeric == 311) { /* RPL_WHOISUSER */
			char	*gecos = irc_irc_to_html(args[7]);
			int	i;

			for (i = 0; gecos[i] != 0; i++)
				if (strncasecmp(gecos+i, "<HTML>", sizeof("<HTML>")-1) == 0)
					break;
			if (gecos[i] != 0)
				irc_addwhois(c, args[3], "%s@%s (%s)", args[4], args[5], firetalk_htmlentities(gecos));
			else
				irc_addwhois(c, args[3], "%s@%s (<HTML>%s</HTML>)", args[4], args[5], gecos);
			return(FE_SUCCESS);
		} else if (numeric == 352) { /* WHO output */
			char	buf[1024];
			int	i;

			snprintf(buf, sizeof(buf), "%s %s %s %s", args[7], args[4], args[5], args[6]);
			for (i = 8; args[i] != NULL; i++)
				snprintf(buf, sizeof(buf), "%s, %s", buf, args[i]);
			conn->PI->chat_getmessage(conn, c, ":RAW", args[3], 0, buf);
			return(FE_SUCCESS);
		}

	  unhandled: 
		if (inwhois) {
			char	buf[1024];
			int	i;

			snprintf(buf, sizeof(buf), "<br>%s", args[3]);
			for (i = numeric?4:3; args[i+1] != NULL; i++)
				;
			snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), " %s", args[i]);
			for (i = numeric?4:3; args[i+1] != NULL; i++)
				snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), " %s", args[i]);
			snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), " (%s)", args[1]);
			irc_addwhois(c, args[3], "%s", buf);
		} else {
			char	buf[1024];
			int	i;

			*buf = 0;
			for (i = 1; args[i+1] != NULL; i++)
				snprintf(buf, sizeof(buf), "%s<B>%s</B>, ", buf, args[i]);
			snprintf(buf, sizeof(buf), "%s%s", buf, args[i]);
			conn->PI->chat_getmessage(conn, c, ":RAW", irc_get_nickname(args[0]), 0, buf);
		}
	}

	return(FE_SUCCESS);
}

static fte_t irc_got_data(firetalk_t conn, irc_conn_t *c, unsigned char *buffer, unsigned short *bufferpos) {
	char	**args;

#ifdef DEBUG_ECHO
	irc_echof(conn, c, "got_data", "len=%i", *bufferpos);
#endif

	while (((args = irc_recv_parse(conn, c, buffer, bufferpos)) != NULL) && (args[1] != NULL)) {
		fte_t	fte;

		if ((fte = irc_got_data_parse(conn, c, args)) != FE_SUCCESS)
			return(fte);
	}

	return(FE_SUCCESS);
}

static fte_t irc_got_data_connecting(firetalk_t conn, irc_conn_t *c, unsigned char *buffer, unsigned short *bufferpos) {
	char **args;

#ifdef DEBUG_ECHO
	irc_echof(conn, c, "got_data_connecting", "len=%i", *bufferpos);
#endif

	while (((args = irc_recv_parse(conn, c, buffer, bufferpos)) != NULL) && (args[1] != NULL)) {
		if (strcmp(args[1], "ERROR") == 0) {
			irc_send_printf(conn, c, "QUIT :error");
			if (args[2] == NULL)
				conn->PI->connectfailed(conn, c, FE_PACKET, "Server returned ERROR");
			else
				conn->PI->connectfailed(conn, c, FE_PACKET, args[2]);
			return(FE_PACKET);
		} else {
			switch (atoi(args[1])) {
			  case   1: /* :PREFIX 001 sn :Welcome message */
				if (strcmp(c->nickname, args[2]) != 0) {
					conn->PI->user_nickchanged(conn, c, c->nickname, args[2]);
					free(c->nickname);
					c->nickname = strdup(args[2]);
					if (c->nickname == NULL)
						abort();
					conn->PI->newnick(conn, c, args[2]);
				}
				break;
			  case 376: /* End of MOTD */
			  case 422: /* MOTD is missing */
				conn->PI->doinit(conn, c, c->nickname);
				c->sock.state = FCS_ACTIVE;
				conn->PI->connected(conn, c);
				break;
			  case 431:
			  case 432:
			  case 436:
			  case 461:
				irc_send_printf(conn, c, "QUIT :Invalid nickname");
				conn->PI->connectfailed(conn, c, FE_BADUSER, "Invalid nickname");
				return(FE_BADUSER);
			  case 433:
				irc_send_printf(conn, c, "QUIT :Invalid nickname");
				conn->PI->connectfailed(conn, c, FE_BADUSER, "Nickname in use");
				return(FE_BADUSER);
			  case 465:
				irc_send_printf(conn, c, "QUIT :banned");
				conn->PI->connectfailed(conn, c, FE_BLOCKED, "You are banned");
				return(FE_BLOCKED);
			  default: {
					fte_t	fte;

					if ((fte = irc_got_data_parse(conn, c, args)) != FE_SUCCESS)
						return(fte);
					break;
				}
			}
		}
	}

	return(FE_SUCCESS);
}

static fte_t irc_postselect(firetalk_t conn, irc_conn_t *c, fd_set *read, fd_set *write, fd_set *except) {
	fte_t	e;

	if ((e = firetalk_sock_postselect(&(c->sock), read, write, except)) != FE_SUCCESS) {
		if (c->sock.state == FCS_ACTIVE)
			conn->PI->disconnect(conn, c, e);
		else
			conn->PI->connectfailed(conn, c, e, strerror(errno));
		return(e);
	}

	if (c->sock.state == FCS_SEND_SIGNON) {
		irc_signon(conn, c, c->nickname);
		c->sock.state = FCS_WAITING_SIGNON;
	} else if (c->sock.readdata) {
		if (c->sock.state == FCS_ACTIVE)
			irc_got_data(conn, c, c->sock.buffer, &(c->sock.bufferpos));
		else
			irc_got_data_connecting(conn, c, c->sock.buffer, &(c->sock.bufferpos));
	}

	return(FE_SUCCESS);
}

static fte_t irc_connect(firetalk_t conn, client_t c, const char *server, uint16_t port, const char *const username) {
	fte_t	e;

	free(c->nickname);
	c->nickname = strdup(username);
	if (c->nickname == NULL)
		abort();
	if (server == NULL)
		server = DEFAULT_SERVER;
	if (port == 0)
		port = DEFAULT_PORT;
	if ((e = firetalk_sock_connect_host(&(c->sock), server, port)) != FE_SUCCESS)
		return(e);
	return(FE_SUCCESS);
}

static fte_t irc_sendpass(firetalk_t conn, client_t c, const char *const password) {
	return(FE_SUCCESS);
}

static fte_t irc_chat_join(firetalk_t conn, irc_conn_t *c, const char *const room) {
	return(irc_send_printf(conn, c, "JOIN %s", room));
}

static fte_t irc_chat_part(firetalk_t conn, irc_conn_t *c, const char *const room) {
	return(irc_send_printf(conn, c, "PART %s", room));
}

static fte_t irc_chat_send_message(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const message, const int auto_flag) {
	if (strcasecmp(room, ":RAW") == 0)
		return(irc_send_printf(conn, c, "%s", message));

#ifdef DEBUG_ECHO
	irc_echof(conn, c, "chat_send_message", "c=%#p, room=%#p \"%s\", message=%#p \"%s\", auto_flag=%i\n", c, room, room, message, message, auto_flag);
#endif

	if (auto_flag)
		return(irc_send_printf(conn, c, "NOTICE %s :%s", room, message));
	else
		return(irc_send_printf(conn, c, "PRIVMSG %s :%s", room, message));
}

static fte_t irc_chat_send_action(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const message, const int auto_flag) {
	return(irc_send_printf(conn, c, "PRIVMSG %s :\001ACTION %s\001", room, message));
}

static fte_t irc_chat_invite(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const who, const char *const message) {
	return(irc_send_printf(conn, c, "INVITE %s %s", who, room));
}

static fte_t irc_im_send_message(firetalk_t conn, irc_conn_t *c, const char *const dest, const char *const message, const int auto_flag) {
	char	buf[512+1];

	if (strcasecmp(dest, ":RAW") == 0)
		return(irc_send_printf(conn, c, "%s", message));

	if (auto_flag) {
		snprintf(buf, sizeof(buf), "NOTICE %s :%s", dest, message);
		firetalk_queue_append(buf, sizeof(buf), &(conn->subcode_replies), dest);
	} else {
		snprintf(buf, sizeof(buf), "PRIVMSG %s :%s", dest, message);
		firetalk_queue_append(buf, sizeof(buf), &(conn->subcode_requests), dest);
	}
	return(irc_send_printf(conn, c, "%s", buf));
}

static fte_t irc_im_send_action(firetalk_t conn, irc_conn_t *c, const char *const dest, const char *const message, const int auto_flag) {
	return(irc_send_printf(conn, c, "PRIVMSG %s :\001ACTION %s\001", dest, message));
}

static fte_t irc_chat_set_topic(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const topic) {
	return(irc_send_printf(conn, c, "TOPIC %s :%s", room, topic));
}

static fte_t irc_chat_op(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const who) {
	return(irc_send_printf(conn, c, "MODE %s +o %s", room, who));
}

static fte_t irc_chat_deop(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const who) {
	return(irc_send_printf(conn, c, "MODE %s -o %s", room, who));
}

static fte_t irc_chat_kick(firetalk_t conn, irc_conn_t *c, const char *const room, const char *const who, const char *const reason) {
	if (reason)
		return(irc_send_printf(conn, c, "KICK %s %s :%s", room, who, reason));
	else
		return(irc_send_printf(conn, c, "KICK %s %s", room, who));
}

static fte_t irc_im_add_buddy(firetalk_t conn, irc_conn_t *c, const char *const name, const char *const group, const char *const friendly) {
	if (firetalk_user_visible(conn, name) == FE_SUCCESS)
		conn->PI->im_buddyonline(conn, c, name, 1);
	return(FE_SUCCESS);
}

static fte_t irc_im_remove_buddy(firetalk_t conn, irc_conn_t *c, const char *const name, const char *const group) {
	conn->PI->im_buddyonline(conn, c, name, 0);

	return(FE_SUCCESS);
}

static fte_t irc_im_add_deny(firetalk_t conn, irc_conn_t *c, const char *const nickname) {
	if (c->usesilence == 1)
		return(irc_send_printf(conn, c, "SILENCE +%s!*@*", nickname));
	else
		return(FE_SUCCESS);
}

static fte_t irc_im_remove_deny(firetalk_t conn, irc_conn_t *c, const char *const nickname) {
	if (c->usesilence == 1)
		return(irc_send_printf(conn, c, "SILENCE -%s!*@*", nickname));
	else
		return(FE_SUCCESS);
}

static fte_t irc_im_upload_buddies(firetalk_t conn, irc_conn_t *c) {
	return(FE_SUCCESS);
}

static fte_t irc_im_upload_denies(firetalk_t conn, irc_conn_t *c) {
	return(FE_SUCCESS);
}

static fte_t irc_im_evil(firetalk_t conn, irc_conn_t *c, const char *const who) {
	return(FE_SUCCESS);
}

static fte_t irc_set_privacy(firetalk_t conn, client_t c, const char *const mode) {
	return(FE_SUCCESS);
}

static fte_t irc_get_info(firetalk_t conn, irc_conn_t *c, const char *const nickname) {
	struct s_irc_whois *whoistemp;

	whoistemp = c->whois_head;
	c->whois_head = calloc(1, sizeof(struct s_irc_whois));
	if (c->whois_head == NULL)
		abort();
	c->whois_head->nickname = strdup(nickname);
	if (c->whois_head->nickname == NULL)
		abort();
	c->whois_head->flags = 0;
	c->whois_head->online = 0;
	c->whois_head->idle = 0;
	c->whois_head->info = NULL;
	c->whois_head->next = whoistemp;
	return(irc_send_printf(conn, c, "WHOIS %s", nickname));
}

static fte_t irc_set_info(firetalk_t conn, irc_conn_t *c, const char *const info) {
	return(FE_SUCCESS);
}

static fte_t irc_set_away(firetalk_t conn, irc_conn_t *c, const char *const message, const int auto_flag) {
	if (message)
		return(irc_send_printf(conn, c, "AWAY :%s", message));
	else
		return(irc_send_printf(conn, c, "AWAY"));
}

static fte_t irc_periodic(firetalk_t conn) {
	return(FE_SUCCESS);
}

static char *irc_ctcp_encode(firetalk_t conn, irc_conn_t *c, const char *const command, const char *const message) {
	char	*str;

	if (message != NULL) {
		str = malloc(1 + strlen(command) + 1 + strlen(message) + 1 + 1);
		if (str == NULL)
			abort();
		sprintf(str, "\001%s %s\001", command, message);
	} else {
		str = malloc(1 + strlen(command) + 1 + 1);
		if (str == NULL)
			abort();
		sprintf(str, "\001%s\001", command);
	}

	return(str);
}

const firetalk_PD_t firetalk_protocol_irc = {
	strprotocol:		"IRC",
	periodic:		irc_periodic,
	preselect:		irc_preselect,
	postselect:		irc_postselect,
	comparenicks:		irc_compare_nicks,
	isprintable:		irc_isprint,
	disconnect:		irc_disconnect,
	connect:		irc_connect,
	sendpass:		irc_sendpass,
	save_config:		irc_save_config,
	get_info:		irc_get_info,
	set_info:		irc_set_info,
	set_away:		irc_set_away,
	set_nickname:		irc_set_nickname,
	set_password:		irc_set_password,
	im_add_buddy:		irc_im_add_buddy,
	im_remove_buddy:	irc_im_remove_buddy,
	im_add_deny:		irc_im_add_deny,
	im_remove_deny:		irc_im_remove_deny,
	im_upload_buddies:	irc_im_upload_buddies,
	im_upload_denies:	irc_im_upload_denies,
	im_send_message:	irc_im_send_message,
	im_send_action:		irc_im_send_action,
	im_evil:		irc_im_evil,
	chat_join:		irc_chat_join,
	chat_part:		irc_chat_part,
	chat_invite:		irc_chat_invite,
	chat_set_topic:		irc_chat_set_topic,
	chat_op:		irc_chat_op,
	chat_deop:		irc_chat_deop,
	chat_kick:		irc_chat_kick,
	chat_send_message:	irc_chat_send_message,
	chat_send_action:	irc_chat_send_action,
	subcode_encode:		irc_ctcp_encode,
	set_privacy:		irc_set_privacy,
	room_normalize:		irc_normalize_room_name,
	create_handle:		irc_create_handle,
	destroy_handle:		irc_destroy_handle,
};
