/**
 * @file main.cpp
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

extern "C" {
#include <pjsip.h>
#include <pjlib-util.h>
#include <pjlib.h>
}

#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <semaphore.h>

// Common STL includes.
#include <cassert>
#include <vector>
#include <map>
#include <set>
#include <list>
#include <queue>
#include <string>


#include "logger.h"
#include "utils.h"
#include "analyticslogger.h"
#include "regdata.h"
#include "stack.h"
#include "hssconnection.h"
#include "xdmconnection.h"
#include "stateful_proxy.h"
#include "websockets.h"
#include "callservices.h"
#include "registrar.h"
#include "authentication.h"
#include "options.h"
#include "memcachedstorefactory.h"
#include "localstorefactory.h"
#include "enumservice.h"
#include "bgcfservice.h"
#include "pjutils.h"
#include "log.h"
#include "zmq_lvc.h"
#include "quiescing_manager.h"

struct options
{
  std::string            system_name;
  int                    trusted_port;
  int                    untrusted_port;
  std::string            local_host;
  std::string            home_domain;
  std::string            sprout_domain;
  std::string            alias_hosts;
  pj_bool_t              edge_proxy;
  std::string            upstream_proxy;
  int                    upstream_proxy_port;
  int                    upstream_proxy_connections;
  int                    upstream_proxy_recycle;
  pj_bool_t              ibcf;
  std::string            trusted_hosts;
  pj_bool_t              auth_enabled;
  std::string            auth_realm;
  std::string            auth_config;
  std::string            sas_server;
  std::string            hss_server;
  std::string            xdm_server;
  std::string            store_servers;
  std::string            enum_server;
  std::string            enum_suffix;
  std::string            enum_file;
  pj_bool_t              analytics_enabled;
  std::string            analytics_directory;
  int                    reg_max_expires;
  int                    pjsip_threads;
  int                    worker_threads;
  pj_bool_t              log_to_file;
  std::string            log_directory;
  int                    log_level;
  pj_bool_t              interactive;
  pj_bool_t              daemon;
};


static sem_t term_sem;

static pj_bool_t quiescing = PJ_FALSE;
static sem_t quiescing_sem;
QuiescingManager *quiescing_mgr;

const static int QUIESCE_SIGNAL = SIGQUIT;
const static int UNQUIESCE_SIGNAL = SIGUSR1;

static void usage(void)
{
  puts("Options:\n"
       "\n"
       " -s, --system <name>        System name for SAS logging (defaults to local host name)\n"
       " -t, --trusted-port N       Set local trusted listener port to N\n"
       " -u, --untrusted-port N     Set local untrusted listener port to N\n"
       " -l, --localhost <name>     Override the local host name\n"
       " -D, --domain <name>        Override the home domain name\n"
       " -c, --sprout-domain <name> Override the sprout cluster domain name\n"
       " -n, --alias <names>        Optional list of alias host names\n"
       " -e, --edge-proxy <name>[:<port>[:<connections>[:<recycle time>]]]\n"
       "                            Operate as an edge proxy using the specified node\n"
       "                            as the upstream proxy.  Optionally specifies the port,\n"
       "                            the number of parallel connections to create, and how\n"
       "                            often to recycle these connections (by default a\n"
       "                            single connection to the trusted port is used and never\n"
       "                            recycled).\n"
       " -I, --ibcf <IP addresses>  Operate as an IBCF accepting SIP flows from\n"
       "                            the pre-configured list of IP addresses\n"
       " -R, --realm <realm>        Use specified realm for authentication\n"
       "                            (if not specified, local host name is used)\n"
       " -M, --memstore <servers>   Use memcached store on comma-separated list of\n"
       "                            servers for registration state\n"
       "                            (otherwise uses local store)\n"
       " -S, --sas <ipv4>           Use specified host as software assurance\n"
       "                            server.  Otherwise uses localhost\n"
       " -H, --hss <server>         Name/IP address of HSS server\n"
       " -X, --xdms <server>        Name/IP address of XDM server\n"
       " -E, --enum <server>        Name/IP address of ENUM server (default: 127.0.0.1)\n"
       " -x, --enum-suffix <suffix> Suffix appended to ENUM domains (default: .e164.arpa)\n"
       " -f, --enum-file <file>     JSON ENUM config file (disables DNS-based ENUM lookup)\n"
       " -r, --reg-max-expires <expiry>\n"
       "                            The maximum allowed registration period (in seconds)\n"
       " -p, --pjsip_threads N      Number of PJSIP threads (default: 1)\n"
       " -w, --worker_threads N     Number of worker threads (default: 1)\n"
       " -a, --analytics <directory>\n"
       "                            Generate analytics logs in specified directory\n"
       " -F, --log-file <directory>\n"
       "                            Log to file in specified directory\n"
       " -L, --log-level N          Set log level to N (default: 4)\n"
       " -d, --daemon               Run as daemon\n"
       " -i, --interactive          Run in foreground with interactive menu\n"
       " -h, --help                 Show this help screen\n"
    );
}


static pj_status_t init_options(int argc, char *argv[], struct options *options)
{
  struct pj_getopt_option long_opt[] = {
    { "system",            required_argument, 0, 's'},
    { "trusted-port",      required_argument, 0, 't'},
    { "untrusted-port",    required_argument, 0, 'u'},
    { "localhost",         required_argument, 0, 'l'},
    { "domain",            required_argument, 0, 'D'},
    { "sprout-domain",     required_argument, 0, 'c'},
    { "alias",             required_argument, 0, 'n'},
    { "edge-proxy",        required_argument, 0, 'e'},
    { "ibcf",              required_argument, 0, 'I'},
    { "auth",              required_argument, 0, 'A'},
    { "realm",             required_argument, 0, 'R'},
    { "memstore",          required_argument, 0, 'M'},
    { "sas",               required_argument, 0, 'S'},
    { "hss",               required_argument, 0, 'H'},
    { "xdms",              required_argument, 0, 'X'},
    { "enum",              required_argument, 0, 'E'},
    { "enum-suffix",       required_argument, 0, 'x'},
    { "enum-file",         required_argument, 0, 'f'},
    { "reg-max-expires",   required_argument, 0, 'r'},
    { "pjsip-threads",     required_argument, 0, 'p'},
    { "worker-threads",    required_argument, 0, 'w'},
    { "analytics",         required_argument, 0, 'a'},
    { "log-file",          required_argument, 0, 'F'},
    { "log-level",         required_argument, 0, 'L'},
    { "daemon",            no_argument,       0, 'd'},
    { "interactive",       no_argument,       0, 'i'},
    { "help",              no_argument,       0, 'h'},
    { NULL,                0, 0, 0}
  };
  int c;
  int opt_ind;
  int reg_max_expires;

  pj_optind = 0;
  while((c=pj_getopt_long(argc, argv, "s:t:u:l:e:I:A:R:M:S:H:X:E:x:f:r:p:w:a:F:L:dih", long_opt, &opt_ind))!=-1) {
    switch (c) {
    case 's':
      options->system_name = std::string(pj_optarg);
      fprintf(stdout, "System name is set to %s\n", pj_optarg);
      break;

    case 't':
      options->trusted_port = atoi(pj_optarg);
      fprintf(stdout, "Trusted Port is set to %d\n", options->trusted_port);
      break;

    case 'u':
      options->untrusted_port = atoi(pj_optarg);
      fprintf(stdout, "Untrusted Port is set to %d\n", options->untrusted_port);
      break;

    case 'l':
      options->local_host = std::string(pj_optarg);
      fprintf(stdout, "Override local host name set to %s\n", pj_optarg);
      break;

    case 'D':
      options->home_domain = std::string(pj_optarg);
      fprintf(stdout, "Override home domain set to %s\n", pj_optarg);
      break;

    case 'c':
      options->sprout_domain = std::string(pj_optarg);
      fprintf(stdout, "Override sprout cluster domain set to %s\n", pj_optarg);
      break;

    case 'n':
      options->alias_hosts = std::string(pj_optarg);
      fprintf(stdout, "Alias host names = %s\n", pj_optarg);
      break;

    case 'e':
      {
        std::vector<std::string> upstream_proxy_options;
        Utils::split_string(std::string(pj_optarg), ':', upstream_proxy_options, 0, false);
        options->upstream_proxy = upstream_proxy_options[0];
        options->upstream_proxy_port = 0;
        options->upstream_proxy_connections = 1;
        options->upstream_proxy_recycle = 0;
        if (upstream_proxy_options.size() > 1)
        {
          options->upstream_proxy_port = atoi(upstream_proxy_options[1].c_str());
          if (upstream_proxy_options.size() > 2)
          {
            options->upstream_proxy_connections = atoi(upstream_proxy_options[2].c_str());
            if (upstream_proxy_options.size() > 3)
            {
              options->upstream_proxy_recycle = atoi(upstream_proxy_options[3].c_str());
            }
          }
        }
        fprintf(stdout, "Upstream proxy is set to %s", options->upstream_proxy.c_str());
        if (options->upstream_proxy_port != 0)
        {
          fprintf(stdout, ":%d", options->upstream_proxy_port);
        }
        fprintf(stdout, "\n");
        fprintf(stdout, "  connections = %d\n", options->upstream_proxy_connections);
        fprintf(stdout, "  recycle time = %d seconds\n", options->upstream_proxy_recycle);
        options->edge_proxy = PJ_TRUE;
      }
      break;

    case 'I':
      options->ibcf = PJ_TRUE;
      options->trusted_hosts = std::string(pj_optarg);
      fprintf(stdout, "IBCF mode enabled, trusted hosts = %s\n", pj_optarg);
      break;

    case 'R':
      options->auth_realm = std::string(pj_optarg);
      fprintf(stdout, "Authentication realm %s\n", pj_optarg);
      break;

    case 'M':
      options->store_servers = std::string(pj_optarg);
      fprintf(stdout, "Using memcached store on servers %s\n", pj_optarg);
      break;

    case 'S':
      options->sas_server = std::string(pj_optarg);
      fprintf(stdout, "SAS set to %s\n", pj_optarg);
      break;

    case 'H':
      options->hss_server = std::string(pj_optarg);
      fprintf(stdout, "HSS server set to %s\n", pj_optarg);
      break;

    case 'X':
      options->xdm_server = std::string(pj_optarg);
      fprintf(stdout, "XDM server set to %s\n", pj_optarg);
      break;

    case 'E':
      options->enum_server = std::string(pj_optarg);
      fprintf(stdout, "ENUM server set to %s\n", pj_optarg);
      break;

    case 'x':
      options->enum_suffix = std::string(pj_optarg);
      fprintf(stdout, "ENUM suffix set to %s\n", pj_optarg);
      break;

    case 'f':
      options->enum_file = std::string(pj_optarg);
      fprintf(stdout, "ENUM file set to %s\n", pj_optarg);
      break;

    case 'r':
      reg_max_expires = atoi(pj_optarg);

      if (reg_max_expires > 0)
      {
        options->reg_max_expires = reg_max_expires;
        fprintf(stdout, "Maximum registration period set to %d seconds\n",
                options->reg_max_expires);
      }
      else
      {
        // The parameter could be invalid either because it's -ve, or it's not
        // an integer (in which case atoi returns 0). Log, but don't store it.
        LOG_WARNING("Invalid value for reg_max_expires: '%s'. "
                    "The default value of %d will be used.",
                    pj_optarg, options->reg_max_expires);
      }
      break;

    case 'p':
      options->pjsip_threads = atoi(pj_optarg);
      fprintf(stdout, "Use %d PJSIP threads\n", options->pjsip_threads);
      break;

    case 'w':
      options->worker_threads = atoi(pj_optarg);
      fprintf(stdout, "Use %d worker threads\n", options->worker_threads);
      break;

    case 'a':
      options->analytics_enabled = PJ_TRUE;
      options->analytics_directory = std::string(pj_optarg);
      fprintf(stdout, "Analytics directory set to %s\n", pj_optarg);
      break;

    case 'L':
      options->log_level = atoi(pj_optarg);
      fprintf(stdout, "Log level set to %s\n", pj_optarg);
      break;

    case 'F':
      options->log_to_file = PJ_TRUE;
      options->log_directory = std::string(pj_optarg);
      fprintf(stdout, "Log directory set to %s\n", pj_optarg);
      break;

    case 'd':
      options->daemon = PJ_TRUE;
      break;

    case 'i':
      options->interactive = PJ_TRUE;
      break;

    case 'h':
      usage();
      return -1;

    default:
      fprintf(stdout, "Unknown option. Run with --help for help.\n");
      return -1;
    }
  }

  // If the upstream proxy port is not set, default it to the trusted port.
  // We couldn't do this earlier because the trusted port might be set after
  // the upstream proxy.
  if (options->upstream_proxy_port == 0)
  {
    options->upstream_proxy_port = options->trusted_port;
  }

  return PJ_SUCCESS;
}


int daemonize()
{
  LOG_STATUS("Switching to daemon mode");

  pid_t pid = fork();
  if (pid == -1)
  {
    // Fork failed, return error.
    return errno;
  }
  else if (pid > 0)
  {
    // Parent process, fork successful, so exit.
    exit(0);
  }

  // Must now be running in the context of the child process.

  // Redirect standard files to /dev/null
  if (freopen("/dev/null", "r", stdin) == NULL)
    return errno;
  if (freopen("/dev/null", "w", stdout) == NULL)
    return errno;
  if (freopen("/dev/null", "w", stderr) == NULL)
    return errno;

  if (setsid() == -1)
  {
    // Create a new session to divorce the child from the tty of the parent.
    return errno;
  }

  signal(SIGHUP, SIG_IGN);

  umask(0);

  return 0;
}


// Signal handler that simply dumps the stack and then crashes out.
void exception_handler(int sig)
{
  // Reset the signal handlers so that another exception will cause a crash.
  signal(SIGABRT, SIG_DFL);
  signal(SIGSEGV, SIG_DFL);

  // Log the signal, along with a backtrace.
  LOG_BACKTRACE("Signal %d caught", sig);

  // Dump a core.
  abort();
}


// Signal handler that receives requests to (un)quiesce.
void quiesce_unquiesce_handler(int sig)
{
  // Set the flag indicating whether we're quiescing or not.
  if (sig == QUIESCE_SIGNAL)
  {
    LOG_STATUS("Quiesce signal received");
    quiescing = PJ_TRUE;
  }
  else
  {
    LOG_STATUS("Unquiesce signal received");
    quiescing = PJ_FALSE;
  }

  // Wake up the thread that acts on the notification (don't act on it in this
  // thread since we're in a signal haandler).
  sem_post(&quiescing_sem);
}


// Signal handler that triggers sprout termination.
void terminate_handler(int sig)
{
  sem_post(&term_sem);
}


void *quiesce_unquiesce_thread_func(void *_)
{
  pj_bool_t curr_quiescing = PJ_FALSE;
  pj_bool_t new_quiescing = quiescing;

  while (PJ_TRUE)
  {
    // Only act if the quiescing state has changed.
    if (curr_quiescing != new_quiescing)
    {
      curr_quiescing = new_quiescing;

      if (new_quiescing) {
        quiescing_mgr->quiesce();
      } else {
        quiescing_mgr->unquiesce();
      }
    }

    // Wait for the quiescing flag to be written to and read in the new value.
    // Read into a local variable to avoid issues if the flag changes under our
    // feet.
    //
    // Note that sem_wait is a cancel point, so calling pthread_cancel on this
    // thread while it is waiting on the semaphore will cause it to cancel. 
    sem_wait(&quiescing_sem);
    new_quiescing = quiescing;
  }

  return NULL;
}

class QuiesceCompleteHandler : public QuiesceCompletionInterface
{
public:
  void quiesce_complete()
  {
    sem_post(&term_sem);
  }
};


/*
 * main()
 */
int main(int argc, char *argv[])
{
  pj_status_t status;
  struct options opt;
  HSSConnection* hss_connection = NULL;
  XDMConnection* xdm_connection = NULL;
  CallServices* call_services = NULL;
  IfcHandler* ifc_handler = NULL;
  AnalyticsLogger* analytics_logger = NULL;
  EnumService* enum_service = NULL;
  BgcfService* bgcf_service = NULL;
  pthread_t quiesce_unquiesce_thread;

  // Set up our exception signal handler for asserts and segfaults.
  signal(SIGABRT, exception_handler);
  signal(SIGSEGV, exception_handler);

  // Initialize the semaphore that unblocks the quiesce thread, and the thread
  // itself.
  sem_init(&quiescing_sem, 0, 0);
  pthread_create(&quiesce_unquiesce_thread,
                 NULL,
                 quiesce_unquiesce_thread_func,
                 NULL);

  // Set up our signal handler for (un)quiesce signals. 
  signal(QUIESCE_SIGNAL, quiesce_unquiesce_handler);
  signal(UNQUIESCE_SIGNAL, quiesce_unquiesce_handler);

  sem_init(&term_sem, 0, 0);
  signal(SIGTERM, terminate_handler);

  // Create a new quiescing manager instance and register our completion handler
  // with it.
  quiescing_mgr = new QuiescingManager();
  quiescing_mgr->register_completion_handler(new QuiesceCompleteHandler());

  opt.edge_proxy = PJ_FALSE;
  opt.upstream_proxy_port = 0;
  opt.ibcf = PJ_FALSE;
  opt.trusted_port = 0;
  opt.untrusted_port = 0;
  opt.auth_enabled = PJ_FALSE;
  opt.sas_server = "127.0.0.1";
  opt.enum_server = "127.0.0.1";
  opt.enum_suffix = ".e164.arpa";
  opt.reg_max_expires = 300;
  opt.pjsip_threads = 1;
  opt.worker_threads = 1;
  opt.analytics_enabled = PJ_FALSE;
  opt.log_to_file = PJ_FALSE;
  opt.log_level = 0;
  opt.daemon = PJ_FALSE;
  opt.interactive = PJ_FALSE;

  status = init_options(argc, argv, &opt);
  if (status != PJ_SUCCESS)
  {
    return 1;
  }

  Log::setLoggingLevel(opt.log_level);
  LOG_STATUS("Log level set to %d", opt.log_level);

  if (opt.daemon && opt.interactive)
  {
    LOG_ERROR("Cannot specify both --daemon and --interactive");
    return 1;
  }

  if ((opt.trusted_port == 0) && (opt.untrusted_port == 0))
  {
    LOG_ERROR("Must specify at least one listener port");
    return 1;
  }

  if (opt.auth_enabled)
  {
    if (opt.hss_server == "")
    {
      LOG_ERROR("Authentication enable, but no HSS server specified");
      return 1;
    }
  }

  if ((opt.xdm_server != "") && (opt.hss_server == ""))
  {
    LOG_ERROR("XDM server configured for services, but no HSS server specified");
    return 1;
  }

  if ((opt.store_servers != "") &&
      (opt.auth_enabled) &&
      (opt.worker_threads == 1))
  {
    LOG_WARNING("Use multiple threads for good performance when using memstore and/or authentication");
  }

  if (opt.daemon)
  {
    int errnum = daemonize();
    if (errnum != 0)
    {
      LOG_ERROR("Failed to convert to daemon, %d (%s)", errnum, strerror(errnum));
      exit(0);
    }
  }

  if (opt.edge_proxy && (opt.reg_max_expires != 0))
  {
    LOG_WARNING("A registration expiry period should not be specified for an edge proxy");
  }

  // Ensure our random numbers are unpredictable.
  unsigned int seed;
  pj_time_val now;
  pj_gettimeofday(&now);
  seed = (unsigned int)now.sec ^ (unsigned int)now.msec ^ getpid();
  srand(seed);

  init_pjsip_logging(opt.log_level, opt.log_to_file, opt.log_directory);

  if ((opt.log_to_file) && (opt.log_directory != ""))
  {
    // Work out the program name from argv[0], stripping anything before the final slash.
    char* prog_name = argv[0];
    char* slash_ptr = rindex(argv[0], '/');
    if (slash_ptr != NULL) {
      prog_name = slash_ptr + 1;
    }
    Log::setLogger(new Logger(opt.log_directory, prog_name));
  }

  if (opt.analytics_enabled)
  {
    analytics_logger = new AnalyticsLogger(opt.analytics_directory);
  }

  // Initialize the PJSIP stack and associated subsystems.
  status = init_stack(opt.system_name,
                      opt.sas_server,
                      opt.trusted_port,
                      opt.untrusted_port,
                      opt.local_host,
                      opt.home_domain,
                      opt.sprout_domain,
                      opt.alias_hosts,
                      opt.pjsip_threads,
                      opt.worker_threads,
                      quiescing_mgr);

  if (status != PJ_SUCCESS)
  {
    LOG_ERROR("Error initializing stack %s", PJUtils::pj_status_to_string(status).c_str());
    return 1;
  }

  RegData::Store* registrar_store = NULL;
  if (opt.store_servers != "")
  {
    // Use memcached store.
    LOG_STATUS("Using memcached compatible store with ASCII protocol");
    std::list<std::string> servers;
    Utils::split_string(opt.store_servers, ',', servers, 0, true);
    registrar_store = RegData::create_memcached_store(servers, 100, false);
  }
  else
  {
    // Use local store.
    LOG_STATUS("Using local store");
    registrar_store = RegData::create_local_store();
  }

  if (registrar_store == NULL)
  {
    LOG_ERROR("Failed to connect to data store");
    exit(0);
  }

  if (opt.hss_server != "")
  {
    // Create a connection to the HSS.
    LOG_STATUS("Creating connection to HSS %s", opt.hss_server.c_str());
    hss_connection = new HSSConnection(opt.hss_server);
  }

  if (opt.xdm_server != "")
  {
    // Create a connection to the XDMS.
    LOG_STATUS("Creating connection to XDMS %s", opt.xdm_server.c_str());
    xdm_connection = new XDMConnection(opt.xdm_server);
  }

  if (xdm_connection != NULL)
  {
    LOG_STATUS("Creating call services handler");
    call_services = new CallServices(xdm_connection);
  }

  if (hss_connection != NULL)
  {
    LOG_STATUS("Initializing iFC handler");
    ifc_handler = new IfcHandler(hss_connection, registrar_store);
  }

  // Initialise the OPTIONS handling module.
  status = init_options();

  if (!opt.edge_proxy)
  {
    status = init_authentication(opt.auth_realm, hss_connection, analytics_logger);

    // Create Enum and BGCF services required for SIP router.
    if (!opt.enum_file.empty())
    {
      enum_service = new JSONEnumService(opt.enum_file);
    }
    else
    {
      enum_service = new DNSEnumService(opt.enum_server, opt.enum_suffix);
    }
    bgcf_service = new BgcfService();
  }

  status = init_stateful_proxy(registrar_store,
                               call_services,
                               ifc_handler,
                               opt.edge_proxy,
                               opt.upstream_proxy,
                               opt.upstream_proxy_port,
                               opt.upstream_proxy_connections,
                               opt.upstream_proxy_recycle,
                               opt.ibcf,
                               opt.trusted_hosts,
                               analytics_logger,
                               enum_service,
                               bgcf_service,
                               hss_connection);
  if (status != PJ_SUCCESS)
  {
    LOG_ERROR("Error initializing stateful proxy, %s",
              PJUtils::pj_status_to_string(status).c_str());
    return 1;
  }

  // An edge proxy doesn't handle registrations, it passes them through.
  pj_bool_t registrar_enabled = !opt.edge_proxy;
  if (registrar_enabled)
  {
    status = init_registrar(registrar_store,
                            hss_connection,
                            analytics_logger,
                            ifc_handler,
                            opt.reg_max_expires);
    if (status != PJ_SUCCESS)
    {
      LOG_ERROR("Error initializing registrar, %s",
                PJUtils::pj_status_to_string(status).c_str());

      return 1;
    }
  }

  // Only the edge proxies need to handle websockets
  pj_bool_t websockets_enabled = opt.edge_proxy;
  if (websockets_enabled)
  {
    status = init_websockets();
    if (status != PJ_SUCCESS)
    {
      LOG_ERROR("Error initializing websockets, %s",
                PJUtils::pj_status_to_string(status).c_str());

      return 1;
    }
  }

  status = start_stack();
  if (status != PJ_SUCCESS)
  {
    LOG_ERROR("Error starting SIP stack, %s", PJUtils::pj_status_to_string(status).c_str());
    return 1;
  }

  // Wait here until the quite semaphore is signaled.
  sem_wait(&term_sem);

  stop_stack();
  // We must unregister stack modules here because this terminates the
  // transaction layer, which can otherwise generate work for other modules
  // after they have unregistered.
  unregister_stack_modules();
  if (registrar_enabled)
  {
    destroy_registrar();
  }
  if (websockets_enabled)
  {
    destroy_websockets();
  }
  destroy_stateful_proxy();
  if (opt.auth_enabled)
  {
    destroy_authentication();
  }
  destroy_options();
  destroy_stack();

  delete ifc_handler;
  delete call_services;
  delete hss_connection;
  delete xdm_connection;
  delete enum_service;
  delete bgcf_service;
  delete quiescing_mgr;

  if (opt.store_servers != "")
  {
    RegData::destroy_memcached_store(registrar_store);
  }
  else
  {
    RegData::destroy_local_store(registrar_store);
  }

  // Unregister the handlers that use semaphores (so we can safely destroy
  // them). 
  signal(QUIESCE_SIGNAL, SIG_DFL);
  signal(UNQUIESCE_SIGNAL, SIG_DFL);
  signal(SIGTERM, SIG_DFL);

  // Cancel the (un)quiesce thread (so that we can safely destroy the semaphore
  // it uses). 
  pthread_cancel(quiesce_unquiesce_thread);
  pthread_join(quiesce_unquiesce_thread, NULL);

  sem_destroy(&quiescing_sem);
  sem_destroy(&term_sem);

  return 0;
}

