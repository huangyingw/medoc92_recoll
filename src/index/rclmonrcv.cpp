#include "autoconfig.h"
#ifdef RCL_MONITOR
#ifndef lint
static char rcsid[] = "@(#$Id: rclmonrcv.cpp,v 1.4 2006-10-23 14:29:49 dockes Exp $ (C) 2006 J.F.Dockes";
#endif
/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the
 *   Free Software Foundation, Inc.,
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "debuglog.h"
#include "rclmon.h"
#include "fstreewalk.h"
#include "indexer.h"
#include "pathut.h"

/**
 * Recoll real time monitor event receiver. This file has code to interface 
 * to FAM or inotify and place events on the event queue.
 */

/** A small virtual interface for monitors. Probably suitable to let
    either of fam/gamin or raw imonitor hide behind */
class RclMonitor {
public:
    RclMonitor(){}
    virtual ~RclMonitor() {}
    virtual bool addWatch(const string& path, const struct stat&) = 0;
    virtual bool getEvent(RclMonEvent& ev, int secs = -1) = 0;
    virtual bool ok() = 0;
    // Does this monitor generate 'exist' events at startup?
    virtual bool generatesExist() = 0; 
};

// Monitor factory. We only have one compiled-in kind at a time, no
// need for a 'kind' parameter
static RclMonitor *makeMonitor();

/** This class is a callback for the file system tree walker
    class. The callback method alternatively creates the directory
    watches and flushes the event queue (to avoid a possible overflow
    while we create the watches)*/
class WalkCB : public FsTreeWalkerCB {
public:
    WalkCB(RclConfig *conf, RclMonitor *mon, RclMonEventQueue *queue)
	: m_conf(conf), m_mon(mon), m_queue(queue)
    {}
    virtual ~WalkCB() 
    {}

    virtual FsTreeWalker::Status 
    processone(const string &fn, const struct stat *st, FsTreeWalker::CbFlag flg)
    {
	LOGDEB2(("rclMonRcvRun: processone %s m_mon %p m_mon->ok %d\n", 
		 fn.c_str(), m_mon, m_mon?m_mon->ok():0));
	// Create watch when entering directory
	if (flg == FsTreeWalker::FtwDirEnter) {
	    // Empty whatever events we may already have on queue
	    while (m_queue->ok() && m_mon->ok()) {
		RclMonEvent ev;
		if (m_mon->getEvent(ev, 0)) {
		    m_queue->pushEvent(ev);
		} else {
		    break;
		}
	    }
	    if (!m_mon || !m_mon->ok() || !m_mon->addWatch(fn, *st))
		return FsTreeWalker::FtwError;
	} else if (!m_mon->generatesExist() && 
		   flg == FsTreeWalker::FtwRegular) {
	    // Have to synthetize events for regular files existence
	    // at startup because the monitor does not do it
	    RclMonEvent ev;
	    ev.m_path = fn;
	    ev.m_etyp = RclMonEvent::RCLEVT_MODIFY;
	    m_queue->pushEvent(ev);
	}
	return FsTreeWalker::FtwOk;
    }

private:
    RclConfig         *m_conf;
    RclMonitor        *m_mon;
    RclMonEventQueue  *m_queue;
};

/** Main thread routine: create watches, then forever wait for and queue events */
void *rclMonRcvRun(void *q)
{
    RclMonEventQueue *queue = (RclMonEventQueue *)q;

    LOGDEB(("rclMonRcvRun: running\n"));

    // Create the fam/whatever interface object
    RclMonitor *mon;
    if ((mon = makeMonitor()) == 0) {
	LOGERR(("rclMonRcvRun: makeMonitor failed\n"));
	queue->setTerminate();
	return 0;
    }

    // Get top directories from config 
    list<string> tdl = queue->getConfig()->getTopdirs();
    if (tdl.empty()) {
	LOGERR(("rclMonRcvRun:: top directory list (topdirs param.) not"
		"found in config or Directory list parse error"));
	queue->setTerminate();
	return 0;
    }

    // Walk the directory trees to add watches
    FsTreeWalker walker;
    WalkCB walkcb(queue->getConfig(), mon, queue);
    for (list<string>::iterator it = tdl.begin(); it != tdl.end(); it++) {
	queue->getConfig()->setKeyDir(*it);
	// Adjust the skipped names according to config
	walker.clearSkippedNames();
	string skipped; 
	if (queue->getConfig()->getConfParam("skippedNames", skipped)) {
	    list<string> skpl;
	    stringToStrings(skipped, skpl);
	    walker.setSkippedNames(skpl);
	}
	LOGDEB(("rclMonRcvRun: walking %s\n", it->c_str()));
	walker.walk(*it, walkcb);
    }

    // Forever wait for monitoring events and add them to queue:
    LOGDEB2(("rclMonRcvRun: waiting for events. queue->ok() %d\n", queue->ok()));
    while (queue->ok() && mon->ok()) {
	RclMonEvent ev;
	// Note: under Linux, I could find no way to get the select
	// call to return when a signal is delivered to the process
	// (it goes to the main thread, from which I tried to close or
	// write to the select fd, with no effect. So set a 
	// timeout so that an intr will be detected
	if (mon->getEvent(ev, 
#ifdef linux
			  2
#else
			  -1
#endif
			  )) {
	    queue->pushEvent(ev);
	}
    }

    LOGDEB(("rclMonRcvRun: exiting\n"));
    queue->setTerminate();
    return 0;
}

#ifdef RCL_USE_FAM
//////////////////////////////////////////////////////////////////////////
/** Fam/gamin -based monitor class */
#include <fam.h>
#include <sys/select.h>

/** FAM based monitor class. We have to keep a record of FAM watch
    request numbers to directory names as the event only contain the
    request number and file name, not the full path */
class RclFAM : public RclMonitor {
public:
    RclFAM();
    virtual ~RclFAM();
    virtual bool addWatch(const string& path, const struct stat& st);
    virtual bool getEvent(RclMonEvent& ev, int secs = -1);
    bool ok() {return m_ok;}
    virtual bool generatesExist() {return true;}

private:
    bool m_ok;
    FAMConnection m_conn;
    void close() {
	FAMClose(&m_conn);
	m_ok = false;
    }
    map<int,string> m_reqtopath;
    const char *event_name(int code);
};

// Translate event code to string (debug)
const char *RclFAM::event_name(int code)
{
    static const char *famevent[] = {
        "",
        "FAMChanged",
        "FAMDeleted",
        "FAMStartExecuting",
        "FAMStopExecuting",
        "FAMCreated",
        "FAMMoved",
        "FAMAcknowledge",
        "FAMExists",
        "FAMEndExist"
    };
    static char unknown_event[20];
 
    if (code < FAMChanged || code > FAMEndExist) {
        sprintf(unknown_event, "unknown (%d)", code);
        return unknown_event;
    }
    return famevent[code];
}

RclFAM::RclFAM()
    : m_ok(false)
{
    if (FAMOpen2(&m_conn, "Recoll")) {
	LOGERR(("RclFAM::RclFAM: FAMOpen2 failed, errno %d\n", errno));
	return;
    }
    m_ok = true;
}

RclFAM::~RclFAM()
{
    if (ok())
	FAMClose(&m_conn);
}

bool RclFAM::addWatch(const string& path, const struct stat& st)
{
    if (!ok())
	return false;
    LOGDEB(("RclFAM::addWatch: adding %s\n", path.c_str()));
    FAMRequest req;
    if (S_ISDIR(st.st_mode)) {
	if (FAMMonitorDirectory(&m_conn, path.c_str(), &req, 0) != 0) {
	    LOGERR(("RclFAM::addWatch: FAMMonitorDirectory failed\n"));
	    return false;
	}
    } else if (S_ISREG(st.st_mode)) {
	if (FAMMonitorFile(&m_conn, path.c_str(), &req, 0) != 0) {
	    LOGERR(("RclFAM::addWatch: FAMMonitorFile failed\n"));
	    return false;
	}
    }
    m_reqtopath[req.reqnum] = path;
    return true;
}

bool RclFAM::getEvent(RclMonEvent& ev, int secs)
{
    if (!ok())
	return false;
    LOGDEB2(("RclFAM::getEvent:\n"));

    fd_set readfds;
    int fam_fd = FAMCONNECTION_GETFD(&m_conn);
    FD_ZERO(&readfds);
    FD_SET(fam_fd, &readfds);

    LOGDEB2(("RclFAM::getEvent: select\n"));
    struct timeval timeout;
    if (secs >= 0) {
	memset(&timeout, 0, sizeof(timeout));
	timeout.tv_sec = secs;
    }
    int ret;
    if ((ret=select(fam_fd + 1, &readfds, 0, 0, secs >= 0 ? &timeout : 0)) < 0) {
	LOGERR(("RclFAM::getEvent: select failed, errno %d\n", errno));
	close();
	return false;
    } else if (ret == 0) {
	// timeout
	return false;
    }

    if (!FD_ISSET(fam_fd, &readfds))
	return false;

    FAMEvent fe;
    if (FAMNextEvent(&m_conn, &fe) < 0) {
	LOGERR(("RclFAM::getEvent: FAMNextEvent failed, errno %d\n", errno));
	close();
	return false;
    }
    
    map<int,string>::const_iterator it;
    if ((fe.filename[0] != '/') && 
	(it = m_reqtopath.find(fe.fr.reqnum)) != m_reqtopath.end()) {
	ev.m_path = path_cat(it->second, fe.filename);
    } else {
	ev.m_path = fe.filename;
    }
    LOGDEB(("RclFAM::getEvent: %-12s %s\n", 
	    event_name(fe.code), ev.m_path.c_str()));

    switch (fe.code) {
    case FAMChanged:
    case FAMCreated:
    case FAMExists:
	// Let the other side sort out the status of this file vs the db
	ev.m_etyp = RclMonEvent::RCLEVT_MODIFY;
	break;

    case FAMDeleted:
	ev.m_etyp = RclMonEvent::RCLEVT_DELETE;
	break;

    case FAMMoved: /* Never generated it seems */
	LOGDEB(("RclFAM::getEvent: got move event !\n"));
	ev.m_etyp = RclMonEvent::RCLEVT_MODIFY;
	break;

    case FAMStartExecuting:
    case FAMStopExecuting:
    case FAMAcknowledge:
    case FAMEndExist:
    default:
	return false;
    }
    return true;
}
#endif // RCL_USE_FAM


#ifdef RCL_USE_INOTIFY
//////////////////////////////////////////////////////////////////////////
/** Inotify-based monitor class */
#include <sys/inotify.h>
#include <sys/select.h>

class RclIntf : public RclMonitor {
public:
    RclIntf();
    virtual ~RclIntf();
    virtual bool addWatch(const string& path, const struct stat& st);
    virtual bool getEvent(RclMonEvent& ev, int secs = -1);
    bool ok() {return m_ok;}
    virtual bool generatesExist() {return false;}

private:
    bool m_ok;
    int m_fd;
    map<int,string> m_wdtopath; // Watch descriptor to name
#define EVBUFSIZE (32*1024)
    char m_evbuf[EVBUFSIZE]; // Event buffer
    char *m_evp; // Pointer to next event or 0
    char *m_ep;  // Pointer to end of events
    const char *event_name(int code);
    void close() {
	if (m_fd >= 0) {
	    ::close(m_fd);
	    m_fd = -1;
	}
	m_ok = false;
    }
};

const char *RclIntf::event_name(int code) 
{
    code &= ~(IN_ISDIR|IN_ONESHOT);
    switch (code) {
    case IN_ACCESS: return "IN_ACCESS";
    case IN_MODIFY: return "IN_MODIFY";
    case IN_ATTRIB: return "IN_ATTRIB";
    case IN_CLOSE_WRITE: return "IN_CLOSE_WRITE";
    case IN_CLOSE_NOWRITE: return "IN_CLOSE_NOWRITE";
    case IN_CLOSE: return "IN_CLOSE";
    case IN_OPEN: return "IN_OPEN";
    case IN_MOVED_FROM: return "IN_MOVED_FROM";
    case IN_MOVED_TO: return "IN_MOVED_TO";
    case IN_MOVE: return "IN_MOVE";
    case IN_CREATE: return "IN_CREATE";
    case IN_DELETE: return "IN_DELETE";
    case IN_DELETE_SELF: return "IN_DELETE_SELF";
    case IN_MOVE_SELF: return "IN_MOVE_SELF";
    case IN_UNMOUNT: return "IN_UNMOUNT";
    case IN_Q_OVERFLOW: return "IN_Q_OVERFLOW";
    case IN_IGNORED: return "IN_IGNORED";
    default: {
	static char msg[50];
	sprintf(msg, "Unknown event 0x%x", code);
	return msg;
    }
    };
}

RclIntf::RclIntf()
    : m_ok(false), m_fd(-1), m_evp(0), m_ep(0)
{
    if ((m_fd = inotify_init()) < 0) {
	LOGERR(("RclIntf::RclIntf: inotify_init failed, errno %d\n", errno));
	return;
    }
    m_ok = true;
}

RclIntf::~RclIntf()
{
    close();
}

bool RclIntf::addWatch(const string& path, const struct stat& st)
{
   if (!ok())
        return false;
    LOGDEB(("RclIntf::addWatch: adding %s\n", path.c_str()));
    // CLOSE_WRITE and CREATE are covered through MODIFY
    uint32_t mask = IN_MODIFY 
        | IN_MOVED_FROM | IN_MOVED_TO
	| IN_DELETE
#ifdef IN_DONT_FOLLOW
	| IN_DONT_FOLLOW
#endif
;
    int wd;
    if ((wd = inotify_add_watch(m_fd, path.c_str(), mask)) < 0) {
        LOGERR(("RclIntf::addWatch: inotify_add_watch failed\n"));
	return false;
    }
    m_wdtopath[wd] = path;
    return true;
}

bool RclIntf::getEvent(RclMonEvent& ev, int secs)
{
    if (!ok())
	return false;
    LOGDEB2(("RclIntf::getEvent:\n"));

    if (m_evp == 0) {
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(m_fd, &readfds);
	struct timeval timeout;
	if (secs >= 0) {
	    memset(&timeout, 0, sizeof(timeout));
	    timeout.tv_sec = secs;
	}
	int ret;
	LOGDEB2(("RclIntf::getEvent: select\n"));
	if ((ret=select(m_fd + 1, &readfds, 0, 0, secs >= 0 ? &timeout : 0)) < 0) {
	    LOGERR(("RclIntf::getEvent: select failed, errno %d\n", errno));
	    close();
	    return false;
	} else if (ret == 0) {
	    LOGDEB2(("RclIntf::getEvent: select timeout\n"));
	    // timeout
	    return false;
	}
	LOGDEB(("RclIntf::getEvent: select returned\n"));

	if (!FD_ISSET(m_fd, &readfds))
	    return false;
	int rret;
	if ((rret=read(m_fd, m_evbuf, sizeof(m_evbuf))) <= 0) {
	    LOGERR(("RclIntf::getEvent: read failed, %d->%d errno %d\n", 
		    sizeof(m_evbuf), rret, errno));
	    close();
	    return false;
	}
	m_evp = m_evbuf;
	m_ep = m_evbuf + rret;
    }

    struct inotify_event *evp = (struct inotify_event *)m_evp;
    m_evp += sizeof(struct inotify_event);
    if (evp->len > 0)
	m_evp += evp->len;
    if (m_evp >= m_ep)
	m_evp = m_ep = 0;
    
    map<int,string>::const_iterator it;
    if ((it = m_wdtopath.find(evp->wd)) == m_wdtopath.end()) {
	LOGERR(("RclIntf::getEvent: unknown wd\n"));
	return false;
    }
    ev.m_path = it->second;

    if (evp->len > 0) {
	ev.m_path = path_cat(ev.m_path, evp->name);
    }

    LOGDEB(("RclIntf::getEvent: %-12s %s\n", 
	    event_name(evp->mask), ev.m_path.c_str()));

    if (evp->mask & (IN_MODIFY | IN_MOVED_TO)) {
	ev.m_etyp = RclMonEvent::RCLEVT_MODIFY;
    } else if (evp->mask & (IN_DELETE | IN_MOVED_FROM)) {
	ev.m_etyp = RclMonEvent::RCLEVT_DELETE;
    } else {
	LOGDEB(("RclIntf::getEvent: unhandled event %s 0x%x %s\n", 
		event_name(evp->mask), evp->mask, ev.m_path.c_str()));
	return false;
    }
    return true;
}

#endif // RCL_USE_INOTIFY


///////////////////////////////////////////////////////////////////////
// The monitor 'factory'
static RclMonitor *makeMonitor()
{
#ifdef RCL_USE_FAM    
    return new RclFAM;
#endif
#ifdef RCL_USE_INOTIFY
    return new RclIntf;
#endif
    return 0;
}
#endif // RCL_MONITOR
