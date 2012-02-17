#include <unistd.h>
#include "cl_listen.h"
#include "botz.h"
#include "lnet.h"
#include "serv.h"
#include "string1.h"
#include "trace.h"

static void serv_msg_cb(EV_P_ struct serv_node *s, char *msg)
{
  struct x_node *x;
  char *nid;
  double d[NR_STATS];

  nid = wsep(&msg);
  if (nid == NULL || msg == NULL)
    return;

  TRACE("nid `%s', msg `%s'\n", nid, msg);

  x = lnet_lookup_nid(s->s_lnet, nid, L_CREATE);
  if (x == NULL)
    return;

  /* ASSERT(NR_STATS == 3); */
  if (sscanf(msg, "%lf %lf %lf", &d[0], &d[1], &d[2]) != 3)
    return;

  x_update(EV_A_ x, &s->s_x, d);
}

static void serv_put_cb(EV_P_ struct botz_x *bx, struct n_buf *nb)
{
  struct serv_node *s = bx->x_data;
  char *msg;
  size_t msg_len;

  /* TODO AUTH. */

  s->s_modified = ev_now(EV_A);

  while (n_buf_get_msg(nb, &msg, &msg_len) == 0)
    serv_msg_cb(EV_A_ s, msg);
}

static void serv_get_cb(EV_P_ struct botz_x *bx, struct n_buf *nb)
{
  struct serv_node *s = bx->x_data;

  n_buf_printf(nb,
               "serv: %s\n"
               "interval: %f\n"
               "offset: %f\n"
               "modified: %f\n"
               "lnet: %s\n",
               s->s_x.x_name,
               s->s_interval,
               s->s_offset,
               s->s_modified,
               s->s_lnet->l_name);
  x_printf(nb, &s->s_x);
}

struct serv_node *
serv_create(const char *name, struct x_node *p, struct lnet_struct *l)
{
  size_t hash;
  struct hlist_head *head;
  struct x_node *x;
  struct serv_node *s;

  x = x_lookup_hash(X_SERV, name, &hash, &head);
  if (x != NULL)
    return container_of(x, struct serv_node, s_x);

  s = malloc(sizeof(*s) + strlen(name) + 1);
  if (s == NULL)
    return NULL;

  memset(s, 0, sizeof(*s));

  s->s_lnet = l;
  x_init(&s->s_x, X_SERV, p, hash, head, name);

  return s;
}

static const struct botz_entry_ops serv_entry_ops = {
  .o_method_ops = {
    [BOTZ_GET] = { .o_rsp_body_cb = &serv_get_cb },
    [BOTZ_PUT] = { .o_req_body_cb = &serv_put_cb },
  }
};

static int serv_lookup_cb(EV_P_ struct botz_x *bx, char *name)
{
  struct x_node *x = x_lookup(X_SERV, name, NULL, 0);
  TRACE("serv_lookup_cb name `%s', x %p\n", name, x);
  if (x == NULL)
    return -1;

  bx->x_ops = &serv_entry_ops;
  bx->x_data = container_of(x, struct serv_node, s_x);
  return 0;
}

static struct botz_entry_ops serv_type_entry_ops = {
  .o_lookup_cb = &serv_lookup_cb,
};

int serv_type_init(void)
{
  return cl_listen_add("serv", &serv_type_entry_ops, NULL);
}
