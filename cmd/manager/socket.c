/*******************************************************************

    LICENCE NOTICE

These coded instructions, statements, and computer programs are part
of the  InitWare Suite of Middleware,  and  they are protected under
copyright law. They may not be distributed,  copied,  or used except
under the provisions of  the  terms  of  the  Library General Public
Licence version 2.1 or later, in the file "LICENSE.md", which should
have been included with this software

    (c) 2021 David Mackay
        All rights reserved.
*********************************************************************/
/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#ifdef HAVE_XATTR
//#include <attr/xattr.h>
#endif

#include "dbus-common.h"
#include "dbus-socket.h"
#include "def.h"
#include "ev-util.h"
#include "exit-status.h"
#include "label.h"
#include "load-dropin.h"
#include "load-fragment.h"
#include "log.h"
#include "missing.h"
#include "mkdir.h"
#include "netinet/tcp.h"
#include "path-util.h"
#include "socket.h"
#include "special.h"
#include "strv.h"
#include "unit-name.h"
#include "unit-printf.h"
#include "unit.h"

#ifdef Have_mqueue_h
#include <mqueue.h>
#endif

static const UnitActiveState state_translation_table[_SOCKET_STATE_MAX] = {
        [SOCKET_DEAD] = UNIT_INACTIVE,
        [SOCKET_START_PRE] = UNIT_ACTIVATING,
        [SOCKET_START_CHOWN] = UNIT_ACTIVATING,
        [SOCKET_START_POST] = UNIT_ACTIVATING,
        [SOCKET_LISTENING] = UNIT_ACTIVE,
        [SOCKET_RUNNING] = UNIT_ACTIVE,
        [SOCKET_STOP_PRE] = UNIT_DEACTIVATING,
        [SOCKET_STOP_PRE_SIGTERM] = UNIT_DEACTIVATING,
        [SOCKET_STOP_PRE_SIGKILL] = UNIT_DEACTIVATING,
        [SOCKET_STOP_POST] = UNIT_DEACTIVATING,
        [SOCKET_FINAL_SIGTERM] = UNIT_DEACTIVATING,
        [SOCKET_FINAL_SIGKILL] = UNIT_DEACTIVATING,
        [SOCKET_FAILED] = UNIT_FAILED
};

static void socket_init(Unit *u) {
        Socket *s = SOCKET(u);

        assert(u);
        assert(u->load_state == UNIT_STUB);

        s->backlog = SOMAXCONN;
        s->timeout_usec = u->manager->default_timeout_start_usec;
        s->directory_mode = 0755;
        s->socket_mode = 0666;

        s->max_connections = 64;

        s->priority = -1;
        s->ip_tos = -1;
        s->ip_ttl = -1;
        s->mark = -1;

        exec_context_init(&s->exec_context);
        s->exec_context.std_output = u->manager->default_std_output;
        s->exec_context.std_error = u->manager->default_std_error;
        kill_context_init(&s->kill_context);
#ifdef Use_CGroups
        cgroup_context_init(&s->cgroup_context);
#endif
        ev_timer_zero(s->timer_watch);

        s->control_command_id = _SOCKET_EXEC_COMMAND_INVALID;
}

static void socket_unwatch_control_pid(Socket *s) {
        assert(s);

        if (s->control_pid <= 0)
                return;

        unit_unwatch_pid(UNIT(s), s->control_pid);
        s->control_pid = 0;
}

void socket_free_ports(Socket *s) {
        SocketPort *p;

        assert(s);

        while ((p = s->ports)) {
                IWLIST_REMOVE(SocketPort, port, s->ports, p);

                if (p->fd >= 0) {
                        unit_unwatch_fd(UNIT(s), &p->fd_watch);
                        safe_close(p->fd);
                }

                free(p->path);
                free(p);
        }
}

static void socket_done(Unit *u) {
        Socket *s = SOCKET(u);

        assert(s);

        socket_free_ports(s);

        exec_context_done(&s->exec_context, manager_is_reloading_or_reexecuting(u->manager));
#ifdef Use_CGroups
        cgroup_context_init(&s->cgroup_context);
#endif

        exec_command_free_array(s->exec_command, _SOCKET_EXEC_COMMAND_MAX);
        s->control_command = NULL;

        socket_unwatch_control_pid(s);

        unit_ref_unset(&s->service);

        free(s->tcp_congestion);
        s->tcp_congestion = NULL;

        free(s->bind_to_device);
        s->bind_to_device = NULL;

        free(s->smack);
        free(s->smack_ip_in);
        free(s->smack_ip_out);

        unit_unwatch_timer(u, &s->timer_watch);

        free(s->user);
        free(s->group);
}

static int socket_instantiate_service(Socket *s) {
        char *prefix, *name;
        int r;
        Unit *u;

        assert(s);

        /* This fills in s->service if it isn't filled in yet. For
         * Accept=yes sockets we create the next connection service
         * here. For Accept=no this is mostly a NOP since the service
         * is figured out at load time anyway. */

        if (UNIT_DEREF(s->service))
                return 0;

        assert(s->accept);

        if (!(prefix = unit_name_to_prefix(UNIT(s)->id)))
                return -ENOMEM;

        r = asprintf(&name, "%s@%u.service", prefix, s->n_accepted);
        free(prefix);

        if (r < 0)
                return -ENOMEM;

        r = manager_load_unit(UNIT(s)->manager, name, NULL, NULL, &u);
        free(name);

        if (r < 0)
                return r;

#ifdef HAVE_SYSV_COMPAT
        if (SERVICE(u)->is_sysv) {
                log_error("Using SysV services for socket activation is not supported. Refusing.");
                return -ENOENT;
        }
#endif

        u->no_gc = true;
        unit_ref_set(&s->service, u);

        return unit_add_two_dependencies(UNIT(s), UNIT_BEFORE, UNIT_TRIGGERS, u, false);
}

static bool have_non_accept_socket(Socket *s) {
        SocketPort *p;

        assert(s);

        if (!s->accept)
                return true;

        IWLIST_FOREACH(port, p, s->ports) {

                if (p->type != SOCKET_SOCKET)
                        return true;

                if (!socket_address_can_accept(&p->address))
                        return true;
        }

        return false;
}

static int socket_verify(Socket *s) {
        assert(s);

        if (UNIT(s)->load_state != UNIT_LOADED)
                return 0;

        if (!s->ports) {
                log_error_unit(UNIT(s)->id,
                               "%s lacks Listen setting. Refusing.", UNIT(s)->id);
                return -EINVAL;
        }

        if (s->accept && have_non_accept_socket(s)) {
                log_error_unit(UNIT(s)->id,
                               "%s configured for accepting sockets, but sockets are non-accepting. Refusing.",
                               UNIT(s)->id);
                return -EINVAL;
        }

        if (s->accept && s->max_connections <= 0) {
                log_error_unit(UNIT(s)->id,
                               "%s's MaxConnection setting too small. Refusing.", UNIT(s)->id);
                return -EINVAL;
        }

        if (s->accept && UNIT_DEREF(s->service)) {
                log_error_unit(UNIT(s)->id,
                               "Explicit service configuration for accepting sockets not supported on %s. Refusing.",
                               UNIT(s)->id);
                return -EINVAL;
        }

        if (s->exec_context.pam_name && s->kill_context.kill_mode != KILL_CONTROL_GROUP) {
                log_error_unit(UNIT(s)->id,
                               "%s has PAM enabled. Kill mode must be set to 'control-group'. Refusing.",
                               UNIT(s)->id);
                return -EINVAL;
        }

        return 0;
}

static int socket_add_mount_links(Socket *s) {
        SocketPort *p;
        int r;

        assert(s);

        IWLIST_FOREACH(port, p, s->ports) {
                const char *path = NULL;

                if (p->type == SOCKET_SOCKET)
                        path = socket_address_get_path(&p->address);
                else if (p->type == SOCKET_FIFO || p->type == SOCKET_SPECIAL)
                        path = p->path;

                if (!path)
                        continue;

                r = unit_require_mounts_for(UNIT(s), path);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int socket_add_device_link(Socket *s) {
        char *t;
        int r;

        assert(s);

        if (!s->bind_to_device || streq(s->bind_to_device, "lo"))
                return 0;

        if (asprintf(&t, "/sys/subsystem/net/devices/%s", s->bind_to_device) < 0)
                return -ENOMEM;

        r = unit_add_node_link(UNIT(s), t, false);
        free(t);

        return r;
}

static int socket_add_default_dependencies(Socket *s) {
        int r;
        assert(s);

        r = unit_add_dependency_by_name(UNIT(s), UNIT_BEFORE, SPECIAL_SOCKETS_TARGET, NULL, true);
        if (r < 0)
                return r;

        if (UNIT(s)->manager->running_as == SYSTEMD_SYSTEM) {
                r = unit_add_two_dependencies_by_name(UNIT(s), UNIT_AFTER, UNIT_REQUIRES, SPECIAL_SYSINIT_TARGET, NULL, true);
                if (r < 0)
                        return r;
        }

        return unit_add_two_dependencies_by_name(UNIT(s), UNIT_BEFORE, UNIT_CONFLICTS, SPECIAL_SHUTDOWN_TARGET, NULL, true);
}

_pure_ static bool socket_has_exec(Socket *s) {
        unsigned i;
        assert(s);

        for (i = 0; i < _SOCKET_EXEC_COMMAND_MAX; i++)
                if (s->exec_command[i])
                        return true;

        return false;
}

static int socket_load(Unit *u) {
        Socket *s = SOCKET(u);
        int r;

        assert(u);
        assert(u->load_state == UNIT_STUB);

        if ((r = unit_load_fragment_and_dropin(u)) < 0)
                return r;

        /* This is a new unit? Then let's add in some extras */
        if (u->load_state == UNIT_LOADED) {

                if (have_non_accept_socket(s)) {

                        if (!UNIT_DEREF(s->service)) {
                                Unit *x;

                                r = unit_load_related_unit(u, ".service", &x);
                                if (r < 0)
                                        return r;

                                unit_ref_set(&s->service, x);
                        }

                        r = unit_add_two_dependencies(u, UNIT_BEFORE, UNIT_TRIGGERS, UNIT_DEREF(s->service), true);
                        if (r < 0)
                                return r;
                }

                if ((r = socket_add_mount_links(s)) < 0)
                        return r;

                if ((r = socket_add_device_link(s)) < 0)
                        return r;

                if (socket_has_exec(s))
                        if ((r = unit_add_exec_dependencies(u, &s->exec_context)) < 0)
                                return r;

                r = unit_add_default_slice(u);
                if (r < 0)
                        return r;

                if (UNIT(s)->default_dependencies)
                        if ((r = socket_add_default_dependencies(s)) < 0)
                                return r;

                r = unit_exec_context_defaults(u, &s->exec_context);
                if (r < 0)
                        return r;
        }

        return socket_verify(s);
}

_const_ static const char* listen_lookup(int family, int type) {

#ifdef Have_linux_netlink_h
        if (family == AF_NETLINK)
                return "ListenNetlink";
#endif

        if (type == SOCK_STREAM)
                return "ListenStream";
        else if (type == SOCK_DGRAM)
                return "ListenDatagram";
        else if (type == SOCK_SEQPACKET)
                return "ListenSequentialPacket";

        assert_not_reached("Unknown socket type");
        return NULL;
}

static void socket_dump(Unit *u, FILE *f, const char *prefix) {

        SocketExecCommand c;
        Socket *s = SOCKET(u);
        SocketPort *p;
        const char *prefix2;
        char *p2;

        assert(s);
        assert(f);

        p2 = strappend(prefix, "\t");
        prefix2 = p2 ? p2 : prefix;

        fprintf(f,
                "%sSocket State: %s\n"
                "%sResult: %s\n"
                "%sBindIPv6Only: %s\n"
                "%sBacklog: %u\n"
                "%sSocketMode: %04o\n"
                "%sDirectoryMode: %04o\n"
                "%sKeepAlive: %s\n"
                "%sFreeBind: %s\n"
                "%sTransparent: %s\n"
                "%sBroadcast: %s\n"
                "%sPassCredentials: %s\n"
                "%sPassSecurity: %s\n"
                "%sTCPCongestion: %s\n",
                prefix, socket_state_to_string(s->state),
                prefix, socket_result_to_string(s->result),
                prefix, socket_address_bind_ipv6_only_to_string(s->bind_ipv6_only),
                prefix, s->backlog,
                prefix, s->socket_mode,
                prefix, s->directory_mode,
                prefix, yes_no(s->keep_alive),
                prefix, yes_no(s->free_bind),
                prefix, yes_no(s->transparent),
                prefix, yes_no(s->broadcast),
                prefix, yes_no(s->pass_cred),
                prefix, yes_no(s->pass_sec),
                prefix, strna(s->tcp_congestion));

        if (s->control_pid > 0)
                fprintf(f,
                        "%sControl PID: %lu\n",
                        prefix, (unsigned long) s->control_pid);

        if (s->bind_to_device)
                fprintf(f,
                        "%sBindToDevice: %s\n",
                        prefix, s->bind_to_device);

        if (s->accept)
                fprintf(f,
                        "%sAccepted: %u\n"
                        "%sNConnections: %u\n"
                        "%sMaxConnections: %u\n",
                        prefix, s->n_accepted,
                        prefix, s->n_connections,
                        prefix, s->max_connections);

        if (s->priority >= 0)
                fprintf(f,
                        "%sPriority: %i\n",
                        prefix, s->priority);

        if (s->receive_buffer > 0)
                fprintf(f,
                        "%sReceiveBuffer: %zu\n",
                        prefix, s->receive_buffer);

        if (s->send_buffer > 0)
                fprintf(f,
                        "%sSendBuffer: %zu\n",
                        prefix, s->send_buffer);

        if (s->ip_tos >= 0)
                fprintf(f,
                        "%sIPTOS: %i\n",
                        prefix, s->ip_tos);

        if (s->ip_ttl >= 0)
                fprintf(f,
                        "%sIPTTL: %i\n",
                        prefix, s->ip_ttl);

        if (s->pipe_size > 0)
                fprintf(f,
                        "%sPipeSize: %zu\n",
                        prefix, s->pipe_size);

        if (s->mark >= 0)
                fprintf(f,
                        "%sMark: %i\n",
                        prefix, s->mark);

        if (s->mq_maxmsg > 0)
                fprintf(f,
                        "%sMessageQueueMaxMessages: %li\n",
                        prefix, s->mq_maxmsg);

        if (s->mq_msgsize > 0)
                fprintf(f,
                        "%sMessageQueueMessageSize: %li\n",
                        prefix, s->mq_msgsize);

        if (s->reuseport)
                fprintf(f,
                        "%sReusePort: %s\n",
                         prefix, yes_no(s->reuseport));

        if (s->smack)
                fprintf(f,
                        "%sSmackLabel: %s\n",
                        prefix, s->smack);

        if (s->smack_ip_in)
                fprintf(f,
                        "%sSmackLabelIPIn: %s\n",
                        prefix, s->smack_ip_in);

        if (s->smack_ip_out)
                fprintf(f,
                        "%sSmackLabelIPOut: %s\n",
                        prefix, s->smack_ip_out);

        if (!isempty(s->user) || !isempty(s->group))
                fprintf(f,
                        "%sOwnerUser: %s\n"
                        "%sOwnerGroup: %s\n",
                        prefix, strna(s->user),
                        prefix, strna(s->group));

        IWLIST_FOREACH(port, p, s->ports) {

                if (p->type == SOCKET_SOCKET) {
                        const char *t;
                        int r;
                        char *k = NULL;

                        if ((r = socket_address_print(&p->address, &k)) < 0)
                                t = strerror(-r);
                        else
                                t = k;

                        fprintf(f, "%s%s: %s\n", prefix, listen_lookup(socket_address_family(&p->address), p->address.type), t);
                        free(k);
                } else if (p->type == SOCKET_SPECIAL)
                        fprintf(f, "%sListenSpecial: %s\n", prefix, p->path);
                else if (p->type == SOCKET_MQUEUE)
                        fprintf(f, "%sListenMessageQueue: %s\n", prefix, p->path);
                else
                        fprintf(f, "%sListenFIFO: %s\n", prefix, p->path);
        }

        exec_context_dump(&s->exec_context, f, prefix);
        kill_context_dump(&s->kill_context, f, prefix);

        for (c = 0; c < _SOCKET_EXEC_COMMAND_MAX; c++) {
                if (!s->exec_command[c])
                        continue;

                fprintf(f, "%s-> %s:\n",
                        prefix, socket_exec_command_to_string(c));

                exec_command_dump_list(s->exec_command[c], f, prefix2);
        }

        free(p2);
}

static int instance_from_socket(int fd, unsigned nr, char **instance) {
        socklen_t l;
        char *r;
        union {
                struct sockaddr sa;
                struct sockaddr_un un;
                struct sockaddr_in in;
                struct sockaddr_in6 in6;
                struct sockaddr_storage storage;
        } local, remote;

        assert(fd >= 0);
        assert(instance);

        l = sizeof(local);
        if (getsockname(fd, &local.sa, &l) < 0)
                return -errno;

        l = sizeof(remote);
        if (getpeername(fd, &remote.sa, &l) < 0)
                return -errno;

        switch (local.sa.sa_family) {

        case AF_INET: {
                uint32_t
                        a = ntohl(local.in.sin_addr.s_addr),
                        b = ntohl(remote.in.sin_addr.s_addr);

                if (asprintf(&r,
                             "%u-%u.%u.%u.%u:%u-%u.%u.%u.%u:%u",
                             nr,
                             a >> 24, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF,
                             ntohs(local.in.sin_port),
                             b >> 24, (b >> 16) & 0xFF, (b >> 8) & 0xFF, b & 0xFF,
                             ntohs(remote.in.sin_port)) < 0)
                        return -ENOMEM;

                break;
        }

        case AF_INET6: {
                static const unsigned char ipv4_prefix[] = {
                        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF
                };

                if (memcmp(&local.in6.sin6_addr, ipv4_prefix, sizeof(ipv4_prefix)) == 0 &&
                    memcmp(&remote.in6.sin6_addr, ipv4_prefix, sizeof(ipv4_prefix)) == 0) {
                        const uint8_t
                                *a = local.in6.sin6_addr.s6_addr+12,
                                *b = remote.in6.sin6_addr.s6_addr+12;

                        if (asprintf(&r,
                                     "%u-%u.%u.%u.%u:%u-%u.%u.%u.%u:%u",
                                     nr,
                                     a[0], a[1], a[2], a[3],
                                     ntohs(local.in6.sin6_port),
                                     b[0], b[1], b[2], b[3],
                                     ntohs(remote.in6.sin6_port)) < 0)
                                return -ENOMEM;
                } else {
                        char a[INET6_ADDRSTRLEN], b[INET6_ADDRSTRLEN];

                        if (asprintf(&r,
                                     "%u-%s:%u-%s:%u",
                                     nr,
                                     inet_ntop(AF_INET6, &local.in6.sin6_addr, a, sizeof(a)),
                                     ntohs(local.in6.sin6_port),
                                     inet_ntop(AF_INET6, &remote.in6.sin6_addr, b, sizeof(b)),
                                     ntohs(remote.in6.sin6_port)) < 0)
                                return -ENOMEM;
                }

                break;
        }

        case AF_UNIX: {
#ifdef SO_PEERCRED
                struct ucred ucred;

                l = sizeof(ucred);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &l) < 0)
                        return -errno;

                if (asprintf(&r,
                             "%u-%lu-%lu",
                             nr,
                             (unsigned long) ucred.pid,
                             (unsigned long) ucred.uid) < 0)
                        return -ENOMEM;
#endif

                break;
        }

        default:
                assert_not_reached("Unhandled socket type.");
        }

        *instance = r;
        return 0;
}

static void socket_close_fds(Socket *s) {
        SocketPort *p;

        assert(s);

        IWLIST_FOREACH(port, p, s->ports) {
                if (p->fd < 0)
                        continue;

                unit_unwatch_fd(UNIT(s), &p->fd_watch);
                p->fd = safe_close(p->fd);

                /* One little note: we should never delete any sockets
                 * in the file system here! After all some other
                 * process we spawned might still have a reference of
                 * this fd and wants to continue to use it. Therefore
                 * we delete sockets in the file system before we
                 * create a new one, not after we stopped using
                 * one! */
        }
}

static void socket_apply_socket_options(Socket *s, int fd) {
        assert(s);
        assert(fd >= 0);

        if (s->keep_alive) {
                int b = s->keep_alive;
                if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &b, sizeof(b)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_KEEPALIVE failed: %m");
        }

        if (s->broadcast) {
                int one = 1;
                if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_BROADCAST failed: %m");
        }

#ifdef SO_PASSCRED
        if (s->pass_cred) {
                int one = 1;
                if (setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_PASSCRED failed: %m");
        }
#endif

#ifdef SO_PASSSEC
        if (s->pass_sec) {
                int one = 1;
                if (setsockopt(fd, SOL_SOCKET, SO_PASSSEC, &one, sizeof(one)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_PASSSEC failed: %m");
        }
#endif

#ifdef SO_PRIORITY
        if (s->priority >= 0)
                if (setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &s->priority, sizeof(s->priority)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_PRIORITY failed: %m");
#endif

        if (s->receive_buffer > 0) {
                int value = (int) s->receive_buffer;

                /* We first try with SO_RCVBUFFORCE, in case we have the perms for that */

#ifdef SO_RCVBUFFORCE
                if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &value, sizeof(value)) < 0)
#endif
                        if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)) < 0)
                                log_warning_unit(UNIT(s)->id, "SO_RCVBUF failed: %m");
        }

        if (s->send_buffer > 0) {
                int value = (int) s->send_buffer;

#ifdef SO_SNDBUFFORCE
                if (setsockopt(fd, SOL_SOCKET, SO_SNDBUFFORCE, &value, sizeof(value)) < 0)
#endif
                        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) < 0)
                                log_warning_unit(UNIT(s)->id, "SO_SNDBUF failed: %m");
        }

#ifdef SO_MARK
        if (s->mark >= 0)
                if (setsockopt(fd, SOL_SOCKET, SO_MARK, &s->mark, sizeof(s->mark)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_MARK failed: %m");
#endif

        if (s->ip_tos >= 0)
                if (setsockopt(fd, IPPROTO_IP, IP_TOS, &s->ip_tos, sizeof(s->ip_tos)) < 0)
                        log_warning_unit(UNIT(s)->id, "IP_TOS failed: %m");

        if (s->ip_ttl >= 0) {
                int r, x;

                r = setsockopt(fd, IPPROTO_IP, IP_TTL, &s->ip_ttl, sizeof(s->ip_ttl));

                if (socket_ipv6_is_supported())
                        x = setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &s->ip_ttl, sizeof(s->ip_ttl));
                else {
                        x = -1;
                        errno = EAFNOSUPPORT;
                }

                if (r < 0 && x < 0)
                        log_warning_unit(UNIT(s)->id,
                                         "IP_TTL/IPV6_UNICAST_HOPS failed: %m");
        }

#ifdef SOL_TCP
        if (s->tcp_congestion)
                if (setsockopt(fd, SOL_TCP, TCP_CONGESTION, s->tcp_congestion, strlen(s->tcp_congestion)+1) < 0)
                        log_warning_unit(UNIT(s)->id, "TCP_CONGESTION failed: %m");
#endif

        if (s->reuseport) {
                int b = s->reuseport;
                if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &b, sizeof(b)) < 0)
                        log_warning_unit(UNIT(s)->id, "SO_REUSEPORT failed: %m");
        }

#ifdef HAVE_SMACK
        if (s->smack_ip_in)
                if (fsetxattr(fd, "security.SMACK64IPIN", s->smack_ip_in, strlen(s->smack_ip_in), 0) < 0)
                        log_error_unit(UNIT(s)->id,
                                       "fsetxattr(\"security.SMACK64IPIN\"): %m");

        if (s->smack_ip_out)
                if (fsetxattr(fd, "security.SMACK64IPOUT", s->smack_ip_out, strlen(s->smack_ip_out), 0) < 0)
                        log_error_unit(UNIT(s)->id,
                                       "fsetxattr(\"security.SMACK64IPOUT\"): %m");
#endif
}

static void socket_apply_fifo_options(Socket *s, int fd) {
        assert(s);
        assert(fd >= 0);

        if (s->pipe_size > 0)
#ifdef F_SETPIPE_SZ
                if (fcntl(fd, F_SETPIPE_SZ, s->pipe_size) < 0)
#else
                (errno = ENOENT) &&
#endif
                        log_warning_unit(UNIT(s)->id,
                                         "F_SETPIPE_SZ: %m");

#ifdef HAVE_SMACK
        if (s->smack)
                if (fsetxattr(fd, "security.SMACK64", s->smack, strlen(s->smack), 0) < 0)
                        log_error_unit(UNIT(s)->id,
                                       "fsetxattr(\"security.SMACK64\"): %m");
#endif
}

static int fifo_address_create(
                const char *path,
                mode_t directory_mode,
                mode_t socket_mode,
                int *_fd) {

        int fd = -1, r = 0;
        struct stat st;
        mode_t old_mask;

        assert(path);
        assert(_fd);

        mkdir_parents_label(path, directory_mode);

        r = label_context_set(path, S_IFIFO);
        if (r < 0)
                goto fail;

        /* Enforce the right access mode for the fifo */
        old_mask = umask(~ socket_mode);

        /* Include the original umask in our mask */
        umask(~socket_mode | old_mask);

        r = mkfifo(path, socket_mode);
        umask(old_mask);

        if (r < 0 && errno != EEXIST) {
                r = -errno;
                goto fail;
        }

        if ((fd = open(path, O_RDWR|O_CLOEXEC|O_NOCTTY|O_NONBLOCK|O_NOFOLLOW)) < 0) {
                r = -errno;
                goto fail;
        }

        label_context_clear();

        if (fstat(fd, &st) < 0) {
                r = -errno;
                goto fail;
        }

        if (!S_ISFIFO(st.st_mode) ||
            (st.st_mode & 0777) != (socket_mode & ~old_mask) ||
            st.st_uid != getuid() ||
            st.st_gid != getgid()) {

                r = -EEXIST;
                goto fail;
        }

        *_fd = fd;
        return 0;

fail:
        label_context_clear();
        safe_close(fd);

        return r;
}

static int special_address_create(
                const char *path,
                int *_fd) {

        int fd = -1, r = 0;
        struct stat st;

        assert(path);
        assert(_fd);

        if ((fd = open(path, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK|O_NOFOLLOW)) < 0) {
                r = -errno;
                goto fail;
        }

        if (fstat(fd, &st) < 0) {
                r = -errno;
                goto fail;
        }

        /* Check whether this is a /proc, /sys or /dev file or char device */
        if (!S_ISREG(st.st_mode) && !S_ISCHR(st.st_mode)) {
                r = -EEXIST;
                goto fail;
        }

        *_fd = fd;
        return 0;

fail:
        safe_close(fd);

        return r;
}

#ifdef Use_MQ
static int mq_address_create(
                const char *path,
                mode_t mq_mode,
                long maxmsg,
                long msgsize,
                int *_fd) {

        int fd = -1, r = 0;
        struct stat st;
        mode_t old_mask;
        struct mq_attr _attr, *attr = NULL;

        assert(path);
        assert(_fd);

        if (maxmsg > 0 && msgsize > 0) {
                zero(_attr);
                _attr.mq_flags = O_NONBLOCK;
                _attr.mq_maxmsg = maxmsg;
                _attr.mq_msgsize = msgsize;
                attr = &_attr;
        }

        /* Enforce the right access mode for the mq */
        old_mask = umask(~ mq_mode);

        /* Include the original umask in our mask */
        umask(~mq_mode | old_mask);

        fd = mq_open(path, O_RDONLY|O_CLOEXEC|O_NONBLOCK|O_CREAT, mq_mode, attr);
        umask(old_mask);

        if (fd < 0) {
                r = -errno;
                goto fail;
        }

        if (fstat(fd, &st) < 0) {
                r = -errno;
                goto fail;
        }

        if ((st.st_mode & 0777) != (mq_mode & ~old_mask) ||
            st.st_uid != getuid() ||
            st.st_gid != getgid()) {

                r = -EEXIST;
                goto fail;
        }

        *_fd = fd;
        return 0;

fail:
        safe_close(fd);
        return r;
}
#endif

static int socket_open_fds(Socket *s) {
        SocketPort *p;
        int r;
        char *label = NULL;
        bool know_label = false;

        assert(s);

        IWLIST_FOREACH(port, p, s->ports) {

                if (p->fd >= 0)
                        continue;

                if (p->type == SOCKET_SOCKET) {

                        if (!know_label) {

                                if ((r = socket_instantiate_service(s)) < 0)
                                        return r;

                                if (UNIT_ISSET(s->service) &&
                                    SERVICE(UNIT_DEREF(s->service))->exec_command[SERVICE_EXEC_START]) {
                                        r = label_get_create_label_from_exe(SERVICE(UNIT_DEREF(s->service))->exec_command[SERVICE_EXEC_START]->path, &label);

                                        if (r < 0) {
                                                if (r != -EPERM)
                                                        return r;
                                        }
                                }

                                know_label = true;
                        }

                        if ((r = socket_address_listen(
                                             &p->address,
                                             s->backlog,
                                             s->bind_ipv6_only,
                                             s->bind_to_device,
                                             s->free_bind,
                                             s->transparent,
                                             s->directory_mode,
                                             s->socket_mode,
                                             label,
                                             &p->fd)) < 0)
                                goto rollback;

                        socket_apply_socket_options(s, p->fd);

                } else  if (p->type == SOCKET_SPECIAL) {

                        if ((r = special_address_create(
                                             p->path,
                                             &p->fd)) < 0)
                                goto rollback;

                } else  if (p->type == SOCKET_FIFO) {

                        if ((r = fifo_address_create(
                                             p->path,
                                             s->directory_mode,
                                             s->socket_mode,
                                             &p->fd)) < 0)
                                goto rollback;

                        socket_apply_fifo_options(s, p->fd);
#ifdef Use_MQ
                } else if (p->type == SOCKET_MQUEUE) {

                        if ((r = mq_address_create(
                                             p->path,
                                             s->socket_mode,
                                             s->mq_maxmsg,
                                             s->mq_msgsize,
                                             &p->fd)) < 0)
                                goto rollback;
#endif
                } else
                        assert_not_reached("Unknown port type");
        }

        label_free(label);
        return 0;

rollback:
        socket_close_fds(s);
        label_free(label);
        return r;
}

static void socket_unwatch_fds(Socket *s) {
        SocketPort *p;

        assert(s);

        IWLIST_FOREACH(port, p, s->ports) {
                if (p->fd < 0)
                        continue;

                unit_unwatch_fd(UNIT(s), &p->fd_watch);
        }
}

static int socket_watch_fds(Socket *s) {
        SocketPort *p;
        int r;

        assert(s);

        IWLIST_FOREACH(port, p, s->ports) {
                if (p->fd < 0)
                        continue;

#if 0 // FIXME: libev
                p->fd_watch.socket_accept =
                        s->accept &&
                        p->type == SOCKET_SOCKET &&
                        socket_address_can_accept(&p->address);
#endif

                if ((r = unit_watch_fd(UNIT(s), p->fd, EV_READ, &p->fd_watch)) < 0)
                        goto fail;
        }

        return 0;

fail:
        socket_unwatch_fds(s);
        return r;
}

static void socket_set_state(Socket *s, SocketState state) {
        SocketState old_state;
        assert(s);

        old_state = s->state;
        s->state = state;

        if (state != SOCKET_START_PRE &&
            state != SOCKET_START_CHOWN &&
            state != SOCKET_START_POST &&
            state != SOCKET_STOP_PRE &&
            state != SOCKET_STOP_PRE_SIGTERM &&
            state != SOCKET_STOP_PRE_SIGKILL &&
            state != SOCKET_STOP_POST &&
            state != SOCKET_FINAL_SIGTERM &&
            state != SOCKET_FINAL_SIGKILL) {
                unit_unwatch_timer(UNIT(s), &s->timer_watch);
                socket_unwatch_control_pid(s);
                s->control_command = NULL;
                s->control_command_id = _SOCKET_EXEC_COMMAND_INVALID;
        }

        if (state != SOCKET_LISTENING)
                socket_unwatch_fds(s);

        if (state != SOCKET_START_CHOWN &&
            state != SOCKET_START_POST &&
            state != SOCKET_LISTENING &&
            state != SOCKET_RUNNING &&
            state != SOCKET_STOP_PRE &&
            state != SOCKET_STOP_PRE_SIGTERM &&
            state != SOCKET_STOP_PRE_SIGKILL)
                socket_close_fds(s);

        if (state != old_state)
                log_debug_unit(UNIT(s)->id,
                               "%s changed %s -> %s", UNIT(s)->id,
                               socket_state_to_string(old_state),
                               socket_state_to_string(state));

        unit_notify(UNIT(s), state_translation_table[old_state], state_translation_table[state], true);
}

static int socket_coldplug(Unit *u) {
        Socket *s = SOCKET(u);
        int r;

        assert(s);
        assert(s->state == SOCKET_DEAD);

        if (s->deserialized_state != s->state) {

                if (s->deserialized_state == SOCKET_START_PRE ||
                    s->deserialized_state == SOCKET_START_CHOWN ||
                    s->deserialized_state == SOCKET_START_POST ||
                    s->deserialized_state == SOCKET_STOP_PRE ||
                    s->deserialized_state == SOCKET_STOP_PRE_SIGTERM ||
                    s->deserialized_state == SOCKET_STOP_PRE_SIGKILL ||
                    s->deserialized_state == SOCKET_STOP_POST ||
                    s->deserialized_state == SOCKET_FINAL_SIGTERM ||
                    s->deserialized_state == SOCKET_FINAL_SIGKILL) {

                        if (s->control_pid <= 0)
                                return -EBADMSG;

                        r = unit_watch_pid(UNIT(s), s->control_pid);
                        if (r < 0)
                                return r;

                        r = unit_watch_timer(UNIT(s), s->timeout_usec, &s->timer_watch);
                        if (r < 0)
                                return r;
                }

                if (s->deserialized_state == SOCKET_START_CHOWN ||
                    s->deserialized_state == SOCKET_START_POST ||
                    s->deserialized_state == SOCKET_LISTENING ||
                    s->deserialized_state == SOCKET_RUNNING ||
                    s->deserialized_state == SOCKET_STOP_PRE ||
                    s->deserialized_state == SOCKET_STOP_PRE_SIGTERM ||
                    s->deserialized_state == SOCKET_STOP_PRE_SIGKILL)
                        if ((r = socket_open_fds(s)) < 0)
                                return r;

                if (s->deserialized_state == SOCKET_LISTENING)
                        if ((r = socket_watch_fds(s)) < 0)
                                return r;

                socket_set_state(s, s->deserialized_state);
        }

        return 0;
}

static int socket_spawn(Socket *s, ExecCommand *c, pid_t *_pid) {
        _cleanup_free_ char **argv = NULL;
        pid_t pid;
        int r;

        assert(s);
        assert(c);
        assert(_pid);

#ifdef Use_CGroup
        unit_realize_cgroup(UNIT(s));
#elif defined(Use_PTGroups)
        unit_realize_ptgroup(UNIT(s));
#endif

        r = unit_watch_timer(UNIT(s), s->timeout_usec, &s->timer_watch);
        if (r < 0)
                goto fail;

        r = unit_full_printf_strv(UNIT(s), c->argv, &argv);
        if (r < 0)
                goto fail;

        r = exec_spawn(c,
                       argv,
                       &s->exec_context,
                       NULL, 0,
                       UNIT(s)->manager->environment,
                       true,
                       true,
                       true,
                       UNIT(s)->manager->confirm_spawn,
#ifdef Use_CGroups
                       UNIT(s)->manager->cgroup_supported,
                       UNIT(s)->cgroup_path,
#elif defined(Use_PTGroups)
                       UNIT(s)->manager->pt_manager,
                       UNIT(s)->ptgroup,
#endif
                       UNIT(s)->id,
                       NULL,
                       &pid);
        if (r < 0)
                goto fail;

        r = unit_watch_pid(UNIT(s), pid);
        if (r < 0)
                /* FIXME: we need to do something here */
                goto fail;

        *_pid = pid;
        return 0;

fail:
        unit_unwatch_timer(UNIT(s), &s->timer_watch);
        return r;
}

static int socket_chown(Socket *s, pid_t *_pid) {
        pid_t pid;
        int r;

        /* We have to resolve the user names out-of-process, hence
         * let's fork here. It's messy, but well, what can we do? */

        pid = fork();
        if (pid < 0)
                return -errno;

        if (pid == 0) {
                SocketPort *p;
                uid_t uid = (uid_t) -1;
                gid_t gid = (gid_t) -1;
                int ret;

                default_signals(SIGNALS_CRASH_HANDLER, SIGNALS_IGNORE, -1);
                ignore_signals(SIGPIPE, -1);
                log_forget_fds();

                if (!isempty(s->user)) {
                        const char *user = s->user;

                        r = get_user_creds(&user, &uid, &gid, NULL, NULL);
                        if (r < 0) {
                                ret = EXIT_USER;
                                goto fail_child;
                        }
                }

                if (!isempty(s->group)) {
                        const char *group = s->group;

                        r = get_group_creds(&group, &gid);
                        if (r < 0) {
                                ret = EXIT_GROUP;
                                goto fail_child;
                        }
                }

                IWLIST_FOREACH(port, p, s->ports) {
                        const char *path;

                        if (p->type == SOCKET_SOCKET)
                                path = socket_address_get_path(&p->address);
                        else if (p->type == SOCKET_FIFO)
                                path = p->path;

                        if (!path)
                                continue;

                        if (chown(path, uid, gid) < 0) {
                                r = -errno;
                                ret = EXIT_CHOWN;
                                goto fail_child;
                        }
                }

                _exit(0);

        fail_child:
                log_open();
                log_error("Failed to chown socket at step %s: %s", exit_status_to_string(ret, EXIT_STATUS_SYSTEMD), strerror(-r));

                _exit(ret);
        }

        r = unit_watch_pid(UNIT(s), pid);
        if (r < 0)
                goto fail;

        *_pid = pid;
        return 0;

fail:
        return r;
}

static void socket_enter_dead(Socket *s, SocketResult f) {
        assert(s);

        if (f != SOCKET_SUCCESS)
                s->result = f;

        exec_context_tmp_dirs_done(&s->exec_context);
        socket_set_state(s, s->result != SOCKET_SUCCESS ? SOCKET_FAILED : SOCKET_DEAD);
}

static void socket_enter_signal(Socket *s, SocketState state, SocketResult f);

static void socket_enter_stop_post(Socket *s, SocketResult f) {
        int r;
        assert(s);

        if (f != SOCKET_SUCCESS)
                s->result = f;

        socket_unwatch_control_pid(s);
        s->control_command_id = SOCKET_EXEC_STOP_POST;
        s->control_command = s->exec_command[SOCKET_EXEC_STOP_POST];

        if (s->control_command) {
                r = socket_spawn(s, s->control_command, &s->control_pid);
                if (r < 0)
                        goto fail;

                socket_set_state(s, SOCKET_STOP_POST);
        } else
                socket_enter_signal(s, SOCKET_FINAL_SIGTERM, SOCKET_SUCCESS);

        return;

fail:
        log_warning_unit(UNIT(s)->id,
                         "%s failed to run 'stop-post' task: %s",
                         UNIT(s)->id, strerror(-r));
        socket_enter_signal(s, SOCKET_FINAL_SIGTERM, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_signal(Socket *s, SocketState state, SocketResult f) {
        int r;

        assert(s);

        if (f != SOCKET_SUCCESS)
                s->result = f;

        r = unit_kill_context(
                        UNIT(s),
                        &s->kill_context,
                        state != SOCKET_STOP_PRE_SIGTERM && state != SOCKET_FINAL_SIGTERM,
                        -1,
                        s->control_pid,
                        false);
        if (r < 0)
                goto fail;

        if (r > 0) {
                r = unit_watch_timer(UNIT(s), s->timeout_usec, &s->timer_watch);
                if (r < 0)
                        goto fail;

                socket_set_state(s, state);
        } else if (state == SOCKET_STOP_PRE_SIGTERM || state == SOCKET_STOP_PRE_SIGKILL)
                socket_enter_stop_post(s, SOCKET_SUCCESS);
        else
                socket_enter_dead(s, SOCKET_SUCCESS);

        return;

fail:
        log_warning_unit(UNIT(s)->id,
                         "%s failed to kill processes: %s",
                         UNIT(s)->id, strerror(-r));

        if (state == SOCKET_STOP_PRE_SIGTERM || state == SOCKET_STOP_PRE_SIGKILL)
                socket_enter_stop_post(s, SOCKET_FAILURE_RESOURCES);
        else
                socket_enter_dead(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_stop_pre(Socket *s, SocketResult f) {
        int r;
        assert(s);

        if (f != SOCKET_SUCCESS)
                s->result = f;

        socket_unwatch_control_pid(s);
        s->control_command_id = SOCKET_EXEC_STOP_PRE;
        s->control_command = s->exec_command[SOCKET_EXEC_STOP_PRE];

        if (s->control_command) {
                r = socket_spawn(s, s->control_command, &s->control_pid);
                if (r < 0)
                        goto fail;

                socket_set_state(s, SOCKET_STOP_PRE);
        } else
                socket_enter_stop_post(s, SOCKET_SUCCESS);

        return;

fail:
        log_warning_unit(UNIT(s)->id,
                         "%s failed to run 'stop-pre' task: %s",
                         UNIT(s)->id, strerror(-r));
        socket_enter_stop_post(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_listening(Socket *s) {
        int r;
        assert(s);

        r = socket_watch_fds(s);
        if (r < 0) {
                log_warning_unit(UNIT(s)->id,
                                 "%s failed to watch sockets: %s",
                                 UNIT(s)->id, strerror(-r));
                goto fail;
        }

        socket_set_state(s, SOCKET_LISTENING);
        return;

fail:
        socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_start_post(Socket *s) {
        int r;
        assert(s);

        socket_unwatch_control_pid(s);
        s->control_command_id = SOCKET_EXEC_START_POST;
        s->control_command = s->exec_command[SOCKET_EXEC_START_POST];

        if (s->control_command) {
                r = socket_spawn(s, s->control_command, &s->control_pid);
                if (r < 0) {
                        log_warning_unit(UNIT(s)->id, "%s failed to run 'start-post' task: %s", UNIT(s)->id, strerror(-r));
                        goto fail;
                }

                socket_set_state(s, SOCKET_START_POST);
        } else
                socket_enter_listening(s);

        return;

fail:
        socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_start_chown(Socket *s) {
        int r;

        assert(s);

        r = socket_open_fds(s);
        if (r < 0) {
                log_warning_unit(UNIT(s)->id,
                                 "%s failed to listen on sockets: %s",
                                 UNIT(s)->id, strerror(-r));
                goto fail;
        }

        if (!isempty(s->user) || !isempty(s->group)) {

                socket_unwatch_control_pid(s);
                s->control_command_id = SOCKET_EXEC_START_CHOWN;
                s->control_command = NULL;

                r = socket_chown(s, &s->control_pid);
                if (r < 0) {
                        log_warning_unit(UNIT(s)->id,
                                         "%s failed to fork 'start-chown' task: %s",
                                         UNIT(s)->id, strerror(-r));
                        goto fail;
                }

                socket_set_state(s, SOCKET_START_CHOWN);
        } else
                socket_enter_start_post(s);

        return;

fail:
        socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_start_pre(Socket *s) {
        int r;
        assert(s);

        socket_unwatch_control_pid(s);
        s->control_command_id = SOCKET_EXEC_START_PRE;
        s->control_command = s->exec_command[SOCKET_EXEC_START_PRE];

        if (s->control_command) {
                r = socket_spawn(s, s->control_command, &s->control_pid);
                if (r < 0) {
                        log_warning_unit(UNIT(s)->id, "%s failed to run 'start-pre' task: %s", UNIT(s)->id, strerror(-r));
                        goto fail;
                }

                socket_set_state(s, SOCKET_START_PRE);
        } else
                socket_enter_start_chown(s);

        return;

fail:
        socket_enter_dead(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_enter_running(Socket *s, int cfd) {
        int r;
        DBusError error;

        assert(s);
        dbus_error_init(&error);

        /* We don't take connections anymore if we are supposed to
         * shut down anyway */
        if (unit_stop_pending(UNIT(s))) {
                log_debug_unit(UNIT(s)->id,
                               "Suppressing connection request on %s since unit stop is scheduled.",
                               UNIT(s)->id);

                if (cfd >= 0)
                        safe_close(cfd);
                else  {
                        /* Flush all sockets by closing and reopening them */
                        socket_close_fds(s);

                        r = socket_watch_fds(s);
                        if (r < 0) {
                                log_warning_unit(UNIT(s)->id,
                                                 "%s failed to watch sockets: %s",
                                                 UNIT(s)->id, strerror(-r));
                                socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);
                        }
                }

                return;
        }

        if (cfd < 0) {
                Iterator i;
                Unit *u;
                bool pending = false;

                /* If there's already a start pending don't bother to
                 * do anything */
                SET_FOREACH(u, UNIT(s)->dependencies[UNIT_TRIGGERS], i)
                        if (unit_active_or_pending(u)) {
                                pending = true;
                                break;
                        }

                if (!pending) {
                        if (!UNIT_ISSET(s->service)) {
                                log_error_unit(UNIT(s)->id, "%s: service to activate vanished, refusing activation.", UNIT(s)->id);
                                r = -ENOENT;
                                goto fail;
                        }

                        r = manager_add_job(UNIT(s)->manager, JOB_START, UNIT_DEREF(s->service), JOB_REPLACE, true, &error, NULL);
                        if (r < 0)
                                goto fail;
                }

                socket_set_state(s, SOCKET_RUNNING);
        } else {
                char *prefix, *instance = NULL, *name;
                Service *service;

                if (s->n_connections >= s->max_connections) {
                        log_warning_unit(UNIT(s)->id,
                                         "%s: Too many incoming connections (%u)",
                                         UNIT(s)->id, s->n_connections);
                        safe_close(cfd);
                        return;
                }

                r = socket_instantiate_service(s);
                if (r < 0)
                        goto fail;

                r = instance_from_socket(cfd, s->n_accepted, &instance);
                if (r < 0) {
                        if (r != -ENOTCONN)
                                goto fail;

                        /* ENOTCONN is legitimate if TCP RST was received.
                         * This connection is over, but the socket unit lives on. */
                        safe_close(cfd);
                        return;
                }

                prefix = unit_name_to_prefix(UNIT(s)->id);
                if (!prefix) {
                        free(instance);
                        r = -ENOMEM;
                        goto fail;
                }

                name = unit_name_build(prefix, instance, ".service");
                free(prefix);
                free(instance);

                if (!name) {
                        r = -ENOMEM;
                        goto fail;
                }

                r = unit_add_name(UNIT_DEREF(s->service), name);
                if (r < 0) {
                        free(name);
                        goto fail;
                }

                service = SERVICE(UNIT_DEREF(s->service));
                unit_ref_unset(&s->service);
                s->n_accepted ++;

                UNIT(service)->no_gc = false;

                unit_choose_id(UNIT(service), name);
                free(name);

                r = service_set_socket_fd(service, cfd, s);
                if (r < 0)
                        goto fail;

                cfd = -1;
                s->n_connections ++;

                r = manager_add_job(UNIT(s)->manager, JOB_START, UNIT(service), JOB_REPLACE, true, &error, NULL);
                if (r < 0)
                        goto fail;

                /* Notify clients about changed counters */
                unit_add_to_dbus_queue(UNIT(s));
        }

        return;

fail:
        log_warning_unit(UNIT(s)->id,
                         "%s failed to queue service startup job (Maybe the service file is missing or not a %s unit?): %s",
                         UNIT(s)->id,
                         cfd >= 0 ? "template" : "non-template",
                         bus_error(&error, r));
        socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);

        safe_close(cfd);

        dbus_error_free(&error);
}

static void socket_run_next(Socket *s) {
        int r;

        assert(s);
        assert(s->control_command);
        assert(s->control_command->command_next);

        socket_unwatch_control_pid(s);

        s->control_command = s->control_command->command_next;

        if ((r = socket_spawn(s, s->control_command, &s->control_pid)) < 0)
                goto fail;

        return;

fail:
        log_warning_unit(UNIT(s)->id,
                         "%s failed to run next task: %s",
                         UNIT(s)->id, strerror(-r));

        if (s->state == SOCKET_START_POST)
                socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);
        else if (s->state == SOCKET_STOP_POST)
                socket_enter_dead(s, SOCKET_FAILURE_RESOURCES);
        else
                socket_enter_signal(s, SOCKET_FINAL_SIGTERM, SOCKET_FAILURE_RESOURCES);
}

static int socket_start(Unit *u) {
        Socket *s = SOCKET(u);

        assert(s);

        /* We cannot fulfill this request right now, try again later
         * please! */
        if (IN_SET(s->state,
                   SOCKET_STOP_PRE,
                   SOCKET_STOP_PRE_SIGKILL,
                   SOCKET_STOP_PRE_SIGTERM,
                   SOCKET_STOP_POST,
                   SOCKET_FINAL_SIGTERM,
                   SOCKET_FINAL_SIGKILL))
                return -EAGAIN;

        if (IN_SET(s->state,
                   SOCKET_START_PRE,
                   SOCKET_START_CHOWN,
                   SOCKET_START_POST))
                return 0;

        /* Cannot run this without the service being around */
        if (UNIT_ISSET(s->service)) {
                Service *service;

                service = SERVICE(UNIT_DEREF(s->service));

                if (UNIT(service)->load_state != UNIT_LOADED) {
                        log_error_unit(u->id,
                                       "Socket service %s not loaded, refusing.",
                                       UNIT(service)->id);
                        return -ENOENT;
                }

                /* If the service is already active we cannot start the
                 * socket */
                if (service->state != SERVICE_DEAD &&
                    service->state != SERVICE_FAILED &&
                    service->state != SERVICE_AUTO_RESTART) {
                        log_error_unit(u->id,
                                       "Socket service %s already active, refusing.",
                                       UNIT(service)->id);
                        return -EBUSY;
                }

#ifdef HAVE_SYSV_COMPAT
                if (service->is_sysv) {
                        log_error_unit(u->id,
                                       "Using SysV services for socket activation is not supported. Refusing.");
                        return -ENOENT;
                }
#endif
        }

        assert(s->state == SOCKET_DEAD || s->state == SOCKET_FAILED);

        s->result = SOCKET_SUCCESS;
        socket_enter_start_pre(s);
        return 0;
}

static int socket_stop(Unit *u) {
        Socket *s = SOCKET(u);

        assert(s);

        /* Already on it */
        if (IN_SET(s->state,
                   SOCKET_STOP_PRE,
                   SOCKET_STOP_PRE_SIGTERM,
                   SOCKET_STOP_PRE_SIGKILL,
                   SOCKET_STOP_POST,
                   SOCKET_FINAL_SIGTERM,
                   SOCKET_FINAL_SIGKILL))
                return 0;

        /* If there's already something running we go directly into
         * kill mode. */
        if (IN_SET(s->state,
                   SOCKET_START_PRE,
                   SOCKET_START_CHOWN,
                   SOCKET_START_POST)) {
                socket_enter_signal(s, SOCKET_STOP_PRE_SIGTERM, SOCKET_SUCCESS);
                return -EAGAIN;
        }

        assert(s->state == SOCKET_LISTENING || s->state == SOCKET_RUNNING);

        socket_enter_stop_pre(s, SOCKET_SUCCESS);
        return 0;
}

static int socket_serialize(Unit *u, FILE *f, FDSet *fds) {
        Socket *s = SOCKET(u);
        SocketPort *p;
        int r;

        assert(u);
        assert(f);
        assert(fds);

        unit_serialize_item(u, f, "state", socket_state_to_string(s->state));
        unit_serialize_item(u, f, "result", socket_result_to_string(s->result));
        unit_serialize_item_format(u, f, "n-accepted", "%u", s->n_accepted);

        if (s->control_pid > 0)
                unit_serialize_item_format(u, f, "control-pid", "%lu", (unsigned long) s->control_pid);

        if (s->control_command_id >= 0)
                unit_serialize_item(u, f, "control-command", socket_exec_command_to_string(s->control_command_id));

        IWLIST_FOREACH(port, p, s->ports) {
                int copy;

                if (p->fd < 0)
                        continue;

                if ((copy = fdset_put_dup(fds, p->fd)) < 0)
                        return copy;

                if (p->type == SOCKET_SOCKET) {
                        char *t;

                        r = socket_address_print(&p->address, &t);
                        if (r < 0)
                                return r;

#ifdef Have_linux_netlink_h
                        if (socket_address_family(&p->address) == AF_NETLINK)
                                unit_serialize_item_format(u, f, "netlink", "%i %s", copy, t);
                        else
#endif
                                unit_serialize_item_format(u, f, "socket", "%i %i %s", copy, p->address.type, t);
                        free(t);
                } else if (p->type == SOCKET_SPECIAL)
                        unit_serialize_item_format(u, f, "special", "%i %s", copy, p->path);
                else if (p->type == SOCKET_MQUEUE)
                        unit_serialize_item_format(u, f, "mqueue", "%i %s", copy, p->path);
                else {
                        assert(p->type == SOCKET_FIFO);
                        unit_serialize_item_format(u, f, "fifo", "%i %s", copy, p->path);
                }
        }

        exec_context_serialize(&s->exec_context, UNIT(s), f);

        return 0;
}

static int socket_deserialize_item(Unit *u, const char *key, const char *value, FDSet *fds) {
        Socket *s = SOCKET(u);

        assert(u);
        assert(key);
        assert(value);
        assert(fds);

        if (streq(key, "state")) {
                SocketState state;

                state = socket_state_from_string(value);
                if (state < 0)
                        log_debug_unit(u->id,
                                       "Failed to parse state value %s", value);
                else
                        s->deserialized_state = state;
        } else if (streq(key, "result")) {
                SocketResult f;

                f = socket_result_from_string(value);
                if (f < 0)
                        log_debug_unit(u->id,
                                       "Failed to parse result value %s", value);
                else if (f != SOCKET_SUCCESS)
                        s->result = f;

        } else if (streq(key, "n-accepted")) {
                unsigned k;

                if (safe_atou(value, &k) < 0)
                        log_debug_unit(u->id,
                                       "Failed to parse n-accepted value %s", value);
                else
                        s->n_accepted += k;
        } else if (streq(key, "control-pid")) {
                pid_t pid;

                if (parse_pid(value, &pid) < 0)
                        log_debug_unit(u->id,
                                       "Failed to parse control-pid value %s", value);
                else
                        s->control_pid = pid;
        } else if (streq(key, "control-command")) {
                SocketExecCommand id;

                id = socket_exec_command_from_string(value);
                if (id < 0)
                        log_debug_unit(u->id,
                                       "Failed to parse exec-command value %s", value);
                else {
                        s->control_command_id = id;
                        s->control_command = s->exec_command[id];
                }
        } else if (streq(key, "fifo")) {
                int fd, skip = 0;
                SocketPort *p;

                if (sscanf(value, "%i %n", &fd, &skip) < 1 || fd < 0 || !fdset_contains(fds, fd))
                        log_debug_unit(u->id,
                                       "Failed to parse fifo value %s", value);
                else {

                        IWLIST_FOREACH(port, p, s->ports)
                                if (p->type == SOCKET_FIFO &&
                                    streq_ptr(p->path, value+skip))
                                        break;

                        if (p) {
                                safe_close(p->fd);
                                p->fd = fdset_remove(fds, fd);
                        }
                }

        } else if (streq(key, "special")) {
                int fd, skip = 0;
                SocketPort *p;

                if (sscanf(value, "%i %n", &fd, &skip) < 1 || fd < 0 || !fdset_contains(fds, fd))
                        log_debug_unit(u->id,
                                       "Failed to parse special value %s", value);
                else {

                        IWLIST_FOREACH(port, p, s->ports)
                                if (p->type == SOCKET_SPECIAL &&
                                    streq_ptr(p->path, value+skip))
                                        break;

                        if (p) {
                                safe_close(p->fd);
                                p->fd = fdset_remove(fds, fd);
                        }
                }

        } else if (streq(key, "mqueue")) {
                int fd, skip = 0;
                SocketPort *p;

                if (sscanf(value, "%i %n", &fd, &skip) < 1 || fd < 0 || !fdset_contains(fds, fd))
                        log_debug_unit(u->id,
                                       "Failed to parse mqueue value %s", value);
                else {

                        IWLIST_FOREACH(port, p, s->ports)
                                if (p->type == SOCKET_MQUEUE &&
                                    streq_ptr(p->path, value+skip))
                                        break;

                        if (p) {
                                safe_close(p->fd);
                                p->fd = fdset_remove(fds, fd);
                        }
                }

        } else if (streq(key, "socket")) {
                int fd, type, skip = 0;
                SocketPort *p;

                if (sscanf(value, "%i %i %n", &fd, &type, &skip) < 2 || fd < 0 || type < 0 || !fdset_contains(fds, fd))
                        log_debug_unit(u->id,
                                       "Failed to parse socket value %s", value);
                else {

                        IWLIST_FOREACH(port, p, s->ports)
                                if (socket_address_is(&p->address, value+skip, type))
                                        break;

                        if (p) {
                                safe_close(p->fd);
                                p->fd = fdset_remove(fds, fd);
                        }
                }

#ifdef Have_linux_netlink_h
        } else if (streq(key, "netlink")) {
                int fd, skip = 0;
                SocketPort *p;

                if (sscanf(value, "%i %n", &fd, &skip) < 1 || fd < 0 || !fdset_contains(fds, fd))
                        log_debug_unit(u->id,
                                       "Failed to parse socket value %s", value);
                else {

                        IWLIST_FOREACH(port, p, s->ports)
                                if (socket_address_is_netlink(&p->address, value+skip))
                                        break;

                        if (p) {
                                safe_close(p->fd);
                                p->fd = fdset_remove(fds, fd);
                        }
                }
#endif
        } else if (streq(key, "tmp-dir")) {
                char *t;

                t = strdup(value);
                if (!t)
                        return log_oom();

                s->exec_context.tmp_dir = t;
        } else if (streq(key, "var-tmp-dir")) {
                char *t;

                t = strdup(value);
                if (!t)
                        return log_oom();

                s->exec_context.var_tmp_dir = t;
        } else
                log_debug_unit(UNIT(s)->id,
                               "Unknown serialization key '%s'", key);

        return 0;
}

static int socket_distribute_fds(Unit *u, FDSet *fds) {
        Socket *s = SOCKET(u);
        SocketPort *p;

        assert(u);

        IWLIST_FOREACH(port, p, s->ports) {
                Iterator i;
                int fd;

                if (p->type != SOCKET_SOCKET)
                        continue;

                if (p->fd >= 0)
                        continue;

                FDSET_FOREACH(fd, fds, i) {
                        if (socket_address_matches_fd(&p->address, fd)) {
                                p->fd = fdset_remove(fds, fd);
                                s->deserialized_state = SOCKET_LISTENING;
                                break;
                        }
                }
        }

        return 0;
}

_pure_ static UnitActiveState socket_active_state(Unit *u) {
        assert(u);

        return state_translation_table[SOCKET(u)->state];
}

_pure_ static const char *socket_sub_state_to_string(Unit *u) {
        assert(u);

        return socket_state_to_string(SOCKET(u)->state);
}

const char* socket_port_type_to_string(SocketPort *p) {

        assert(p);

        switch (p->type) {
                case SOCKET_SOCKET:
                        switch (p->address.type) {
                                case SOCK_STREAM: return "Stream";
                                case SOCK_DGRAM: return "Datagram";
                                case SOCK_SEQPACKET: return "SequentialPacket";
                                case SOCK_RAW:
#ifdef Have_linux_netlink_h
                                        if (socket_address_family(&p->address) == AF_NETLINK)
                                                return "Netlink";
#endif
                                default: return "Invalid";
                        }
                case SOCKET_SPECIAL: return "Special";
                case SOCKET_MQUEUE: return "MessageQueue";
                case SOCKET_FIFO: return "FIFO";
                default: return NULL;
        }
}

_pure_ static bool socket_check_gc(Unit *u) {
        Socket *s = SOCKET(u);

        assert(u);

        return s->n_connections > 0;
}

static void socket_fd_event(Unit *u, int fd, int revents, ev_io *w)
{
        Socket *s = SOCKET(u);
        int cfd = -1;

        assert(s);
        assert(fd >= 0);

        if (s->state != SOCKET_LISTENING)
                return;

        log_debug_unit(u->id, "Incoming traffic on %s", u->id);

        if (revents != EV_READ) {

                log_error_unit(u->id, "%s: Got unexpected poll event (0x%x) on socket.", u->id, revents);

                goto fail;
        }

#if 0 // FIXME: libev
        if (w->socket_accept) {
                for (;;) {

                        cfd = accept4(fd, NULL, NULL, SOCK_NONBLOCK);
                        if (cfd < 0) {

                                if (errno == EINTR)
                                        continue;

                                log_error_unit(u->id,
                                               "Failed to accept socket: %m");
                                goto fail;
                        }

                        break;
                }

                socket_apply_socket_options(s, cfd);
        }
#endif

        socket_enter_running(s, cfd);
        return;

fail:
        socket_enter_stop_pre(s, SOCKET_FAILURE_RESOURCES);
}

static void socket_sigchld_event(Unit *u, pid_t pid, int code, int status) {
        Socket *s = SOCKET(u);
        SocketResult f;

        assert(s);
        assert(pid >= 0);

        if (pid != s->control_pid)
                return;

        s->control_pid = 0;

        if (is_clean_exit(code, status, NULL))
                f = SOCKET_SUCCESS;
        else if (code == CLD_EXITED)
                f = SOCKET_FAILURE_EXIT_CODE;
        else if (code == CLD_KILLED)
                f = SOCKET_FAILURE_SIGNAL;
        else if (code == CLD_DUMPED)
                f = SOCKET_FAILURE_CORE_DUMP;
        else
                assert_not_reached("Unknown code");

        if (s->control_command) {
                exec_status_exit(&s->control_command->exec_status, &s->exec_context, pid, code, status);

                if (s->control_command->ignore)
                        f = SOCKET_SUCCESS;
        }

        log_full_unit(f == SOCKET_SUCCESS ? LOG_DEBUG : LOG_NOTICE,
                      u->id,
                      "%s control process exited, code=%s status=%i",
                      u->id, sigchld_code_to_string(code), status);

        if (f != SOCKET_SUCCESS)
                s->result = f;

        if (s->control_command &&
            s->control_command->command_next &&
            f == SOCKET_SUCCESS) {

                log_debug_unit(u->id,
                               "%s running next command for state %s",
                               u->id, socket_state_to_string(s->state));
                socket_run_next(s);
        } else {
                s->control_command = NULL;
                s->control_command_id = _SOCKET_EXEC_COMMAND_INVALID;

                /* No further commands for this step, so let's figure
                 * out what to do next */

                log_debug_unit(u->id,
                               "%s got final SIGCHLD for state %s",
                               u->id, socket_state_to_string(s->state));

                switch (s->state) {

                case SOCKET_START_PRE:
                        if (f == SOCKET_SUCCESS)
                                socket_enter_start_chown(s);
                        else
                                socket_enter_signal(s, SOCKET_FINAL_SIGTERM, f);
                        break;

                case SOCKET_START_CHOWN:
                        if (f == SOCKET_SUCCESS)
                                socket_enter_start_post(s);
                        else
                                socket_enter_stop_pre(s, f);
                        break;

                case SOCKET_START_POST:
                        if (f == SOCKET_SUCCESS)
                                socket_enter_listening(s);
                        else
                                socket_enter_stop_pre(s, f);
                        break;

                case SOCKET_STOP_PRE:
                case SOCKET_STOP_PRE_SIGTERM:
                case SOCKET_STOP_PRE_SIGKILL:
                        socket_enter_stop_post(s, f);
                        break;

                case SOCKET_STOP_POST:
                case SOCKET_FINAL_SIGTERM:
                case SOCKET_FINAL_SIGKILL:
                        socket_enter_dead(s, f);
                        break;

                default:
                        assert_not_reached("Uh, control process died at wrong time.");
                }
        }

        /* Notify clients about changed exit status */
        unit_add_to_dbus_queue(u);
}

static void socket_timer_event(Unit *u, uint64_t elapsed, ev_timer *w)
{
        Socket *s = SOCKET(u);

        assert(s);
        assert(elapsed == 1);
        assert(w == &s->timer_watch);

        switch (s->state) {

        case SOCKET_START_PRE:
                log_warning_unit(u->id,
                                 "%s starting timed out. Terminating.", u->id);
                socket_enter_signal(s, SOCKET_FINAL_SIGTERM, SOCKET_FAILURE_TIMEOUT);
                break;

        case SOCKET_START_CHOWN:
        case SOCKET_START_POST:
                log_warning_unit(u->id,
                                 "%s starting timed out. Stopping.", u->id);
                socket_enter_stop_pre(s, SOCKET_FAILURE_TIMEOUT);
                break;

        case SOCKET_STOP_PRE:
                log_warning_unit(u->id,
                                 "%s stopping timed out. Terminating.", u->id);
                socket_enter_signal(s, SOCKET_STOP_PRE_SIGTERM, SOCKET_FAILURE_TIMEOUT);
                break;

        case SOCKET_STOP_PRE_SIGTERM:
                if (s->kill_context.send_sigkill) {
                        log_warning_unit(u->id,
                                         "%s stopping timed out. Killing.", u->id);
                        socket_enter_signal(s, SOCKET_STOP_PRE_SIGKILL, SOCKET_FAILURE_TIMEOUT);
                } else {
                        log_warning_unit(u->id,
                                         "%s stopping timed out. Skipping SIGKILL. Ignoring.",
                                         u->id);
                        socket_enter_stop_post(s, SOCKET_FAILURE_TIMEOUT);
                }
                break;

        case SOCKET_STOP_PRE_SIGKILL:
                log_warning_unit(u->id,
                                 "%s still around after SIGKILL. Ignoring.", u->id);
                socket_enter_stop_post(s, SOCKET_FAILURE_TIMEOUT);
                break;

        case SOCKET_STOP_POST:
                log_warning_unit(u->id,
                                 "%s stopping timed out (2). Terminating.", u->id);
                socket_enter_signal(s, SOCKET_FINAL_SIGTERM, SOCKET_FAILURE_TIMEOUT);
                break;

        case SOCKET_FINAL_SIGTERM:
                if (s->kill_context.send_sigkill) {
                        log_warning_unit(u->id,
                                         "%s stopping timed out (2). Killing.", u->id);
                        socket_enter_signal(s, SOCKET_FINAL_SIGKILL, SOCKET_FAILURE_TIMEOUT);
                } else {
                        log_warning_unit(u->id,
                                         "%s stopping timed out (2). Skipping SIGKILL. Ignoring.",
                                         u->id);
                        socket_enter_dead(s, SOCKET_FAILURE_TIMEOUT);
                }
                break;

        case SOCKET_FINAL_SIGKILL:
                log_warning_unit(u->id,
                                 "%s still around after SIGKILL (2). Entering failed mode.",
                                 u->id);
                socket_enter_dead(s, SOCKET_FAILURE_TIMEOUT);
                break;

        default:
                assert_not_reached("Timeout at wrong time.");
        }
}

int socket_collect_fds(Socket *s, int **fds, unsigned *n_fds) {
        int *rfds;
        unsigned rn_fds, k;
        SocketPort *p;

        assert(s);
        assert(fds);
        assert(n_fds);

        /* Called from the service code for requesting our fds */

        rn_fds = 0;
        IWLIST_FOREACH(port, p, s->ports)
                if (p->fd >= 0)
                        rn_fds++;

        if (rn_fds <= 0) {
                *fds = NULL;
                *n_fds = 0;
                return 0;
        }

        if (!(rfds = new(int, rn_fds)))
                return -ENOMEM;

        k = 0;
        IWLIST_FOREACH(port, p, s->ports)
                if (p->fd >= 0)
                        rfds[k++] = p->fd;

        assert(k == rn_fds);

        *fds = rfds;
        *n_fds = rn_fds;

        return 0;
}

static void socket_notify_service_dead(Socket *s, bool failed_permanent) {
        assert(s);

        /* The service is dead. Dang!
         *
         * This is strictly for one-instance-for-all-connections
         * services. */

        if (s->state == SOCKET_RUNNING) {
                log_debug_unit(UNIT(s)->id,
                               "%s got notified about service death (failed permanently: %s)",
                               UNIT(s)->id, yes_no(failed_permanent));
                if (failed_permanent)
                        socket_enter_stop_pre(s, SOCKET_FAILURE_SERVICE_FAILED_PERMANENT);
                else
                        socket_enter_listening(s);
        }
}

void socket_connection_unref(Socket *s) {
        assert(s);

        /* The service is dead. Yay!
         *
         * This is strictly for one-instance-per-connection
         * services. */

        assert(s->n_connections > 0);
        s->n_connections--;

        log_debug_unit(UNIT(s)->id,
                       "%s: One connection closed, %u left.", UNIT(s)->id, s->n_connections);
}

static void socket_reset_failed(Unit *u) {
        Socket *s = SOCKET(u);

        assert(s);

        if (s->state == SOCKET_FAILED)
                socket_set_state(s, SOCKET_DEAD);

        s->result = SOCKET_SUCCESS;
}

static void socket_trigger_notify(Unit *u, Unit *other) {
        Socket *s = SOCKET(u);
        Service *se = SERVICE(other);

        assert(u);
        assert(other);

        /* Don't propagate state changes from the service if we are
           already down or accepting connections */
        if ((s->state !=  SOCKET_RUNNING &&
            s->state != SOCKET_LISTENING) ||
            s->accept)
                return;

        if (other->load_state != UNIT_LOADED ||
            other->type != UNIT_SERVICE)
                return;

        if (se->state == SERVICE_FAILED)
                socket_notify_service_dead(s, se->result == SERVICE_FAILURE_START_LIMIT);

        if (se->state == SERVICE_DEAD ||
            se->state == SERVICE_STOP ||
            se->state == SERVICE_STOP_SIGTERM ||
            se->state == SERVICE_STOP_SIGKILL ||
            se->state == SERVICE_STOP_POST ||
            se->state == SERVICE_FINAL_SIGTERM ||
            se->state == SERVICE_FINAL_SIGKILL ||
            se->state == SERVICE_AUTO_RESTART)
                socket_notify_service_dead(s, false);

        if (se->state == SERVICE_RUNNING)
                socket_set_state(s, SOCKET_RUNNING);
}

static int socket_kill(Unit *u, KillWho who, int signo, DBusError *error) {
        return unit_kill_common(u, who, signo, -1, SOCKET(u)->control_pid, error);
}

static int socket_get_timeout(Unit *u, usec_t *timeout)
{
        Socket *s = SOCKET(u);
        int r;

        if (!ev_is_active(&s->timer_watch))
                return 0;

        *timeout = (ev_now(u->manager->evloop) + ev_timer_remaining(u->manager->evloop, &s->timer_watch)) *
                USEC_PER_SEC;

        return 1;
}

static const char* const socket_state_table[_SOCKET_STATE_MAX] = {
        [SOCKET_DEAD] = "dead",
        [SOCKET_START_PRE] = "start-pre",
        [SOCKET_START_CHOWN] = "start-chown",
        [SOCKET_START_POST] = "start-post",
        [SOCKET_LISTENING] = "listening",
        [SOCKET_RUNNING] = "running",
        [SOCKET_STOP_PRE] = "stop-pre",
        [SOCKET_STOP_PRE_SIGTERM] = "stop-pre-sigterm",
        [SOCKET_STOP_PRE_SIGKILL] = "stop-pre-sigkill",
        [SOCKET_STOP_POST] = "stop-post",
        [SOCKET_FINAL_SIGTERM] = "final-sigterm",
        [SOCKET_FINAL_SIGKILL] = "final-sigkill",
        [SOCKET_FAILED] = "failed"
};

DEFINE_STRING_TABLE_LOOKUP(socket_state, SocketState);

static const char* const socket_exec_command_table[_SOCKET_EXEC_COMMAND_MAX] = {
        [SOCKET_EXEC_START_PRE] = "StartPre",
        [SOCKET_EXEC_START_CHOWN] = "StartChown",
        [SOCKET_EXEC_START_POST] = "StartPost",
        [SOCKET_EXEC_STOP_PRE] = "StopPre",
        [SOCKET_EXEC_STOP_POST] = "StopPost"
};

DEFINE_STRING_TABLE_LOOKUP(socket_exec_command, SocketExecCommand);

static const char* const socket_result_table[_SOCKET_RESULT_MAX] = {
        [SOCKET_SUCCESS] = "success",
        [SOCKET_FAILURE_RESOURCES] = "resources",
        [SOCKET_FAILURE_TIMEOUT] = "timeout",
        [SOCKET_FAILURE_EXIT_CODE] = "exit-code",
        [SOCKET_FAILURE_SIGNAL] = "signal",
        [SOCKET_FAILURE_CORE_DUMP] = "core-dump",
        [SOCKET_FAILURE_SERVICE_FAILED_PERMANENT] = "service-failed-permanent"
};

DEFINE_STRING_TABLE_LOOKUP(socket_result, SocketResult);

const UnitVTable socket_vtable = {
        .object_size = sizeof(Socket),

        .sections =
                "Unit\0"
                "Socket\0"
                "Install\0",

        .private_section = "Socket",
        .exec_context_offset = offsetof(Socket, exec_context),
#ifdef Use_CGroups
        .cgroup_context_offset = offsetof(Socket, cgroup_context),
#endif

        .init = socket_init,
        .done = socket_done,
        .load = socket_load,

        .kill = socket_kill,

        .coldplug = socket_coldplug,

        .dump = socket_dump,

        .start = socket_start,
        .stop = socket_stop,

        .get_timeout = socket_get_timeout,

        .serialize = socket_serialize,
        .deserialize_item = socket_deserialize_item,
        .distribute_fds = socket_distribute_fds,

        .active_state = socket_active_state,
        .sub_state_to_string = socket_sub_state_to_string,

        .check_gc = socket_check_gc,

        .fd_event = socket_fd_event,
        .sigchld_event = socket_sigchld_event,
        .timer_event = socket_timer_event,

        .trigger_notify = socket_trigger_notify,

        .reset_failed = socket_reset_failed,

        .bus_interface = "org.freedesktop.systemd1.Socket",
        .bus_message_handler = bus_socket_message_handler,
        .bus_invalidating_properties =  bus_socket_invalidating_properties,
        .bus_set_property = bus_socket_set_property,
        .bus_commit_properties = bus_socket_commit_properties,

        .status_message_formats = {
                /*.starting_stopping = {
                        [0] = "Starting socket %s...",
                        [1] = "Stopping socket %s...",
                },*/
                .finished_start_job = {
                        [JOB_DONE]       = "Listening on %s.",
                        [JOB_FAILED]     = "Failed to listen on %s.",
                        [JOB_DEPENDENCY] = "Dependency failed for %s.",
                        [JOB_TIMEOUT]    = "Timed out starting %s.",
                },
                .finished_stop_job = {
                        [JOB_DONE]       = "Closed %s.",
                        [JOB_FAILED]     = "Failed stopping %s.",
                        [JOB_TIMEOUT]    = "Timed out stopping %s.",
                },
        },
};
