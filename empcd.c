/***********************************************************
 EMPCd - Event Music Player Client daemon
 by Jeroen Massar <jeroen@massar.ch>
************************************************************
 $Author: jeroen $
 $Id: $
 $Date: $
***********************************************************/

#include "empcd.h"

#define EMPCD_VERSION "2013.12.28"
#define EMPCD_VSTRING "empcd %s by Jeroen Massar <jeroen@massar.ch>\n"

/* MPD functions */
#include "support/mpc-0.12.2/src/libmpdclient.h"
#define MPD_HOST_DEFAULT "localhost"
#define MPD_PORT_DEFAULT "6600"

struct empcd_events	events[100];
unsigned int		maxevent = 0;
mpd_Connection		*mpd = NULL;
unsigned int		verbosity = 0, drop_uid = 0, drop_gid = 0;
bool			daemonize = true;
bool			running = true;
bool			exclusive = true;
bool			giveup = true;
bool			nompd = false;
char			*mpd_host = NULL, *mpd_port = NULL;

/* When we receive a signal, we abort */
static void handle_signal(int i);
static void handle_signal(int i)
{
	running = false;
	signal(i, &handle_signal);
}

static void doelogA(int level, int errnum, const char *fmt, va_list ap) ATTR_FORMAT(printf, 3, 0);
static void doelogA(int level, int errnum, const char *fmt, va_list ap)
{
	char		buf[8192];
	int		k;
	unsigned int	i;

	if (level == LOG_DEBUG && verbosity < 1) return;

	k = vsnprintf(buf, sizeof(buf), fmt, ap);
	if (!snprintfok(k, sizeof(buf)))
	{
		snprintf(buf, sizeof(buf), "[Log line way too long: %u/%u bytes]\n", k, (unsigned int)sizeof(buf));
		level = LOG_ERR;
	}

	/* Append errno description? */
	if (errnum != 0)
	{
		i = strlen(buf);
		if (i == 0) i = 1;
		if (i < (sizeof(buf) - 10))
		{
			/* Add ": " overwriting the \n which has to be present */
			buf[i-1] = ':';
			buf[i] = ' ';
			buf[i+1] = '\0';

			errno = 0;
			k = strerror_r(errnum, &buf[i+1], sizeof(buf) - (i+2));
			if (k == 0)
			{
				i = strlen(buf);
				k = snprintf(&buf[i], sizeof(buf)-i, " (errno %d)\n", errnum);
				if (!snprintfok(k, sizeof(buf)-i))
				{
					/* Doesn't hurt when it doesn't fit, just terminate it */
					buf[i] = '\0';
				}
			}
			else
			{
				k = snprintf(&buf[i+1], sizeof(buf) - (i+2), " [unknown erro %u]\n", errnum);
				if (!snprintfok(k, sizeof(buf)-i))
				{
					/* Just terminate if all is failing */
					buf[i+1] = '\0';
				}
			}
		}
	}

	if (daemonize)
	{
		syslog(LOG_LOCAL7 | level, "%s", buf);
	}
	else
	{
		FILE *out = (level == LOG_DEBUG || level == LOG_ERR ? stderr : stdout);
		fprintf(out, "[%6s] ",
			level == LOG_DEBUG ?    "debug" :
			(level == LOG_ERR ?     "error" :
			(level == LOG_WARNING ? "warn" :
			(level == LOG_NOTICE ?  "notice" :
			(level == LOG_INFO ?    "info" : "(!?)")))));
		fprintf(out, "%s", buf);
	}
}

static void doelog(int level, int errnum, const char *fmt, ...) ATTR_FORMAT(printf, 3, 4);
static void doelog(int level, int errnum, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	doelogA(level, errnum, fmt, ap);
	va_end(ap);
}

static void dolog(int level, const char *fmt, ...) ATTR_FORMAT(printf, 2, 3);
static void dolog(int level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	doelogA(level, 0, fmt, ap);
	va_end(ap);
}

static mpd_Connection *empcd_setup(void);
static mpd_Connection *empcd_setup(void)
{
	int		iport;
	char		*test;
	int		password_len = 0;
	int		parsed_len = 0;
	mpd_Connection	*m = NULL;

	if (nompd)
	{
		dolog(LOG_ERR, "MPD connection disabled\n");
		return NULL;
	}

	if (!mpd_host || !mpd_port)
	{
		dolog(LOG_ERR, "Either MPD_HOST or MPD_PORT not configured\n");
		return NULL;
	}
	
	iport = strtol(mpd_port, &test, 10);
	if (iport <= 0 || test[0] != '\0')
	{
		dolog(LOG_ERR, "MPD_PORT \"%s\" is not a positive integer\n", mpd_port);
		return NULL;
	}

	/* parse password and host */
	test = strstr(mpd_host,"@");
	password_len = test - mpd_host;
	if (test) parsed_len++;

	if (!test) password_len = 0;
	if (test && password_len != 0) parsed_len += password_len;

	m = mpd_newConnection(mpd_host + parsed_len, iport, 10);
	if (!m) return NULL;

	if (m->error)
	{
		dolog(LOG_ERR, "MPD Connection Error: %s\n", m->errorStr);
		return NULL;
	}

	if (password_len)
	{
		char *pass = strdup(mpd_host);
		pass[password_len] = '\0';
		mpd_sendPasswordCommand(m, pass);
		mpd_finishCommand(m);
		free(pass);

		if (m->error)
		{
			dolog(LOG_ERR, "MPD Authentication Error: %s\n", m->errorStr);
			return NULL;
		}
	}

	return m;
}

static bool mpd_check(void);
static bool mpd_check(void)
{
	if (nompd) return true;

	if (!mpd->error) return false;

	/* Ignore timeouts */
	if (mpd->error != MPD_ERROR_CONNCLOSED)
	{
		dolog(LOG_WARNING, "MPD error: %s\n", mpd->errorStr);
	}

	/* Don't reconnect for non-fatal errors */
	if (mpd->error < 10 && mpd->error > 19)
	{
		return false;
	}

	/* Close the old connection */
	mpd_closeConnection(mpd);

	/* Setup a new connection */
	mpd = empcd_setup();
	if (!mpd)
	{
		dolog(LOG_ERR, "MPD Connection Lost, exiting\n");
		exit(0);
	}

	return true;
}

static mpd_Status *empcd_status(void);
static mpd_Status *empcd_status(void)
{
	int retry = 5;
	mpd_Status *s = NULL;

	if (nompd) return NULL;

	while (retry > 0)
	{
		retry--;

		mpd_sendStatusCommand(mpd);
		if (mpd_check()) continue;

		s = mpd_getStatus(mpd);
		if (mpd_check()) continue;

		mpd_finishCommand(mpd);
		if (mpd_check()) continue;

		break;
	}

	return s;
}

/********************************************************************/

static void f_exec(const char *arg, const char *args);
static void f_exec(const char *arg, const char *args)
{
	if ((!arg || strlen(arg) == 0) && args)
	{
		dolog(LOG_WARNING, "f_exec requires '%s' as an argument, none given, ignoring\n", args);
		return;
	}

	if (system(arg) < 0)
	{
		doelog(LOG_WARNING, errno, "f_exec failed to execute '%s'\n", arg);
	}
}

static void f_quit(const char UNUSED *arg, const char UNUSED *args);
static void f_quit(const char UNUSED *arg, const char UNUSED *args)
{
	running = false;
}

#define QUOTE(s) #s
#define STR(s) QUOTE(s)
#define F_CMDG(fn, f)											\
static void f_##fn(const char *arg, const char *args);							\
static void f_##fn(const char *arg, const char *args)							\
{													\
	int retries;											\
													\
	if (nompd)											\
	{												\
		dolog(LOG_INFO, "%s not executing as MPD is disabled (nompd)\n", STR(fn));		\
		return;											\
	}												\
													\
	if ((!arg || strlen(arg) == 0) && args)								\
	{												\
		dolog(LOG_WARNING, "%s requires '%s' as an argument, none given, ignoring\n", #fn, args); \
		return;											\
	}												\
													\
	for (retries = 5; retries > 0; retries--)							\
	{												\
		f;											\
		if (mpd_check()) continue;								\
		mpd_finishCommand(mpd);									\
		if (mpd_check()) continue;								\
		break;											\
	}												\
}

/* G = Given argument, N = No Argument, A = 'arg' as argument */
#define F_CMDN(fn, f)	F_CMDG(fn,f(mpd))
#define F_CMDA(fn, f)	F_CMDG(fn,f(mpd, arg))

F_CMDN(next,	mpd_sendNextCommand)
F_CMDN(prev,	mpd_sendPrevCommand)
F_CMDN(stop,	mpd_sendStopCommand)
F_CMDG(play,	mpd_sendPlayCommand(mpd,-1))
F_CMDA(save,	mpd_sendSaveCommand)
F_CMDA(load,	mpd_sendLoadCommand)
F_CMDA(remove,	mpd_sendRmCommand)
F_CMDN(clear,	mpd_sendClearCommand)

static void f_volume(const char *arg, const char UNUSED *args);
static void f_volume(const char *arg, const char UNUSED *args)
{
	int	dir = 0, volume = 0, i = 0, retry = 5;
	bool	perc = false;
	mpd_Status *status;

	status = empcd_status();
	if (!status) return;

	if (arg[0] == '-')	{ i++; dir = -1; }
	else if (arg[0] == '+') { i++; dir = +1; }
	volume = strlen(&arg[i]);
	if (arg[i+volume] == '%') perc = true;

	volume = atoi(&arg[i]);

	if (perc)
	{
		if (dir != 0) volume = status->volume + ((status->volume * volume / 100) * dir);
		else volume = (status->volume * perc / 100);
	}
	else
	{
		if (dir != 0) volume = status->volume + (volume*dir);
		/* dir == 0 case is set correctly above, as no adjustment is needed */
	}

	/* Take care of limits */
	if (volume < 0 || volume > 100) return;

	while (retry > 0)
	{
		retry--;
		mpd_sendSetvolCommand(mpd, volume);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}

	mpd_freeStatus(status);
}

static void f_seek(const char *arg, const char UNUSED *args);
static void f_seek(const char *arg, const char UNUSED *args)
{
	int	dir = 0, seekto = 0, i = 0, retry = 5;
	bool	perc = false;

	mpd_Status *status = empcd_status();
	if (!status) return;

	if (arg[0] == '-')	{ i++; dir = -1; }
	else if (arg[0] == '+') { i++; dir = +1; }
	seekto = strlen(&arg[i]);
	if (arg[i+seekto] == '%') perc = true;

	seekto = atoi(&arg[i]);

	if (perc)
	{
		if (dir != 0) seekto = status->elapsedTime + ((status->totalTime * seekto / 100) * dir);
		else seekto = (status->totalTime * perc / 100);
	}
	else
	{
		if (dir != 0) seekto = status->elapsedTime + (seekto*dir);
		/* dir == 0 case is set correctly above */
	}

	/*
	 * Take care of limits
	 * (end-10 so that one can search till the end easily)
	 */
	if (seekto < 0 || seekto > (status->totalTime-10)) return;

	while (retry > 0)
	{
		retry--;
		mpd_sendSeekIdCommand(mpd, status->songid, seekto);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}

	mpd_freeStatus(status);
}

static void f_pause(const char *arg, const char UNUSED *args);
static void f_pause(const char *arg, const char UNUSED *args)
{
	int retry = 5, mode = 0;
	if (!arg || strlen(arg) == 0 || (strcasecmp(arg, "toggle") == 0))
	{
		/* Toggle the pause mode */
		mpd_Status *status = empcd_status();
		if (!status) return;

		mode = (status->state == MPD_STATUS_STATE_PAUSE ? 0 : 1);
		mpd_freeStatus(status);
	}
	else if (strcasecmp(arg, "on" ) == 0) mode = 1;
	else if (strcasecmp(arg, "off") == 0) mode = 0;

	while (retry > 0)
	{
		retry--;
		mpd_sendPauseCommand(mpd, mode);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}
}

static void f_random(const char *arg, const char UNUSED *args);
static void f_random(const char *arg, const char UNUSED *args)
{
	int retry = 5, mode = 0;

	if (!arg || strlen(arg) == 0 || (strcasecmp(arg, "toggle") == 0))
	{
		/* Toggle the random mode */
		mpd_Status *status = empcd_status();
		if (!status) return;

		mode = !status->random;
		mpd_freeStatus(status);
	}
	else if (strcasecmp(arg, "on" ) == 0) mode = 1;
	else if (strcasecmp(arg, "off") == 0) mode = 0;

	while (retry > 0)
	{
		retry--;
		mpd_sendRandomCommand(mpd, mode);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}
}

static void f_update(const char *arg, const char UNUSED *args);
static void f_update(const char *arg, const char UNUSED *args)
{
	char	*path;
	int	retry = 5;

	path = (char *)(arg == NULL ? "" : arg);

	while (retry > 0)
	{
		retry--;
		mpd_sendUpdateCommand(mpd, path);
		if (mpd_check()) continue;
		mpd_finishCommand(mpd);
		if (mpd_check()) continue;
		break;
	}
}

static const struct empcd_funcs
{
	void		(*function)(const char *arg, const char *args);
	bool		requires_mpd;
	const char	*name;
	const char	*args;
	const char	*desc;
} func_map[] =
{
	/* empcd builtin commands */
	{ f_exec,	false, "exec",			"<shellcmd>",		"Execute a command"							},
	{ f_quit,	false, "quit",			NULL,			"Quit empcd"								},

	/* MPD specific commands */
	{ f_next,	true, "mpd_next",		NULL,			"MPD Next Track"							},
	{ f_prev,	true, "mpd_prev",		NULL,			"MPD Previous Track"							},
	{ f_stop,	true, "mpd_stop",		NULL,			"MPD Stop Playing"							},
	{ f_play,	true, "mpd_play",		NULL,			"MPD Start Playing"							},
	{ f_pause,	true, "mpd_pause",		"[toggle|on|off]",	"MPD Pause Toggle or Set"						},
	{ f_seek,	true, "mpd_seek",		"[+|-]<val>[%]",	"MPD Seek direct or relative (+|-) percentage when ends in %"		},
	{ f_volume,	true, "mpd_volume",		"[+|-]<val>[%]",	"MPD Volume direct or relative (+|-) percentage when ends in %"		},
	{ f_random,	true, "mpd_random",		"[toggle|on|off]",	"MPD Random Toggle or Set"						},
	{ f_update,	true, "mpd_update",		"[<path>]",		"MPD Update"								},
	{ f_load,	true, "mpd_plst_load",		"<playlist>",		"MPD Load Playlist"							},
	{ f_save,	true, "mpd_plst_save",		"<playlist>",		"MPD Save Playlist"							},
	{ f_clear,	true, "mpd_plst_clear",		NULL,			"MPD Clear Playlist"							},
	{ f_remove,	true, "mpd_plst_remove",	"<playlist>",		"MPD Remove Playlist"							},

	/* End */
	{ NULL,		false, NULL,			NULL,			"undefined"								}
};

/********************************************************************/

static bool set_event(uint16_t type, uint16_t code, int32_t value, void (*action)(const char *arg, const char *args), const char *args, const char *needargs);
static bool set_event(uint16_t type, uint16_t code, int32_t value, void (*action)(const char *arg, const char *args), const char *args, const char *needargs)
{
	if (maxevent >= (sizeof(events)/sizeof(events[0])))
	{
		dolog(LOG_ERR, "Maximum number of events reached\n");
		return false;
	}

	/* Handle no-repeating 'up' key */
	if (type == EV_KEY && value == EMPCD_KEY_UPNR)
	{
		value = EV_KEY_UP;
		events[maxevent].norepeat = true;
	}

	events[maxevent].type = type;
	events[maxevent].code = code;
	events[maxevent].value = value;
	events[maxevent].prev_value = -1; 
	events[maxevent].action = action;
	events[maxevent].args = args ? strdup(args) : args;
	events[maxevent].needargs = needargs;

	maxevent++;
	return true;
}

static bool which_func(const char *buf, unsigned int len, unsigned int *o_, unsigned int *func_, const char **arg);
static bool which_func(const char *buf, unsigned int len, unsigned int *o_, unsigned int *func_, const char **arg)
{
	unsigned int i, o = *o_, l;

	/* Figure out the function */
	for (i=0; func_map[i].name != NULL; i++)
	{
		l = strlen(func_map[i].name);
		if (len != o+l && (len < o+l || buf[o+l] != ' ')) continue;
		if (strncasecmp(&buf[o], func_map[i].name, l) == 0) break;
	}

	if (func_map[i].name == NULL)
	{
		*o_ = o;
		return false;
	}
	*func_ = i;

	o += l+1;
	if (len > o) *arg = &buf[o];
	else *arg = NULL;

	*o_ = o;

	return true;
}

/*
	KEY_KPSLASH DOWN f_seek -1
	<key> <value> <action> <arg>
*/
static bool set_event_from_map(const char *buf, struct empcd_mapping *event_map, struct empcd_mapping *value_map);
static bool set_event_from_map(const char *buf, struct empcd_mapping *event_map, struct empcd_mapping *value_map)
{
	unsigned int	i = 0, o = 0, len = strlen(buf), l = 0,
			event_code = 0,
			value = 0, func = 0;
	const char	*arg = NULL;
	const char	*event_name = "custom", *event_desc = "custom";

	/* Not a numeric value? */
	if (sscanf(&buf[o], "%u", &i) == 1 && i == 0)
	{
		/* This is our event_code */
		event_code = i;
	}
	else
	{
		/* Try a name match */
		for (i=0; event_map[i].code != EMPCD_MAPPING_END; i++)
		{
			l = strlen(event_map[i].name);
			if (len < o+l || buf[o+l] != ' ') continue;
			if (strncasecmp(&buf[o], event_map[i].name, l) == 0) break;
		}

		if (event_map[i].code == EMPCD_MAPPING_END)
		{
			dolog(LOG_DEBUG, "Undefined Code at %u in '%s'\n", o, buf);
			return false;
		}

		/* This is our event_code */
		event_code = event_map[i].code;
		event_name = event_map[i].name;
		event_desc = event_map[i].desc;
	}

	/* Figure out the value (up/down/release/...) */
	o += l+1;
	for (i=0; value_map[i].code != EMPCD_MAPPING_END; i++)
	{
		l = strlen(value_map[i].name);
		if (len < o+l || buf[o+l] != ' ') continue;
		if (strncasecmp(&buf[o], value_map[i].name, l) == 0) break;
	}

	if (value_map[i].code == EMPCD_MAPPING_END)
	{
		dolog(LOG_DEBUG, "Undefined Key Value at %u in '%s'\n", o, buf);
		return false;
	}
	value = i;

	o += l+1;

	/* Figure out the function */
	if (!which_func(buf, len, &o, &i, &arg))
	{
		dolog(LOG_DEBUG, "Undefined Function at %u in '%s'\n", o, buf);
		return false;
	}
	func = i;

	dolog(LOG_DEBUG, "Mapping Event %s (%s/%u) %s (%s) to do %s (%s) with arg %s\n",
		event_name, event_desc, event_code,
		value_map[value].name, value_map[value].desc,
		func_map[func].name, func_map[func].desc,
		arg ? arg : "<none>");

	if (func_map[func].requires_mpd && nompd)
	{
		dolog(LOG_ERR, "Function requires MPD but MPD is disabled\n");
		return false;
	}

	return set_event(EV_KEY, event_code, value_map[value].code, func_map[func].function, arg, func_map[func].args);
}

static bool set_event_from_custom(char *buf);
static bool set_event_from_custom(char *buf)
{
	unsigned int	type, code, value;
	unsigned int	o, c = 0, func;
	const char	*arg;

	dolog(LOG_ERR, "Custom Event: '%s'\n", buf);

	if (sscanf(buf, "%u %u %u ", &type, &code, &value) != 3)
	{
		dolog(LOG_ERR, "'custom' requires a numeric type, code and value\n");
		return false;
	}

	/* Check if they are max 16 bits (value is 32bits) */
	if (type > 0xffff || code > 0xffff)
	{
		dolog(LOG_ERR, "'custom' type/code/value are only 16bits, value provided too large\n");
		return false;
	}

	/* Skip over the type/code/value */
	for (o = 0; c < 3 && buf[o] != '\0'; o++)
	{
		if (buf[o] == ' ') c++;
	}

	if (c != 3 || buf[o] == '\0')
	{
		dolog(LOG_ERR, "'custom' requires a numeric type, code and value plus a function and optional arguments\n");
		return false;
	}

	if (!which_func(buf, strlen(buf), &o, &func, &arg)) return false;

	dolog(LOG_DEBUG,
		"Mapping Custom Event (type %u, code %u, value %u) to do %s (%s) with arg %s\n",
		type, code, value,
		func_map[func].name, func_map[func].desc,
		arg ? arg : "<none>");

	if (func_map[func].requires_mpd && nompd)
	{
		dolog(LOG_ERR, "Function requires MPD but MPD is disabled\n");
		return false;
	}

	return set_event(type, code, value, func_map[func].function, arg, func_map[func].args);
}

/********************************************************************/

/*
	 0 = failed to open file
	>0 = all okay (lines read)
	<0 = error parsing file (line number)
*/
static int readconfig(const char *cfgfile, char **device);
static int readconfig(const char *cfgfile, char **device)
{
	unsigned int	line = 0;
	int		ret = 0;
	FILE		*f;

	f = fopen(cfgfile, "r");

	dolog(LOG_DEBUG, "ReadConfig(%s) = %s\n", cfgfile, f ? "ok" : "error");

	if (!f) return 0;

	while (!feof(f) && ret == 0)
	{
		char buf[1024], buf2[1024];
		unsigned int n, i = 0, j = 0;

		line++;

		if (fgets(buf2, sizeof(buf2), f) == 0) break;
		n = strlen(buf2)-1;

		/*
		 * Trim whitespace
		 * - Translate \t to space
		 * - strip multiple whitespaces
		 * Saves handling them below
		 */
		for (i=0,j=0; i<n; i++)
		{
			if (buf2[i] == '\t') buf2[i] = ' ';
			if (	(i == 0 || (i > 0 && buf2[i-1] == ' ')) &&
				(buf2[i] == ' ' || buf2[i] == '\t'))
			{
				continue;
			}

			buf[j++] = buf2[i];
		}

		/* Trim trailing space if it is there */
		if (j>0 && buf[j-1] == ' ') j--;

		/* Terminate our new constructed string */
		buf[j] = '\0';
		n = j;

		/* Empty or comment line? */
		if (	n == 0 ||
			buf[0] == '#' ||
			(buf[0] == '/' && buf[1] == '/'))
		{
			continue;
		}

		dolog(LOG_DEBUG, "%s@%04u: %s\n", cfgfile, line, buf);

		if (strncasecmp("mpd_host ", buf, 9) == 0)
		{
			dolog(LOG_DEBUG, "Setting MPD_HOST to %s\n", &buf[9]);
			if (mpd_host) free(mpd_host);
			mpd_host = strdup(&buf[9]);
		}
		else if (strncasecmp("mpd_port ", buf, 9) == 0)
		{
			dolog(LOG_DEBUG, "Setting MPD_PORT to %s\n", &buf[9]);
			if (mpd_port) free(mpd_port);
			mpd_port = strdup(&buf[9]);
		}
		else if (strncasecmp("eventdevice ", buf, 12) == 0)
		{
			if (*device) free(*device);
			*device = strdup(&buf[12]);
		}
		else if (strncasecmp("exclusive ", buf, 10) == 0)
		{
			if (strncasecmp("on", &buf[10], 2) == 0) exclusive = true;
			else if (strncasecmp("off", &buf[10], 3) == 0) exclusive = false;
			else
			{
				dolog(LOG_ERR, "Exclusive is either 'on' or 'off'\n");
				ret = -line;
				break;
			}
		}
		else if (strncasecmp("exclusive", buf, 9) == 0)
		{
			exclusive = true;
		}
		else if (strncasecmp("nonexclusive", buf, 12) == 0)
		{
			exclusive = false;
		}
		else if (strncasecmp("key ", buf, 4) == 0)
		{
			if (!set_event_from_map(&buf[4], key_event_map, key_value_map))
			{
				ret = -line;
				break;
			}
		}
		else if (strncasecmp("custom ", buf, 7) == 0)
		{
			if (!set_event_from_custom(&buf[7]))
			{
				ret = -line;
				break;
			}
		}
		else if (strncasecmp("user ", buf, 5) == 0)
		{
			struct passwd *passwd;

			/* setuid()+setgid() to another user+group */
			passwd = getpwnam(&buf[5]);
			if (passwd)
			{
				drop_uid = passwd->pw_uid;
				drop_gid = passwd->pw_gid;
			}
			else
			{
				dolog(LOG_ERR, "Couldn't find user %s\n", optarg);
				ret = -line;
				break;
			}
		}
		else if (strncasecmp("giveup", buf, 6) == 0)
		{
			giveup = true;
		}
		else if (strncasecmp("dontgiveup", buf, 10) == 0)
		{
			giveup = false;
		}
		else if (strncasecmp("nompd", buf, 5) == 0)
		{
			nompd = true;
		}
		else
		{
			dolog(LOG_ERR, "Unrecognized configuration line %u: %s\n", line, buf);
			ret = -line;
			break;
		}
	}

	fclose(f);

	return ret == 0 ? (int)line : ret;
}

static void handle_event(struct input_event *ev);
static void handle_event(struct input_event *ev)
{
	struct empcd_events	*evt;
	unsigned int		i, i_event;

	/* Checking all events, thus multiple events can be set for an event */
	for (i_event = 0; i_event < maxevent; i_event++)
	{
		/* Does not match yet */
		evt = NULL;

		/* Right Type & Code? */
		if (	events[i_event].type == ev->type &&
			events[i_event].code == ev->code)
		{
			/* It is this current value, then it is this event */
			if (events[i_event].value == ev->value)
			{
				/* This is the night^Wevent */
				evt = &events[i_event];
			}
			else
			{
				/* Note the 'previous' value */
				events[i_event].prev_value = ev->value;
			}
		}

		/* Handle logging */
		if (	(evt != NULL && verbosity > 2) ||
			(evt == NULL && verbosity > 5))
		{
			char				buf[1024];
			unsigned int			n = 0;
			struct empcd_mapping		*map = NULL, *val = NULL;
			const struct empcd_funcs	*func = func_map;

			if (ev->type == EV_KEY)
			{
				map = key_event_map;
				val = key_value_map;
			}

			if (map)
			{
				for (i=0; map[i].code != EMPCD_MAPPING_END && map[i].code != ev->code; i++);
				map = &map[i];
			}

			if (val)
			{
				for (i=0; val[i].code != EMPCD_MAPPING_END && val[i].code != ev->value; i++);
				val = &val[i];
			}

			if (evt)
			{
				for (i=0; func[i].name != NULL && func[i].function != evt->action; i++);
				func = &func[i];
			}

			n += snprintf(&buf[n], sizeof(buf)-n, "Event: T%lu.%06lu, type %u, code %u, value %d",
					ev->time.tv_sec, ev->time.tv_usec, ev->type,
					ev->code, ev->value);

			if (map)
			{
				n += snprintf(&buf[n], sizeof(buf)-n, ": %s, name: %s, desc: %s",
						val ? val->name : "<unknown value>",
						map->name ? map->name : "<unknown name>",
						map->desc ? map->desc : "");
			}

			if (evt)
			{
				n += snprintf(&buf[n], sizeof(buf)-n, ", action: %s(%s)",
						func->name ? func->name : "?",
						evt->args ? evt->args : "");
			}

			dolog(LOG_DEBUG, "%s\n", buf);
		}

		if (!evt) continue;

		/* Handle REPEAT and then UP event for keys */
		if (	ev->type == EV_KEY &&
			ev->value == EV_KEY_UP &&
			evt->norepeat)
		{
			/* Was it noted that we before had a repeat event? */
			if (evt->prev_value == EV_KEY_REPEAT)
			{
				/* Ignore this event, but reset the previous */
				evt->prev_value = -1;
				continue;
			}
		}

		evt->action(evt->args, evt->needargs);
	}
}

/* Long options */
static struct option const long_options[] = {
	{"config",		required_argument,	NULL, 'c'},
	{"daemonize",		no_argument,		NULL, 'd'},
	{"eventdevice",		required_argument,	NULL, 'e'},
	{"nodaemonize",		no_argument,		NULL, 'f'},
	{"giveup",		no_argument,		NULL, 'g'},
	{"dontgiveup",		no_argument,		NULL, 'G'},
	{"help",		no_argument,		NULL, 'h'},
	{"list-keys",		no_argument,		NULL, 'K'},
	{"list-functions",	no_argument,		NULL, 'L'},
	{"nompd",		no_argument,		NULL, 'n'},
	{"quiet",		no_argument,		NULL, 'q'},
	{"user",		required_argument,	NULL, 'u'},
	{"verbose",		no_argument,		NULL, 'v'},
	{"version",		no_argument,		NULL, 'V'},
	{"exclusive",		no_argument,		NULL, 'x'},
	{"nonexclusive",	no_argument,		NULL, 'X'},
	{"verbosity",		required_argument,	NULL, 'y'},
	{NULL,			no_argument,		NULL, 0},
};

static char short_options[] = "c:de:fgGhKLnqu:vVxXy:";

static struct
{
	const char *args;
	const char *desc;
} desc_options[] =
{
	/* c:	*/ {"<file>",		"Configuration File Location"},
	/* d	*/ {NULL,		"Detach the program into the background"},
	/* e:	*/ {"<eventdevice>",	"The event device to use (default: /dev/input/event0)"},
	/* f	*/ {NULL,		"Don't detach, stay in the foreground"},
	/* g	*/ {NULL,		"Give up when opening the device fails (default)"},
	/* G	*/ {NULL,		"Do not give up when opening the device fails"},
	/* h	*/ {NULL,		"This help"},
	/* K	*/ {NULL,		"List the keys that are known to this program"},
	/* L	*/ {NULL,		"List the functions known to this program"},
	/* n	*/ {NULL,		"no-mpd mode, does not connect to mpd"},
	/* q	*/ {NULL,		"Lower the verbosity level to 0 (quiet)"},
	/* u:	*/ {"<username>",	"Drop priveleges to <user>"},
	/* v	*/ {NULL,		"Increase the verbosity level by 1"},
	/* V	*/ {NULL,		"Show the version of this program"},
	/* x	*/ {NULL,		"Exclusive device access (default)"},
	/* X	*/ {NULL,		"Non-Exclusive device access"},
	/* y:	*/ {"<level>",		"Set the verbosity level to <level>"},
	{NULL,			NULL}
};

int main (int argc, char **argv)
{
	int			fd = -1, option_index, j;
	char			*device = NULL, *conffile = NULL, *t;
	const char		*cfgfile = NULL;
	struct input_event	ev;
	unsigned int		i;

	while ((j = getopt_long(argc, argv, short_options, long_options, &option_index)) != EOF)
	{
		switch (j)
		{
		case 'c':
			if (conffile) free(conffile);
			conffile = strdup(optarg);
			break;

		case 'd':
			daemonize = true;
			break;

		case 'e':
			if (device) free(device);
			device = strdup(optarg);
			break;

		case 'f':
			daemonize = false;
			break;

		case 'g':
			giveup = true;
			break;

		case 'G':
			giveup = true;
			break;

		case 'h':
			fprintf(stderr, "usage: %s\n", argv[0]);

			for (i=0; long_options[i].name; i++)
			{
				char buf[3] = "  ";
				if (long_options[i].val != 0)
				{
					buf[0] = '-';
					buf[1] = long_options[i].val;
				}

				fprintf(stderr, "%2s, --%-15s %-15s %s\n",
					buf,
					long_options[i].name,
					desc_options[i].args ? desc_options[i].args : "",
					desc_options[i].desc ? desc_options[i].desc : "");
			}

			return 1;

		case 'K':
			for (i=0; key_event_map[i].code != EMPCD_MAPPING_END; i++)
			{
				fprintf(stderr, "%-25s %s\n", key_event_map[i].name, key_event_map[i].desc);
			}
			return 0;

		case 'L':
			for (i=0; func_map[i].name; i++)
			{
				fprintf(stderr, "%-15s %20s %s\n", func_map[i].name, func_map[i].args ? func_map[i].args : "", func_map[i].desc);
			}
			return 0;

		case 'n':
			nompd = true;
			break;

		case 'q':
			verbosity++;
			break;

		case 'u':
			{
				struct passwd *passwd;
				/* setuid()+setgid() to another user+group */
				passwd = getpwnam(optarg);
				if (passwd)
				{
					drop_uid = passwd->pw_uid;
					drop_gid = passwd->pw_gid;
				}
				else
				{
					dolog(LOG_ERR, "Couldn't find user %s\n", optarg);
					return 1;
				}
			}

			break;

		case 'v':
			verbosity++;
			break;

		case 'V':
			fprintf(stderr, EMPCD_VSTRING, EMPCD_VERSION);
			return 1;

		case 'x':
			exclusive = true;
			break;

		case 'X':
			exclusive = false;
			break;

		case 'y':
			verbosity = atoi(optarg);
			break;

		default:
			if (j != 0) fprintf(stderr, "Unknown short option '%c'\n", j);
			else fprintf(stderr, "Unknown long option\n");
			fprintf(stderr, "See '%s -h' for help\n", argv[0]);

			return 1;
		}
	}

	dolog(LOG_INFO, EMPCD_VSTRING, EMPCD_VERSION);

	if (!device) device = strdup("/dev/input/event0");

	if ((t = getenv("MPD_HOST"))) mpd_host = strdup(t);
	else mpd_host = strdup(MPD_HOST_DEFAULT);
	if ((t = getenv("MPD_PORT"))) mpd_port = strdup(t);
	else mpd_port = strdup(MPD_PORT_DEFAULT);

	if (!conffile)
	{
		/* Try user's config */
		cfgfile = getenv("HOME");
		if (cfgfile)
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "%s/%s", cfgfile, ".empcd.conf");
			cfgfile = conffile = strdup(buf);
			j = readconfig(cfgfile, &device);
		}
		else j = 0;

		if (j == 0)
		{
			cfgfile = "/etc/empcd.conf";
			j = readconfig(cfgfile, &device);
		}
	}
	else
	{
		/* Try specified config */
		cfgfile = conffile;
		j = readconfig(cfgfile, &device);
	}

	if (j <= 0)
	{
		if (j == 0)
		{
			dolog(LOG_ERR, "Configuration file '%s' not found\n", cfgfile);
		}
		else
		{
			dolog(LOG_ERR, "Parse error in configuration file '%s' on line %u\n", cfgfile, (unsigned int)-j);
		}

		if (device) free(device);
		if (conffile) free(conffile);

		return 1;
	}

	if (conffile)
	{
		free(conffile);
		conffile = NULL;
	}

	if (daemonize)
	{
		j = fork();
		if (j < 0)
		{
			dolog(LOG_ERR, "Couldn't fork for daemonization\n");
			return 1;
		}

		/* Exit the mother fork */
		if (j != 0) return 0;

		/* Child fork */
		if (setsid() < 0)
		{
			doelog(LOG_ERR, errno, "Failed to reopen stdout\n");
			return 1;
		}

		/* Cleanup stdin/out/err */
		if (freopen("/dev/null", "r", stdin) == NULL)
		{
			doelog(LOG_ERR, errno, "Failed to reopen stdin\n");
			return 1;
		}

		if (freopen("/dev/null", "w", stdout) == NULL)
		{
			doelog(LOG_ERR, errno, "Failed to reopen stdout\n");
			return 1;
		}

		if (freopen("/dev/null", "w", stderr) == NULL)
		{
			doelog(LOG_ERR, errno, "Failed to reopen stderr\n");
			return 1;
		}
	}

	/* Handle these signals for a clean exit */
	signal(SIGHUP,  &handle_signal);
	signal(SIGTERM, &handle_signal);
	signal(SIGINT,  &handle_signal);
	signal(SIGKILL, &handle_signal);

	/* Ignore some odd signals */
	signal(SIGILL,  SIG_IGN);
	signal(SIGABRT, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGSTOP, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);

	while (running)
	{
		/* Try to open the device */
		fd = open(device, O_RDONLY);

		/* Worked? */
		if (fd >= 0) break;

		doelog(LOG_ERR, errno, "Couldn't open event device %s\n", device);

		if (giveup) break;

		/* Sleep a bit and try it all again, cheap enough to not flood the CPU */
		sleep(1);
	}

	free(device);
	device = NULL;

	if (fd < 0)
	{
		dolog(LOG_ERR, "Couldn't open event device, gave up\n");
		return 1;
	}

	/* Obtain Exclusive device access */
	if (exclusive)
	{
		ioctl(fd, EVIOCGRAB, 1);
	}

	/* Allow usage of empcd without contacting MPD, thus effectively making it a input daemon */
	if (!nompd)
	{
		/* Setup MPD connectivity */
		mpd = empcd_setup();
		if (!mpd)
		{
			dolog(LOG_ERR, "Couldn't contact MPD server\n");
			return 1;
		}
	}

	/*
	 * Drop our root privileges.
	 * We don't need them anymore anyways
	 */
	if (drop_gid != 0)
	{
		dolog(LOG_INFO, "Dropping groupid to %u...\n", drop_gid);
		if (setgid(drop_gid) < 0)
		{
			doelog(LOG_ERR, errno, "Could not drop to GID %un", drop_gid);
			running = false;
		}
	}

	if (drop_uid != 0)
	{
		dolog(LOG_INFO, "Dropping userid to %u...\n", drop_uid);
		if (setuid(drop_uid) < 0)
		{
			doelog(LOG_ERR, errno, "Could not drop to UID %un", drop_uid);
			running = false;
		}
	}

	if (running)
	{
		dolog(LOG_INFO, "Running as PID %u, processing your strokes\n", getpid());
	}

	while (running)
	{
		struct timeval	tv;
		fd_set		fdread;

		FD_ZERO(&fdread);
		FD_SET(fd, &fdread);
		tv.tv_sec = 5;
		tv.tv_usec = 0;
		j = select(fd+1, &fdread, NULL, NULL, &tv);
		if (j == 0) continue;
		if (j < 0 || read(fd, &ev, sizeof(ev)) == -1) break;

		handle_event(&ev);
	}

	dolog(LOG_INFO, "empcd shutting down\n");

	if (!nompd) mpd_closeConnection(mpd);

	close(fd);
	return 0;
}

