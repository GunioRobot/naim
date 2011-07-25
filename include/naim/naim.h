/*  _ __   __ _ ___ __  __
** | '_ \ / _` |_ _|  \/  | naim
** | | | | (_| || || |\/| | Copyright 1998-2003 Daniel Reed <n@ml.org>
** |_| |_|\__,_|___|_|  |_| ncurses-based chat client
*/
#ifndef naim_h
#define naim_h	1

#include <ctype.h>
#include <time.h>
#include <stdarg.h>
#include <stdio.h>

#include <naim/secs.h>
#include <firetalk.h>

#define CONIO_MAXPARMS	10

#define cEVENT		0
#define cEVENT_ALT	1
#define cTEXT		2
#define cSELF		3
#define cBUDDY		4
#define cBUDDY_WAITING	5
#define cBUDDY_IDLE	6
#define cBUDDY_AWAY	7
#define cBUDDY_OFFLINE	8
#define cBUDDY_QUEUED	9
#define cBUDDY_TAGGED	10
#define NUMEVENTS	11

#define cINPUT		0
#define cWINLIST	1
#define cCONN		2
#define cIMWIN		3
#define cSTATUSBAR	4
#define NUMWINS		5

typedef enum {
	CH_NONE = 0,
	IM_MESSAGE = (1 << 0),
	CH_ATTACKED = (1 << 1),
	CH_ATTACKS = (1 << 2),
	CH_MESSAGE = (1 << 3),
	CH_MISC = (1 << 4),
	CH_USERS = (1 << 5),
	CH_UNKNOWN6 = (1 << 6),
	CH_UNKNOWN7 = (1 << 7),
} chatter_t;

typedef enum {
	ECHOSTATUS_NONE = 0,
	ALSO_STATUS = (1 << 0),
	ALWAYS_STATUS = (1 << 1),
	NOLOG = (1 << 2),
} echostyle_t;

typedef struct {
	char	*name;
	int	gotaway;
} awayar_t;

typedef struct buddylist_ts {
	char	*_account,
		*_group,
		*_name,
		*crypt,
		*tzname,
		*tag;
	struct buddylist_ts	*next;
	unsigned char
		offline:1,
		isaway:1,
		isidle:1;	// is the buddy idle for more than some threshhold?
	int	peer;
} buddylist_t;
#define DEFAULT_GROUP	"User"
#define CHAT_GROUP	"Chat"
#define USER_ACCOUNT(x)	((x)->_account)
#define USER_NAME(x)	(((x)->_name != NULL)?(x)->_name:(x)->_account)
#define USER_GROUP(x)	((x)->_group)
#define USER_PERMANENT(x) (strcasecmp((x)->_group, DEFAULT_GROUP) != 0)

typedef struct {
	unsigned char
		isoper:1,	// chat operator
		offline:1;
	char	*key;
} chatlist_t;

typedef struct {
	void	*handle;
	char	*from,
		*remote,
		*local;
	long	size,
		bytes;
	double	started;
	time_t	lastupdate;
} transfer_t;

typedef enum {
	CHAT,
	BUDDY,
	TRANSFER,
} et_t;

#ifndef WIN_T
# if 1
typedef struct {
	void	*_win;
	FILE	*logfile;
	unsigned char
		dirty:1;
} win_t;
# else
typedef void *win_t;
# endif
#endif

typedef struct buddywin_ts {
	char	*winname,
		*blurb;
	unsigned char
		waiting:1,	/* text waiting to be read (overrides
				** offline and isbuddy in bupdate())
				*/
		keepafterso:1;	/* keep window open after buddy signs off? */
	win_t	nwin;
	char	**pouncear;
	int	pouncec;
	time_t	informed,
		closetime;
	double	viewtime;
	union {
		buddylist_t	*buddy;
		chatlist_t	*chat;
		transfer_t	*transfer;
	} e;
	et_t	et;
	struct buddywin_ts
		*next;
} buddywin_t;

typedef struct ignorelist_ts {
	char	*screenname,
		*notes;
	struct ignorelist_ts
		*next;
	time_t	informed;
} ignorelist_t;

typedef struct {
	int	f[NUMEVENTS],
		b[NUMWINS];
	struct {
		int	startx, starty,
			widthx, widthy, pady;
	} wstatus, winput, winfo, waway;
} faimconf_t;
#define nw_COLORS	8
#define C(back, fore)	(nw_COLORS*faimconf.b[c ## back] +            faimconf.f[c ## fore])
#define CI(back, fore)	(          faimconf.b[c ## back] + nw_COLORS*(faimconf.f[c ## fore]%COLOR_PAIRS))
#define CB(back, fore)	(nw_COLORS*faimconf.b[c ## back] +            faimconf.b[c ## fore])

typedef struct conn_ts {
	char	*sn,
		*password,
		*winname,
		*server,
		*profile;
	int	port, proto;
	time_t	online;
	double	lastupdate, lag;
	void	*conn;
	FILE	*logfile;
	win_t	nwin;
	buddylist_t	*buddyar;
	ignorelist_t	*idiotar;
	buddywin_t	*curbwin;
	struct conn_ts
		*next;
} conn_t;

typedef struct {
	char	*name,
		*script;
} script_t;

typedef struct {
	const char
		*var,
		*val,
		*desc;
} rc_var_s_t;

typedef struct {
	const char
		*var;
	int	val;
	const char
		*desc;
} rc_var_i_t;

typedef struct {
	const char
		*var;
	char	val;
	const char
		*desc;
} rc_var_b_t;

typedef struct {
	char	*from,
		*replace;
} html_clean_t;



static inline char *user_name(char *buf, int buflen, conn_t *conn, buddylist_t *user) {
	static char _buf[256];

	if (buf == NULL) {
		buf = _buf;
		buflen = sizeof(_buf);
	}

	secs_setvar("user_name_account", USER_ACCOUNT(user));
	secs_setvar("user_name_name", USER_NAME(user));

	if (firetalk_compare_nicks(conn->conn, USER_ACCOUNT(user), USER_NAME(user)) == FE_SUCCESS)
		snprintf(buf, buflen, "%s", secs_script_expand(NULL, secs_getvar("nameformat")));
	else
		snprintf(buf, buflen, "%s", secs_script_expand(NULL, secs_getvar("nameformat_named")));
	secs_setvar("user_name_account", "");
	secs_setvar("user_name_name", "");
	return(buf);
}

static inline const char *naim_basename(const char *name) {
	const char *slash = strrchr(name, '/');

	if (slash != NULL)
		return(slash+1);
	return(name);
}

static inline int naim_strtocol(const char *str) {
	int	i, srccol = 0;

	for (i = 0; str[i] != 0; i++)
		srccol += str[i] << (8*(i%3));
	return(srccol%0xFFFFFF);
}

#define STRREPLACE(target, source) do { \
	assert(source != NULL); \
	assert(source != target); \
	if ((target = realloc(target, strlen(source)+1)) == NULL) { \
		echof(curconn, NULL, "Fatal error %i in strdup(%s): %s\n", errno, \
			source, strerror(errno)); \
		statrefresh(); \
		sleep(5); \
		abort(); \
	} \
	strcpy(target, source); \
} while (0)

#define WINTIME(win, cpre) do { \
	struct tm	*tptr = localtime(&now); \
	unsigned char	buf[64]; \
	char		*format; \
	\
	if ((format = secs_getvar("timeformat")) == NULL) \
		format = "[%H:%M:%S]&nbsp;"; \
	strftime(buf, sizeof(buf), format, tptr); \
	hwprintf(win, C(cpre,EVENT), "</B>%s", buf); \
} while (0)

#define WINTIMENOLOG(win, cpre) do { \
	struct tm	*tptr = localtime(&now); \
	unsigned char	buf[64]; \
	char		*format; \
	\
	if ((format = secs_getvar("timeformat")) == NULL) \
		format = "[%H:%M:%S]&nbsp;"; \
	strftime(buf, sizeof(buf), format, tptr); \
	hwprintf(win, -C(cpre,EVENT)-1, "</B>%s", buf); \
} while (0)

extern int	consolescroll;
#define inconsole	(consolescroll != -1)
#define inconn_real	((curconn != NULL) && (curconn->curbwin != NULL))
#define inconn		(!inconsole && inconn_real)

#define hexdigit(c) \
	(isdigit(c)?(c - '0'):((c >= 'A') && (c <= 'F'))?(c - 'A' + 10):((c >= 'a') && (c <= 'f'))?(c - 'a' + 10):(0))
static inline int naimisprint(int c) {
	return((c >= 0) && (c <= 255) && (isprint(c) || (c >= 160)));
}

/* buddy.c */
void	htmlstrip(char *bb);
const unsigned char *const
	naim_normalize(const unsigned char *const name);
void	playback(conn_t *conn, buddywin_t *);
void	bnewwin(conn_t *conn, const char *, et_t);
void	bupdate(void);
void	bcoming(conn_t *conn, const char *, int, int);
void	bgoing(conn_t *conn, const char *);
void	bclose(conn_t *conn, buddywin_t *bwin, int _auto);
buddywin_t
	*bgetwin(conn_t *conn, const char *, et_t);
buddywin_t
	*bgetanywin(conn_t *conn, const char *);
void	bclearall(conn_t *conn, int);
void	naim_changetime(void);

/* conio.c */
void	conio_handlecmd(const char *);
void	conio_handleline(const char *line);
void	gotkey(int);
void	conio_hook_init(void);

/* events.c */
void	updateidletime(void);
void	event_handle(time_t);

/* fireio.c */
int	getvar_int(conn_t *conn, const char *);
char	*getvar(conn_t *conn, const char *);
conn_t	*naim_newconn(int);
void	naim_set_info(void *, const char *);
void	naim_lastupdate(conn_t *conn);
buddywin_t
	*cgetwin(conn_t *, const char *);
void	fremove(transfer_t *);
transfer_t
	*fnewtransfer(void *handle, const char *filename,
		const char *from, long size);
void	fireio_hook_init(void);
void	naim_awaylog(conn_t *conn, const char *src, const char *msg);
void	naim_setversion(conn_t *conn);

/* hamster.c */
void	logim(conn_t *conn, const char *source, const char *target, const unsigned char *message);
void	naim_send_im(conn_t *conn, const char *, const char *, const int _auto);
void	naim_send_im_away(conn_t *conn, const char *, const char *);
void	naim_send_act(conn_t *conn, const char *, const char *);
void	setaway(const int auto_flag);
void	unsetaway(void);
void	sendaway(conn_t *conn, const char *);

/* helpcmd.c */
void	help_printhelp(const char *);

/* hwprintf.c */
int	vhwprintf(win_t *, int, const unsigned char *, va_list);
int	hwprintf(win_t *, int, const unsigned char *, ...);

/* echof.c */
void	status_echof(conn_t *conn, const unsigned char *format, ...);
void	window_echof(buddywin_t *bwin, const unsigned char *format, ...);
void	echof(conn_t *conn, const unsigned char *where, const unsigned char *format, ...);

/* rc.c */
const char
	*buddy_tabcomplete(conn_t *const conn, const char *start, const char *buf, const int bufloc, int *const match, const char **desc);
buddylist_t
	*rgetlist(conn_t *, const char *);
buddylist_t
	*raddbuddy(conn_t *, const char *, const char *, const char *);
void	rdelbuddy(conn_t *, const char *);
void	raddidiot(conn_t *, const char *, const char *);
void	rdelidiot(conn_t *, const char *);
int	rc_resize(faimconf_t *);
void	rc_initdefs(faimconf_t *);
int	naim_read_config(const char *);

/* rodents.c */
int	aimcmp(const unsigned char *, const unsigned char *);
int	aimncmp(const unsigned char *, const unsigned char *, int len);
const char
	*dtime(double t);
const char
	*dsize(double b);

/* script.c */
void	script_makealias(const char *, const char *);
int	script_doalias(const char *, const char *);

/* set.c */
const char
	*set_tabcomplete(conn_t *const conn, const char *start, const char *buf, const int bufloc, int *const match, const char **desc);
void	set_echof(const char *const format, ...);
void	set_setvar(const char *, const char *);

/* win.c */
void	do_resize(conn_t *conn, buddywin_t *bwin);
void	statrefresh();
void	wsetup(void);
void	wshutitdown(void);
void	win_resize(void);
int	nw_printf(win_t *win, int, int, const unsigned char *, ...);
void	nw_initwin(win_t *win, int bg);
void	nw_erase(win_t *win);
void	nw_refresh(win_t *win);
void	nw_attr(win_t *win, char B, char I, char U, char EM,
		char STRONG, char CODE);
void	nw_color(win_t *win, int pair);
void	nw_flood(win_t *win, int pair);
void	nw_addch(win_t *win, const unsigned long ch);
void	nw_addstr(win_t *win, const unsigned char *);
void	nw_move(win_t *win, int row, int col);
void	nw_delwin(win_t *win);
void	nw_touchwin(win_t *win);
void	nw_newwin(win_t *win);
void	nw_hline(win_t *win, unsigned long ch, int row);
void	nw_vline(win_t *win, unsigned long ch, int col);
void	nw_mvwin(win_t *win, int row, int col);
void	nw_resize(win_t *win, int row, int col);
int	nw_getcol(win_t *win);
int	nw_getrow(win_t *win);
void	nw_getline(win_t *win, char *buf, int buflen);
int	nw_getch(void);
void	nw_getpass(win_t *win, char *pass, int len);

#endif /* naim_h */
