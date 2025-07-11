/**************************************************************************
 * ringserver.c
 *
 * A streaming data server with support for SeedLink, DataLink and HTTP
 * protocols.
 *
 * This file is part of the ringserver.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Copyright (C) 2024:
 * @author Chad Trabant, EarthScope Data Services
 **************************************************************************/

/* _GNU_SOURCE needed to get strcasestr() under Linux */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <sys/un.h>

#include <libmseed.h>

#include "clients.h"
#include "dlclient.h"
#include "slclient.h"
#include "dsarchive.h"
#include "generic.h"
#include "logging.h"
#include "mseedscan.h"
#include "ring.h"
#include "ringserver.h"
#include "config.h"
#include "loadbuffer.h"

/* Reserve connection count, allows connections from addresses with write
 * permission even when the maximum connection count has been reached. */
#define RESERVECONNECTIONS 10

#define GIBIBYTE (1024ULL * 1024ULL * 1024ULL)

/* Global parameter declaration and defaults */
struct param_s param = {
    .serverstarttime     = NSTUNSET,
    .clientcount         = 0,
    .shutdownsig         = 0,
    .configfilemtime     = 0,
    .sthreads_lock       = PTHREAD_MUTEX_INITIALIZER,
    .sthreads            = NULL,
    .cthreads_lock       = PTHREAD_MUTEX_INITIALIZER,
    .cthreads            = NULL,
};

/* Configuration parameter declaration and defaults */
struct config_s config = {
    .configfile          = NULL,
    .serverid            = NULL,
    .ringdir             = NULL,
    .ringsize            = GIBIBYTE,
    .pktsize             = sizeof (RingPacket) + 512,
    .maxclients          = 600,
    .maxclientsperip     = 0,
    .clienttimeout       = 3600,
    .timewinlimit        = 1.0,
    .resolvehosts        = 1,
    .memorymapring       = 1,
    .volatilering        = 0,
    .autorecovery        = 1,
    .webroot             = NULL,
    .httpheaders         = NULL,
    .mseedarchive        = NULL,
    .mseedidleto         = 300,
    .limitips            = NULL,
    .matchips            = NULL,
    .rejectips           = NULL,
    .writeips            = NULL,
    .trustedips          = NULL,
    .tlscertfile         = NULL,
    .tlskeyfile          = NULL,
    .tlsverifyclientcert = 0,
};

/* Local functions and variables */
static void LogServerParameters ();
static struct thread_data *InitThreadData (void *prvtptr);
static void *ListenThread (void *arg);
static int CalcStats (ClientInfo *cinfo);
static IPNet *MatchIP (IPNet *list, struct sockaddr *addr);
static int ClientIPCount (struct sockaddr *addr);
static void *SignalThread (void *arg);
static void PrintHandler ();

static sigset_t globalsigset;
static int tcpprotonumber = -1;

static RingParams *ringparams = NULL;

int
main (int argc, char *argv[])
{
  char ringfilename[PATH_MAX]      = {0};
  char streamfilename[PATH_MAX]    = {0};
  char ringfile_backup[PATH_MAX]   = {0};
  char streamfile_backup[PATH_MAX] = {0};
  nstime_t hpcurtime;
  time_t curtime;
  time_t chktime;
  struct timespec timereq;
  pthread_t stid;
  pthread_t sigtid;
  struct sthread *stp;
  struct sthread *loopstp;
  struct cthread *ctp;
  struct cthread *loopctp;
  int tlogwrite   = 0;
  int servercount = 0;

  double txpacketrate;
  double txbyterate;
  double rxpacketrate;
  double rxbyterate;

  struct stat cfstat;
  int configreset = 0;
  int ringinit;

  uint16_t convert_version = 0;

  struct protoent *pe_tcp;

  /* Ring descriptor */
  int ringfd = -1;

  /* Process command line parameters */
  if (ProcessParam (argc, argv) < 0)
    return 1;

  /* Redirect libmseed logging facility to lprintf() via the lprint() shim */
  ms_loginit (lprint_wrapper, NULL, lprint_wrapper, NULL);

  /* Signal handling using POSIX routines, create set of all signals */
  if (sigfillset (&globalsigset))
  {
    lprintf (0, "Error: sigfillset() failed, cannot initialize signal set");
    return 1;
  }

  /* Block signals in set, mask is inherited by all child threads */
  if (pthread_sigmask (SIG_BLOCK, &globalsigset, NULL))
  {
    lprintf (0, "Error: pthread_sigmask() failed, cannot set thread signal mask");
    return 1;
  }

  /* Start signal handling thread */
  lprintf (2, "Starting signal handling thread");

  if ((errno = pthread_create (&sigtid, NULL, SignalThread, NULL)))
  {
    lprintf (0, "Error creating signal handling thread: %s", strerror (errno));
    return 1;
  }

  /* Look up & store the system TCP protocol entry */
  if (!(pe_tcp = getprotobyname ("tcp")))
  {
    lprintf (0, "Error: cannot determine the system protocol number for TCP");
    return 1;
  }
  else
  {
    tcpprotonumber = pe_tcp->p_proto;
  }

  /* Init ring parameters */
  if (config.ringdir || config.volatilering)
  {
    if (!config.volatilering)
    {
      /* Create ring file path: "<ringdir>/packetbuf" */
      strncpy (ringfilename, config.ringdir, sizeof (ringfilename) - 20);
      strcat (ringfilename, "/packetbuf");

      /* Create stream index file path: "<ringdir>/streamidx" */
      strncpy (streamfilename, config.ringdir, sizeof (streamfilename) - 20);
      strcat (streamfilename, "/streamidx");
    }

    /* Initialize ring system */
    if ((ringinit = RingInitialize (ringfilename, streamfilename,
                                    config.ringsize, config.pktsize,
                                    config.memorymapring, config.volatilering,
                                    &ringfd, &ringparams)))
    {
      /* Exit on unrecoverable errors or if no auto recovery */
      if (ringinit == -2 || !config.autorecovery)
      {
        lprintf (0, "Error initializing ring buffer (%d)", ringinit);
        return 1;
      }

      if (ringfd > 0)
      {
        if (close (ringfd))
        {
          lprintf (0, "Error closing ring buffer file: %s", strerror (errno));
        }
      }

      /* Move corrupt packet buffer and index to backup (.corrupt) files */
      if (config.autorecovery == 1 && (ringinit == -1 || ringinit > 0))
      {
        if (ringinit == -1)
        {
          lprintf (0, "Auto recovery, moving packet buffer and stream index files to .corrupt");

          snprintf (ringfile_backup, sizeof (ringfile_backup), "%s.corrupt", ringfilename);
          snprintf (streamfile_backup, sizeof (streamfile_backup), "%s.corrupt", streamfilename);
        }
        else
        {
          lprintf (0, "Auto recovery, moving packet buffer and stream index files to .version%d", ringinit);

          snprintf (ringfile_backup, sizeof (ringfile_backup), "%s.version%d", ringfilename, ringinit);
          snprintf (streamfile_backup, sizeof (streamfile_backup), "%s.version%d", streamfilename, ringinit);
          convert_version = ringinit;
        }

        /* Rename original ring and stream files to the backup names */
        if (rename (ringfilename, ringfile_backup) && errno != ENOENT)
        {
          lprintf (0, "Error renaming %s to %s: %s", ringfilename, ringfile_backup,
                   strerror (errno));
          return 1;
        }
        if (rename (streamfilename, streamfile_backup) && errno != ENOENT)
        {
          lprintf (0, "Error renaming %s to %s: %s", streamfilename, streamfile_backup,
                   strerror (errno));
          return 1;
        }
      }
      /* Removing existing packet buffer and index */
      else if (config.autorecovery == 2)
      {
        lprintf (0, "Auto recovery, removing exising packet buffer and stream index files");

        /* Delete existing ring and stream files */
        if (unlink (ringfilename) && errno != ENOENT)
        {
          lprintf (0, "Error removing %s: %s", ringfilename, strerror (errno));
          return 1;
        }
        if (unlink (streamfilename) && errno != ENOENT)
        {
          lprintf (0, "Error renaming %s: %s", streamfilename, strerror (errno));
          return 1;
        }
      }
      else
      {
        lprintf (0, "Unrecognized combination of auto recovery: %u, and ringinit return %d",
                 config.autorecovery, ringinit);
        return 1;
      }

      /* Re-initialize ring system */
      if ((ringinit = RingInitialize (ringfilename, streamfilename,
                                      config.ringsize, config.pktsize,
                                      config.memorymapring, config.volatilering,
                                      &ringfd, &ringparams)))
      {
        lprintf (0, "Error re-initializing ring buffer on auto-recovery (%d)", ringinit);
        return 1;
      }

      if (config.autorecovery == 1 && convert_version > 0)
      {
        int64_t loaded_packets = 0;

        if (convert_version == 1)
        {
          loaded_packets = LoadBufferV1 (ringfile_backup, ringparams);
        }
        else
        {
          lprintf (0, "Error: unsupported conversion version %d", convert_version);
          return 1;
        }

        if (loaded_packets >= 0)
        {
          lprintf (0, "Loaded %" PRId64 " packets, removing backup files", loaded_packets);

          /* Remove the backup files that have been converted */
          if (unlink (ringfile_backup) && errno != ENOENT)
          {
            lprintf (0, "Error removing %s: %s", ringfile_backup, strerror (errno));
            return 1;
          }
          if (unlink (streamfile_backup) && errno != ENOENT)
          {
            lprintf (0, "Error renaming %s: %s", streamfile_backup, strerror (errno));
            return 1;
          }
        }
        else
        {
          lprintf (0, "Error loading packets from backup file: %s", ringfile_backup);
        }
      }
    }
  }
  else
  {
    lprintf (0, "Error: ring directory is not set and ring is not volatile");
    return 1;
  }

  /* Set server start time */
  param.serverstarttime = NSnow ();

  /* Initialize watchdog loop interval timers */
  curtime = time (NULL);
  chktime = curtime;

  /* Initialize transfer log window timers */
  if (TLogParams.tlogbasedir)
  {
    TLogParams.tlogstart = curtime;

    if (CalcIntWin (curtime, TLogParams.tloginterval,
                    &TLogParams.tlogstartint, &TLogParams.tlogendint))
    {
      lprintf (0, "Error calculating interval time window");
      return 1;
    }
  }

  LogRingParameters (ringparams);
  LogServerParameters ();

  /* Set loop interval check tick to 1/4 second */
  timereq.tv_sec  = 0;
  timereq.tv_nsec = 250000000;

  /* Watchdog loop: monitors the server and client threads
     performing restarts and cleanup when necessary. */
  for (;;)
  {
    hpcurtime = NSnow ();

    /* If shutdown is requested signal all client threads */
    if (param.shutdownsig == 1)
    {
      param.shutdownsig = 2;

      /* Set shutdown loop throttle of .1 seconds */
      timereq.tv_nsec = 100000000;

      /* Request shutdown of server threads */
      pthread_mutex_lock (&param.sthreads_lock);
      loopstp = param.sthreads;
      while (loopstp)
      {
        /* Close listening server sockets, causing the listen thread to exit too */
        if (loopstp->type == LISTEN_THREAD)
        {
          ListenPortParams *lpp = loopstp->params;

          if (lpp->socket > 0)
          {
            lprintf (3, "Closing port %s server socket", lpp->portstr);
            shutdown (lpp->socket, SHUT_RDWR);
            close (lpp->socket);
            lpp->socket = -1;
          }
        }
        /* Otherwise set thread flag to CLOSE */
        else if (loopstp->td && (loopstp->td->td_state != TDS_CLOSING) && loopstp->td->td_state != TDS_CLOSED)
        {
          lprintf (3, "Requesting shutdown of server thread %lu",
                   (unsigned long int)loopstp->td->td_id);

          pthread_mutex_lock (&(loopstp->td->td_lock));
          loopstp->td->td_state = TDS_CLOSE;
          pthread_mutex_unlock (&(loopstp->td->td_lock));
        }

        loopstp = loopstp->next;
      }
      pthread_mutex_unlock (&param.sthreads_lock);

      /* Request shutdown of client threads */
      pthread_mutex_lock (&param.cthreads_lock);
      loopctp = param.cthreads;
      while (loopctp)
      {
        if (loopctp->td->td_state != TDS_CLOSING && loopctp->td->td_state != TDS_CLOSED)
        {
          lprintf (3, "Requesting shutdown of client thread %lu",
                   (unsigned long int)loopctp->td->td_id);

          pthread_mutex_lock (&(loopctp->td->td_lock));
          loopctp->td->td_state = TDS_CLOSE;
          pthread_mutex_unlock (&(loopctp->td->td_lock));
        }
        loopctp = loopctp->next;
      }
      pthread_mutex_unlock (&param.cthreads_lock);
    } /* Done initializing shutdown sequence */

    if (param.shutdownsig > 1)
    {
      /* Safety valve for deadlock, should never get here */
      if (param.shutdownsig >= 100)
      {
        lprintf (0, "Shutdown did not complete cleanly after ~10 seconds");
        break;
      }

      param.shutdownsig++;
    }

    /* Transmission log writing time window check */
    if (TLogParams.tlogbasedir && !param.shutdownsig)
    {
      if (curtime >= TLogParams.tlogendint)
        tlogwrite = 1;
      else
        tlogwrite = 0;
    }

    /* Loop through server thread list to monitor threads, print status and perform cleanup */
    pthread_mutex_lock (&param.sthreads_lock);
    loopstp     = param.sthreads;
    servercount = 0;
    while (loopstp)
    {
      char *threadtype             = "";
      void *(*threadfunc) (void *) = NULL;

      stp     = loopstp;
      loopstp = loopstp->next;

      if (stp->type == LISTEN_THREAD)
      {
        threadtype = "Listen";
        threadfunc = &ListenThread;
      }
      else if (stp->type == MSEEDSCAN_THREAD)
      {
        threadtype = "MSeedScan";
        threadfunc = &MS_ScanThread;
      }

      /* Report status of server thread */
      if (stp->td)
      {
        char *state;
        if (stp->td->td_state == TDS_SPAWNING)
          state = "SPAWNING";
        else if (stp->td->td_state == TDS_ACTIVE)
          state = "ACTIVE";
        else if (stp->td->td_state == TDS_CLOSE)
          state = "CLOSE";
        else if (stp->td->td_state == TDS_CLOSING)
          state = "CLOSING";
        else if (stp->td->td_state == TDS_CLOSED)
          state = "CLOSED";
        else
          state = "UNKNOWN";

        lprintf (3, "Server thread (%s) %lu state: %s",
                 threadtype, (unsigned long int)stp->td->td_id, state);

        servercount++;
      }
      else
      {
        lprintf (2, "Server thread (%s) not running", threadtype);
      }

      if (stp->type == LISTEN_THREAD)
      {
        ListenPortParams *lpp = stp->params;

        /* Cleanup CLOSED listen thread */
        if (stp->td && stp->td->td_state == TDS_CLOSED)
        {
          lprintf (1, "Joining CLOSED %s thread", threadtype);

          if ((errno = pthread_join (stp->td->td_id, NULL)))
          {
            lprintf (0, "Error joining CLOSED %s thread %lu: %s", threadtype,
                     (unsigned long int)stp->td->td_id, strerror (errno));
          }

          free (stp->td);
          stp->td = NULL;
        }

        /* Start new listening thread if needed */
        if (stp->td == NULL && !param.shutdownsig)
        {
          /* Initialize thread data and create thread */
          if (!(stp->td = InitThreadData (lpp)))
          {
            lprintf (0, "Error initializing %s thread_data: %s", threadtype, strerror (errno));
          }
          else
          {
            lprintf (2, "Starting %s listen thread for port %s", threadtype, lpp->portstr);

            if ((errno = pthread_create (&stid, NULL, threadfunc, (void *)stp->td)))
            {
              lprintf (0, "Error creating %s thread: %s", threadtype, strerror (errno));
              if (stp->td)
                free (stp->td);
              stp->td = NULL;
            }
            else
            {
              stp->td->td_id = stid;
            }
          }
        }
      } /* Done with LISTEN_THREAD handling */
      else if (stp->type == MSEEDSCAN_THREAD)
      {
        MSScanInfo *mssinfo = stp->params;

        /* Cleanup CLOSED scanning thread */
        if (stp->td && stp->td->td_state == TDS_CLOSED)
        {
          lprintf (1, "Joining CLOSED %s thread", threadtype);

          if ((errno = pthread_join (stp->td->td_id, NULL)))
          {
            lprintf (0, "Error joining CLOSED %s thread %lu: %s", threadtype,
                     (unsigned long int)stp->td->td_id, strerror (errno));
          }

          free (stp->td);
          stp->td = NULL;
        }

        /* Start new thread if needed */
        if (stp->td == NULL && !param.shutdownsig)
        {
          mssinfo->ringparams = ringparams;

          /* Initialize thread data and create thread */
          if (!(stp->td = InitThreadData (mssinfo)))
          {
            lprintf (0, "Error initializing %s thread_data: %s", threadtype, strerror (errno));
          }
          else
          {
            lprintf (2, "Starting %s thread [%s]", threadtype, mssinfo->dirname);

            if ((errno = pthread_create (&stid, NULL, threadfunc, (void *)stp->td)))
            {
              lprintf (0, "Error creating %s thread: %s", threadtype, strerror (errno));
              if (stp->td)
                free (stp->td);
              stp->td = NULL;
            }
            else
            {
              stp->td->td_id = stid;
            }
          }
        }
      } /* Done with MSEEDSCAN_THREAD handling */
      else
      {
        lprintf (0, "Error, unrecognized server thread type: %d", stp->type);
      }

    } /* Done looping through server threads */
    pthread_mutex_unlock (&param.sthreads_lock);

    /* Reset total count and byte rates */
    txpacketrate = txbyterate = rxpacketrate = rxbyterate = 0.0;

    /* Loop through client thread list printing status and doing cleanup */
    pthread_mutex_lock (&param.cthreads_lock);
    loopctp = param.cthreads;
    while (loopctp)
    {
      ctp     = loopctp;
      loopctp = loopctp->next;

      char *state;
      if (ctp->td->td_state == TDS_SPAWNING)
        state = "SPAWNING";
      else if (ctp->td->td_state == TDS_ACTIVE)
        state = "ACTIVE";
      else if (ctp->td->td_state == TDS_CLOSE)
        state = "CLOSE";
      else if (ctp->td->td_state == TDS_CLOSING)
        state = "CLOSING";
      else if (ctp->td->td_state == TDS_CLOSED)
        state = "CLOSED";
      else
        state = "UNKNOWN";

      lprintf (3, "Client thread %lu state: %s",
               (unsigned long int)ctp->td->td_id, state);

      /* Free associated resources and join CLOSED client threads */
      if (ctp->td->td_state == TDS_CLOSED)
      {
        lprintf (3, "Removing client thread %lu from the cthreads list",
                 (unsigned long int)ctp->td->td_id);

        /* Unlink from the cthreads list */
        if (!ctp->prev && !ctp->next)
          param.cthreads = NULL;
        if (!ctp->prev && ctp->next)
          param.cthreads = ctp->next;
        if (ctp->prev)
          ctp->prev->next = ctp->next;
        if (ctp->next)
          ctp->next->prev = ctp->prev;

        if ((errno = pthread_join (ctp->td->td_id, NULL)))
        {
          lprintf (0, "Error joining CLOSED thread %lu: %s",
                   (unsigned long int)ctp->td->td_id, strerror (errno));
        }

        /* Free the ClientInfo structure stored at the prvtptr */
        if (ctp->td->td_prvtptr)
          free (ctp->td->td_prvtptr);

        /* Free thread data structure */
        if (ctp->td)
          free (ctp->td);

        /* Decrement client count */
        if (param.clientcount > 0)
          param.clientcount--;

        /* Free thread data */
        free (ctp);
      }
      else
      {
        /* Update transmission and reception rates */
        CalcStats ((ClientInfo *)ctp->td->td_prvtptr);

        /* Update packet and byte count totals */
        txpacketrate += ((ClientInfo *)ctp->td->td_prvtptr)->txpacketrate;
        txbyterate += ((ClientInfo *)ctp->td->td_prvtptr)->txbyterate;
        rxpacketrate += ((ClientInfo *)ctp->td->td_prvtptr)->rxpacketrate;
        rxbyterate += ((ClientInfo *)ctp->td->td_prvtptr)->rxbyterate;

        /* Write transfer logs and reset byte counts */
        if (tlogwrite)
          WriteTLog ((ClientInfo *)ctp->td->td_prvtptr, 1);

        /* Close idle clients if limit is set and exceeded */
        if (config.clienttimeout &&
            (hpcurtime - ((ClientInfo *)ctp->td->td_prvtptr)->lastxchange) > (config.clienttimeout * (nstime_t)NSTMODULUS))
        {
          pthread_mutex_lock (&(ctp->td->td_lock));
          if (ctp->td->td_state != TDS_CLOSE &&
              ctp->td->td_state != TDS_CLOSING &&
              ctp->td->td_state != TDS_CLOSED)
          {
            lprintf (1, "Closing idle client connection: %s", ((ClientInfo *)ctp->td->td_prvtptr)->hostname);
            ctp->td->td_state = TDS_CLOSE;
          }
          pthread_mutex_unlock (&(ctp->td->td_lock));
        }
      }
    } /* Done looping through client threads */
    pthread_mutex_unlock (&param.cthreads_lock);

    lprintf (3, "Client connections: %d", param.clientcount);

    /* Update count and byte rate ring parameters */
    ringparams->txpacketrate = txpacketrate;
    ringparams->txbyterate   = txbyterate;
    ringparams->rxpacketrate = rxpacketrate;
    ringparams->rxbyterate   = rxbyterate;

    /* Check for config file updates */
    if (config.configfile && !lstat (config.configfile, &cfstat))
    {
      if (cfstat.st_mtime > param.configfilemtime)
      {
        lprintf (1, "Re-reading configuration parameters from %s", config.configfile);
        ReadConfigFile (config.configfile, 1, cfstat.st_mtime);
        configreset = 1;
      }
    }

    /* Reset transfer log writing time windows using the current time as the reference */
    if (TLogParams.tlogbasedir && !param.shutdownsig && (tlogwrite || configreset))
    {
      tlogwrite = 0;

      if (CalcIntWin (time (NULL), TLogParams.tloginterval,
                      &TLogParams.tlogstartint, &TLogParams.tlogendint))
      {
        lprintf (0, "Error calculating interval time window");
        return 1;
      }
    }

    /* All done if shutting down and no threads left */
    if (param.shutdownsig >= 2 && param.clientcount == 0 && servercount == 0)
      break;

    /* Throttle the loop during shutdown */
    if (param.shutdownsig)
    {
      nanosleep (&timereq, NULL);
    }
    /* Otherwise, throttle the loop for a second, signals will interrupt */
    else
    {
      while (((curtime = time (NULL)) - chktime) < 1 && !param.shutdownsig)
        nanosleep (&timereq, NULL);
    }

    configreset = 0;
    chktime     = curtime;
  } /* End of main watchdog loop */

  /* Shutdown ring buffer */
  if (config.ringdir || config.volatilering)
  {
    if (RingShutdown (ringfd, streamfilename, ringparams))
    {
      lprintf (0, "Error shutting down ring buffer");
      return 1;
    }
  }

  /* Cancel and re-joing the signal handling thread */
  if ((errno = pthread_cancel (sigtid)))
  {
    lprintf (0, "Error cancelling signal handling thread: %s", strerror (errno));
    return 1;
  }

  if ((errno = pthread_join (sigtid, NULL)))
  {
    lprintf (0, "Error joining signal handling thread: %s", strerror (errno));
    return 1;
  }

  return 0;
} /* End of main() */

/***********************************************************************
 * InitThreadData:
 *
 * Initialize thread data.
 *
 * Return pointer to thread_data on success and 0 on error.
 ***********************************************************************/
struct thread_data *
InitThreadData (void *prvtptr)
{
  struct thread_data *rtdp;

  rtdp = (struct thread_data *)malloc (sizeof (struct thread_data));

  if (!rtdp)
  {
    lprintf (0, "Error malloc'ing thread_data: %s", strerror (errno));
    return 0;
  }

  if (pthread_mutex_init (&(rtdp->td_lock), NULL))
  {
    lprintf (0, "Error initializing thread_data mutex: %s", strerror (errno));
    return 0;
  }

  rtdp->td_id    = 0;
  rtdp->td_state = TDS_SPAWNING;

  rtdp->td_prvtptr = prvtptr;

  return rtdp;
} /* End of InitThreadData() */

/***********************************************************************
 * ListenThread:
 *
 * Thread to accept connections and dispatch client threads.
 *
 * Return NULL.
 ***********************************************************************/
void *
ListenThread (void *arg)
{
  pthread_t ctid;
  int clientsocket;
  struct thread_data *mytdp;
  struct thread_data *tdp;
  struct cthread *ctp;
  ClientInfo *cinfo;
  ListenPortParams *lpp;

  char ipstr[100];
  char portstr[32];
  char protocolstr[100];

  struct sockaddr_storage addr_storage;
  struct sockaddr *paddr = (struct sockaddr *)&addr_storage;
  socklen_t addrlen      = sizeof (addr_storage);
  int one                = 1;

  mytdp = (struct thread_data *)arg;
  lpp   = (ListenPortParams *)mytdp->td_prvtptr;

  /* Set thread active status */
  pthread_mutex_lock (&(mytdp->td_lock));
  mytdp->td_state = TDS_ACTIVE;
  pthread_mutex_unlock (&(mytdp->td_lock));

  /* Generate string of protocols and options supported by this listener */
  if (GenProtocolString (lpp->protocols, lpp->options, protocolstr, sizeof (protocolstr)) > 0)
    lprintf (1, "Listening for connections on port %s (%s)",
             lpp->portstr, protocolstr);
  else
    lprintf (1, "Listening for connections on port %s (unknown protocols?)",
             lpp->portstr);

  /* Enter connection dispatch loop, spawning a new thread for each incoming connection */
  while (!param.shutdownsig)
  {
    if (lpp->options & FAMILY_UNIX) {
      struct sockaddr_un addr_un;
      socklen_t addrlen_un = sizeof(addr_un);
      clientsocket = accept(lpp->socket, (struct sockaddr*)&addr_un, &addrlen_un);
      if (clientsocket == -1) {
        if (errno == ECONNABORTED || errno == EINTR)
          continue;
        if (!param.shutdownsig)
          lprintf(0, "Could not accept incoming UNIX connection: %s", strerror(errno));
        break;
      }
      snprintf(ipstr, sizeof(ipstr), "unix");
      snprintf(portstr, sizeof(portstr), "%s", lpp->portstr);
    } else {
      clientsocket = accept(lpp->socket, paddr, &addrlen);
      if (clientsocket == -1) {
        if (errno == ECONNABORTED || errno == EINTR)
          continue;
        if (!param.shutdownsig)
          lprintf(0, "Could not accept incoming connection: %s", strerror(errno));
        break;
      }
      if (setsockopt(clientsocket, tcpprotonumber, TCP_NODELAY, (void *)&one, sizeof(one))) {
        lprintf(0, "Could not disable TCP delay algorithm: %s", strerror (errno));
      }
      if (getnameinfo(paddr, addrlen, ipstr, sizeof(ipstr), portstr, sizeof(portstr), NI_NUMERICHOST | NI_NUMERICSERV)) {
        lprintf(0, "Error creating IP and port strings");
        close(clientsocket);
        continue;
      }
    }

    lprintf (2, "Incoming connection on port %s from %s:%s", lpp->portstr, ipstr, portstr);

    /* Reject clients not in matching list */
    if (config.matchips)
    {
      if (!MatchIP (config.matchips, paddr))
      {
        lprintf (1, "Rejecting non-matching connection from: %s:%s", ipstr, portstr);
        close (clientsocket);
        continue;
      }
    }

    /* Reject clients in the rejection list */
    if (config.rejectips)
    {
      if (MatchIP (config.rejectips, paddr))
      {
        lprintf (1, "Rejecting connection from: %s:%s", ipstr, portstr);
        close (clientsocket);
        continue;
      }
    }

    /* Enforce per-address connection limit for non write permission addresses */
    if (config.maxclientsperip)
    {
      if (!(config.writeips && MatchIP (config.writeips, paddr)))
      {
        if (ClientIPCount (paddr) >= config.maxclientsperip)
        {
          lprintf (1, "Too many connections from: %s:%s", ipstr, portstr);
          close (clientsocket);
          continue;
        }
      }
    }

    /* Enforce maximum number of clients if specified */
    if (config.maxclients && param.clientcount >= config.maxclients)
    {
      if ((config.writeips && MatchIP (config.writeips, paddr)) &&
          param.clientcount <= (config.maxclients + RESERVECONNECTIONS))
      {
        lprintf (1, "Allowing connection in reserve space from %s:%s", ipstr, portstr);
      }
      else
      {
        lprintf (1, "Maximum number of clients exceeded: %u", config.maxclients);
        lprintf (1, "  Rejecting connection from: %s:%s", ipstr, portstr);
        close (clientsocket);
        continue;
      }
    }

    /* Allocate and initialize connection info struct */
    if ((cinfo = (ClientInfo *)calloc (1, sizeof (ClientInfo))) == NULL)
    {
      lprintf (0, "Error allocating memory for connection info");
      close (clientsocket);
      break;
    }

    cinfo->socket     = clientsocket;
    cinfo->protocols  = lpp->protocols;
    cinfo->tls        = (lpp->options & ENCRYPTION_TLS) ? 1 : 0;
    cinfo->type       = CLIENT_UNDETERMINED;
    cinfo->ringparams = ringparams;

    /* Store client socket address structure */
    if ((cinfo->addr = (struct sockaddr *)malloc (addrlen)) == NULL)
    {
      lprintf (0, "Error allocating memory for socket structure");
      close (clientsocket);
      break;
    }
    memcpy (cinfo->addr, &addr_storage, addrlen);
    cinfo->addrlen = addrlen;

    /* Store IP address and port number strings */
    snprintf (cinfo->ipstr, sizeof (cinfo->ipstr), "%s", ipstr);
    snprintf (cinfo->portstr, sizeof (cinfo->portstr), "%s", portstr);
    snprintf (cinfo->serverport, sizeof (cinfo->serverport), "%s", lpp->portstr);

    /* Set initial client ID string */
    strncpy (cinfo->clientid, "Client", sizeof (cinfo->clientid));

    /* Set stream limit if specified for address */
    if (config.limitips)
    {
      IPNet *ipnet;

      if ((ipnet = MatchIP (config.limitips, paddr)))
      {
        cinfo->limitstr = ipnet->limitstr;
      }
    }

    /* Grant write permission if address is in the write list */
    if (config.writeips)
    {
      if (MatchIP (config.writeips, paddr))
      {
        cinfo->writeperm = 1;
      }
    }

    /* Set trusted flag if address is in the trusted list */
    if (config.trustedips)
    {
      if (MatchIP (config.trustedips, paddr))
      {
        cinfo->trusted = 1;
      }
    }

    /* Set configured fixed HTTP headers */
    cinfo->httpheaders = config.httpheaders;

    /* Set time window search limit */
    cinfo->timewinlimit = config.timewinlimit;

    /* Set client connect time */
    cinfo->conntime = NSnow ();

    /* Set last data exchange time to the connect time */
    cinfo->lastxchange = cinfo->conntime;

    /* Initialize streams lock */
    pthread_mutex_init (&cinfo->streams_lock, NULL);

    /* Initialize the miniSEED write parameters */
    if (config.mseedarchive)
    {
      if (!(cinfo->mswrite = (DataStream *)malloc (sizeof (DataStream))))
      {
        lprintf (0, "Error allocating memory for miniSEED write parameters");
        if (clientsocket)
          close (clientsocket);
        break;
      }

      cinfo->mswrite->path          = config.mseedarchive;
      cinfo->mswrite->idletimeout   = config.mseedidleto;
      cinfo->mswrite->maxopenfiles  = 50;
      cinfo->mswrite->openfilecount = 0;
      cinfo->mswrite->grouproot     = NULL;
    }

    /* Create new client thread */
    if (!(tdp = InitThreadData (cinfo)))
    {
      lprintf (0, "Error initializing thread_data: %s", strerror (errno));
      if (clientsocket)
        close (clientsocket);
      break;
    }

    if ((errno = pthread_create (&ctid, NULL, ClientThread, (void *)tdp)))
    {
      lprintf (0, "Error creating new client thread: %s", strerror (errno));
      if (clientsocket)
        close (clientsocket);
      if (tdp)
        free (tdp);
      tdp = NULL;
      continue;
    }
    else
    {
      /* Update thread id, no locking, should be safe */
      tdp->td_id = ctid;

      ctp = (struct cthread *)malloc (sizeof (struct cthread));
      if (ctp == NULL)
      {
        lprintf (0, "Error malloc'ing cthread: %s", strerror (errno));
        if (clientsocket)
          close (clientsocket);
        if (tdp)
          free (tdp);
        break;
      }

      ctp->td   = tdp;
      ctp->prev = NULL;

      /* Add ctp to the beginning of the client threads list (cthreads) */
      pthread_mutex_lock (&param.cthreads_lock);
      if (param.cthreads)
      {
        ctp->next            = param.cthreads;
        param.cthreads->prev = ctp;
      }
      else
      {
        ctp->next = NULL;
      }
      param.cthreads = ctp;
      pthread_mutex_unlock (&param.cthreads_lock);

      /* Increment client count */
      param.clientcount++;
    }
  }

  /* Set thread closing status */
  pthread_mutex_lock (&(mytdp->td_lock));
  mytdp->td_state = TDS_CLOSED;
  pthread_mutex_unlock (&(mytdp->td_lock));

  lprintf (1, "Listening thread closing");
  if (lpp->options & FAMILY_UNIX) {
    unlink(lpp->portstr);
  }
  return NULL;
} /* End of ListenThread() */

/***************************************************************************
 * CalcStats:
 *
 * Calculate statisics for the specified client connection.  This
 * includes the following calculations:
 *
 * 1) Percent lag in the ring buffer, with the latest packet
 * representing 0% lag and the earliest packet representing 100% lag.
 *
 * 2) Transmission and reception rates in Hz (packet count and bytes).
 *
 * This routine assumes that the packet and byte counts will always
 * increase.
 *
 * Returns 0 on success and -1 on error.
 ***************************************************************************/
static int
CalcStats (ClientInfo *cinfo)
{
  nstime_t nsnow = NSnow ();
  int64_t latestoffset_unwrapped;
  int64_t readeroffset_unwrapped;
  double deltasec;

  if (!cinfo)
    return -1;

  /* Determine percent lag if the current pktid is set */
  if (cinfo->reader && cinfo->reader->pktid <= RINGID_MAXIMUM)
  {
    if (ringparams->latestoffset < ringparams->earliestoffset)
      latestoffset_unwrapped = ringparams->latestoffset + ringparams->maxoffset;
    else
      latestoffset_unwrapped = ringparams->latestoffset;

    if (cinfo->reader->pktoffset < ringparams->earliestoffset)
      readeroffset_unwrapped = cinfo->reader->pktoffset + ringparams->maxoffset;
    else
      readeroffset_unwrapped = cinfo->reader->pktoffset;

    /* Calculate percentage lag as position in ring where 0% = latest offset and 100% = earliest offset */
    cinfo->percentlag = (int)(((double)(latestoffset_unwrapped - readeroffset_unwrapped) / (latestoffset_unwrapped - ringparams->earliestoffset)) * 100);
  }
  else
  {
    cinfo->percentlag = 0;
  }

  /* Determine time difference since the previous history values were set in seconds */
  if (cinfo->ratetime == 0)
    deltasec = 1.0;
  else
    deltasec = (double)(nsnow - cinfo->ratetime) / NSTMODULUS;

  /* Transmission */
  if (cinfo->txpackets[0] > 0)
  {
    /* Calculate the transmission rates */
    cinfo->txpacketrate = (double)(cinfo->txpackets[0] - cinfo->txpackets[1]) / deltasec;
    cinfo->txbyterate   = (double)(cinfo->txbytes[0] - cinfo->txbytes[1]) / deltasec;

    /* Shift current values to history values */
    cinfo->txpackets[1] = cinfo->txpackets[0];
    cinfo->txbytes[1]   = cinfo->txbytes[0];
  }

  /* Reception */
  if (cinfo->rxpackets[0] > 0)
  {
    /* Calculate the reception rates */
    cinfo->rxpacketrate = (double)(cinfo->rxpackets[0] - cinfo->rxpackets[1]) / deltasec;
    cinfo->rxbyterate   = (double)(cinfo->rxbytes[0] - cinfo->rxbytes[1]) / deltasec;

    /* Shift current values to history values */
    cinfo->rxpackets[1] = cinfo->rxpackets[0];
    cinfo->rxbytes[1]   = cinfo->rxbytes[0];
  }

  /* Update time stamp of history values */
  cinfo->ratetime = nsnow;

  return 0;
} /* End of CalcStats() */

/***************************************************************************
 * MatchIP:
 *
 * Search the specified IPNet list for an entry that matches the given
 * IP address.
 *
 * Returns the matching IPNet entry if match found and NULL if no match found.
 ***************************************************************************/
static IPNet *
MatchIP (IPNet *list, struct sockaddr *addr)
{
  IPNet *net                = list;
  struct in_addr *testnet   = &((struct sockaddr_in *)addr)->sin_addr;
  struct in6_addr *testnet6 = &((struct sockaddr_in6 *)addr)->sin6_addr;

  if (!list)
    return 0;

  /* Sanity, only IPv4 and IPv6 addresses */
  if (addr->sa_family != AF_INET && addr->sa_family != AF_INET6)
    return 0;

  /* Search IPNet list for a matching entry for addr */
  while (net)
  {
    /* Check for match between test network and list network */
    if (addr->sa_family == AF_INET && net->family == AF_INET)
    {
      if ((testnet->s_addr & net->netmask.in_addr.s_addr) == net->network.in_addr.s_addr)
      {
        return net;
      }
    }
    else if (addr->sa_family == AF_INET6 && net->family == AF_INET6)
    {
      if ((testnet6->s6_addr[0] & net->netmask.in6_addr.s6_addr[0]) == net->network.in6_addr.s6_addr[0] &&
          (testnet6->s6_addr[1] & net->netmask.in6_addr.s6_addr[1]) == net->network.in6_addr.s6_addr[1] &&
          (testnet6->s6_addr[2] & net->netmask.in6_addr.s6_addr[2]) == net->network.in6_addr.s6_addr[2] &&
          (testnet6->s6_addr[3] & net->netmask.in6_addr.s6_addr[3]) == net->network.in6_addr.s6_addr[3] &&
          (testnet6->s6_addr[4] & net->netmask.in6_addr.s6_addr[4]) == net->network.in6_addr.s6_addr[4] &&
          (testnet6->s6_addr[5] & net->netmask.in6_addr.s6_addr[5]) == net->network.in6_addr.s6_addr[5] &&
          (testnet6->s6_addr[6] & net->netmask.in6_addr.s6_addr[6]) == net->network.in6_addr.s6_addr[6] &&
          (testnet6->s6_addr[7] & net->netmask.in6_addr.s6_addr[7]) == net->network.in6_addr.s6_addr[7] &&
          (testnet6->s6_addr[8] & net->netmask.in6_addr.s6_addr[8]) == net->network.in6_addr.s6_addr[8] &&
          (testnet6->s6_addr[9] & net->netmask.in6_addr.s6_addr[9]) == net->network.in6_addr.s6_addr[9] &&
          (testnet6->s6_addr[10] & net->netmask.in6_addr.s6_addr[10]) == net->network.in6_addr.s6_addr[10] &&
          (testnet6->s6_addr[11] & net->netmask.in6_addr.s6_addr[11]) == net->network.in6_addr.s6_addr[11] &&
          (testnet6->s6_addr[12] & net->netmask.in6_addr.s6_addr[12]) == net->network.in6_addr.s6_addr[12] &&
          (testnet6->s6_addr[13] & net->netmask.in6_addr.s6_addr[13]) == net->network.in6_addr.s6_addr[13] &&
          (testnet6->s6_addr[14] & net->netmask.in6_addr.s6_addr[14]) == net->network.in6_addr.s6_addr[14] &&
          (testnet6->s6_addr[15] & net->netmask.in6_addr.s6_addr[15]) == net->network.in6_addr.s6_addr[15])
      {
        return net;
      }
    }

    net = net->next;
  }

  return NULL;
} /* End of MatchIP() */

/***************************************************************************
 * ClientIPCount:
 *
 * Search the global client list and return a count of the connected
 * clients that match the specified address.
 *
 * Returns count of the client connections with a matching address.
 ***************************************************************************/
static int
ClientIPCount (struct sockaddr *addr)
{
  struct cthread *ctp;
  struct sockaddr_in *tsin[2];
  struct sockaddr_in6 *tsin6[2];
  int addrcount = 0;

  pthread_mutex_lock (&param.cthreads_lock);
  ctp = param.cthreads;
  while (ctp)
  {
    /* If the same IP protocol family */
    if (((ClientInfo *)ctp->td->td_prvtptr)->addr->sa_family == addr->sa_family)
    {
      /* Compare IPv4 addresses */
      if (addr->sa_family == AF_INET)
      {
        tsin[0] = (struct sockaddr_in *)((ClientInfo *)ctp->td->td_prvtptr)->addr;
        tsin[1] = (struct sockaddr_in *)addr;

        if (0 == memcmp (&tsin[0]->sin_addr.s_addr,
                         &tsin[1]->sin_addr.s_addr,
                         sizeof (tsin[0]->sin_addr.s_addr)))
        {
          addrcount++;
        }
      }
      /* Compare IPv6 addresses */
      else if (addr->sa_family == AF_INET6)
      {
        tsin6[0] = (struct sockaddr_in6 *)((ClientInfo *)ctp->td->td_prvtptr)->addr;
        tsin6[1] = (struct sockaddr_in6 *)addr;

        if (0 == memcmp (&tsin6[0]->sin6_addr.s6_addr,
                         &tsin6[1]->sin6_addr.s6_addr,
                         sizeof (tsin6[0]->sin6_addr.s6_addr)))
        {
          addrcount++;
        }
      }
    }

    ctp = ctp->next;
  }
  pthread_mutex_unlock (&param.cthreads_lock);

  return addrcount;
} /* End of ClientIPCount() */

/***************************************************************************
 * GenProtocolString:
 *
 * Generate a string containing the given protocols and options.
 *
 * Return length of string in result on success or 0 for error.
 ***************************************************************************/
int
GenProtocolString (ListenProtocols protocols, ListenOptions options,
                   char *result, size_t maxlength)
{
  int length;
  char *family;

  if (!result)
    return 0;

  if (options & FAMILY_IPv4)
    family = "IPv4";
  else if (options & FAMILY_IPv6)
    family = "IPv6";
  else if (options & FAMILY_UNIX)
    family = "UNIX";
  else
    family = "Unknown family?";

  length = snprintf (result, maxlength,
                     "%s: %s%s%s%s",
                     family,
                     (protocols & PROTO_DATALINK) ? "DataLink " : "",
                     (protocols & PROTO_SEEDLINK) ? "SeedLink " : "",
                     (protocols & PROTO_HTTP) ? "HTTP " : "",
                     (options & ENCRYPTION_TLS) ? "over TLS " : "");

  if (length < maxlength && result[length - 1] == ' ')
    result[length - 1] = '\0';

  return (length > maxlength) ? maxlength - 1 : length;
} /* End of GenProtocolString() */

/***********************************************************************
 * SignalThread:
 *
 * Thread to handle signals.
 *
 * Return NULL.
 ***********************************************************************/
void *
SignalThread (void *arg)
{
  int sig;
  int rc;

  /* Remove SIGPIPE from complete set, it will remain blocked */
  if (sigdelset (&globalsigset, SIGPIPE))
  {
    lprintf (0, "Error: sigdelset() failed, cannot remove SIGPIPE");
  }

  for (;;)
  {
    if ((rc = sigwait (&globalsigset, &sig)))
    {
      lprintf (0, "Error: sigwait() failed with %d", rc);
      continue;
    }

    switch (sig)
    {
    case SIGINT:
    case SIGTERM:
      lprintf (1, "Received termination signal");
      param.shutdownsig = 1; /* Set global termination flag */
      break;
    case SIGUSR1:
      PrintHandler (); /* Print global ring details */
      break;
    case SIGSEGV:
      lprintf (0, "Received segmentation fault signal, exiting");
      exit (1);
      break;
    default:
      lprintf (0, "Summarily ignoring %s (%d) signal", strsignal (sig), sig);
      break;
    }
  }

  return NULL;
} /* End of SignalThread() */

/***************************************************************************
 * LogServerParameters:
 *
 * Log high-level server parameters, not ring buffer specific.
 ***************************************************************************/
void LogServerParameters ()
{
  char network[INET6_ADDRSTRLEN];
  char netmask[INET6_ADDRSTRLEN];
  char timestring[32];

  lprintf (1, "Server parameters:");
  lprintf (1, "   server ID: %s", config.serverid);
  lprintf (1, "   ring directory: %s", (config.ringdir) ? config.ringdir : "NONE");
  lprintf (1, "   max clients: %u", config.maxclients);
  lprintf (1, "   max clients per IP: %u", config.maxclientsperip);

  lprintf (2, "   configuration file: %s", (config.configfile) ? config.configfile : "NONE");
  lprintf (2, "   client timeout: %u seconds", config.clienttimeout);
  lprintf (2, "   time window limit: %.0f%%", config.timewinlimit * 100);
  lprintf (2, "   resolve hostnames: %s", (config.resolvehosts) ? "yes" : "no");
  lprintf (2, "   auto recovery: %u", config.autorecovery);
  lprintf (2, "   TLS certificate file: %s", (config.tlscertfile) ? config.tlscertfile : "NONE");
  lprintf (2, "   TLS key file: %s", (config.tlskeyfile) ? config.tlskeyfile : "NONE");
  lprintf (2, "   TLS verify client certificate: %s", (config.tlsverifyclientcert) ? "yes" : "no");

  lprintf (3, "   web root: %s", (config.webroot) ? config.webroot : "NONE");
  lprintf (3, "   HTTP headers: %s", (config.httpheaders) ? config.httpheaders : "NONE");
  lprintf (3, "   miniSEED archive: %s", (config.mseedarchive) ? config.mseedarchive : "NONE");
  lprintf (3, "   miniSEED idle file timeout: %u seconds", config.mseedidleto);

  lprintf (3, "   transfer log: %s", (TLogParams.tlogbasedir) ? TLogParams.tlogbasedir : "NONE");
  if (TLogParams.tlogbasedir && verbose >= 3)
  {
    lprintf (3, "     log prefix: %s", (TLogParams.tlogprefix) ? TLogParams.tlogprefix : "NONE");
    lprintf (3, "     log interval: %d seconds", TLogParams.tloginterval);
    lprintf (3, "     log transmission: %s", (TLogParams.txlog) ? "yes" : "no");
    lprintf (3, "     log reception: %s", (TLogParams.rxlog) ? "yes" : "no");

    if (TLogParams.tlogstartint)
    {
      ms_nstime2timestr (MS_EPOCH2NSTIME (TLogParams.tlogstartint), timestring, ISOMONTHDAY_Z, NONE);
      lprintf (3, "     log interval start: %s", timestring);
    }
    else
    {
      lprintf (3, "     log interval start: NONE");
    }

    if (TLogParams.tlogendint)
    {
      ms_nstime2timestr (MS_EPOCH2NSTIME (TLogParams.tlogendint), timestring, ISOMONTHDAY_Z, NONE);
      lprintf (3, "     log interval end: %s", timestring);
    }
    else
    {
      lprintf (3, "     log interval end: NONE");
    }

    if (TLogParams.tlogstart)
    {
      ms_nstime2timestr (MS_EPOCH2NSTIME (TLogParams.tlogstart), timestring, ISOMONTHDAY_Z, NONE);
      lprintf (3, "     log window start: %s", timestring);
    }
    else
    {
      lprintf (3, "     log window start: NONE");
    }
  }

  if (config.limitips && verbose >= 3)
  {
    IPNet *ipn = config.limitips;
    while (ipn)
    {
      inet_ntop (ipn->family, &ipn->network, network, sizeof (network));
      inet_ntop (ipn->family, &ipn->netmask, netmask, sizeof (netmask));

      lprintf (3, "   limit IP range: %s/%s", network, netmask);
      lprintf (3, "     limit pattern: %s", (ipn->limitstr) ? ipn->limitstr : "NONE");

      ipn = ipn->next;
    }
  }
  else
  {
    lprintf (3, "   limit IP: NONE");
  }

  if (config.matchips && verbose >= 3)
  {
    IPNet *ipn = config.matchips;
    while (ipn)
    {
      inet_ntop (ipn->family, &ipn->network, network, sizeof (network));
      inet_ntop (ipn->family, &ipn->netmask, netmask, sizeof (netmask));

      lprintf (3, "   match IP range: %s/%s", network, netmask);

      ipn = ipn->next;
    }
  }
  else
  {
    lprintf (3, "   match IP range: NONE");
  }

  if (config.rejectips && verbose >= 3)
  {
    IPNet *ipn = config.rejectips;
    while (ipn)
    {
      inet_ntop (ipn->family, &ipn->network, network, sizeof (network));
      inet_ntop (ipn->family, &ipn->netmask, netmask, sizeof (netmask));

      lprintf (3, "   reject IP range: %s/%s", network, netmask);

      ipn = ipn->next;
    }
  }
  else
  {
    lprintf (3, "   reject IP range: NONE");
  }

  if (config.writeips && verbose >= 3)
  {
    IPNet *ipn = config.writeips;
    while (ipn)
    {
      inet_ntop (ipn->family, &ipn->network, network, sizeof (network));
      inet_ntop (ipn->family, &ipn->netmask, netmask, sizeof (netmask));

      lprintf (3, "   write IP range: %s/%s", network, netmask);

      ipn = ipn->next;
    }
  }
  else
  {
    lprintf (3, "   write IP range: NONE");
  }

  if (config.trustedips && verbose >= 3)
  {
    IPNet *ipn = config.trustedips;
    while (ipn)
    {
      inet_ntop (ipn->family, &ipn->network, network, sizeof (network));
      inet_ntop (ipn->family, &ipn->netmask, netmask, sizeof (netmask));

      lprintf (3, "   trusted IP range: %s/%s", network, netmask);

      ipn = ipn->next;
    }
  }
  else
  {
    lprintf (3, "   trusted IP range: NONE");
  }
} /* End of LogServerParameters() */

/***************************************************************************
 * PrintHandler (USR1 signal):
 *
 * Use a high verbosity for an explicit request to print details.
 ***************************************************************************/
static void
PrintHandler ()
{
  uint8_t verbose_save = verbose;
  verbose = 3;
  LogRingParameters (ringparams);
  LogServerParameters ();
  verbose = verbose_save;
}
