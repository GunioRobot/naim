#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>

#define DEFAULT_SERVER	"toc.n.ml.org"
#define DEFAULT_PORT	9898

//#define TOC_HTML_MAXLEN 65536
#define TOC_CLIENTSEND_MAXLEN (2*1024)
#define TOC_SERVERSEND_MAXLEN (8*1024)
#define ECT_TOKEN	"<font ECT=\""
#define ECT_ENDING	"\"></font>"

struct s_toc_room {
	struct s_toc_room *next;
	int	exchange;
	char	*name;
	long	id;
	unsigned char
		invited:1,
		joined:1;
};

typedef struct s_toc_connection *client_t;
#define _HAVE_CLIENT_T

#include "firetalk-int.h"
#include "firetalk.h"

struct s_toc_infoget {
	struct s_toc_infoget *next;
	firetalk_sock_t sock;
};

struct s_toc_connection {
	unsigned short local_sequence;		/* our sequence number */
	unsigned short remote_sequence;		/* toc's sequence number */
	char	*nickname,			/* our nickname (correctly spaced) */
		*password,
		*awaymsg;
	firetalk_sock_t sock;
	time_t	awaysince;
	struct s_toc_room *room_head;
	struct s_toc_infoget *infoget_head;
	time_t	lastself,			/* last time we checked our own status */
		lasttalk;			/* last time we talked */
	long	lastidle;			/* last idle that we told the server */
	unsigned char
		passchange:1,			/* whether we're changing our password right now */
		gotconfig:1;
	int	connectstate,
		permit_mode;
	char	buddybuf[1024];
	int	buddybuflen;
	char	*buddybuflastgroup;
};

#define SFLAP_FRAME_SIGNON ((unsigned char)1)
#define SFLAP_FRAME_DATA ((unsigned char)2)
#define SFLAP_FRAME_ERROR ((unsigned char)3)
#define SFLAP_FRAME_SIGNOFF ((unsigned char)4)
#define SFLAP_FRAME_KEEPALIVE ((unsigned char)5)

#define SIGNON_STRING "FLAPON\r\n\r\n"

#define TOC_HEADER_LENGTH 6
#define TOC_SIGNON_LENGTH 24
#define TOC_HOST_SIGNON_LENGTH 4
#define TOC_USERNAME_MAXLEN 16

#include "toc2_uuids.h"

static char lastinfo[TOC_USERNAME_MAXLEN+1] = "";

#ifdef ENABLE_DIRECTORY
static char lastdir[TOC_USERNAME_MAXLEN+1] = "";
#endif

/* Internal Function Declarations */

static unsigned short toc_fill_header(unsigned char *const header, unsigned char const frame_type, unsigned short const sequence, unsigned short const length);
static unsigned short toc_fill_signon(unsigned char *const signon, const char *const username);
static unsigned char toc_get_frame_type_from_header(const unsigned char *const header);
static unsigned short toc_get_sequence_from_header(const unsigned char *const header);
static unsigned short toc_get_length_from_header(const unsigned char *const header);
static char *toc_quote(const char *const string, const int outside_flag);
static int toc_internal_disconnect(firetalk_t conn, client_t c, const int error);
static int toc_internal_split_exchange(const char *const string);
static char *toc_internal_split_name(const char *const string);
static fte_t toc_send_printf(firetalk_t conn, client_t c, const char *const format, ...);

#include <assert.h>
#ifdef DEBUG_ECHO
static void toc_echof(firetalk_t conn, client_t c, const char *const where, const char *const format, ...) {
	va_list ap;
	char	buf[8*1024];
	void	statrefresh(void);

	va_start(ap, format);
	vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	while (buf[strlen(buf)-1] == '\n')
		buf[strlen(buf)-1] = 0;
	if (*buf != 0)
		conn->PI->chat_getmessage(conn, c, ":DEBUG", where, 0, firetalk_htmlentities(buf));

	statrefresh();
}

static void toc_echo_send(firetalk_t conn, client_t c, const char *const where, const unsigned char *const data, size_t _length) {
	unsigned char	ft;
	unsigned short	sequence,
			length;

	assert(_length > 4);

	length = toc_get_length_from_header(data);
	ft = toc_get_frame_type_from_header(data);
	sequence = toc_get_sequence_from_header(data);

	assert(length == (_length-6));

	toc_echof(conn, c, where, "frame=%X, sequence=out:%i, length=%i, value=[%.*s]\n",
		ft, sequence, length, length, data+TOC_HEADER_LENGTH);
}
#endif

/* Internal Function Definitions */

static fte_t toc_find_packet(firetalk_t conn, client_t c, unsigned char *buffer, unsigned short *bufferpos, char *outbuffer, const int frametype, unsigned short *l) {
	unsigned char	ft;
	unsigned short	sequence,
			length;

	if (*bufferpos < TOC_HEADER_LENGTH) /* don't have the whole header yet */
		return(FE_NOTFOUND);

	length = toc_get_length_from_header(buffer);
	if (length > (TOC_SERVERSEND_MAXLEN - TOC_HEADER_LENGTH)) {
		toc_internal_disconnect(conn, c, FE_PACKETSIZE);
		return(FE_DISCONNECT);
	}

	if (*bufferpos < length + TOC_HEADER_LENGTH) /* don't have the whole packet yet */
		return(FE_NOTFOUND);

	ft = toc_get_frame_type_from_header(buffer);
	sequence = toc_get_sequence_from_header(buffer);

	memcpy(outbuffer, &buffer[TOC_HEADER_LENGTH], length);
	*bufferpos -= length + TOC_HEADER_LENGTH;
	memmove(buffer, &buffer[TOC_HEADER_LENGTH + length], *bufferpos);
	outbuffer[length] = '\0';
	*l = length;

#ifdef DEBUG_ECHO
	if (1 /*ft != 5*/) {
		char	buf[1024*8];
		int	j;

		assert(length < sizeof(buf));
		memmove(buf, outbuffer, length+1);
		for (j = 0; j < length; j++)
			if (buf[j] == 0)
				buf[j] = '0';

		toc_echof(conn, c, "find_packet", "frame=%X, sequence=in:%i, length=%i, value=[%s]\n",
			ft, sequence, length, buf);
	}
#endif

	if (frametype == SFLAP_FRAME_SIGNON)
		c->remote_sequence = sequence;
	else {
		if (sequence != ++c->remote_sequence) {
			toc_internal_disconnect(conn, c, FE_SEQUENCE);
			return(FE_DISCONNECT);
		}
	}

	if (ft == frametype)
		return(FE_SUCCESS);
	else
		return(FE_WEIRDPACKET);
}

static unsigned short toc_fill_header(unsigned char *const header,
	unsigned char const frame_type, unsigned short const sequence,
	unsigned short const length) {
	header[0] = '*';		/* byte 0, length 1, magic 42 */
	header[1] = frame_type;		/* byte 1, length 1, frame type (defined above SFLAP_FRAME_* */
	header[2] = sequence/256;	/* byte 2, length 2, sequence number, network byte order */
	header[3] = (unsigned char) sequence%256;
	header[4] = length/256;		/* byte 4, length 2, length, network byte order */
	header[5] = (unsigned char) length%256;
	return(6+length);
}

static unsigned short toc_fill_signon(unsigned char *const signon, const char *const username) {
	size_t	length = strlen(username);

	signon[0] = 0;		/* byte 0, length 4, flap version (1) */
	signon[1] = 0;
	signon[2] = 0;
	signon[3] = 1;
	signon[4] = 0;		/* byte 4, length 2, tlv tag (1) */
	signon[5] = 1;
	signon[6] = length/256;	/* byte 6, length 2, username length, network byte order */
	signon[7] = (unsigned char)length%256;
	memcpy(signon+8, username, length);
	return(length + 8);
}

static char *aim_interpolate_variables(const char *const input, const char *const nickname) {
	static char output[16384]; /* 2048 / 2 * 16 + 1 (max size with a string full of %n's, a 16-char nick and a null at the end) */
	int o = 0, gotpercent = 0;
	size_t nl, dl, tl, l, i;
	char date[15], tim[15];
	{ /* build the date and time */
		int hour;
		int am = 1;
		struct tm *t;
		time_t b;
		b = time(NULL);
		t = localtime(&b);
		if (t == NULL)
			return(NULL);
		hour = t->tm_hour;
		if (hour >= 12)
			am = 0;
		if (hour > 12)
			hour -= 12;
		if (hour == 0)
			hour = 12;
		sprintf(tim, "%d:%02d:%02d %s", hour, t->tm_min, t->tm_sec, am == 1 ? "AM" : "PM");
		snprintf(date, 15, "%d/%d/%d", t->tm_mon + 1, t->tm_mday, t->tm_year + 1900);
	}
	nl = strlen(nickname);
	dl = strlen(date);
	tl = strlen(tim);
	l = strlen(input);
	for (i = 0; i < l; i++) {
		switch (input[i]) {
			case '%':
				if (gotpercent == 1) {
					gotpercent = 0;
					output[o++] = '%';
					output[o++] = '%';
				} else
					gotpercent = 1;
				break;
			case 'n':
				if (gotpercent == 1) {
					gotpercent = 0;
					memcpy(&output[o], nickname, nl);
					o += nl;
				} else
					output[o++] = 'n';
				break;
			case 'd':
				if (gotpercent == 1) {
					gotpercent = 0;
					memcpy(&output[o], date, dl);
					o += dl;
				} else
					output[o++] = 'd';
				break;
			case 't':
				if (gotpercent == 1) {
					gotpercent = 0;
					memcpy(&output[o], tim, tl);
					o += tl;
				} else
					output[o++] = 't';
				break;
			default:
				if (gotpercent == 1) {
					gotpercent = 0;
					output[o++] = '%';
				}
				output[o++] = input[i];

		}
	}
	output[o] = 0;
	return(output);
}

static const char *aim_normalize_room_name_int(const char *const name) {
	static char newname[2048];

	if (name == NULL)
		return(NULL);
	if (strchr(name+1, ':') != NULL)
		return(name);
	if (strlen(name) >= (sizeof(newname)-2))
		return(NULL);

	strcpy(newname, "4:");
	strcpy(newname+2, name);

	return(newname);
}

static const char *aim_normalize_room_name(firetalk_t conn, const char *const name) {
	return(aim_normalize_room_name_int(name));
}

#define STRNCMP(x,y)	(strncmp((x), (y), sizeof(y)-1))
static char *htmlclean(const char *str) {
	static char
		buf[2048];
	int	i, b = 0;

	for (i = 0; (str[i] != 0) && (b < sizeof(buf)-1); i++)
		if (STRNCMP(str+i, "&gt;") == 0) {
			buf[b++] = '>';
			i += sizeof("&gt;")-2;
		} else if (STRNCMP(str+i, "&lt;") == 0) {
			buf[b++] = '<';
			i += sizeof("&lt;")-2;
		} else if (STRNCMP(str+i, "&quot;") == 0) {
			buf[b++] = '"';
			i += sizeof("&quot;")-2;
		} else if (STRNCMP(str+i, "&nbsp;") == 0) {
			buf[b++] = ' ';
			i += sizeof("&nbsp;")-2;
		} else if (STRNCMP(str+i, "&amp;") == 0) {
			buf[b++] = '&';
			i += sizeof("&amp;")-2;
		} else
			buf[b++] = str[i];
	buf[b] = 0;

	return(buf);
}
#undef STRNCMP

static char *aim_handle_ect(firetalk_t conn, client_t c, const char *const from,
		char *const message, const int reply) {
	char	*ectbegin, *ectend, *textbegin, *textend;

	while ((ectbegin = strstr(message, ECT_TOKEN)) != NULL) {
		textbegin = ectbegin+sizeof(ECT_TOKEN)-1;

		if ((textend = strstr(textbegin, ECT_ENDING)) != NULL) {
			char	*arg;

			*textend = 0;
			ectend = textend+sizeof(ECT_ENDING)-1;

			if ((arg = strchr(textbegin, ' ')) != NULL) {
				*arg++ = 0;
				if (reply == 1)
					conn->PI->subcode_reply(conn, c, from, textbegin, htmlclean(arg));
				else
					conn->PI->subcode_request(conn, c, from, textbegin, htmlclean(arg));
			} else {
				if (reply == 1)
					conn->PI->subcode_reply(conn, c, from, textbegin, NULL);
				else
					conn->PI->subcode_request(conn, c, from, textbegin, NULL);
			}
			memmove(ectbegin, ectend, strlen(ectend)+1);
		} else
			break;
	}
	return(message);
}

static void toc_infoget_parse_userinfo(firetalk_t conn, client_t c, struct s_toc_infoget *i) {
	char	*tmp, *str = i->sock.buffer,
		*name, *info;
	int	class = 0, warning;
	long	online, idle;

#ifdef DEBUG_ECHO
	toc_echof(conn, c, "infoget_parse_userinfo", "buffer=[%s]", str);
#endif

#	define USER_STRING	"Username : <B>"
#	define WARNING_STRING	"Warning Level : <B>"
#	define ONLINE_STRING	"Online Since : <B>"
#	define IDLE_STRING	"Idle Minutes : <B>" 
#	define INFO_STRING	"<hr><br>\n"
#	define INFO_END_STRING	"<br><hr><I>Legend:"

	if ((tmp = strstr(str, USER_STRING)) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, (lastinfo[0] == '\0')?NULL:lastinfo, "Can't find username in info HTML");
		return;
	}
	tmp += sizeof(USER_STRING) - 1;
	if ((str = strchr(tmp, '<')) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, (lastinfo[0] == '\0')?NULL:lastinfo, "Can't find end of username in info HTML");
		return;
	}
	*str++ = 0;
	name = tmp;


	if ((tmp = strstr(str, WARNING_STRING)) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, name, "Can't find warning level in info HTML");
		return;
	}
		*tmp = 0;
		if ((strstr(str, "free_icon.") != NULL) || (strstr(str, "dt_icon.") != NULL))
			class |= FF_SUBSTANDARD;
		if (strstr(str, "aol_icon.") != NULL)
			class |= FF_NORMAL;
		if (strstr(str, "admin_icon.") != NULL)
			class |= FF_ADMIN;
	tmp += sizeof(WARNING_STRING) - 1;
	if ((str = strchr(tmp, '<')) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, (lastinfo[0] == '\0')?NULL:lastinfo, "Can't find end of warning level in info HTML");
		return;
	}
	*str++ = 0;
	warning = atoi(tmp);


	if ((tmp = strstr(str, ONLINE_STRING)) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, name, "Can't find online time in info HTML");
		return;
	}
	tmp += sizeof(ONLINE_STRING) - 1;
	if ((str = strchr(tmp, '<')) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, (lastinfo[0] == '\0')?NULL:lastinfo, "Can't find end of online time in info HTML");
		return;
	}
	*str++ = 0;
#ifdef HAVE_STRPTIME
	{
		struct tm tm;

		memset(&tm, 0, sizeof(tm));
		if ((void *)strptime(tmp, "%a %b %d %H:%M:%S %Y", &tm) == NULL)
			online = 0;
		else
			online = mktime(&tm);
		{ /* AOL's servers are permanently in UTC-0400 */
			struct tm *tmptr;
			time_t loc = 100000, gm = 100000;
			int	dsto;

			tmptr = localtime(&loc);
			loc = mktime(tmptr);
			dsto = tm.tm_isdst?4:5;
			tmptr = gmtime(&gm);
			gm = mktime(tmptr);
			online += loc-gm+dsto*60*60;
		}
	}
#else
	online = 0;
#endif


	if ((tmp = strstr(str, IDLE_STRING)) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, name, "Can't find idle time in info HTML");
		return;
	}
	tmp += sizeof(IDLE_STRING) - 1;
	if ((str = strchr(tmp, '<')) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, (lastinfo[0] == '\0')?NULL:lastinfo, "Can't find end of idle time in info HTML");
		return;
	}
	*str++ = 0;
	idle = atoi(tmp);


	if ((tmp = strstr(str, INFO_STRING)) == NULL) {
		conn->PI->error(conn, c, FE_INVALIDFORMAT, (lastinfo[0] == '\0')?NULL:lastinfo, "Can't find info string in info HTML");
		return;
	}
	tmp += sizeof(INFO_STRING) - 1;
	if ((str = strstr(tmp, INFO_END_STRING)) == NULL)
		info = NULL;
	else {
		*str = 0;
		info = aim_interpolate_variables(tmp, c->nickname);
		info = aim_handle_ect(conn, c, name, info, 1);
	}

	conn->PI->gotinfo(conn, c, name, info, warning, online, idle, class);
}

#ifdef ENABLE_DIRECTORY
static void toc_infoget_parse_dir(client_t c, struct s_toc_infoget *inf) {
	const char *tokens[] = {
			/* 0 */ "<TR><TD>First Name:</TD><TD><B>",
			/* 1 */ "<TR><TD>Last Name:</TD><TD><B>",
			/* 2 */ "<TR><TD>Middle Name:</TD><TD><B>",
			/* 3 */ "<TR><TD>Maiden Name:</TD><TD><B>",
			/* 4 */ "<TR><TD>Country:</TD><TD><B>",
			/* 5 */ "<TR><TD>State:</TD><TD><B>",
			/* 6 */ "<TR><TD>City:</TD><TD><B>",
			/* 7 */ "<TR><TD>Web Access:</TD><TD><B>",
			/* 8 */ "<TR><TD>Nick Name:</TD><TD><B>",
			/* 9 */ "<TR><TD>Postal Code:</TD><TD><B>",
			/* 0 */ "<TR><TD>Street Address:</TD><TD><B>",
		};
	char	*values[sizeof(tokens)/sizeof(*tokens)] = { 0 };
	int	i;

	for (i = 0; i < sizeof(tokens)/sizeof(*tokens); i++) {
		char	*begin, *end;

		if ((begin = strstr(inf->buffer, tokens[i])) != NULL) {
			begin += strlen(tokens[i]);
			if ((end = strchr(begin, '<')) != NULL) {
				*end = 0;
				values[i] = strdup(begin);
				*end = '<';
			}
		}
	}

	if (values[1] == NULL) {
		values[1] = values[3];
		values[3] = NULL;
	}
	if (values[8] == NULL) {
		values[8] = values[2];
		values[2] = NULL;
	}

	conn->PI->gotdir(conn, c, lastdir, values[0], values[1], values[8],
		values[6], values[5], values[4]);

	for (i = 0; i < sizeof(values)/sizeof(*values); i++)
		free(values[i]);
}
#endif

static void toc_infoget_parse(firetalk_t conn, client_t c, struct s_toc_infoget *inf) {
	if (inf->sock.bufferpos == sizeof(inf->sock.buffer))
		inf->sock.bufferpos = sizeof(inf->sock.buffer)-1;
	inf->sock.buffer[inf->sock.bufferpos] = 0;

	if (strstr(inf->sock.buffer, "Not found.") != NULL)
		conn->PI->error(conn, c, FE_NOTFOUND, NULL, NULL);
#ifdef ENABLE_DIRECTORY
	else if (strstr(inf->sock.buffer, "Dir Information") != NULL)
		toc_infoget_parse_dir(c, inf);
#endif
	else
		toc_infoget_parse_userinfo(conn, c, inf);
}

static void toc_infoget_remove(firetalk_t conn, client_t c, struct s_toc_infoget *inf, char *error) {
	struct s_toc_infoget *m, *m2;

	if (error != NULL)
		conn->PI->error(conn, c, FE_USERINFOUNAVAILABLE, NULL, error);
	m = c->infoget_head;
	m2 = NULL;
	while (m != NULL) {
		if (m == inf) {
			if (m2 == NULL)
				c->infoget_head = m->next;
			else
				m2->next = m->next;
			firetalk_sock_close(&(m->sock));
			free(m);
			return;
		}
		m2 = m;
		m = m->next;
	}
}

static fte_t toc_im_add_buddy_flush(firetalk_t conn, client_t c) {
	if (c->buddybuflen > 0) {
		free(c->buddybuflastgroup);
		c->buddybuflastgroup = strdup("");
		c->buddybuflen = 0;
		return(toc_send_printf(conn, c, "toc2_new_buddies {%S}", c->buddybuf));
	}
	return(FE_SUCCESS);
}

static unsigned char toc_get_frame_type_from_header(const unsigned char *const header) {
	return(header[1]);
}

static unsigned short toc_get_sequence_from_header(const unsigned char *const header) {
	unsigned short sequence;

	sequence = ntohs(*((unsigned short *)(&header[2])));
	return(sequence);
}

static unsigned short toc_get_length_from_header(const unsigned char *const header) {
	unsigned short length;

	length = ntohs(*((unsigned short *)(&header[4])));
	return(length);
}

static char *toc_quote(const char *string, const int outside_flag) {
	static char output[TOC_CLIENTSEND_MAXLEN];
	size_t	length,
		counter;
	int	newcounter;

	while (*string == ' ')
		string++;

	length = strlen(string);
	if (outside_flag == 1) {
 		newcounter = 1;
		output[0] = '"';
	} else
		newcounter = 0;

	while ((length > 0) && (string[length-1] == ' '))
		length--;

	for (counter = 0; counter < length; counter++)
		if (string[counter] == '$' || string[counter] == '{' || string[counter] == '}' || string[counter] == '[' || string[counter] == ']' || string[counter] == '(' || string[counter] == ')' || string[counter] == '\'' || string[counter] == '`' || string[counter] == '"' || string[counter] == '\\') {
			if (newcounter > (sizeof(output)-4))
				return(NULL);
			output[newcounter++] = '\\';
			output[newcounter++] = string[counter];
		} else {
			if (newcounter > (sizeof(output)-3))
				return(NULL);
			output[newcounter++] = string[counter];
		}

	if (outside_flag == 1)
		output[newcounter++] = '"';
	output[newcounter] = 0;

	return(output);
}

static char *toc_hash_password(const char *const password) {
#define HASH "Tic/Toc"
	const unsigned char hash[] = HASH;
	static char output[TOC_CLIENTSEND_MAXLEN];
	size_t	length;
	int	i, newcounter;

	length = strlen(password);

	output[0] = '0';
	output[1] = 'x';

	newcounter = 2;

	for (i = 0; i < length; i++) {
		if (newcounter >= sizeof(output)-2-1)
			return(NULL);
		sprintf(output+newcounter, "%02X", password[i]^hash[i%(sizeof(hash)-1)]);
		newcounter += 2;
	}

	output[newcounter] = 0;

	return(output);
}

static fte_t toc_compare_nicks_int(const char *s1, const char *s2) {
	if ((s1 == NULL) || (s2 == NULL))
		return(FE_NOMATCH);

	while (*s1 == ' ')
		s1++;
	while (*s2 == ' ')
		s2++;
	while (*s1 != 0) {
		if (tolower((unsigned char)(*s1)) != tolower((unsigned char)(*s2)))
			return(FE_NOMATCH);
		s1++;
		s2++;
		while (*s1 == ' ')
			s1++;
		while (*s2 == ' ')
			s2++;
	}
	if (*s2 != 0)
		return(FE_NOMATCH);
	return(FE_SUCCESS);
}

static fte_t toc_compare_nicks(firetalk_t conn, const char *s1, const char *s2) {
	return(toc_compare_nicks_int(s1, s2));
}

static int toc_internal_disconnect(firetalk_t conn, client_t c, const int error) {
	if (c->sock.state != FCS_NOTCONNECTED)
		toc_send_printf(conn, c, "toc_noop");
	firetalk_sock_close(&(c->sock));

	if (c->nickname != NULL) {
		free(c->nickname);
		c->nickname = NULL;
	}
	if (c->password != NULL) {
		free(c->password);
		c->password = NULL;
	}
	if (c->awaymsg != NULL) {
		free(c->awaymsg);
		c->awaymsg = NULL;

		assert(c->awaysince > 0);
		c->awaysince = 0;
	}
	if (c->room_head != NULL) {
		struct s_toc_room *iter, *iternext;

		for (iter = c->room_head; iter != NULL; iter = iternext) {
			iternext = iter->next;
			free(iter->name);
			iter->name = NULL;
			free(iter);
		}
		c->room_head = NULL;
	}
	if (c->infoget_head != NULL) {
		struct s_toc_infoget *iter, *iternext;

		for (iter = c->infoget_head; iter != NULL; iter = iternext) {
			iternext = iter->next;
			free(iter);
		}
		c->infoget_head = NULL;
	}
	if (c->buddybuflastgroup != NULL) {
		free(c->buddybuflastgroup);
		c->buddybuflastgroup = strdup("");
		c->buddybuflen = 0;
	}

	conn->PI->disconnect(conn, c, error);
	return(FE_SUCCESS);
}

static int toc_internal_add_room(client_t c, const char *const name, const int exchange) {
	struct s_toc_room *iter;

	iter = c->room_head;
	c->room_head = calloc(1, sizeof(struct s_toc_room));
	if (c->room_head == NULL)
		abort();

	c->room_head->next = iter;
	c->room_head->name = strdup(name);
	if (c->room_head->name == NULL)
		abort();
	c->room_head->exchange = exchange;
	return(FE_SUCCESS);
}

static int toc_internal_set_joined(client_t c, const long id) {
	struct s_toc_room *iter;

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (iter->joined == 0)
			if (iter->id == id) {
				iter->joined = 1;
				return(FE_SUCCESS);
			}
	return(FE_NOTFOUND);
}

static int toc_internal_set_id(client_t c, const char *const name, const int exchange, const long id) {
	struct s_toc_room *iter;

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (iter->joined == 0)
			if ((iter->exchange == exchange) && (toc_compare_nicks_int(iter->name, name) == 0)) {
				iter->id = id;
				return(FE_SUCCESS);
			}
	return(FE_NOTFOUND);
}
	
static int toc_internal_find_exchange(client_t c, const char *const name) {
	struct s_toc_room *iter;

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (iter->joined == 0)
			if (toc_compare_nicks_int(iter->name, name) == 0)
				return(iter->exchange);
	firetalkerror = FE_NOTFOUND;
	return(0);
}

static int toc_internal_find_room_id(client_t c, const char *const name) {
	struct s_toc_room *iter;
	char	*namepart;
	int	exchange;

	namepart = toc_internal_split_name(name);
	exchange = toc_internal_split_exchange(name);

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (iter->exchange == exchange)
			if (toc_compare_nicks_int(iter->name, namepart) == 0)
				return(iter->id);
	firetalkerror = FE_NOTFOUND;
	return(0);
}

static char *toc_internal_find_room_name(client_t c, const long id) {
	struct s_toc_room *iter;
	static char newname[TOC_CLIENTSEND_MAXLEN];

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (iter->id == id) {
			snprintf(newname, sizeof(newname), "%d:%s", 
				iter->exchange, iter->name);
			return(newname);
		}
	firetalkerror = FE_NOTFOUND;
	return(NULL);
}

static int toc_internal_split_exchange(const char *const string) {
	return(atoi(string));
}

static char *toc_internal_split_name(const char *const string) {
	return(strchr(string, ':')+1);
}

static const char *toc_get_tlv_value(char **args, int arg, const int type) {
	for (; args[arg] && args[arg+1]; arg += 2)
		if (atoi(args[arg]) == type)
			return(firetalk_debase64(args[arg+1]));
	return(NULL);
}

static int toc_internal_set_room_invited(client_t c, const char *const name, const int invited) {
	struct s_toc_room *iter;

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (toc_compare_nicks_int(iter->name, name) == 0) {
			iter->invited = invited;
			return(FE_SUCCESS);
		}

	return(FE_NOTFOUND);
}

static int toc_internal_get_room_invited(client_t c, const char *const name) {
	struct s_toc_room *iter;

	for (iter = c->room_head; iter != NULL; iter = iter->next)
		if (toc_compare_nicks_int(aim_normalize_room_name_int(iter->name), name) == 0)
			return(iter->invited);

	return(-1);
}

static fte_t toc_send_printf(firetalk_t conn, client_t c, const char *const format, ...) {
	va_list	ap;
	size_t	i,
		datai = TOC_HEADER_LENGTH;
	char	data[TOC_CLIENTSEND_MAXLEN];

	va_start(ap, format);
	for (i = 0; format[i] != 0; i++) {
		if (format[i] == '%') {
			switch (format[++i]) {
			  case 'd':
			  case 'i': {
					int	i = va_arg(ap, int);

					i = snprintf(data+datai, sizeof(data)-datai, "%i", i);
					if (i > 0)
						datai += i;
					break;
				}
			  case 'x': {
					int	i = va_arg(ap, int);

					i = snprintf(data+datai, sizeof(data)-datai, "%x", i);
					if (i > 0)
						datai += i;
					break;
				}
			  case 's': {
					const char
						*s = toc_quote(va_arg(ap, char *), 1);
					size_t	slen = strlen(s);

					if ((datai+slen) > (sizeof(data)-1))
						return(FE_PACKETSIZE);
					strcpy(data+datai, s);
					datai += slen;
					break;
				}
			  case 'S': {
					const char
						*s = va_arg(ap, char *);
					size_t	slen = strlen(s);

					if ((datai+slen) > (sizeof(data)-1))
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
			if (datai > (sizeof(data)-1))
				return(FE_PACKETSIZE);
		}
	}
	va_end(ap);

#ifdef DEBUG_ECHO
	toc_echof(conn, c, "send_printf", "frame=%X, sequence=out:%i, length=%i, value=[%.*s]\n",
		SFLAP_FRAME_DATA, c->local_sequence+1,
		datai-TOC_HEADER_LENGTH, datai-TOC_HEADER_LENGTH, data+TOC_HEADER_LENGTH);
#endif

	{
		unsigned short
			length;

		data[datai] = 0;
		length = toc_fill_header((unsigned char *)data, SFLAP_FRAME_DATA, ++c->local_sequence, datai-TOC_HEADER_LENGTH+1);
		firetalk_sock_send(&(c->sock), data, length);
	}
	return(FE_SUCCESS);
}

static char *toc_get_arg0(char *const instring) {
	static char data[TOC_SERVERSEND_MAXLEN];
	char	*colon;

	if (strlen(instring) > TOC_SERVERSEND_MAXLEN) {
		firetalkerror = FE_PACKETSIZE;
		return(NULL);
	}

	strncpy(data, instring, sizeof(data)-1);
	data[sizeof(data)-1] = 0;
	colon = strchr(data, ':');
	if (colon != NULL)
		*colon = 0;
	return(data);
}

static char **toc_parse_args(char *str, const int maxargs, const char sep) {
	static char *args[256];
	int	curarg = 0;
	char	*colon;

	while ((curarg < (maxargs-1)) && (curarg < 256)
		&& ((colon = strchr(str, sep)) != NULL)) {
		args[curarg++] = str;
		*colon = 0;
		str = colon+1;
	}
	args[curarg++] = str;
	args[curarg] = NULL;
	return(args);
}

/* External Function Definitions */

static fte_t toc_isprint_int(const int c) {
	if ((c >= 0) && (c <= 255) && (isprint(c) || (c >= 160)))
		return(FE_SUCCESS);
	return(FE_INVALIDFORMAT);
}

static fte_t toc_isprint(firetalk_t conn, const int c) {
	return(toc_isprint_int(c));
}

static client_t toc_create_handle(firetalk_t conn) {
	client_t c;

	c = calloc(1, sizeof(struct s_toc_connection));
	if (c == NULL)
		abort();

	c->lasttalk = time(NULL);
	c->buddybuflastgroup = strdup("");
	firetalk_sock_init(&(c->sock));

	return(c);
}

static void toc_destroy_handle(firetalk_t conn, client_t c) {
	toc_internal_disconnect(conn, c, FE_USERDISCONNECT);

	assert(c->nickname == NULL);
	assert(c->password == NULL);
	assert(c->awaymsg == NULL);
	assert(c->awaysince == 0);
	assert(c->room_head == NULL);
	assert(c->infoget_head == NULL);
	free(c->buddybuflastgroup);
	free(c);
}

static fte_t toc_disconnect(firetalk_t conn, client_t c) {
	return(toc_internal_disconnect(conn, c, FE_USERDISCONNECT));
}

static fte_t toc_connect(firetalk_t conn, client_t c, const char *server, uint16_t port, const char *const username) {
	fte_t	e;

	free(c->nickname);
	c->nickname = strdup(username);
	if (c->nickname == NULL)
		abort();
	if (c->password != NULL) {
		free(c->password);
		c->password = NULL;
	}
	if (server == NULL)
		server = DEFAULT_SERVER;
	if (port == 0)
		port = DEFAULT_PORT;
	if ((e = firetalk_sock_connect_host(&(c->sock), server, port)) != FE_SUCCESS)
		return(e);
	return(FE_SUCCESS);
}

static fte_t toc_sendpass(firetalk_t conn, client_t c, const char *const password) {
	if (c->password != NULL)
		free(c->password);
	c->password = strdup(password);
	if (c->password == NULL)
		abort();
	if (c->sock.state == FCS_WAITING_PASSWORD)
		c->sock.state = FCS_GOT_PASSWORD;
	return(FE_SUCCESS);
}

#ifdef ENABLE_NEWGROUPS
static fte_t toc_im_remove_group(firetalk_t conn, client_t c, const char *const group) {
	char	buf[TOC_CLIENTSEND_MAXLEN];
	int	count = 0, slen = 0;

	if (conn->buddy_head != NULL) {
		struct s_firetalk_buddy *iter;

		for (iter = conn->buddy_head; iter != NULL; iter = iter->next)
			if (toc_compare_nicks_int(iter->group, group) != FE_NOMATCH) {
				char	*str = toc_quote(iter->nickname, 1);
				int	i = strlen(str);

				if (slen+i+2 >= sizeof(buf))
					return(FE_PACKET);

				buf[slen++] = ' ';
				strcpy(buf+slen, str);
				slen += i;
				count++;
			}
	}

	if (count > 0)
		toc_send_printf(conn, c, "toc2_remove_buddy%S %s", buf, group);
	toc_send_printf(conn, c, "toc2_del_group %s", group);
	return(toc_send_printf(conn, c, "toc_get_status %s", c->nickname));
}
#endif

static fte_t toc_im_remove_buddy(firetalk_t conn, client_t c, const char *const name, const char *const group) {
	return(toc_send_printf(conn, c, "toc2_remove_buddy %s %s", name, group));
}

static fte_t toc_im_add_buddy(firetalk_t conn, client_t c, const char *const name, const char *const group, const char *const friendly) {
	char	buf[1024];
	int	slen;

	if (c->gotconfig == 0)
		return(FE_SUCCESS);

	if (strcmp(c->buddybuflastgroup, group) == 0) {
		if (friendly != NULL)
			snprintf(buf, sizeof(buf), "b:%s:%s\n", name, friendly);
		else
			snprintf(buf, sizeof(buf), "b:%s\n", name);
	} else {
		if (friendly != NULL)
			snprintf(buf, sizeof(buf), "g:%s\nb:%s:%s\n", group, name, friendly);
		else
			snprintf(buf, sizeof(buf), "g:%s\nb:%s\n", group, name);
		free(c->buddybuflastgroup);
		c->buddybuflastgroup = strdup(group);
		if (c->buddybuflastgroup == NULL)
			abort();
	}
	slen = strlen(buf);

	if ((c->buddybuflen+slen+1) >= sizeof(c->buddybuf))
		toc_im_add_buddy_flush(conn, c);
	if ((c->buddybuflen+slen+1) >= sizeof(c->buddybuf))
		return(FE_PACKET);

	strcpy(c->buddybuf+c->buddybuflen, buf);
	c->buddybuflen += slen;

	return(FE_SUCCESS);
}

static fte_t toc_im_add_deny(firetalk_t conn, client_t c, const char *const name) {
	return(toc_send_printf(conn, c, "toc2_add_deny %s", name));
}

static fte_t toc_im_remove_deny(firetalk_t conn, client_t c, const char *const name) {
	return(toc_send_printf(conn, c, "toc2_remove_deny %s", name));
}

static fte_t toc_im_upload_buddies(firetalk_t conn, client_t c) {
	if (conn->buddy_head != NULL) {
		struct s_firetalk_buddy *buddyiter;

		for (buddyiter = conn->buddy_head; buddyiter != NULL; buddyiter = buddyiter->next)
			toc_im_add_buddy(conn, c, buddyiter->nickname, buddyiter->group, buddyiter->friendly);
	}

	return(FE_SUCCESS);
}

static fte_t toc_im_upload_denies(firetalk_t conn, client_t c) {
	char	data[TOC_CLIENTSEND_MAXLEN];
	struct s_firetalk_deny *denyiter;
	unsigned short length; 

	if (conn->deny_head == NULL)
		return(FE_SUCCESS);

	data[sizeof(data)-1] = 0;
	strncpy(data+TOC_HEADER_LENGTH, "toc_add_deny", (size_t)(sizeof(data)-TOC_HEADER_LENGTH-1));
	for (denyiter = conn->deny_head; denyiter != NULL; denyiter = denyiter->next) {
		strncat(data+TOC_HEADER_LENGTH, " ", (size_t)(sizeof(data)-TOC_HEADER_LENGTH-1));
		strncat(data+TOC_HEADER_LENGTH, toc_quote(denyiter->nickname, 0), (size_t)(sizeof(data)-TOC_HEADER_LENGTH-1));
		if (strlen(data+TOC_HEADER_LENGTH) > 2000) {
			length = toc_fill_header((unsigned char *)data, SFLAP_FRAME_DATA, ++c->local_sequence, strlen(&data[TOC_HEADER_LENGTH])+1);

#ifdef DEBUG_ECHO
			toc_echo_send(conn, c, "im_upload_denies", data, length);
#endif
			firetalk_sock_send(&(c->sock), data, length);
			strncpy(data+TOC_HEADER_LENGTH, "toc_add_deny", (size_t)(sizeof(data)-TOC_HEADER_LENGTH-1));
		}
	}
	length = toc_fill_header((unsigned char *)data, SFLAP_FRAME_DATA, ++c->local_sequence, strlen(&data[TOC_HEADER_LENGTH])+1);

#ifdef DEBUG_ECHO
	toc_echo_send(conn, c, "im_upload_denies", data, length);
#endif
	firetalk_sock_send(&(c->sock), data, length);
	return(FE_SUCCESS);
}

static fte_t toc_internal_send_message(firetalk_t conn, client_t c, const char *const dest, const unsigned char *const message, const int isauto, firetalk_queue_t *queue) {
	char	buf[TOC_CLIENTSEND_MAXLEN];
	int	i, j, len = strlen(message);

	assert(c != NULL);
	assert(dest != NULL);
	assert(*dest != 0);
	assert(message != NULL);

	if (!isauto)
		c->lasttalk = time(NULL);

	if (len >= sizeof(buf)-1)
		return(FE_PACKETSIZE);

	for (j = i = 0; (i < len) && (j < sizeof(buf)-1); i++)
		if (toc_isprint_int(message[i]) == FE_SUCCESS)
			buf[j++] = message[i];
		else {
			char	numbuf[10];

			snprintf(numbuf, sizeof(numbuf), "&#%i;", message[i]);
			if (j+strlen(numbuf) >= sizeof(buf)-1)
				return(FE_PACKETSIZE);
			strcpy(buf+j, numbuf);
			j += strlen(numbuf);
		}
	buf[j] = 0;

	firetalk_queue_append(buf, sizeof(buf), queue, dest);

#ifdef DEBUG_ECHO
	toc_echof(conn, c, "internal_send_message", "dest=[%s] message=[%s] len=%i", dest, message, len);
#endif

	return(toc_send_printf(conn, c, "toc2_send_im %s %s%S", dest, buf, isauto?" auto":""));
}

static fte_t toc_im_send_reply(firetalk_t conn, client_t c, const char *const dest, const char *const message) {
	assert(dest != NULL);
	assert(message != NULL);

	if (strcasecmp(dest, ":RAW") == 0)
		return(toc_send_printf(conn, c, "%S", message));

	return(toc_internal_send_message(conn, c, dest, aim_interpolate_variables(message, dest), 1, &(conn->subcode_replies)));
}

static fte_t toc_im_send_message(firetalk_t conn, client_t c, const char *const dest, const char *const message, const int auto_flag) {
	assert(dest != NULL);
	assert(message != NULL);

	if (strcasecmp(dest, ":RAW") == 0)
		return(toc_send_printf(conn, c, "%S", message));

	if (auto_flag)
		return(toc_im_send_reply(conn, c, dest, message));

	return(toc_internal_send_message(conn, c, dest, message, 0, &(conn->subcode_requests)));
}

static fte_t toc_im_send_action(firetalk_t conn, client_t c, const char *const dest, const char *const message, const int auto_flag) {
	char	tempbuf[TOC_CLIENTSEND_MAXLEN]; 

	if (strcasecmp(dest, ":RAW") == 0)
		return(toc_send_printf(conn, c, "%S", message));

	if (strlen(message) > 2042)
		return(FE_PACKETSIZE);

	snprintf(tempbuf, sizeof(tempbuf), "/me %s", message);
	return(toc_internal_send_message(conn, c, dest, tempbuf, 0, &(conn->subcode_requests)));
}

static fte_t toc_preselect(firetalk_t conn, client_t c, fd_set *my_read, fd_set *my_write, fd_set *my_except, int *n) {
	struct s_toc_infoget *inf;

	if (c->sock.state == FCS_NOTCONNECTED)
		return(FE_SUCCESS);

	firetalk_sock_preselect(&(c->sock), my_read, my_write, my_except, n);

	for (inf = c->infoget_head; inf != NULL; inf = inf->next)
		firetalk_sock_preselect(&(inf->sock), my_read, my_write, my_except, n);

	toc_im_add_buddy_flush(conn, c);

	return(FE_SUCCESS);
}

static fte_t toc_get_info(firetalk_t conn, client_t c, const char *const nickname) {
	strncpy(lastinfo, nickname, (size_t)TOC_USERNAME_MAXLEN);
	lastinfo[TOC_USERNAME_MAXLEN] = 0;
	return(toc_send_printf(conn, c, "toc_get_info %s", nickname));
}

#ifdef ENABLE_DIRECTORY
static fte_t toc_get_dir(firetalk_t conn, client_t c, const char *const nickname) {
	strncpy(lastdir, nickname, (size_t)TOC_USERNAME_MAXLEN);
	lastdir[TOC_USERNAME_MAXLEN] = 0;
	return(toc_send_printf(conn, c, "toc_get_dir %s", nickname));
}
#endif

static char *toc_ctcp_encode(firetalk_t conn, client_t c, const char *const command, const char *const message) {
	char	*str;

	if (message != NULL) {
		const char *encodedmsg = firetalk_htmlentities(message);

		str = malloc(sizeof(ECT_TOKEN)-1+strlen(command)+1+strlen(encodedmsg)+sizeof(ECT_ENDING)-1+1);
		if (str == NULL)
			abort();
		sprintf(str, ECT_TOKEN "%s %s" ECT_ENDING, command, encodedmsg);
	} else {
		str = malloc(sizeof(ECT_TOKEN)-1+strlen(command)+sizeof(ECT_ENDING)-1+1);
		if (str == NULL)
			abort();
		sprintf(str, ECT_TOKEN "%s" ECT_ENDING, command);
	}

	return(str);
}

static fte_t toc_set_info(firetalk_t conn, client_t c, const char *const info) {
	char	profile[1024],	/* profiles can only be a maximum of 1023 characters long, so this is good */
		*versionctcp = NULL,
		*awayctcp = NULL;
	size_t	infolen = strlen(info),
		extralen = 0;

	if (infolen >= sizeof(profile)) {
		conn->PI->error(conn, c, FE_MESSAGETRUNCATED, NULL, "Profile too long");
		return(FE_MESSAGETRUNCATED);
	}
	if ((c->awaymsg != NULL) && (c->awaysince)) {
		char	awaymsg[1024];

		snprintf(awaymsg, sizeof(awaymsg), "%lu %s", (time(NULL)-(c->awaysince))/60, c->awaymsg);
		awayctcp = toc_ctcp_encode(conn, c, "AWAY", awaymsg);
		extralen += strlen(awayctcp);
	}
	if ((versionctcp = (char *)firetalk_subcode_get_request_reply(conn, c, "VERSION")) == NULL)
		versionctcp = PACKAGE_NAME ":" PACKAGE_VERSION ":unknown";
	if ((versionctcp = toc_ctcp_encode(conn, c, "VERSION", versionctcp)) != NULL)
		extralen += strlen(versionctcp);

	if (infolen+extralen >= 1024) {
		conn->PI->error(conn, c, FE_MESSAGETRUNCATED, NULL, "Profile+away message too long, truncating");
		infolen = 1024-extralen-1;
	}
	snprintf(profile, sizeof(profile), "%.*s%s%s", infolen, info, versionctcp?versionctcp:"", awayctcp?awayctcp:"");
	free(versionctcp);
	free(awayctcp);
	return(toc_send_printf(conn, c, "toc_set_info %s", profile));
}

static fte_t toc_set_away(firetalk_t conn, client_t c, const char *const message, const int auto_flag) {
	if (message != NULL) {
		if (strlen(message) >= 1024) {
			conn->PI->error(conn, c, FE_MESSAGETRUNCATED, NULL, "Message too long");
			return(FE_MESSAGETRUNCATED);
		}
		free(c->awaymsg);
		c->awaymsg = strdup(message);
		if (c->awaymsg == NULL)
			abort();
		c->awaysince = time(NULL);
		return(toc_send_printf(conn, c, "toc_set_away %s", message));
	} else {
		if (c->awaymsg != NULL) {
			free(c->awaymsg);
			c->awaymsg = NULL;
		}
		c->awaysince = 0;
		return(toc_send_printf(conn, c, "toc_set_away"));
	}
}

static fte_t toc_set_nickname(firetalk_t conn, client_t c, const char *const nickname) {
	return(toc_send_printf(conn, c, "toc_format_nickname %s", nickname));
}

static fte_t toc_set_password(firetalk_t conn, client_t c, const char *const oldpass, const char *const newpass) {
	c->passchange++;
	return(toc_send_printf(conn, c, "toc_change_passwd %s %s", oldpass, newpass));
}

static fte_t toc_set_privacy(firetalk_t conn, client_t c, const char *const mode) {
	if (strcasecmp(mode, "ALLOW ALL") == 0)
		c->permit_mode = 1;
	else if (strcasecmp(mode, "BLOCK ALL") == 0)
		c->permit_mode = 2;
	else if (strcasecmp(mode, "ALLOW PERMIT") == 0)
		c->permit_mode = 3;
	else if (strcasecmp(mode, "BLOCK DENY") == 0)
		c->permit_mode = 4;
	else if (strcasecmp(mode, "ALLOW BUDDY") == 0)
		c->permit_mode = 5;
	else {
		conn->PI->error(conn, c, FE_BADMESSAGE, NULL, "Supported modes: ALLOW ALL, BLOCK ALL, ALLOW PERMIT, BLOCK DENY, ALLOW BUDDY");
		return(FE_BADMESSAGE);
	}

	return(toc_send_printf(conn, c, "toc2_set_pdmode %i", c->permit_mode));
}

static fte_t toc_im_evil(firetalk_t conn, client_t c, const char *const who) {
	return(toc_send_printf(conn, c, "toc_evil %s norm", who));
}

static void toc_uuid(const char *const uuid, int *A1, int *A2, int *B, int *C, int *D, int *E1, int *E2, int *E3) {
	if (strlen(uuid) <= 4) {
		*A1 = 0x0946;
		*A2 = strtol(uuid, NULL, 16);
		*B = 0x4C7F;
		*C = 0x11D1;
		*D = 0x8222;
		*E1 = 0x4445;
		*E2 = 0x5354;
		*E3 = 0x0000;
	} else {
		char	buf[5];

		buf[4] = 0;
		strncpy(buf, uuid+0, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*A1 = strtol(buf, NULL, 16);
		strncpy(buf, uuid+4, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*A2 = strtol(buf, NULL, 16);
		strncpy(buf, uuid+9, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*B = strtol(buf, NULL, 16);
		strncpy(buf, uuid+14, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*C = strtol(buf, NULL, 16);
		strncpy(buf, uuid+19, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*D = strtol(buf, NULL, 16);
		strncpy(buf, uuid+24, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*E1 = strtol(buf, NULL, 16);
		strncpy(buf, uuid+28, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*E2 = strtol(buf, NULL, 16);
		strncpy(buf, uuid+32, 4);
		assert((buf[4] == 0) && (strlen(buf) == 4));
		*E3 = strtol(buf, NULL, 16);
	}
}

static int toc_cap(int A1, int A2, int B, int C, int D, int E1, int E2, int E3) {
	int	j;

	for (j = 0; j < sizeof(toc_uuids)/sizeof(*toc_uuids); j++)
		if (((toc_uuids[j].A1 == A1) || (toc_uuids[j].A1 == -1))
		 && ((toc_uuids[j].A2 == A2) || (toc_uuids[j].A2 == -1))
		 && ((toc_uuids[j].B  == B)  || (toc_uuids[j].B  == -1))
		 && ((toc_uuids[j].C  == C)  || (toc_uuids[j].C  == -1))
		 && ((toc_uuids[j].D  == D)  || (toc_uuids[j].D  == -1))
		 && ((toc_uuids[j].E1 == E1) || (toc_uuids[j].E1 == -1))
		 && ((toc_uuids[j].E2 == E2) || (toc_uuids[j].E2 == -1))
		 && ((toc_uuids[j].E3 == E3) || (toc_uuids[j].E3 == -1)))
		return(j);
	return(-1);
}

static struct {
	fte_t		fte;
	unsigned char	hastarget:1;
	const char	*str;
} toc2_errors[] = {
  /* General Errors */
	/* 900 */ {	FE_UNKNOWN,		0, NULL },
	/* 901 */ {	FE_USERUNAVAILABLE,	1, NULL },
			/* TOC1 $1 not currently available */
			/* TOC2 User Not Logged In
				You can only send Instant Messanges to, or Chat with, buddies who are currently signed on. You need to wait and try again later when your buddy is signed on.
			*/
	/* 902 */ {	FE_USERUNAVAILABLE,	1, "You have either tried to warn someone you haven't sent a message to yet, or you have already warned them too many times today." },
			/* TOC1 Warning of $1 not currently available */
			/* TOC2 Warning Not Allowed
				You have either tried to warn someone you haven't sent an Instant Message to yet; or, you have already warned them too many times today.
			*/
	/* 903 */ {	FE_TOOFAST,		0, "You are sending commands too fast." },
			/* TOC1 A message has been dropped, you are exceeding the server speed limit */
			/* TOC2 Server Limit Exceeded
				If you send messages too fast, you will exceed the server's capacity to handle them. Please try to send your message again.
			*/
	/* 904 */ {	FE_UNKNOWN,		0, NULL },
	/* 905 */ {	FE_UNKNOWN,		0, NULL },
	/* 906 */ {	FE_UNKNOWN,		0, NULL },
	/* 907 */ {	FE_UNKNOWN,		0, NULL },
	/* 908 */ {	FE_UNKNOWN,		0, NULL },
	/* 909 */ {	FE_UNKNOWN,		0, NULL },


  /* Admin Errors */
	/* 910 */ {	FE_UNKNOWN,		0, NULL },
	/* 911 */ {	FE_BADUSER,		0, NULL },
	/* 912 */ {	FE_UNKNOWN,		0, NULL },
	/* 913 */ {	FE_UNKNOWN,		0, NULL },
	/* 914 */ {	FE_UNKNOWN,		0, NULL },
	/* 915 */ {	FE_UNKNOWN,		0, NULL },
	/* 916 */ {	FE_UNKNOWN,		0, NULL },
	/* 917 */ {	FE_UNKNOWN,		0, NULL },
	/* 918 */ {	FE_UNKNOWN,		0, NULL },
	/* 919 */ {	FE_UNKNOWN,		0, NULL },


  /* Unknown */
	/* 920 */ {	FE_UNKNOWN,		0, NULL },
	/* 921 */ {	FE_UNKNOWN,		0, NULL },
	/* 922 */ {	FE_UNKNOWN,		0, NULL },
	/* 923 */ {	FE_UNKNOWN,		0, NULL },
	/* 924 */ {	FE_UNKNOWN,		0, NULL },
	/* 925 */ {	FE_UNKNOWN,		0, NULL },
	/* 926 */ {	FE_UNKNOWN,		0, NULL },
	/* 927 */ {	FE_UNKNOWN,		0, NULL },
	/* 928 */ {	FE_UNKNOWN,		0, NULL },
	/* 929 */ {	FE_UNKNOWN,		0, NULL },


  /* Buddy List Errors */
	/* 930 */ {	FE_UNKNOWN,		0, "The Buddy List server is unavailable at this time. Wait a few minutes, and try to sign on again." },
			/* TOC1 UNDOCUMENTED */
			/* TOC2 Buddy List Server is Unavailable
				The Buddy List server is unavailable at this time, and your AIM Express (TM) connection will be closed.
				Wait a few minutes, and try to sign on again.
			*/
	/* 931 */ {	FE_UNKNOWN,		0, "Unable to add buddy or group. You may have the max allowed buddies or groups or are trying to add a buddy to a group that doesn't exist and cannot be created." },
	/* 932 */ {	FE_UNKNOWN,		0, NULL },
	/* 933 */ {	FE_UNKNOWN,		0, NULL },
	/* 934 */ {	FE_UNKNOWN,		0, NULL },
	/* 935 */ {	FE_UNKNOWN,		0, NULL },
	/* 936 */ {	FE_UNKNOWN,		0, NULL },
	/* 937 */ {	FE_UNKNOWN,		0, NULL },
	/* 938 */ {	FE_UNKNOWN,		0, NULL },
	/* 939 */ {	FE_UNKNOWN,		0, NULL },


  /* Unknown */
	/* 940 */ {	FE_UNKNOWN,		0, NULL },
	/* 941 */ {	FE_UNKNOWN,		0, NULL },
	/* 942 */ {	FE_UNKNOWN,		0, NULL },
	/* 943 */ {	FE_UNKNOWN,		0, NULL },
	/* 944 */ {	FE_UNKNOWN,		0, NULL },
	/* 945 */ {	FE_UNKNOWN,		0, NULL },
	/* 946 */ {	FE_UNKNOWN,		0, NULL },
	/* 947 */ {	FE_UNKNOWN,		0, NULL },
	/* 948 */ {	FE_UNKNOWN,		0, NULL },
	/* 949 */ {	FE_UNKNOWN,		0, NULL },


  /* Chat Errors */
	/* 950 */ {	FE_ROOMUNAVAILABLE,	1, NULL },
			/* TOC1 Chat in $1 is unavailable. */
			/* TOC2 User Unavailable
				You can only Chat with buddies who are currently signed on and available. AOL Instant Messenger users have the option to put up an "Unavailable" notice, indicating that they are away from their computer or simply too busy to answer messages.
				You need to wait and try again later when your buddy is available.
			*/
	/* 951 */ {	FE_UNKNOWN,		0, NULL },
	/* 952 */ {	FE_UNKNOWN,		0, NULL },
	/* 953 */ {	FE_UNKNOWN,		0, NULL },
	/* 954 */ {	FE_UNKNOWN,		0, NULL },
	/* 955 */ {	FE_UNKNOWN,		0, NULL },
	/* 956 */ {	FE_UNKNOWN,		0, NULL },
	/* 957 */ {	FE_UNKNOWN,		0, NULL },
	/* 958 */ {	FE_UNKNOWN,		0, NULL },
	/* 959 */ {	FE_UNKNOWN,		0, NULL },


  /* IM & Info Errors */
	/* 960 */ {	FE_TOOFAST,		1, "You are sending messages too fast." },
			/* TOC1 You are sending message too fast to $1 */
			/* TOC2 Server Limit Exceeded
				Sending messages too fast will exceed the server's capacity to handle them.
			*/
	/* 961 */ {	FE_INCOMINGERROR,	1, "You missed a message because it was too big." },
			/* TOC1 You missed an im from $1 because it was too big. */
			/* TOC2 Server Limit Exceeded
				The sender sent their messages too fast, exceeding the server's capacity to handle them.
			*/
	/* 962 */ {	FE_INCOMINGERROR,	1, "You missed a message because it was sent too fast." },
			/* TOC1 You missed an im from $1 because it was sent too fast. */
			/* TOC2 Server Limit Exceeded
				The Instant Messenging system is designed to accommodate normal conversions, consisting of a few lines of text at a time. Larger messages, like a magazine article or a full-length letter, might get dropped. Please ask the sender to send their message through e-mail instead.
			*/
	/* 963 */ {	FE_UNKNOWN,		0, NULL },
	/* 964 */ {	FE_UNKNOWN,		0, NULL },
	/* 965 */ {	FE_UNKNOWN,		0, NULL },
	/* 966 */ {	FE_UNKNOWN,		0, NULL },
	/* 967 */ {	FE_UNKNOWN,		0, NULL },
	/* 968 */ {	FE_UNKNOWN,		0, NULL },
	/* 969 */ {	FE_UNKNOWN,		0, NULL },


  /* Dir Errors */
	/* 970 */ {	FE_SUCCESS,		0, NULL },
			/* TOC1 Failure */
			/* TOC2 UNDOCUMENTED */
	/* 971 */ {	FE_USERINFOUNAVAILABLE,	0, "Too many matches." },
			/* TOC1 Too many matches */
			/* TOC2 UNDOCUMENTED */
	/* 972 */ {	FE_USERINFOUNAVAILABLE,	0, "Need more qualifiers." },
			/* TOC1 Need more qualifiers */
			/* TOC2 UNDOCUMENTED */
	/* 973 */ {	FE_USERINFOUNAVAILABLE,	0, "Directory service unavailable." },
			/* TOC1 Dir service temporarily unavailable */
			/* TOC2 UNDOCUMENTED */
	/* 974 */ {	FE_USERINFOUNAVAILABLE,	0, "Email lookup restricted." },
			/* TOC1 Email lookup restricted */
			/* TOC2 UNDOCMENTED */
	/* 975 */ {	FE_USERINFOUNAVAILABLE,	0, "Keyword ignored." },
			/* TOC1 Keyword Ignored */
			/* TOC2 UNDOCUMENTED */
	/* 976 */ {	FE_USERINFOUNAVAILABLE,	0, "No keywords." },
			/* TOC1 No Keywords */
			/* TOC2 UNDOCUMENTED */
	/* 977 */ {	FE_SUCCESS,		0, NULL },
			/* TOC1 Language not supported */
			/* TOC2 UNDOCUMENTED */
	/* 978 */ {	FE_USERINFOUNAVAILABLE,	0, "Country not supported." },
			/* TOC1 Country not supported */
			/* TOC2 UNDOCUMENTED */
	/* 979 */ {	FE_USERINFOUNAVAILABLE,	0, "Failure unknown." },
			/* TOC1 Failure unknown $1 */
			/* TOC2 UNDOCUMENTED */


  /* Auth errors */
	/* 980 */ {	FE_BADUSERPASS,		0, "The Screen Name or Password was incorrect. Passwords are case sensitive, so make sure your caps lock key isn't on." },
			/* TOC1 Incorrect nickname or password. */
			/* TOC2 Incorrect Screen Name or Password
				The Screen Name or Password was incorrect. Passwords are case sensitive, so make sure your caps lock key isn't on.
				AIM Express (TM) uses your AIM Screen Name and Password, not your AOL Password. If you need an AIM password please visit AOL Instant Messenger.
				Forgot your password? Click here.
			*/
	/* 981 */ {	FE_SERVER,		0, "The service is unavailable currently. Please try again in a few minutes." },
			/* TOC1 The service is temporarily unavailable. */
			/* TOC2 Service Offline
				The service is unavailable currently. Please try again in a few minutes.
			*/
	/* 982 */ {	FE_BLOCKED,		0, "Your warning level is too high to sign on." },
			/* TOC1 Your warning level is currently too high to sign on. */
			/* TOC2 Warning Level too High
				Your warning level is current too high to sign on. You will need to wait between a few minutes and several hours before you can log back in.
			*/
	/* 983 */ {	FE_BLOCKED,		0, "You have been connected and disconnecting too frequently. Wait 10 minutes and try again. If you continue to try, you will need to wait even longer." },
			/* TOC1 You have been connecting and disconnecting too frequently.  Wait 10 minutes and try again. If you continue to try, you will need to wait even longer. */
			/* TOC2 Connecting too Frequently
				You have been connecting and disconnecting too frequently. You will need to wait around 10 minutes before you will be allowed to sign on again.
			*/
	/* 984 */ {	FE_UNKNOWN,		0, NULL },
	/* 985 */ {	FE_UNKNOWN,		0, NULL },
	/* 986 */ {	FE_UNKNOWN,		0, NULL },
	/* 987 */ {	FE_UNKNOWN,		0, NULL },
	/* 988 */ {	FE_UNKNOWN,		0, NULL },
	/* 989 */ {	FE_UNKNOWN,		0, "An unknown signon error has occured. Please wait 10 minutes and try again." },
			/* TOC1 An unknown signon error has occurred $1 */
			/* TOC2 Unknown Error
				An unknown signon error has occured. Please wait 10 minutes and try again.
			*/


  /* Client Errors */
	/* 990 */ {	FE_UNKNOWN,		0, NULL },
	/* 991 */ {	FE_UNKNOWN,		0, NULL },
	/* 992 */ {	FE_UNKNOWN,		0, NULL },
	/* 993 */ {	FE_UNKNOWN,		0, NULL },
	/* 994 */ {	FE_UNKNOWN,		0, NULL },
	/* 995 */ {	FE_UNKNOWN,		0, NULL },
	/* 996 */ {	FE_UNKNOWN,		0, NULL },
	/* 997 */ {	FE_UNKNOWN,		0, NULL },
	/* 998 */ {	FE_UNKNOWN,		0, "The service believes you are using an outdated client; this is possibly an attempt to block third-party programs such as the one you are using. Check your software's web site for an updated version." },
			/* TOC1 UNDOCUMENTED */
			/* TOC2 AIM Express (TM) Update
				Your browser is using a cached version of Quick Buddy that is now outdated. To remedy this, simply quit or close your browser and re-launch it. The next time you load Quick Buddy, it will be automatically updated.
			*/
	/* 999 */ {	FE_UNKNOWN,		0, NULL },
};

static fte_t toc_got_data(firetalk_t conn, client_t c, unsigned char *buffer, unsigned short *bufferpos) {
	char	*tempchr1, *arg0, **args,
		data[TOC_SERVERSEND_MAXLEN - TOC_HEADER_LENGTH + 1];
	fte_t	r;
	unsigned short l;

#ifdef DEBUG_ECHO
	toc_echof(conn, c, "got_data", "len=%i", *bufferpos);
#endif

  got_data_start:
	r = toc_find_packet(conn, c, c->sock.buffer, &(c->sock.bufferpos), data, SFLAP_FRAME_DATA, &l);
	if (r == FE_NOTFOUND)
		return(FE_SUCCESS);
	else if (r != FE_SUCCESS)
		return(r);

	arg0 = toc_get_arg0(data);
	if (arg0 == NULL)
		return(FE_SUCCESS);
	if (strcmp(arg0, "ERROR") == 0) {
		/* ERROR:<Error Code>:Var args */
		int	err;

		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (args[1] == NULL) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}
		err = atoi(args[1]);
		if ((err < 900) || (err > 999)) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}
		if (err == 911) {
			/* TOC1 Error validating input */
			/* TOC2 UNDOCUMENTED */
			if (c->passchange != 0) {
				c->passchange--;
				conn->PI->error(conn, c, FE_NOCHANGEPASS, NULL, NULL);
				goto got_data_start;
			}
		}
		err -= 900;
		if (toc2_errors[err].fte != FE_SUCCESS)
			conn->PI->error(conn, c, toc2_errors[err].fte,
				toc2_errors[err].hastarget?args[2]:NULL,
				toc2_errors[err].str?toc2_errors[err].str:(toc2_errors[err].fte == FE_UNKNOWN)?args[1]:NULL);
	} else if (strcmp(arg0, "IM_IN_ENC2") == 0) {
		/* IM_IN:<Source User>:<Auto Response T/F?>:<Message> */
		/* 1 source
		** 2 'T'=auto, 'F'=normal
		** 3 UNKNOWN TOGGLE
		** 4 UNKNORN TOGGLE
		** 5 user class
		** 6 UNKNOWN TOGGLE from toc2_send_im_enc arg 2
		** 7 encoding [toc2_send_im_enc arg 3] "A"=ASCII "L"=Latin1 "U"=UTF8
		** 8 language, for example "en"
		** 9 message
		** 
		** Cell phone	IM_IN_ENC2:+number:F:F:F: C,:F:L:en:message
		** Spam bot	IM_IN_ENC2:buddysn:F:F:F: U,:F:A:en:message
		** naim 0.11.6	IM_IN_ENC2:buddysn:F:F:F: O,:F:A:en:message
		** naim 0.12.0	IM_IN_ENC2:buddysn:F:F:T: O,:F:A:en:message
		** WinAIM	IM_IN_ENC2:buddysn:F:F:T: O,:F:A:en:<HTML><BODY BGCOLOR="#ffffff"><FONT LANG="0">message</FONT></BODY></HTML>
		** ICQ user	IM_IN_ENC2:useruin:F:F:T: I,:F:A:en:message
		**                                 | | |  |  | |  |
		**                                 | | |  |  | |  language, limited to real languages (either two-letter code or "x-bad"), set by sender in toc2_send_im_enc
		**                                 | | |  |  | character encoding, can be A L or U, set by sender in toc2_send_im_enc
		**                                 | | |  |  unknown meaning, can be T or F, set by sender in toc2_send_im_enc
		**                                 | | |  class, see UPDATE_BUDDY2 below
		**                                 | | seems to be T for TOC2/Oscar and F for cell phones and TOC1
		**                                 | unknown, always seems to be F
		**                                 away status, T when away and F normally
		*/
		char	*name, *message;
		int	isauto;

		args = toc_parse_args(data, 10, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if ((args[1] == NULL) || (args[2] == NULL) || (args[9] == NULL)) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}

		name = args[1];
		isauto = (args[2][0]=='T')?1:0;
		message = args[9];

		aim_handle_ect(conn, c, name, message, isauto);
		if (*message != 0) {
			char	*mestart;

			if (strncasecmp(message, "/me ", 4) == 0)
				conn->PI->im_getaction(conn, c, name, isauto,
					message+4);
			else if ((mestart = strstr(message, ">/me ")) != NULL)
				conn->PI->im_getaction(conn, c, name, isauto,
					mestart+5);
			else {
#if defined(DEBUG_ECHO) && defined(HAVE_ASPRINTF)
				char	*A, *B, *C, *encoding, *newmessage = NULL;

				if (args[5][0] == ' ')
					A = "";
				else if (args[5][0] == 'A')
					A = "AOL ";
				else
					A = "unknown0 ";

				if (args[5][1] == ' ')
					B = "";
				else if (args[5][1] == 'A')
					B = "ADMINISTRATOR";
				else if (args[5][1] == 'C')
					B = "WIRELESS";
				else if (args[5][1] == 'I')
					B = "ICQ";
				else if (args[5][1] == 'O')
					B = "OSCAR_FREE";
				else if (args[5][1] == 'U')
					B = "DAMNED_TRANSIENT";
				else
					B = "unknown1";

				if ((args[5][2] == ' ') || (args[5][2] == 0) || (args[5][2] == ','))
					C = "";
				else if (args[5][1] == 'U')
					C = " UNAVAILABLE";
				else
					C = " unknown2";

				if (args[7][0] == 'A')
					encoding = "ASCII";
				else if (args[7][0] == 'L')
					encoding = "Latin1";
				else if (args[7][0] == 'U')
					encoding = "UTF8";
				else
					encoding = "unknown";

				asprintf(&newmessage, "[%s%s%s, %s, %s %s %s %s] %s",
					A, B, C,
					encoding,
					args[2], args[3], args[4], args[6],
					message);
				message = newmessage;
#endif
				if (isauto) /* interpolate only auto-messages */
					conn->PI->im_getmessage(conn, c,
						name, 1, aim_interpolate_variables(message, c->nickname));
				else
					conn->PI->im_getmessage(conn, c,
						name, 0, message);
#if defined(DEBUG_ECHO) && defined(HAVE_ASPRINTF)
				free(message);
#endif
			}
		}
		conn->PI->im_buddyonline(conn, c, name, 1);
		conn->PI->typing(conn, c, name, 0);
	} else if (strcmp(arg0, "CLIENT_EVENT2") == 0) {
		/* 1 source
		** 2 status
		*/
		char	*name;
		int	typinginfo;

		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2]) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}

		name = args[1];
		typinginfo = atol(args[2]);

		conn->PI->typing(conn, c, name, typinginfo);
	} else if (strcmp(arg0, "UPDATE_BUDDY2") == 0) {
		/* UPDATE_BUDDY:<Buddy User>:<Online? T/F>:<Evil Amount>:<Signon Time>:<IdleTime>:<UC>:<status code> */
		/* 1 source
		** 2 'T'=online, 'F'=offline
		** 3 warning level out of 100
		** 4 signon time in seconds since epoch
		** 5 idle time in minutes
		** 6 flags: ABC; A in 'A'=AOL user;
		** 	B in 'A'=admin, 'C'=cell phone, 'I'=ICQ, 'O'=normal, 'U'=unconfirmed;
		** 	C in 'U'=away
		** 7 status code, used in ICQ
		*/
		char	*name;
		int	isaway;
		long	online, warn, idle;

		args = toc_parse_args(data, 8, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2] || !args[3] || !args[4] || !args[5]
			|| !args[6] || !args[7]) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}

		name = args[1];
		if (args[2][0] == 'T') {
			online = atol(args[4]);
			if (online == 0)
				online = 1;
		} else
			online = 0;
		isaway = (args[6][2]=='U')?1:0;
		warn = atol(args[3]);
		idle = atol(args[5]);
		
		conn->PI->im_buddyonline(conn, c, name, online);
		if (online != 0) {
			conn->PI->im_buddyaway(conn, c, name, isaway);
			conn->PI->idleinfo(conn, c, name, idle);
			conn->PI->warninfo(conn, c, name, warn);
		} else {
			assert(isaway == 0);
			assert(idle == 0);
/*			assert(warn == 0);	Warning levels survive signing off*/
		}
	} else if (strcmp(arg0, "BUDDY_CAPS2") == 0) {
		/* 1 source
		** 2 capabilities separated by commas
		*/
		char	capstring[1024],
			*name, **caps;
		int	i, firstcap;

		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2]) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}

		name = args[1];
		caps = toc_parse_args(args[2], 255, ',');
		firstcap = strtol(caps[0], NULL, 16);
		for (i = 0; i < sizeof(toc_firstcaps)/sizeof(*toc_firstcaps); i++)
			if (toc_firstcaps[i].val == firstcap) {
				snprintf(capstring, sizeof(capstring), "%s", toc_firstcaps[i].name);
				break;
			}
		if (i == sizeof(toc_firstcaps)/sizeof(*toc_firstcaps))
			snprintf(capstring, sizeof(capstring), "UNKNOWN_TYPE_%X", firstcap);

		for (i = 1; (caps[i] != NULL) && (*caps[i] != 0); i++) {
			int	j, A1, A2, B, C, D, E1, E2, E3;

			toc_uuid(caps[i], &A1, &A2, &B, &C, &D, &E1, &E2, &E3);
			j = toc_cap(A1, A2, B, C, D, E1, E2, E3);
			if (j != -1) {
				if (strcmp(toc_uuids[j].name, "THIRD_PARTY_RANGE") != 0)
					snprintf(capstring+strlen(capstring), sizeof(capstring)-strlen(capstring),
						" %s", toc_uuids[j].name);
				else {
#define O(x)	(((x) == 0)?0:isspace(x)?'_':(x))
					snprintf(capstring+strlen(capstring), sizeof(capstring)-strlen(capstring),
						" [%c%c%c%c%c%c%c%c%c%c%c%c", O(B>>8), O(B&0xFF), O(C>>8), O(C&0xFF), O(D>>8), O(D&0xFF), O(E1>>8), O(E1&0xFF), O(E2>>8), O(E2&0xFF), O(E3>>8), O(E3&0xFF));
#undef O
					snprintf(capstring+strlen(capstring), sizeof(capstring)-strlen(capstring), "]");
				}
			} else {
				if (strlen(caps[i]) > 4)
					snprintf(capstring+strlen(capstring), sizeof(capstring)-strlen(capstring),
						" [%04X%04X-%04X-%04X-%04X-%04X%04X%04X]", A1, A2, B, C, D, E1, E2, E3);
				else
					snprintf(capstring+strlen(capstring), sizeof(capstring)-strlen(capstring),
						" UNKNOWN_CAP_%04X", A2);
			}
		}
		conn->PI->capabilities(conn, c, name, capstring);
	} else if (strcmp(arg0, "BART2") == 0) {
		/* 1 source
		** 2 base64-encoded strings, unidentified
		*/
		char	**barts;
		int	i;

		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		barts = toc_parse_args(args[2], 255, ' ');
		for (i = 0; (barts[i] != NULL) && (barts[i+1] != NULL) && (barts[i+2] != NULL); i += 3) {
			int	j, flag = atoi(barts[i]), type = atoi(barts[i+1]);

			for (j = 0; j < sizeof(toc_barts)/sizeof(*toc_barts); j++)
				if (toc_barts[j].val == type) {
#ifdef DEBUG_ECHO
					toc_echof(conn, c, "got_data", "BART %i %i: %s: %s [%s]\n", flag, type, toc_barts[j].name, barts[i+2], firetalk_debase64(args[i+2]));
#endif
					break;
				}
		}
	} else if (strcmp(arg0, "NICK") == 0) {
		/* NICK:<Nickname>
		**    Tells you your correct nickname (ie how it should be capitalized and
		**    spacing)
		*/
		args = toc_parse_args(data, 2, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "NICK");
			return(FE_SUCCESS);
		}
		conn->PI->user_nickchanged(conn, c, c->nickname, args[1]);
		free(c->nickname);
		c->nickname = strdup(args[1]);
		if (c->nickname == NULL)
			abort();
		conn->PI->newnick(conn, c, args[1]);
	} else if (strcmp(arg0, "EVILED") == 0) {
		/* EVILED:<new evil>:<name of eviler, blank if anonymous>
		**    The user was just eviled.
		*/
		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "EVILED");
			return(FE_SUCCESS);
		}

		conn->PI->eviled(conn, c, atoi(args[1]), args[2]);
	} else if (strcmp(arg0, "CHAT_JOIN") == 0) {
		/* CHAT_JOIN:<Chat Room Id>:<Chat Room Name>
		**    We were able to join this chat room.  The Chat Room Id is
		**    internal to TOC.
		*/
		long	id;
		char	*name;
		int	exchange, ret;

		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "CHAT_JOIN");
			return(FE_SUCCESS);
		}
		id = atol(args[1]);
		name = args[2];
		exchange = toc_internal_find_exchange(c, name);

		assert(exchange != 0);
		ret = toc_internal_set_id(c, name, exchange, id);
		assert(ret == FE_SUCCESS);
		ret = toc_internal_set_joined(c, id);
		assert(ret == FE_SUCCESS);
		conn->PI->chat_joined(conn, c, toc_internal_find_room_name(c, id));
	} else if (strcmp(arg0, "CHAT_IN_ENC") == 0) {
		/* CHAT_IN:<Chat Room Id>:<Source User>:<Whisper? T/F>:<Message>
		**    A chat message was sent in a chat room.
		*/
		/* 1 room ID
		** 2 source
		** 3 'T'=private, 'F'=public
		** 4 unknown
		** 5 language
		** 6 message
		*/
		long	id;
		char	*source, *message, *mestart;

		args = toc_parse_args(data, 7, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2] || !args[3] || !args[4] || !args[5] || !args[6]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "CHAT_IN_ENC");
			return(FE_SUCCESS);
		}
		id = atol(args[1]);
		source = args[2];
		message = args[6];

		if (strncasecmp(message, "<HTML><PRE>", 11) == 0) {
			message += 11;
			if ((tempchr1 = strchr(message, '<')))
				*tempchr1 = 0;
		}
		if (strncasecmp(message, "/me ", 4) == 0)
			conn->PI->chat_getaction(conn, c,
				toc_internal_find_room_name(c, id),
				source, 0, message+4);
		else if ((mestart = strstr(message, ">/me ")) != NULL)
			conn->PI->chat_getaction(conn, c,
				toc_internal_find_room_name(c, id),
				source, 0, mestart+5);
		else
			conn->PI->chat_getmessage(conn, c,
				toc_internal_find_room_name(c, id),
				source, 0, message);
	} else if (strcmp(arg0, "CHAT_UPDATE_BUDDY") == 0) {
		/* CHAT_UPDATE_BUDDY:<Chat Room Id>:<Inside? T/F>:<User 1>:<User 2>...
		**    This one command handles arrival/departs from a chat room.  The
		**    very first message of this type for each chat room contains the
		**    users already in the room.
		*/
		int	joined;
		long	id;
		char	*recip,
			*source,
			*colon;

		args = toc_parse_args(data, 4, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2] || !args[3]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "CHAT_UPDATE_BUDDY");
			return(FE_SUCCESS);
		}
		joined = (args[2][0]=='T')?1:0;
		id = atol(args[1]);
		recip = toc_internal_find_room_name(c, id);
		source = args[3];

		while ((colon = strchr(source, ':'))) {
			*colon = 0;
			if (joined)
				conn->PI->chat_user_joined(conn, c, recip, source, NULL);
			else
				conn->PI->chat_user_left(conn, c, recip, source, NULL);
			source = colon+1;
		}
		if (joined) {
			conn->PI->chat_user_joined(conn, c, recip, source, NULL);
			conn->PI->chat_user_joined(conn, c, recip, NULL, NULL);
		} else
			conn->PI->chat_user_left(conn, c, recip, source, NULL);
	} else if (strcmp(arg0, "CHAT_INVITE") == 0) {
		/* CHAT_INVITE:<Chat Room Name>:<Chat Room Id>:<Invite Sender>:<Message>
		**    We are being invited to a chat room.
		*/
		args = toc_parse_args(data, 5, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2] || !args[3] || !args[4]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "CHAT_INVITE");
			return(FE_SUCCESS);
		}
		if (toc_internal_add_room(c, args[1], 4) == FE_SUCCESS)
			if (toc_internal_set_room_invited(c, args[1], 1) == FE_SUCCESS)
				if (toc_internal_set_id(c, args[1], 4, atol(args[2])) == FE_SUCCESS)
					conn->PI->chat_invited(conn, c, args[1], args[3], args[4]);
	} else if (strcmp(arg0, "CHAT_LEFT") == 0) {
		/* CHAT_LEFT:<Chat Room Id>
		**    Tells tic connection to chat room has been dropped
		*/
		args = toc_parse_args(data, 2, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "CHAT_LEFT");
			return(FE_SUCCESS);
		}
		conn->PI->chat_left(conn, c, toc_internal_find_room_name(c, atol(args[1])));
	} else if (strcmp(arg0, "GOTO_URL") == 0) {
		/* GOTO_URL:<Window Name>:<Url>
		**    Goto a URL.  Window Name is the suggested internal name of the window
		**    to use.  (Java supports this.)
		*/
		struct s_toc_infoget *i;

		/* create a new infoget object and set it to connecting state */
		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2]) {
			toc_internal_disconnect(conn, c, FE_INVALIDFORMAT);
			return(FE_INVALIDFORMAT);
		}

		i = c->infoget_head;
		c->infoget_head = calloc(1, sizeof(struct s_toc_infoget));
		if (c->infoget_head == NULL)
			abort();
		firetalk_sock_init(&(c->infoget_head->sock));
		snprintf(c->infoget_head->sock.buffer, sizeof(c->infoget_head->sock.buffer), "GET /%s HTTP/1.0\r\n\r\n", args[2]);
		c->infoget_head->next = i;
		memmove(&(c->infoget_head->sock.remote_addr), &(c->sock.remote_addr), sizeof(c->infoget_head->sock.remote_addr));
#ifdef _FC_USE_IPV6
		memmove(&(c->infoget_head->sock.remote_addr6), &(c->sock.remote_addr6), sizeof(c->infoget_head->sock.remote_addr6));
#endif
		if (firetalk_sock_connect(&(c->infoget_head->sock)) != FE_SUCCESS) {
			conn->PI->error(conn, c, FE_CONNECT, (lastinfo[0]=='\0')?NULL:lastinfo, "Failed to connect to info server");
			free(c->infoget_head);
			c->infoget_head = i;
			return(FE_SUCCESS);
		}
	} else if (strcmp(arg0, "NEW_BUDDY_REPLY2") == 0) {
		/* NEW_BUDDY_REPLY2:19033926:added */
	} else if (strcmp(arg0, "UPDATED2") == 0) {
		/* UPDATED2:a:19033926:Buddy */
	} else if (strcmp(arg0, "INSERTED2") == 0) {
		/* INSERTED2:25:76: */
		/* INSERTED2:b::yankeegurl680997:Recent Buddies */
		args = toc_parse_args(data, 255, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if ((args[1] != NULL) && (args[2] != NULL) && (args[3] != NULL) && (args[4] != NULL) && (strcmp(args[1], "b") == 0)) {
			char	*name = args[3],
				*group = args[4],
				*friendly = args[5];

			if ((friendly != NULL) && (*friendly == 0))
				friendly = NULL;
			conn->PI->buddyadded(conn, c, name, group, friendly);
		}
	} else if (strcmp(arg0, "DELETED2") == 0) {
		/* DELETED2:b:yankeegurl680997: */
		/* DELETED2:b:yankeegurl680997:Recent Buddies */
		/* DELETED2:M-^@T*80���M-^BGM-^Ayankeegurl680997: */
		args = toc_parse_args(data, 255, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if ((args[1] != NULL) && (args[2] != NULL) && (args[3] != NULL) && (strcmp(args[1], "b") == 0)) {
			char	*name = args[2],
				*group = args[4];

			if ((group != NULL) && (*group == 0))
				group = NULL;
			conn->PI->buddyremoved(conn, c, name, group);
		}
	} else if (strcmp(arg0, "DIR_STATUS") == 0) {
		/* DIR_STATUS:<Return Code>:<Optional args>
		**    <Return Code> is always 0 for success status.
		*/
		args = toc_parse_args(data, 2, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (args[1] == NULL) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "DIR_STATUS with no status code");
			return(FE_SUCCESS);
		}
		switch (atoi(args[1])) {
		  case 0:
		  case 1:
			break;
		  case 2:
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "DIR_STATUS failed, invalid format");
			break;
		  default:
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "DIR_STATUS with unknown code");
			break;
		}
	} else if (strcmp(arg0, "ADMIN_NICK_STATUS") == 0) {
		/* ADMIN_NICK_STATUS:<Return Code>:<Optional args>
		**    <Return Code> is always 0 for success status.
		*/
	} else if (strcmp(arg0, "ADMIN_PASSWD_STATUS") == 0) {
		/* ADMIN_PASSWD_STATUS:<Return Code>:<Optional args>
		**    <Return Code> is always 0 for success status.
		*/
		c->passchange--;
		args = toc_parse_args(data, 3, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "ADMIN_PASSWD_STATUS");
			return(FE_SUCCESS);
		}
		if (atoi(args[1]) != 0)
			conn->PI->error(conn, c, FE_NOCHANGEPASS, NULL, NULL);
		else
			conn->PI->passchanged(conn, c);
	} else if (strcmp(arg0, "PAUSE") == 0) {
		/* PAUSE
		**    Tells TIC to pause so we can do migration
		*/
		c->connectstate = 1;
		c->sock.state = FCS_WAITING_SIGNON;
	} else if (strcmp(arg0, "RVOUS_PROPOSE") == 0) {
		/* RVOUS_PROPOSE:<user>:<uuid>:<cookie>:<seq>:<rip>:<pip>:<vip>:<port>
		**               [:tlv tag1:tlv value1[:tlv tag2:tlv value2[:...]]]
		**    Another user has proposed that we rendezvous with them to
		**    perform the service specified by <uuid>.  They want us
		**    to connect to them, we have their rendezvous ip, their
		**    proposer_ip, and their verified_ip. The tlv values are
		**    base64 encoded.
		*/
		int	j, A1, A2, B, C, D, E1, E2, E3;

		args = toc_parse_args(data, 255, ':');
		assert(strcmp(arg0, args[0]) == 0);

		if (!args[1] || !args[2] || !args[3] || !args[4] || !args[5] || !args[6] || !args[7] || !args[8]) {
			conn->PI->error(conn, c, FE_INVALIDFORMAT, NULL, "RVOUS_PROPOSE");
			return(FE_SUCCESS);
		}
#ifdef DEBUG_ECHO
		{
			int	i;

			toc_echof(conn, c, "got_data", "RVOUS_PROPOSE\n1user=[%s]\n2uuid=%s\n3cookie=[%s]\n4seq=%s\n5rendezvous_ip=%s\n6proposer_ip=%s\n7verified_ip=%s\n8port=%s\n",
				args[1], args[2], args[3], args[4], args[5], args[6], args[7], args[8]);
			for (i = 9; args[i] && args[i+1]; i += 2)
				toc_echof(conn, c, "got_data", "RVOUS_PROPOSE\n%itype=%s\n%ivalue=%s [%s]\n", i, args[i], i+1, args[i+1], firetalk_debase64(args[i+1]));
		}
#endif
		toc_uuid(args[2], &A1, &A2, &B, &C, &D, &E1, &E2, &E3);
		j = toc_cap(A1, A2, B, C, D, E1, E2, E3);
		if (j == -1)
			conn->PI->error(conn, c, FE_WEIRDPACKET, NULL, "Unknown rendezvous UUID");
		else {
			if (strcmp(toc_uuids[j].name, "FILE_TRANSFER") == 0) {
				const char *message, *file;

				if ((message = toc_get_tlv_value(args, 9, 12)) != NULL)
					conn->PI->im_getmessage(conn, c, args[1], 0, message);

				if ((file = toc_get_tlv_value(args, 9, 10001)) != NULL) {
					unsigned long	size = ntohl(*((uint32_t *)(file+4)));

					conn->PI->file_offer(conn, c,
						/* from */	args[1],		/* user */
						/* filename */	file+8,			/* filename */
						/* size */	size,
						/* ipstring */	args[7],		/* verified_ip */
						/* ip6string */	NULL,
						/* port */	(uint16_t)atoi(args[8]),/* port */
						/* type */	FF_TYPE_RAW);
				}
			} else
				conn->PI->error(conn, c, FE_WEIRDPACKET, NULL, "Unhandled rendezvous UUID, do we have a capability set we do not actually support?");
		}
	} else
		conn->PI->error(conn, c, FE_WEIRDPACKET, NULL, data);

	goto got_data_start;
}

static const char *toc_make_fake_cap(const unsigned char *const str, const int len) {
	static char buf[sizeof("FFFFFFFF-cccc-dddd-eeee-ffffgggghhhh")];
	const int ar[] = { 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34 };
	const int maxlen = 12;
	int	i;

	strcpy(buf, "FFFFFFFF-0000-0000-0000-000000000000");
	for (i = 0; (i < len) && (i < maxlen); i++) {
		char	b[3];

		sprintf(b, "%02X", str[i]);
		memcpy(buf+ar[i], b, 2);
	}
	return(buf);
}

static fte_t toc_got_data_connecting(firetalk_t conn, client_t c, unsigned char *buffer, unsigned short *bufferpos) {
	char data[TOC_SERVERSEND_MAXLEN - TOC_HEADER_LENGTH + 1];
	fte_t	r;
	unsigned short length;
	char *arg0;
	char **args;
	char *tempchr1;

#ifdef DEBUG_ECHO
	toc_echof(conn, c, "got_data_connecting", "len=%i", *bufferpos);
#endif

got_data_connecting_start:

	r = toc_find_packet(conn, c, c->sock.buffer, &(c->sock.bufferpos), data, (c->connectstate==0)?SFLAP_FRAME_SIGNON:SFLAP_FRAME_DATA, &length);
	if (r == FE_NOTFOUND)
		return(FE_SUCCESS);
	else if (r != FE_SUCCESS)
		return(r);

	switch (c->connectstate) {
	  case 0: /* we're waiting for the flap version number */
		if (length != TOC_HOST_SIGNON_LENGTH) {
			conn->PI->connectfailed(conn, c, FE_PACKETSIZE, "Host signon length incorrect");
			return(FE_PACKETSIZE);
		}
		if ((data[0] != 0) || (data[1] != 0) || (data[2] != 0) || (data[3] != 1)) {
			conn->PI->connectfailed(conn, c, FE_VERSION, NULL);
			return(FE_VERSION);
		}
#if 0
		srand((unsigned int) time(NULL));
		c->local_sequence = (unsigned short)1+(unsigned short)(65536.0*rand()/(RAND_MAX+1.0));
#else
		c->local_sequence = 31337;
#endif

		length = toc_fill_header((unsigned char *)data, SFLAP_FRAME_SIGNON, ++c->local_sequence, toc_fill_signon((unsigned char *)&data[TOC_HEADER_LENGTH], c->nickname));

#ifdef DEBUG_ECHO
		toc_echo_send(conn, c, "got_data_connecting", data, length);
#endif
		firetalk_sock_send(&(c->sock), data, length);

		c->connectstate = 1;
		{
			int	sn = tolower(c->nickname[0]) - 'a' + 1,
				pw = tolower(c->password[0]) - 'a' + 1,
				A = sn*7696 + 738816,
				B = sn*746512,
				C = pw*A,
				magic = C - A + B + 71665152;

			if (!isdigit(c->nickname[0]))			/* AIM Express */
			  r = toc_send_printf(conn, c, "toc2_login "
				 "login.oscar.aol.com"	// authorizer host	[login.oscar.aol.com]
				" 29999"		// authorizer port	[29999]
				" %s"			// username
				" %S"			// roasted, armored password
				" English"		// language		[English]
				" %s"			// client version
				" 160"			// unknown number	[160]
				" %S"			// country code		[US]
				" %s"			// unknown string	[""]
				" %s"			// unknown string	[""]
				" 3"			// unknown number	[3]
				" 0"			// unknown number	[0]
				" 30303"		// unknown number	[30303]
				" -kentucky"		// unknown flag		[-kentucky]
				" -utf8"		// unknown flag		[-utf8]
				" -preakness"		// this enables us to talk to ICQ users
				" %i",			// magic number based on username and password
					c->nickname,
					toc_hash_password(c->password),
					PACKAGE_NAME ":" PACKAGE_VERSION ":contact " PACKAGE_BUGREPORT,
					"US",
					"",
					"",
					magic);
			else						/* ICQ2Go */
			  r = toc_send_printf(conn, c, "toc2_login "
				 "login.icq.com"	// authorizer host	[login.icq.com]
				" 5190"			// authorizer port	[5190]
				" %S"			// username
				" %S"			// roasted, armored password
				" en"			// language		[en]
				" %s"			// client version
				" 135"			// unknown number	[135]
				" %s"			// country code		["US"]
				" %s"			// unknown string	[""]
				" %s"			// unknown string	[""]
				" 30"			// unknown number	[30]
				" 2"			// unknown number	[2]
				" 321"			// unknown number	[321]
				" -utf8"		// unknown flag		[-utf8]
				" -preakness"		// this enables us to talk to ICQ users
				" %i",			// magic number based on username and password
					c->nickname,
					toc_hash_password(c->password),
					PACKAGE_NAME ":" PACKAGE_VERSION ":contact " PACKAGE_BUGREPORT,
					"US",
					"",
					"",
					magic);
		}
		if (r != FE_SUCCESS) {
			conn->PI->connectfailed(conn, c, r, NULL);
			return(r);
		}
		break;
	  case 1:
		arg0 = toc_get_arg0(data);
		if (strcmp(arg0, "SIGN_ON") != 0) {
			if (strcmp(arg0, "ERROR") == 0) {
				args = toc_parse_args(data, 3, ':');
				if (args[1] != NULL) {
					int	err = atoi(args[1]);

					if ((err >= 900) && (err <= 999)) {
						err -= 900;
						conn->PI->connectfailed(conn, c, toc2_errors[err].fte, toc2_errors[err].str);
						return(toc2_errors[err].fte);
					}
				}
			}
			conn->PI->connectfailed(conn, c, FE_UNKNOWN, NULL);
			return(FE_UNKNOWN);
		}
		c->connectstate = 2;
		break;
	  case 2:
	  case 3:
		arg0 = toc_get_arg0(data);
		if (arg0 == NULL)
			return(FE_SUCCESS);
		if (strcmp(arg0, "NICK") == 0) {
			/* NICK:<Nickname> */

			args = toc_parse_args(data, 2, ':');
			if (args[1]) {
				free(c->nickname);
				c->nickname = strdup(args[1]);
				if (c->nickname == NULL)
					abort();
			}
			c->connectstate = 3;
		} else if (strcmp(arg0, "CONFIG2") == 0) {
			/* CONFIG2:<config> */
			char	*nl, *curgroup = strdup("Saved buddy");

			args = toc_parse_args(data, 2, ':');
			if (!args[1]) {
				conn->PI->connectfailed(conn, c, FE_INVALIDFORMAT, "CONFIG2");
				return(FE_INVALIDFORMAT);
			}
			tempchr1 = args[1];
			c->permit_mode = 0;
			while ((nl = strchr(tempchr1, '\n'))) {
				*nl = 0;

				if (tempchr1[1] == ':') {
					/* b:ACCOUNT:REAL NAME:?:CELL PHONE SLOT NUMBER:w:NOTES */

					args = toc_parse_args(tempchr1, 4, ':');

					switch (args[0][0]) {
					  case 'g':	/* Buddy Group (All Buddies until the next g or the end of config are in this group.) */
						free(curgroup);
						curgroup = strdup(args[1]);
						break;
					  case 'b':	/* A Buddy */
					  case 'a': {	/* another kind of buddy */
							char	*friendly = NULL;

							if ((args[2] != NULL) && (args[2][0] != 0))
								friendly = args[2];
							else
								friendly = NULL;
							if (strcmp(curgroup, "Mobile Device") != 0)
								firetalk_im_add_buddy(conn, args[1], curgroup, friendly);
						}
						break;
					  case 'p':	/* Person on permit list */
						toc_send_printf(conn, c, "toc_add_permit %s", args[1]);
						break;
					  case 'd':	/* Person on deny list */
						firetalk_im_internal_add_deny(conn, args[1]);
						break;
					  case 'm':	/* Permit/Deny Mode.  Possible values are 1 - Permit All 2 - Deny All 3 - Permit Some 4 - Deny Some */
						c->permit_mode = atoi(args[1]);
						break;
					}
				}
				tempchr1 = nl+1;
			}
			free(curgroup);

			if ((c->permit_mode < 1) || (c->permit_mode > 5))
				c->permit_mode = 4;
			toc_send_printf(conn, c, "toc2_set_pdmode %i", c->permit_mode);
			c->gotconfig = 1;
		} else {
			conn->PI->connectfailed(conn, c, FE_WEIRDPACKET, data);
			return(FE_WEIRDPACKET);
		}

		if ((c->gotconfig == 1) && (c->connectstate == 3)) {
			/* ask the client to handle its init */
			conn->PI->doinit(conn, c, c->nickname);
			if (toc_im_upload_buddies(conn, c) != FE_SUCCESS) {
				conn->PI->connectfailed(conn, c, FE_PACKET, "Error uploading buddies");
				return(FE_PACKET);
			}
			if (toc_im_upload_denies(conn, c) != FE_SUCCESS) {
				conn->PI->connectfailed(conn, c, FE_PACKET, "Error uploading denies");
				return(FE_PACKET);
			}
			r = toc_send_printf(conn, c, "toc_init_done");
			if (r != FE_SUCCESS) {
				conn->PI->connectfailed(conn, c, r, "Finalizing initialization");
				return(r);
			}

			{
				char	*name, *version;

				name = strdup(toc_make_fake_cap(PACKAGE_NAME, strlen(PACKAGE_NAME)));
				version = strdup(toc_make_fake_cap(PACKAGE_VERSION, strlen(PACKAGE_VERSION)));
				r = toc_send_printf(conn, c, "toc_set_caps %S %S %S",
					"094613494C7F11D18222444553540000", name, version);
				free(name);
				free(version);
				if (r != FE_SUCCESS) {
					conn->PI->connectfailed(conn, c, r, "Setting capabilities");
					return(r);
				}
			}

			c->sock.state = FCS_ACTIVE;
			conn->PI->connected(conn, c);
			return(FE_SUCCESS);
		}
		break;
	}
	goto got_data_connecting_start;
}

static void toc_signon(firetalk_t conn, client_t c) {
	/* fill & send the flap signon packet */

	c->lasttalk = time(NULL);
	c->connectstate = 0;
	c->permit_mode = 0;
	c->gotconfig = 0;

	/* send the signon string to indicate that we're speaking FLAP here */

#ifdef DEBUG_ECHO
	toc_echof(conn, c, "signon", "frame=0, length=%i, value=[%s]\n", strlen(SIGNON_STRING), SIGNON_STRING);
#endif
	firetalk_sock_send(&(c->sock), SIGNON_STRING, sizeof(SIGNON_STRING)-1);
}

static fte_t toc_postselect(firetalk_t conn, client_t c, fd_set *read, fd_set *write, fd_set *except) {
	struct s_toc_infoget *i, *i2;
	fte_t	e;

	if ((e = firetalk_sock_postselect(&(c->sock), read, write, except)) != FE_SUCCESS) {
		if (c->sock.state == FCS_ACTIVE)
			conn->PI->disconnect(conn, c, e);
		else
			conn->PI->connectfailed(conn, c, e, strerror(errno));
		return(e);
	}

	if (c->sock.state == FCS_SEND_SIGNON) {
		conn->PI->needpass(conn, c);
		c->sock.state = FCS_WAITING_PASSWORD;
	} else if (c->sock.state == FCS_GOT_PASSWORD) {
		toc_signon(conn, c);
		c->sock.state = FCS_WAITING_SIGNON;
	} else if (c->sock.readdata) {
		if (c->sock.state == FCS_ACTIVE)
			toc_got_data(conn, c, c->sock.buffer, &(c->sock.bufferpos));
		else
			toc_got_data_connecting(conn, c, c->sock.buffer, &(c->sock.bufferpos));
	}

	for (i = c->infoget_head; i != NULL; i = i2) {
		i2 = i->next;

		e = firetalk_sock_postselect(&(i->sock), read, write, except);
#ifdef DEBUG_ECHO
		toc_echof(conn, c, "postselect", "postselecting on fd %i got %s (bufferpos=%i, readdata=%i, state=%i)",
			i->sock.fd, firetalk_strerror(e), i->sock.bufferpos, i->sock.readdata, i->sock.state);
#endif

		if (e == FE_SUCCESS) {
			if (i->sock.state == FCS_SEND_SIGNON) {
#ifdef DEBUG_ECHO
				toc_echof(conn, c, "postselect", "sock_send(%i, [%s], %i)", i->sock.fd, i->sock.buffer, strlen(i->sock.buffer));
#endif
				firetalk_sock_send(&(i->sock), i->sock.buffer, strlen(i->sock.buffer));
				i->sock.state = FCS_ACTIVE;
			}
		} else if (e == FE_DISCONNECT) {
			toc_infoget_parse(conn, c, i);
			toc_infoget_remove(conn, c, i, NULL);
		} else {
			toc_infoget_remove(conn, c, i, strerror(errno));
		}
	}

	return(FE_SUCCESS);
}

static fte_t toc_periodic(firetalk_t conn) {
	struct s_toc_connection *c;
	time_t	now;
	long	idle;
	char	data[32];
	fte_t	r;

	c = conn->handle;

	if (c->sock.state != FCS_ACTIVE)
		return(FE_NOTCONNECTED);

	now = time(NULL);

	if ((c->lastself+60) <= now) {
		c->lastself = now;
		r = toc_send_printf(conn, c, "toc_get_status %s", c->nickname);
		if (r != FE_SUCCESS)
			return(r);
	}

	idle = (long)(now - c->lasttalk);
	conn->PI->setidle(conn, c, &idle);

	if (idle < 600)
		idle = 0;

	if (idle/60 == c->lastidle/60)
		return(FE_IDLEFAST);

	if ((c->lastidle/60 == 0) && (idle/60 == 0))
		return(FE_IDLEFAST);
	if ((c->lastidle/60 != 0) && (idle/60 != 0))
		return(FE_IDLEFAST);

	c->lastidle = idle;
	sprintf(data, "%ld", idle);
	return(toc_send_printf(conn, c, "toc_set_idle %s", data));
}

static fte_t toc_chat_join(firetalk_t conn, client_t c, const char *const room) {
	int	i;

	i = toc_internal_get_room_invited(c, room);
	if (i == 1) {
		toc_internal_set_room_invited(c, room, 0);
		return(toc_send_printf(conn, c, "toc_chat_accept %i", toc_internal_find_room_id(c, room)));
	} else {
		int	m;
		char	*s;

		s = toc_internal_split_name(room);
		m = toc_internal_split_exchange(room);
		toc_internal_add_room(c, s, m);
		return(toc_send_printf(conn, c, "toc_chat_join %d %s", m, s));
	}
}

static fte_t toc_chat_part(firetalk_t conn, client_t c, const char *const room) {
	int	id = toc_internal_find_room_id(c, room);

	if (id == 0)
		return(FE_ROOMUNAVAILABLE);
	return(toc_send_printf(conn, c, "toc_chat_leave %i", id));
}

static fte_t toc_chat_set_topic(firetalk_t conn, client_t c, const char *const room, const char *const topic) {
	return(FE_SUCCESS);
}

static fte_t toc_chat_op(firetalk_t conn, client_t c, const char *const room, const char *const who) {
	return(FE_SUCCESS);
}

static fte_t toc_chat_deop(firetalk_t conn, client_t c, const char *const room, const char *const who) {
	return(FE_SUCCESS);
}

static fte_t toc_chat_kick(firetalk_t conn, client_t c, const char *const room, const char *const who, const char *const reason) {
	return(FE_SUCCESS);
}

static fte_t toc_chat_send_message(firetalk_t conn, client_t c, const char *const room, const char *const message, const int auto_flag) {
	if (strlen(message) > 232)
		return(FE_PACKETSIZE);

	if (strcasecmp(room, ":RAW") == 0)
		return(toc_send_printf(conn, c, "%S", message));
	else {
		int	id = toc_internal_find_room_id(c, room);

		if (id == 0)
			return(FE_ROOMUNAVAILABLE);
		return(toc_send_printf(conn, c, "toc_chat_send %i %s", id, message));
	}
}

static fte_t toc_chat_send_action(firetalk_t conn, client_t c, const char *const room, const char *const message, const int auto_flag) {
	char	tempbuf[TOC_CLIENTSEND_MAXLEN];

	if (strlen(message) > 232-4)
		return(FE_PACKETSIZE);

	snprintf(tempbuf, sizeof(tempbuf), "/me %s", message);
	return(toc_send_printf(conn, c, "toc_chat_send %i %s",
		toc_internal_find_room_id(c, room), tempbuf));
}

static fte_t toc_chat_invite(firetalk_t conn, client_t c, const char *const room, const char *const who, const char *const message) {
	int	id = toc_internal_find_room_id(c, room);

	if (id != 0)
		return(toc_send_printf(conn, c, "toc_chat_invite %i %s %s", id, message, who));
	return(FE_NOTFOUND);
}

#ifdef ENABLE_FILE_OFFER
static fte_t toc_file_offer(firetalk_t conn, client_t c, const char *const nickname,
		const char *const filename, const unsigned long localip,
		const uint16_t port, const long size) {
	char	args[256];

	snprintf(args, sizeof(args), "SEND %s %lu %u %ld", filename, localip, port, size);
	return(firetalk_subcode_send_request(conn, nickname, "DCC", args));
}
#endif

static fte_t toc_save_config(firetalk_t conn, client_t c) {
	return(FE_SUCCESS);
}

const firetalk_PD_t firetalk_protocol_toc2 = {
	strprotocol:		"TOC2",
	periodic:		toc_periodic,
	preselect:		toc_preselect,
	postselect:		toc_postselect,
	comparenicks:		toc_compare_nicks,
	isprintable:		toc_isprint,
	disconnect:		toc_disconnect,
	connect:		toc_connect,
	sendpass:		toc_sendpass,
	get_info:		toc_get_info,
	set_info:		toc_set_info,
	set_away:		toc_set_away,
	set_nickname:		toc_set_nickname,
	set_password:		toc_set_password,
	set_privacy:		toc_set_privacy,
	im_add_buddy:		toc_im_add_buddy,
	im_remove_buddy:	toc_im_remove_buddy,
	im_add_deny:		toc_im_add_deny,
	im_remove_deny:		toc_im_remove_deny,
	im_upload_buddies:	toc_im_upload_buddies,
	im_upload_denies:	toc_im_upload_denies,
	im_send_message:	toc_im_send_message,
	im_send_action:		toc_im_send_action,
	im_evil:		toc_im_evil,
	chat_join:		toc_chat_join,
	chat_part:		toc_chat_part,
	chat_invite:		toc_chat_invite,
	chat_set_topic:		toc_chat_set_topic,
	chat_op:		toc_chat_op,
	chat_deop:		toc_chat_deop,
	chat_kick:		toc_chat_kick,
	chat_send_message:	toc_chat_send_message,
	chat_send_action:	toc_chat_send_action,
	subcode_encode:		toc_ctcp_encode,
	room_normalize:		aim_normalize_room_name,
	create_handle:		toc_create_handle,
	destroy_handle:		toc_destroy_handle,
	save_config:		toc_save_config,
#ifdef ENABLE_DIRECTORY
	get_dir:		toc_get_dir,
#endif
#ifdef ENABLE_NEWGROUPS
	im_remove_group:	toc_im_remove_group,
#endif
#ifdef ENABLE_FILE_OFFER
	file_offer:		toc_file_offer,
#endif
};
