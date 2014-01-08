/* EMPCd Headers/Structs etc */

#ifndef EMPCD_H
#define EMPCD_H 1

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <string.h>
#include <syslog.h>
#include <getopt.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>

/* Linux specific... */
#include <linux/input.h>
#ifndef EVIOCGRAB
#define EVIOCGRAB	_IOW('E', 0x90, int)	/* Grab/Release device, Linux kernel 2.4 headers don't have this */ 
#endif

#ifndef NULL
#define NULL 0
#endif

#ifndef bool
#define bool int8_t
#endif

#ifndef false
#define false 0
#endif

#ifndef true
#define true (!false)
#endif

#ifdef __GNUC__
#define UNUSED __attribute__ ((__unused__))
#else
#define UNUSED
#endif

#define snprintfok(ret, bufsize) (((ret) >= 0) && (((unsigned int)(ret)) < bufsize))

struct empcd_events
{
	uint16_t		type;
	uint16_t 		code;
	int32_t			value;
	int32_t			prev_value;
	bool			norepeat;

	void			(*action)(const char *arg, const char *args);
	const char		*args, *needargs;
};

/* EV_KEY_UP but signal that there is no repeat; thus, the case where REPEAT and then an UP event happen */
#define EMPCD_KEY_UPNR		0xfffe

/* End of mapping list */
#define EMPCD_MAPPING_END	0xffff

struct empcd_mapping
{
	uint16_t		code;
	char			name[64];
	char			desc[64];
};

#define EV_KEY_UP		0
#define EV_KEY_DOWN		1
#define EV_KEY_REPEAT		2
#define EV_KEY_UNDEFINED	42

extern struct empcd_mapping key_value_map[];
extern struct empcd_mapping key_event_map[];

#endif /* EMPCD_H */

