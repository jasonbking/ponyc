#include "asio.h"
#include "event.h"
#ifdef ASIO_USE_EVPORT

#include "../actor/messageq.h"
#include "../mem/pool.h"
#include "../sched/cpu.h"
#include "../sched/scheduler.h"
#include "ponyassert.h"
#include <sys/debug.h>
#include <sys/queue.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <poll.h>
#include <port.h>

#define EV_TERMINATE  1
#define	EV_SIGNAL 2

/* Older releases do not have this defined */
#ifndef __unused
#define __unused __attribute__((unused))
#endif

#ifndef STAILQ_FOREACH_SAFE
#define STAILQ_FOREACH_SAFE(var, head, field, tvar)     \
  for ((var) = STAILQ_FIRST((head));                    \
      (var) && ((tvar) = STAILQ_NEXT((var), field), 1); \
      (var) = (tvar))
#endif

struct asio_backend_t
{
  int         ab_port;
  messageq_t  ab_q;
};

// Since event ports do not support signals as an event type, we have to fake
// it.  We block all signals (except SIGABRT -- so we can quickly crash on
// failure) on all threads except a single signal handling thread.  This thread
// takes delivery of the signal, and then looks at a table (asio_sigtbl)
// indexed by signal number for any registered events.  It then generates
// a user event (of type EV_SIGNAL) and sends the event to the corresponding
// event port for handling.

typedef struct asio_sigevt {
  STAILQ_ENTRY(asio_sigevt) asevt_link;
  asio_event_t*             asevt_evtp;
  int                       asevt_port;
} asio_sigevt_t;
STAILQ_HEAD(asio_sigevt_list, asio_sigevt);

typedef struct asio_sigtbl {
  pthread_mutex_t         atbl_lock;
  struct asio_sigevt_list atbl_evlist;
} asio_sigtbl_t;
static asio_sigtbl_t asio_sigtbl[NSIG];

static pthread_t asio_signal_tid;
static pthread_once_t asio_signal_once = PTHREAD_ONCE_INIT;

static void* asio_signal_thread(void* arg __unused)
{
  sigset_t sigset;
  int signo;

  VERIFY0(sigfillset(&sigset));
  VERIFY0(sigdelset(&sigset, SIGABRT));

  for(;;)
  {
    if (sigwait(&sigset, &signo) != 0)
    {
      continue;
    }

    VERIFY3S(signo, >, 0);
    VERIFY3S(signo, <, NSIG);

    asio_sigtbl_t*  tbl = &asio_sigtbl[signo];
    asio_sigevt_t*  asevt;

    VERIFY0(pthread_mutex_lock(&tbl->atbl_lock));

    if (STAILQ_EMPTY(&tbl->atbl_evlist))
    {
      VERIFY0(pthread_mutex_unlock(&tbl->atbl_lock));
      continue;
    }

    STAILQ_FOREACH(asevt, &tbl->atbl_evlist, asevt_link)
    {
      port_send(asevt->asevt_port, EV_SIGNAL, asevt->asevt_evtp);
    }

    VERIFY0(pthread_mutex_unlock(&tbl->atbl_lock));
  }

  return NULL;
}

static void asio_signal_init()
{
  pthread_attr_t  attr;
  sigset_t        sigset;

  for (size_t i = 0; i < NSIG; i++)
  {
    memset(&asio_sigtbl[i], 0, sizeof (struct asio_sigtbl));
    VERIFY0(pthread_mutex_init(&asio_sigtbl[i].atbl_lock, NULL));
  }

  VERIFY0(pthread_attr_init(&attr));
  VERIFY0(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));

  VERIFY0(sigfillset(&sigset));
  VERIFY0(sigdelset(&sigset, SIGABRT));

  // Hopefully we're still single threaded so any subsequent threads
  // inherit this signal mask
  VERIFY0(pthread_sigmask(SIG_SETMASK, &sigset, NULL));
  VERIFY0(pthread_create(&asio_signal_tid, &attr, asio_signal_thread, NULL));
}

static void asio_signal_add(asio_backend_t* be, asio_event_t* ev, int signo)
{
  VERIFY3U(signo, >, 0);
  VERIFY3U(signo, <, NSIG);

  asio_sigtbl_t*  tbl = &asio_sigtbl[signo];
  asio_sigevt_t*  asevt = POOL_ALLOC(asio_sigevt_t);

  asevt->asevt_port = be->ab_port;
  asevt->asevt_evtp = ev;

  VERIFY0(pthread_mutex_lock(&tbl->atbl_lock));
  STAILQ_INSERT_TAIL(&tbl->atbl_evlist, asevt, asevt_link);
  VERIFY0(pthread_mutex_unlock(&tbl->atbl_lock));
}

static void asio_signal_remove(const asio_event_t* ev, int signo)
{
  VERIFY3U(signo, >, 0);
  VERIFY3U(signo, <, NSIG);

  asio_sigtbl_t*  tbl = &asio_sigtbl[signo];
  asio_sigevt_t*  asevt;
  asio_sigevt_t*  np;

  VERIFY0(pthread_mutex_lock(&tbl->atbl_lock));
  STAILQ_FOREACH_SAFE(asevt, &tbl->atbl_evlist, asevt_link, np)
  {
    if (asevt->asevt_evtp == ev)
    {
      STAILQ_REMOVE(&tbl->atbl_evlist, asevt, asio_sigevt, asevt_link);
      break;
    }
  }
  VERIFY0(pthread_mutex_unlock(&tbl->atbl_lock));
}

static void asio_signal_remove_backend(const asio_backend_t *b)
{
  asio_sigtbl_t*  tbl;
  asio_sigevt_t*  asevt;
  asio_sigevt_t*  np;
  size_t          i;
  int             port = b->ab_port;

  for(i = 0, tbl = asio_sigtbl; i < NSIG; i++, tbl++)
  {
    VERIFY0(pthread_mutex_lock(&tbl->atbl_lock));
    STAILQ_FOREACH_SAFE(asevt, &tbl->atbl_evlist, asevt_link, np)
    {
      if (asevt->asevt_port == port)
      {
        STAILQ_REMOVE(&tbl->atbl_evlist, asevt, asio_sigevt, asevt_link);
      }
    }
    VERIFY0(pthread_mutex_unlock(&tbl->atbl_lock));
  }
}

asio_backend_t* ponyint_asio_backend_init()
{
  VERIFY0(pthread_once(&asio_signal_once, asio_signal_init));

  asio_backend_t* b = POOL_ALLOC(asio_backend_t);

  memset(b, 0, sizeof(asio_backend_t));
  ponyint_messageq_init(&b->ab_q);

  b->ab_port = port_create();

  if(b->ab_port == -1)
  {
    POOL_FREE(asio_backend_t, b);
    return NULL;
  }

  return b;
}

void ponyint_asio_backend_final(asio_backend_t* b)
{
  port_send(b->ab_port, EV_TERMINATE, NULL);
}

static void handle_queue(asio_backend_t* b)
{
  asio_msg_t* msg;

  while((msg = (asio_msg_t*)ponyint_thread_messageq_pop(
    &b->ab_q
#ifdef USE_DYNAMIC_TRACE
    , SPECIAL_THREADID_EVPORT
#endif
    )) != NULL)
  {
     pony_asio_event_send(msg->event, ASIO_DISPOSABLE, 0);
  }
}

PONY_API void pony_asio_event_resubscribe_read(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if ((ev->flags & ASIO_READ) && !ev->readable)
  {
      /* XXX: return value? */
      port_associate(b->ab_port, PORT_SOURCE_FD, ev->fd, POLLIN, ev);
  }
}

PONY_API void pony_asio_event_resubscribe_write(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED)) {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if ((ev->flags & ASIO_READ) && !ev->readable)
  {
      /* XXX: return value? */
      port_associate(b->ab_port, PORT_SOURCE_FD, ev->fd, POLLOUT, ev);
  }
}

DECLARE_THREAD_FN(ponyint_asio_backend_dispatch)
{
  ponyint_cpu_affinity(ponyint_asio_get_cpu());
  pony_register_thread();

  asio_backend_t* b = arg;
  pony_assert(b != NULL);

  port_event_t evlist[MAX_EVENTS];
  uint_t n;
  bool quit = false;

  while (!quit) {
    n = 1;
    if (port_getn(b->ab_port, evlist, MAX_EVENTS, &n, NULL) == -1)
    {
       return NULL;
    }

    for (uint_t i = 0; i < n; i++)
    {
      port_event_t* pev = &evlist[i];
      asio_event_t* ev = pev->portev_user;
      uint32_t      flags = 0;

      switch(pev->portev_source)
      {
        case PORT_SOURCE_FD:
          if (pev->portev_events & POLLIN)
          {
            ev->readable = true;
            flags |= ASIO_READ;
          }
          if (pev->portev_events & POLLOUT)
          {
            ev->writeable = true;
            flags |= ASIO_WRITE;
          }
          if (flags)
          {
            pony_asio_event_send(ev, flags, 0);
          }
          break;
        case PORT_SOURCE_TIMER:
          if (ev->flags & ASIO_TIMER)
          {
            pony_asio_event_send(ev, ASIO_TIMER, 0);
          }
          break;
        case PORT_SOURCE_USER:
          switch (pev->portev_events)
          {
            case EV_TERMINATE:
              asio_signal_remove_backend(b);
              close(b->ab_port);
              b->ab_port = -1;
              quit = true;
              break;
            case EV_SIGNAL:
              if (ev->flags & ASIO_SIGNAL)
              {
                pony_asio_event_send(ev, ASIO_SIGNAL, (uint32_t)ev->nsec);
              }
              break;
          }
          break;
      }

      handle_queue(b);
    }
  }

  ponyint_messageq_destroy(&b->ab_q);
  POOL_FREE(asio_backend_t, b);
  pony_unregister_thread();
  return NULL;
}

PONY_API void pony_asio_event_subscribe(asio_event_t* ev)
{
  if ((ev == NULL) ||
     (ev->flags == ASIO_DISPOSABLE) ||
     (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  if(ev->noisy)
  {
    uint64_t old_count = ponyint_asio_noisy_add();
    // tell scheduler threads that asio has at least one noisy actor
    // if the old_count was 0
    if (old_count == 0)
      ponyint_sched_noisy_asio(SPECIAL_THREADID_EVPORT);
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if (ev->flags & (ASIO_READ|ASIO_WRITE))
  {
    int events = 0;

    if (ev->flags & ASIO_READ)
      events |= POLLIN;
    if (ev->flags & ASIO_WRITE)
      events |= POLLOUT;

    port_associate(b->ab_port, PORT_SOURCE_FD, ev->fd, events, ev);
  }

  if (ev->flags & ASIO_TIMER)
  {
    /* TODO */
  }

  if (ev->flags & ASIO_SIGNAL)
  {
    asio_signal_add(b, ev, (int)ev->nsec);
  }
}

PONY_API void pony_asio_event_setnsec(asio_event_t* ev, uint64_t nsec __attribute__((unused)))
{
  if((ev == NULL) ||
    (ev->magic != ev) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b __attribute__((unused)) = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  /* TODO */
}

PONY_API void pony_asio_event_unsubscribe(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->magic != ev) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
  {
    pony_assert(0);
    return;
  }

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if(ev->noisy)
  {
    uint64_t old_count = ponyint_asio_noisy_remove();
    // tell scheduler threads that asio has no noisy actors
    // if the old_count was 1
    if (old_count == 1)
    {
      ponyint_sched_unnoisy_asio(SPECIAL_THREADID_EVPORT);

      // maybe wake up a scheduler thread if they've all fallen asleep
      ponyint_sched_maybe_wakeup_if_all_asleep(-1);
    }

    ev->noisy = false;
  }

  // XXX: This removes all events -- if it's possible that we just want to
  // remove either the POLLIN or POLLOUT, this needs to be smarter and possibly
  // reassociate with a modified set
  if(ev->flags & (ASIO_READ|ASIO_WRITE))
  {
    port_dissociate(b->ab_port, PORT_SOURCE_FD, ev->fd);
  }

  if(ev->flags & ASIO_TIMER)
  {
    /* TODO */
  }

  if (ev->flags & ASIO_SIGNAL)
  {
     asio_signal_remove(ev, (int)ev->nsec);
  }

  ev->flags = ASIO_DISPOSABLE;

  asio_msg_t* msg = (asio_msg_t*)pony_alloc_msg(
    POOL_INDEX(sizeof(asio_msg_t)), 0);
  msg->event = ev;
  msg->flags = ASIO_DISPOSABLE;
  ponyint_thread_messageq_push(&b->ab_q, (pony_msg_t*)msg, (pony_msg_t*)msg
#ifdef USE_DYNAMIC_TRACE
    , SPECIAL_THREADID_EVPORT, SPECIAL_THREADID_EVPORT
#endif
    );
}

#endif
