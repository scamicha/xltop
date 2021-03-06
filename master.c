#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <getopt.h>
#include <malloc.h>
#include <ncurses.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <ev.h>
#include "ap_parse.h"
#include "x_botz.h"
#include "confuse.h"
#include "x_node.h"
#include "k_heap.h"
#include "job.h"
#include "clus.h"
#include "fs.h"
#include "lnet.h"
#include "serv.h"
#include "screen.h"
#include "xltop.h"
#include "trace.h"

#define XLTOP_BIND_ADDR "0.0.0.0"
#define XLTOP_CLUS_INTERVAL 120.0
#define XLTOP_NR_HOSTS_HINT 4096
#define XLTOP_NR_JOBS_HINT 256
#define XLTOP_SERV_INTERVAL 300.0

#define BIND_CFG_OPTS \
  CFG_STR("bind", NULL, CFGF_NONE),           \
  CFG_STR("bind_host", NULL, CFGF_NONE),    \
  CFG_STR("bind_address", NULL, CFGF_NONE), \
  CFG_STR("bind_service", NULL, CFGF_NONE), \
  CFG_STR("bind_port", NULL, CFGF_NONE)

static int bind_cfg(cfg_t *cfg, const char *addr, const char *port)
{
  struct ap_struct ap;
  char *opt;

  opt = cfg_getstr(cfg, "bind");
  if (opt != NULL) {
    if (ap_parse(&ap, opt, addr, port) < 0)
      return -1;
    addr = ap.ap_addr;
    port = ap.ap_port;
  }

  opt = cfg_getstr(cfg, "bind_host");
  if (opt != NULL)
    addr = opt;

  opt = cfg_getstr(cfg, "bind_address");
  if (opt != NULL)
    addr = opt;

  opt = cfg_getstr(cfg, "bind_service");
  if (opt != NULL)
    port = opt;

  opt = cfg_getstr(cfg, "bind_port");
  if (opt != NULL)
    port = opt;

  if (evx_listen_add_name(&x_listen.bl_listen, addr, port, 0) < 0) {
    ERROR("cannot bind to host/address `%s', service/port `%s': %m\n",
          addr, port);
    return -1;
  }

  return 0;
}

static cfg_opt_t clus_cfg_opts[] = {
  /* AUTH_CFG_OPTS, */
  BIND_CFG_OPTS,
  CFG_STR_LIST("domains", NULL, CFGF_NONE),
  CFG_FLOAT("interval", XLTOP_CLUS_INTERVAL, CFGF_NONE),
  CFG_FLOAT("offset", 0, CFGF_NONE),
  CFG_END(),
};

static void clus_cfg(EV_P_ cfg_t *cfg, char *addr, char *port)
{
  const char *name = cfg_title(cfg);
  struct clus_node *c;

  if (bind_cfg(cfg, addr, port) < 0)
    FATAL("invalid bind option for cluster `%s'\n", name);

  c = clus_lookup(name, L_CREATE /* |L_EXCLUSIVE */);
  if (c == NULL)
    FATAL("cannot create cluster `%s': %m\n", name);

  c->c_interval = cfg_getfloat(cfg, "interval");
  /* TODO offset. */

  size_t i, nr_domains = cfg_size(cfg, "domains");
  for (i = 0; i < nr_domains; i++) {
    const char *domain = cfg_getnstr(cfg, "domains", i);
    if (clus_add_domain(c, domain) < 0)
      FATAL("cannot add domain `%s' to cluster `%s': %m\n", domain, name);
  }

  TRACE("added cluster `%s'\n", name);
}

static cfg_opt_t lnet_cfg_opts[] = {
  CFG_STR_LIST("files", NULL, CFGF_NONE),
  CFG_END(),
};

static void lnet_cfg(cfg_t *cfg, size_t hint)
{
  const char *name = cfg_title(cfg);
  struct lnet_struct *l;

  l = lnet_lookup(name, L_CREATE, hint);
  if (l == NULL)
    FATAL("cannot create lnet `%s': %m\n", name);

  size_t i, nr_files = cfg_size(cfg, "files");
  for (i = 0; i < nr_files; i++) {
    const char *path = cfg_getnstr(cfg, "files", i);
    if (lnet_read(l, path) < 0)
      FATAL("cannot read lnet file `%s': %m\n", path);
  }

  TRACE("added lnet `%s'\n", name);
}

static cfg_opt_t fs_cfg_opts[] = {
  /* AUTH_CFG_OPTS, */
  BIND_CFG_OPTS,
  CFG_STR("lnet", NULL, CFGF_NONE),
  CFG_STR_LIST("servs", NULL, CFGF_NONE),
  CFG_FLOAT("interval", XLTOP_SERV_INTERVAL, CFGF_NONE),
  CFG_END(),
};

static void fs_cfg(EV_P_ cfg_t *cfg, char *addr, char *port)
{
  const char *name = cfg_title(cfg);
  const char *lnet_name = cfg_getstr(cfg, "lnet");
  struct x_node *x;
  double interval;
  struct lnet_struct *l;
  size_t i, nr_servs;

  if (bind_cfg(cfg, addr, port) < 0)
    FATAL("fs `%s': invalid bind option\n", name); /* XXX */

  x = x_lookup(X_FS, name, x_all[1], L_CREATE);
  if (x == NULL)
    FATAL("fs `%s': cannot create filesystem: %m\n", name);

  l = lnet_lookup(lnet_name, 0, 0);
  if (l == NULL)
    FATAL("fs `%s': unknown lnet `%s': %m\n",
          name, lnet_name != NULL ? lnet_name : "-");

  interval = cfg_getfloat(cfg, "interval");
  if (interval <= 0)
    FATAL("fs `%s': invalid interval %lf\n", name, interval);

  nr_servs = cfg_size(cfg, "servs");
  if (nr_servs == 0)
    FATAL("fs `%s': no servers given\n", name);

  for (i = 0; i < nr_servs; i++) {
    const char *serv_name = cfg_getnstr(cfg, "servs", i);
    struct serv_node *s;

    s = serv_create(serv_name, x, l);
    if (s == NULL)
      FATAL("fs `%s': cannot create server `%s': %m\n", name, serv_name);

    /* TODO AUTH */

    s->s_interval = interval;
    s->s_offset = (i * interval) / nr_servs;
  }
}

/* Screen callbacks. */

static void print_top_1(int line, int COLS, const struct k_node *k)
{
  const char *owner = "";
  const char *title = "";
  char hosts[3 * sizeof(size_t) + 1] = "-";

  int job_col_width = COLS - 78;

  if (job_col_width < 15)
    job_col_width = 15;

  if (x_is_job(k->k_x[0])) {
    const struct job_node *j = container_of(k->k_x[0], struct job_node, j_x);
    owner = j->j_owner;
    title = j->j_title;
    snprintf(hosts, sizeof(hosts), "%zu", k->k_x[0]->x_nr_child);
  }

  mvprintw(line, 0,
           "%-*s %-15s %10.3f %10.3f %10.3f %10s %10s %5s",
           job_col_width, k->k_x[0]->x_name, k->k_x[1]->x_name,
           k->k_rate[STAT_WR_BYTES] / 1048676,
           k->k_rate[STAT_RD_BYTES] / 1048576,
           k->k_rate[STAT_NR_REQS],
           owner, title, hosts);
}

static void screen_refresh_cb(EV_P_ int LINES, int COLS)
{
  time_t now = ev_now(EV_A);
  int job_col_width = COLS - 77;
  int nr_hdr_lines = 2;
  char nr_buf[256];
  int nr_len;

  erase();

  if (job_col_width < 15)
    job_col_width = 15;

  mvprintw(0, 0, "%s - %s\n", program_invocation_short_name, ctime(&now));

  nr_len = snprintf(nr_buf, sizeof(nr_buf),
                    "H %zu, J %zu, C %zu, S %zu, F %zu, K %zu",
                    x_types[X_HOST].x_nr, x_types[X_JOB].x_nr, x_types[X_CLUS].x_nr,
                    x_types[X_SERV].x_nr, x_types[X_FS].x_nr, nr_k);

  mvprintw(0, COLS - nr_len, "%s", nr_buf);

  mvprintw(1, 0, "%-*s %-15s %10s %10s %10s %10s %10s %5s",
           job_col_width, "JOB", "FS", "WR_MB/S", "RD_MB/S", "REQ/S",
           "OWNER", "TITLE", "HOSTS");
  mvchgat(1, 0, -1, A_STANDOUT, CP_RED, NULL);

  /* Print body. */

  size_t i, limit = LINES - nr_hdr_lines - 1;
  struct k_top t = {
    .t_spec = {
      offsetof(struct k_node, k_rate[STAT_WR_BYTES]),
      offsetof(struct k_node, k_rate[STAT_RD_BYTES]),
      offsetof(struct k_node, k_rate[STAT_NR_REQS]),
      -1,
    }
  };

  if (!(limit < 1024))
    limit = 1024;

  if (k_heap_init(&t.t_h, limit) < 0) {
    ERROR("cannot initialize k_heap: %m\n");
    goto out;
  }

  k_heap_top(&t.t_h, x_all[0], 2, x_all[1], 1, NULL, &k_top_cmp, now);
  k_heap_order(&t.t_h, &k_top_cmp);

  for (i = 0; i < t.t_h.h_count; i++)
    print_top_1(nr_hdr_lines + i, COLS, t.t_h.h_k[i]);

 out:
  k_heap_destroy(&t.t_h);
}

static void usage(int status)
{
  fprintf(status == 0 ? stdout : stderr,
          "Usage: %s [OPTIONS]...\n"
          /* ... */
          "\nOPTIONS:\n"
          " -c, --conf=FILE\n"
          /* TODO */
          ,
          program_invocation_short_name);
  exit(status);
}

int main(int argc, char *argv[])
{
  char *bind_addr = XLTOP_BIND_ADDR;
  char *bind_port = XLTOP_BIND_PORT;
  char *conf_dir_path = XLTOP_CONF_DIR;
  char *conf_file_name = "master.conf";
  int want_daemon = 0;

  struct option opts[] = {
    { "bind-addr",   1, NULL, 'a' },
    { "conf-dir",    1, NULL, 'c' },
    { "daemon",      0, NULL, 'd' },
    { "help",        0, NULL, 'h' },
    { "bind-port",   1, NULL, 'p' },
    { NULL,          0, NULL,  0  },
  };

  int opt;
  while ((opt = getopt_long(argc, argv, "a:c:dhp:", opts, 0)) > 0) {
    switch (opt) {
    case 'a':
      bind_addr = optarg;
    case 'c':
      conf_dir_path = optarg;
      break;
    case 'd':
      want_daemon = 1;
      break;
    case 'h':
      usage(0);
      break;
    case 'p':
      bind_port = optarg;
      break;
    case '?':
      FATAL("Try `%s --help' for more information.\n", program_invocation_short_name);
    }
  }

  if (chdir(conf_dir_path) < 0)
    FATAL("cannot access `%s': %m\n", conf_dir_path);

  cfg_opt_t main_cfg_opts[] = {
    BIND_CFG_OPTS,
    CFG_FLOAT("tick", K_TICK, CFGF_NONE),
    CFG_FLOAT("window", K_WINDOW, CFGF_NONE),
    CFG_INT("nr_hosts_hint", XLTOP_NR_HOSTS_HINT, CFGF_NONE),
    CFG_INT("nr_jobs_hint", XLTOP_NR_JOBS_HINT, CFGF_NONE),
    CFG_SEC("clus", clus_cfg_opts, CFGF_MULTI|CFGF_TITLE),
    CFG_SEC("lnet", lnet_cfg_opts, CFGF_MULTI|CFGF_TITLE),
    CFG_SEC("fs", fs_cfg_opts, CFGF_MULTI|CFGF_TITLE),
    CFG_END()
  };

  cfg_t *main_cfg = cfg_init(main_cfg_opts, 0);

  errno = 0;
  int cfg_rc = cfg_parse(main_cfg, conf_file_name);
  if (cfg_rc == CFG_FILE_ERROR) {
    if (errno == 0)
      errno = ENOENT;
    FATAL("cannot open `%s': %m\n", conf_file_name);
  } else if (cfg_rc == CFG_PARSE_ERROR) {
    FATAL("error parsing `%s'\n", conf_file_name);
  }

  k_tick = cfg_getfloat(main_cfg, "tick");
  if (k_tick <= 0)
    FATAL("%s: tick must be positive\n", conf_file_name);

  k_window = cfg_getfloat(main_cfg, "window");
  if (k_window <= 0)
    FATAL("%s: window must be positive\n", conf_file_name);

  size_t nr_host_hint = cfg_getint(main_cfg, "nr_hosts_hint");
  size_t nr_job_hint = cfg_getint(main_cfg, "nr_jobs_hint");
  size_t nr_clus = cfg_size(main_cfg, "clus");
  size_t nr_fs = cfg_size(main_cfg, "fs");
  size_t nr_serv = 0;
  size_t nr_domain = 0;

  size_t i;
  for (i = 0; i < nr_fs; i++)
    nr_serv += cfg_size(cfg_getnsec(main_cfg, "fs", i), "servs");

  for (i = 0; i < nr_clus; i++)
    nr_domain += cfg_size(cfg_getnsec(main_cfg, "clus", i), "domains");

  x_types[X_HOST].x_nr_hint = nr_host_hint;
  x_types[X_JOB].x_nr_hint = nr_job_hint;
  x_types[X_CLUS].x_nr_hint = nr_clus;
  x_types[X_SERV].x_nr_hint = nr_serv;
  x_types[X_FS].x_nr_hint = nr_fs;

  if (x_types_init() < 0)
    FATAL("cannot initialize x_types: %m\n");

  size_t nr_listen_entries = nr_clus + nr_serv + 128; /* XXX */
  if (botz_listen_init(&x_listen, nr_listen_entries) < 0)
    FATAL("%s: cannot initialize listener\n", conf_file_name);

  x_listen.bl_conn_timeout = 600; /* XXX */

  if (bind_cfg(main_cfg, bind_addr, bind_port) < 0)
    FATAL("%s: invalid bind config\n", conf_file_name);

  for (i = 0; i < NR_X_TYPES; i++)
    if (x_dir_init(i, NULL) < 0)
      FATAL("cannot initialize type resources: %m\n");

  if (serv_type_init() < 0)
    FATAL("cannot initialize serv type: %m\n");

  if (clus_type_init(nr_domain) < 0)
    FATAL("cannot initialize default cluster: %m\n");

  if (fs_type_init() < 0)
    FATAL("cannot initialize fs type: %m\n");

  for (i = 0; i < nr_clus; i++)
    clus_cfg(EV_DEFAULT_
             cfg_getnsec(main_cfg, "clus", i),
             bind_addr, bind_port);

  size_t nr_lnet = cfg_size(main_cfg, "lnet");
  for (i = 0; i < nr_lnet; i++)
    lnet_cfg(cfg_getnsec(main_cfg, "lnet", i), nr_host_hint);

  for (i = 0; i < nr_fs; i++)
    fs_cfg(EV_DEFAULT_
           cfg_getnsec(main_cfg, "fs", i),
           bind_addr, bind_port);

  cfg_free(main_cfg);

  extern const struct botz_entry_ops top_entry_ops; /* MOVEME */
  if (botz_add(&x_listen, "top", &top_entry_ops, NULL) < 0)
    FATAL("cannot add listen entry `%s': %m\n", "top");

  extern const struct botz_entry_ops domains_entry_ops; /* MOVEME */
  if (botz_add(&x_listen, "_domains", &domains_entry_ops, NULL) < 0)
    FATAL("cannot add listen entry `%s': %m\n", "_domains");

  signal(SIGPIPE, SIG_IGN);

  evx_listen_start(EV_DEFAULT_ &x_listen.bl_listen);

  if (want_daemon) {
    daemon(0, 0);
  } else {
    chdir("/");
    if (screen_init(&screen_refresh_cb, 1) < 0)
      FATAL("cannot initialize screen: %m\n");
    screen_start(EV_DEFAULT);
  }

  ev_run(EV_DEFAULT_ 0);

  if (screen_is_active)
    screen_stop(EV_DEFAULT);

  return 0;
}
