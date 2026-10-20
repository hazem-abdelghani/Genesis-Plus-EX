/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  upnp.h -- asks the home router to forward the netplay port (UPnP).
 *
 *  upnp_open() starts the work on a background thread, so nothing waits for
 *  the router; upnp_state() says how far it got. upnp_close() takes the
 *  forwarding away again.
 ****************************************************************************/

#ifndef _UPNP_H_
#define _UPNP_H_

enum { UPNP_OFF = 0, UPNP_WORKING, UPNP_OPEN, UPNP_FAILED };

extern void        upnp_open(int port);
extern void        upnp_close(void);
extern int         upnp_state(void);
extern const char *upnp_external_ip(void);   /* "" until the router has told it */
extern const char *upnp_message(void);       /* a short sentence about the result */

#endif
