#include "asio.h"
#include "event.h"
#ifdef ASIO_USE_EVPORT

#include "../actor/messageq.h"
#include "../mem/pool.h"
#include "../sched/cpu.h"
#include "../sched/scheduler.h"
#include "ponyassert.h"
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <poll.h>
#include <port.h>

#define EV_TERMINATE  1

struct asio_backend_t
{
  int port;
  messageq_t q;
};

asio_backend_t *ponyint_asio_backend_init()
{
  asio_backend_t* b = POOL_ALLOC(asio_backend_t);
  memset(b, 0, sizeof(asio_backend_t));
  ponyint_messageq_init(&b->q);

  b->port = port_create();

  if(b->port == -1)
  {
    POOL_FREE(asio_backend_t, b);
    return NULL;
  }

  return b;
}

void ponyint_asio_backend_final(asio_backend_t* b __attribute__((unused)))
{
}

static void handle_queue(asio_backend_t* b)
{
  asio_msg_t* msg;

  while((msg = (asio_msg_t*)ponyint_thread_messageq_pop(
    &b->q
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
    return;

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if ((ev->flags & ASIO_READ) && !ev->readable)
  {
      /* XXX: return value? */
      port_associate(b->port, PORT_SOURCE_FD, ev->fd, POLLIN, ev);
  }
}

PONY_API void pony_asio_event_resubscribe_write(asio_event_t* ev)
{
  if((ev == NULL) ||
    (ev->flags == ASIO_DISPOSABLE) ||
    (ev->flags == ASIO_DESTROYED))
    return;

  asio_backend_t* b = ponyint_asio_get_backend();
  pony_assert(b != NULL);

  if ((ev->flags & ASIO_READ) && !ev->readable)
  {
      /* XXX: return value? */
      port_associate(b->port, PORT_SOURCE_FD, ev->fd, POLLOUT, ev);
  }
}

DECLARE_THREAD_FN(ponyint_asio_backend_dispatch)
{
  ponyint_cpu_affinity(ponyint_asio_get_cpu());
  pony_register_thread();
  asio_backend_t* b = arg;
  pony_assert(b != NULL);

#if !defined(USE_SCHEDULER_SCALING_PTHREADS)
  // Make sure we block signals related to scheduler sleeping/waking
  // so they queue up to avoid race conditions
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, PONY_SCHED_SLEEP_WAKE_SIGNAL);
  pthread_sigmask(SIG_BLOCK, &set, NULL);
#endif

  port_event_t evlist[MAX_EVENTS];
  uint_t n;

  if (port_getn(b->port, evlist, MAX_EVENTS, &n, NULL) == -1)
     return NULL;

  for (uint_t i = 0; i < n; i++)
  {
    port_event_t *pev = &evlist[i];
    asio_event_t *ev = pev->portev_user;
    uint32_t flags = 0;

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
            close(b->port);
            b->port = -1;
            break;
        }
        break;
    }

    handle_queue(b);

  }

  ponyint_messageq_destroy(&b->q);
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

  if (ev->flags & (ASIO_READ|ASIO_WRITE))
  {
    asio_backend_t* b = ponyint_asio_get_backend();
    pony_assert(b != NULL);

    int events = 0;

    if (ev->flags & ASIO_READ)
      events |= POLLIN;
    if (ev->flags & ASIO_WRITE)
      events |= POLLOUT;

    port_associate(b->port, PORT_SOURCE_FD, ev->fd, events, ev);
  }

  /* XXX: TODO */
}


#endif
