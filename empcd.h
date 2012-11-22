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

struct empcd_events
{
	uint16_t		type;
	uint16_t 		code;
	int32_t			value;
	void			(*action)(const char *arg, const char *args);
	const char		*args, *needargs;
};

#define EMPCD_MAPPING_END	0xffff

struct empcd_mapping
{
	uint16_t		code;
	char			name[32];
	char			label[32];
};

#define EV_KEY_UP		0
#define EV_KEY_DOWN		1
#define EV_KEY_REPEAT		2
#define EV_KEY_UNDEFINED	42

extern struct empcd_mapping key_value_map[];
extern struct empcd_mapping key_event_map[];

#endif /* EMPCD_H */

