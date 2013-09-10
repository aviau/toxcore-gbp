/* network.h
 *
 * Functions for the core networking.
 *
 *  Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "network.h"

/*  return current UNIX time in microseconds (us). */
uint64_t current_time(void)
{
    uint64_t time;
#ifdef WIN32
    /* This probably works fine */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    time = ft.dwHighDateTime;
    time <<= 32;
    time |= ft.dwLowDateTime;
    time -= 116444736000000000UL;
    return time / 10;
#else
    struct timeval a;
    gettimeofday(&a, NULL);
    time = 1000000UL * a.tv_sec + a.tv_usec;
    return time;
#endif
}

/*  return a random number.
 * NOTE: This function should probably not be used where cryptographic randomness is absolutely necessary.
 */
uint32_t random_int(void)
{
#ifndef VANILLA_NACL
    /* NOTE: this function comes from libsodium. */
    return randombytes_random();
#else
    return random();
#endif
}

/* Basic network functions:
 * Function to send packet(data) of length length to ip_port.
 */
int sendpacket(Networking_Core *net, IP_Port ip_port, uint8_t *data, uint32_t length)
{
#ifdef TOX_ENABLE_IPV6
    /* socket AF_INET, but target IP NOT: can't send */
   if ((net->family == AF_INET) && (ip_port.ip.family != AF_INET))
        return 0;
#endif
 
    struct sockaddr_storage addr;
    size_t addrsize = 0;

#ifdef TOX_ENABLE_IPV6
    if (ip_port.ip.family == AF_INET) {
        if (net->family == AF_INET6) {
            /* must convert to IPV4-in-IPV6 address */
            addrsize = sizeof(struct sockaddr_in6);
            struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
            addr6->sin6_family = AF_INET6;
            addr6->sin6_port = ip_port.port;

            /* there should be a macro for this in a standards compliant
             * environment, not found */
            addr6->sin6_addr.s6_addr32[0] = 0;
            addr6->sin6_addr.s6_addr32[1] = 0;
            addr6->sin6_addr.s6_addr32[2] = htonl(0xFFFF);
            addr6->sin6_addr.s6_addr32[3] = ip_port.ip.ip4.uint32;

            addr6->sin6_flowinfo = 0;
            addr6->sin6_scope_id = 0;
        }
        else {
            IP4 ip4 = ip_port.ip.ip4;
#else
            IP4 ip4 = ip_port.ip;
#endif
            addrsize = sizeof(struct sockaddr_in);
            struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
            addr4->sin_family = AF_INET;
            addr4->sin_addr = ip4.in_addr;
            addr4->sin_port = ip_port.port;
#ifdef TOX_ENABLE_IPV6
        }
    }
    else if (ip_port.ip.family == AF_INET6) {
        addrsize = sizeof(struct sockaddr_in6);
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = ip_port.port;
        addr6->sin6_addr = ip_port.ip.ip6;

        addr6->sin6_flowinfo = 0;
        addr6->sin6_scope_id = 0;
    } else {
        /* unknown address type*/
        return 0;
    }
#endif

    return sendto(net->sock, (char *) data, length, 0, (struct sockaddr *)&addr, addrsize);
}

/* Function to receive data
 *  ip and port of sender is put into ip_port.
 *  Packet data is put into data.
 *  Packet length is put into length.
 *  Dump all empty packets.
 */
#ifdef WIN32
static int receivepacket(unsigned int sock, IP_Port *ip_port, uint8_t *data, uint32_t *length)
#else
static int receivepacket(int sock, IP_Port *ip_port, uint8_t *data, uint32_t *length)
#endif
{
    struct sockaddr_storage addr;
#ifdef WIN32
    int addrlen = sizeof(addr);
#else
    uint32_t addrlen = sizeof(addr);
#endif
    (*(int32_t *)length) = recvfrom(sock, (char *) data, MAX_UDP_PACKET_SIZE, 0, (struct sockaddr *)&addr, &addrlen);

    if (*(int32_t *)length <= 0)
        return -1; /* Nothing received or empty packet. */

#ifdef TOX_ENABLE_IPV6
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
        ip_port->ip.family = addr_in->sin_family;
        ip_port->ip.ip4.in_addr = addr_in->sin_addr;
        ip_port->port = addr_in->sin_port;
    }
    else if (addr.ss_family == AF_INET6) {
        struct sockaddr_in6 *addr_in6 = (struct sockaddr_in6 *)&addr;
        ip_port->ip.family = addr_in6->sin6_family;
        ip_port->ip.ip6 = addr_in6->sin6_addr;
        ip_port->port = addr_in6->sin6_port;
    }
    else
        return -1;
#else
    if (addr.ss_family == AF_INET) {
        struct sockaddr_in *addr_in = (struct sockaddr_in *)&addr;
        ip_port->ip.in_addr = addr_in->sin_addr;
        ip_port->port = addr_in->sin_port;
    }
    else
        return -1;
#endif

    return 0;
}

void networking_registerhandler(Networking_Core *net, uint8_t byte, packet_handler_callback cb, void *object)
{
    net->packethandlers[byte].function = cb;
    net->packethandlers[byte].object = object;
}

void networking_poll(Networking_Core *net)
{
    IP_Port ip_port;
    uint8_t data[MAX_UDP_PACKET_SIZE];
    uint32_t length;

    while (receivepacket(net->sock, &ip_port, data, &length) != -1) {
        if (length < 1) continue;

        if (!(net->packethandlers[data[0]].function)) continue;

        net->packethandlers[data[0]].function(net->packethandlers[data[0]].object, ip_port, data, length);
    }
}

uint8_t at_startup_ran;
static int at_startup(void)
{
    if (at_startup_ran != 0)
        return 0;

#ifdef WIN32
    WSADATA wsaData;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != NO_ERROR)
        return -1;

#else
    srandom((uint32_t)current_time());
#endif
    srand((uint32_t)current_time());
    at_startup_ran = 1;
    return 0;
}

/* TODO: Put this somewhere
static void at_shutdown(void)
{
#ifdef WIN32
    WSACleanup();
#endif
}
*/

/* Initialize networking.
 * Bind to ip and port.
 * ip must be in network order EX: 127.0.0.1 = (7F000001).
 * port is in host byte order (this means don't worry about it).
 *
 *  return Networking_Core object if no problems
 *  return NULL if there are problems.
 */
Networking_Core *new_networking(IP ip, uint16_t port)
{
#ifdef TOX_ENABLE_IPV6
    /* maybe check for invalid IPs like 224+.x.y.z? if there is any IP set ever */
    if (ip.family != AF_INET && ip.family != AF_INET6)
        return NULL;
#endif

    if (at_startup() != 0)
        return NULL;

    Networking_Core *temp = calloc(1, sizeof(Networking_Core));
    if (temp == NULL)
        return NULL;

    sa_family_t family = 0;
#ifdef TOX_ENABLE_IPV6
    family = ip.family;
#else
    family = AF_INET;
#endif
    temp->family = family;
    temp->port = 0;

    /* Initialize our socket. */
    /* add log message what we're creating */
    temp->sock = socket(temp->family, SOCK_DGRAM, IPPROTO_UDP);

    /* Check for socket error. */
#ifdef WIN32

    if (temp->sock == INVALID_SOCKET) { /* MSDN recommends this. */
        free(temp);
        return NULL;
    }

#else

    if (temp->sock < 0) {
        free(temp);
        return NULL;
    }

#endif

    /* Functions to increase the size of the send and receive UDP buffers.
     * NOTE: Uncomment if necessary.
     */
    /*
    int n = 1024 * 1024 * 2;
    if(setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&n, sizeof(n)) == -1)
    {
        return -1;
    }

    if(setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&n, sizeof(n)) == -1)
        return -1;
    */

    /* Enable broadcast on socket? */
    int broadcast = 1;
    setsockopt(temp->sock, SOL_SOCKET, SO_BROADCAST, (char *)&broadcast, sizeof(broadcast));

    /* Set socket nonblocking. */
#ifdef WIN32
    /* I think this works for Windows. */
    u_long mode = 1;
    /* ioctl(sock, FIONBIO, &mode); */
    ioctlsocket(temp->sock, FIONBIO, &mode);
#else
    fcntl(temp->sock, F_SETFL, O_NONBLOCK, 1);
#endif

    /* Bind our socket to port PORT and address 0.0.0.0 */
    uint16_t *portptr = NULL;
    struct sockaddr_storage addr;
    size_t addrsize;
#ifdef TOX_ENABLE_IPV6
    if (temp->family == AF_INET)
    {
        IP4 ip4 = ip.ip4;
#else
        IP4 ip4 = ip;
#endif
        addrsize = sizeof(struct sockaddr_in);
        struct sockaddr_in *addr4 = (struct sockaddr_in *)&addr;
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(port);
        addr4->sin_addr = ip4.in_addr;

        portptr = &addr4->sin_port;
#ifdef TOX_ENABLE_IPV6
    }
    else if (temp->family == AF_INET6)
    {
        addrsize = sizeof(struct sockaddr_in6);
        struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)&addr;
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(port);
        addr6->sin6_addr = ip.ip6;

        addr6->sin6_flowinfo = 0;
        addr6->sin6_scope_id = 0;

        portptr = &addr6->sin6_port;
    }
    else
        return NULL;

    if (ip.family == AF_INET6) {
        char ipv6only = 0;
        int res = setsockopt(temp->sock, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&ipv6only, sizeof(ipv6only));
        if (res < 0) {
            /* add log message*/
        }
    }
#endif

    /* a hanging program or a different user might block the standard port;
     * as long as it isn't a parameter coming from the commandline,
     * try a few ports after it, to see if we can find a "free" one
     */
    int tries, res;
    for(tries = 0; tries < 9; tries++)
    {
        res = bind(temp->sock, (struct sockaddr *)&addr, addrsize);
        if (!res)
        {
            temp->port = *portptr;
            return temp;
        }

        uint16_t port = ntohs(*portptr);
        port++;
        *portptr = htons(port);
    }

    printf("Failed to bind socket: %s (IP/Port: %s:%u\n", strerror(errno), ip_ntoa(&ip), port);
    free(temp);
    return NULL;
}

/* Function to cleanup networking stuff. */
void kill_networking(Networking_Core *net)
{
#ifdef WIN32
    closesocket(net->sock);
#else
    close(net->sock);
#endif
    free(net);
    return;
}

/* ip_equal
 *  compares two IPAny structures
 *  unset means unequal
 *
 * returns 0 when not equal or when uninitialized
 */
int ip_equal(IP *a, IP *b)
{
    if (!a || !b)
        return 0;

#ifdef TOX_ENABLE_IPV6
    if (a->family == AF_INET)
        return (a->ip4.in_addr.s_addr == b->ip4.in_addr.s_addr);

    if (a->family == AF_INET6)
        return IN6_ARE_ADDR_EQUAL(&a->ip6, &b->ip6);

    return 0;
#else
    return (a->uint32 == b->uint32);
#endif
};

/* ipport_equal
 *  compares two IPAny_Port structures
 *  unset means unequal
 *
 * returns 0 when not equal or when uninitialized
 */
int ipport_equal(IP_Port *a, IP_Port *b)
{
    if (!a || !b)
        return 0;

    if (!a->port || (a->port != b->port))
        return 0;

    return ip_equal(&a->ip, &b->ip);
};

/* nulls out ip */
void ip_reset(IP *ip)
{
    if (!ip)
        return;

#ifdef TOX_ENABLE_IPV6
    memset(ip, 0, sizeof(*ip));
#else
    ip->uint32 = 0;
#endif
};

/* nulls out ip, sets family according to flag */
void ip_init(IP *ip, uint8_t ipv6enabled)
{
    if (!ip)
        return;

#ifdef TOX_ENABLE_IPV6
    memset(ip, 0, sizeof(ip));
    ip->family = ipv6enabled ? AF_INET6 : AF_INET;
#else
    ip->uint32 = 0;
#endif
};

/* checks if ip is valid */
int ip_isset(IP *ip)
{
    if (!ip)
        return 0;

#ifdef TOX_ENABLE_IPV6
    return (ip->family != 0);
#else
    return (ip->uint32 != 0);
#endif
};

/* checks if ip is valid */
int ipport_isset(IP_Port *ipport)
{
    if (!ipport)
        return 0;

    if (!ipport->port)
        return 0;

    return ip_isset(&ipport->ip);
};

/* copies an ip structure (careful about direction!) */
void ip_copy(IP *target, IP *source)
{
    if (!source || !target)
        return;

    memcpy(target, source, sizeof(IP));
};

/* copies an ip_port structure (careful about direction!) */
void ipport_copy(IP_Port *target, IP_Port *source)
{
    if (!source || !target)
        return;

    memcpy(target, source, sizeof(IP_Port));
};

/* ip_ntoa
 *   converts ip into a string
 *   uses a static buffer, so mustn't used multiple times in the same output
 */
/* there would be INET6_ADDRSTRLEN, but it might be too short for the error message */
static char addresstext[96];
const char *ip_ntoa(IP *ip)
{
    if (ip) {
#ifdef TOX_ENABLE_IPV6
        if (ip->family == AF_INET) {
            addresstext[0] = 0;
            struct in_addr *addr = (struct in_addr *)&ip->ip4;
            inet_ntop(ip->family, addr, addresstext, sizeof(addresstext));
        }
        else if (ip->family == AF_INET6) {
            addresstext[0] = '[';
            struct in6_addr *addr = (struct in6_addr *)&ip->ip6;
            inet_ntop(ip->family, addr, &addresstext[1], sizeof(addresstext) - 3);
            size_t len = strlen(addresstext);
            addresstext[len] = ']';
            addresstext[len + 1] = 0;
        }
        else
            snprintf(addresstext, sizeof(addresstext), "(IP invalid, family %u)", ip->family);
#else
        addresstext[0] = 0;
        struct in_addr *addr = (struct in_addr *)&ip;
        inet_ntop(AF_INET, addr, addresstext, sizeof(addresstext));
#endif
    }
    else
        snprintf(addresstext, sizeof(addresstext), "(IP invalid: NULL)");

    addresstext[INET6_ADDRSTRLEN + 2] = 0;
    return addresstext;
};

/*
 * addr_parse_ip
 *  directly parses the input into an IP structure
 *  tries IPv4 first, then IPv6
 *
 * input
 *  address: dotted notation (IPv4: quad, IPv6: 16) or colon notation (IPv6)
 *
 * output
 *  IP: family and the value is set on success
 *
 * returns 1 on success, 0 on failure
 */

int addr_parse_ip(const char *address, IP *to)
{
    if (!address || !to)
        return 0;

#ifdef TOX_ENABLE_IPV6
    struct in_addr addr4;
    if (1 == inet_pton(AF_INET, address, &addr4)) {
        to->family = AF_INET;
        to->ip4.in_addr = addr4;
        return 1;
    };

    struct in6_addr addr6;
    if (1 == inet_pton(AF_INET6, address, &addr6)) {
        to->family = AF_INET6;
        to->ip6 = addr6;
        return 1;
    };
#else
    struct in_addr addr4;
    if (1 == inet_pton(AF_INET, address, &addr4)) {
        to->in_addr = addr4;
        return 1;
    };
#endif

    return 0;
};

/*
 * addr_resolve():
 *  uses getaddrinfo to resolve an address into an IP address
 *  uses the first IPv4/IPv6 addresses returned by getaddrinfo
 *
 * input
 *  address: a hostname (or something parseable to an IP address)
 *  ip: ip.family MUST be initialized, either set to a specific IP version
 *     (AF_INET/AF_INET6) or to the unspecified AF_UNSPEC (= 0), if both
 *     IP versions are acceptable
 *
 * returns in ip a valid IPAny (v4/v6),
 *     prefers v6 if ip.family was AF_UNSPEC and both available
 * returns 0 on failure
 */

int addr_resolve(const char *address, IP *to)
{
    if (!address || !to)
        return 0;

    sa_family_t family;
#ifdef TOX_ENABLE_IPV6
    family = to->family;
#else
    family = AF_INET;
#endif

    struct addrinfo *server = NULL;
    struct addrinfo *walker = NULL;
    struct addrinfo  hints;
    int              rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = family;
    hints.ai_socktype = SOCK_DGRAM; // type of socket Tox uses.

#ifdef __WIN32__
    WSADATA wsa_data;

    /* CLEANUP: really not the best place to put this */
    rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);

    if (rc != 0) {
        return 0;
    }

#endif

    rc = getaddrinfo(address, NULL, &hints, &server);
    // Lookup failed.
    if (rc != 0) {
#ifdef __WIN32__
        WSACleanup();
#endif
        return 0;
    }

#ifdef TOX_ENABLE_IPV6
    IP4 ip4;
    memset(&ip4, 0, sizeof(ip4));
    IP6 ip6;
    memset(&ip6, 0, sizeof(ip6));
#endif

    walker = server;
    while (walker && (rc != 3)) {
        if (family != AF_UNSPEC) {
            if (walker->ai_family == family) {
                if (family == AF_INET) {
                    if (walker->ai_addrlen == sizeof(struct sockaddr_in)) {
                        struct sockaddr_in *addr = (struct sockaddr_in *)walker->ai_addr;
#ifdef TOX_ENABLE_IPV6
                        to->ip4.in_addr = addr->sin_addr;
#else
                        to->in_addr = addr->sin_addr;
#endif
                        rc = 3;
                    }
                }
#ifdef TOX_ENABLE_IPV6
                else if (family == AF_INET6) {
                    if (walker->ai_addrlen == sizeof(struct sockaddr_in6)) {
                        struct sockaddr_in6 *addr = (struct sockaddr_in6 *)walker->ai_addr;
                        to->ip6 = addr->sin6_addr;
                        rc = 3;
                    }
                }
#endif
            }
        }
#ifdef TOX_ENABLE_IPV6
        else {
            if (walker->ai_family == AF_INET) {
                if (walker->ai_addrlen == sizeof(struct sockaddr_in)) {
                    struct sockaddr_in *addr = (struct sockaddr_in *)walker->ai_addr;
                    ip4.in_addr = addr->sin_addr;
                    rc |= 1;
                }
            }
            else if (walker->ai_family == AF_INET6) {
                if (walker->ai_addrlen == sizeof(struct sockaddr_in6)) {
                    struct sockaddr_in6 *addr = (struct sockaddr_in6 *)walker->ai_addr;
                    ip6 = addr->sin6_addr;
                    rc |= 2;
                }
            }
        }
#endif

        walker = walker->ai_next;
    }

#ifdef TOX_ENABLE_IPV6
    if (to->family == AF_UNSPEC) {
        if (rc & 2) {
            to->family = AF_INET6;
            to->ip6 = ip6;
        }
        else if (rc & 1) {
            to->family = AF_INET;
            to->ip4 = ip4;
        }
        else
            rc = 0;
    }
#endif

    
    freeaddrinfo(server);
#ifdef __WIN32__
    WSACleanup();
#endif
    return rc;
}

/*
 * addr_resolve_or_parse_ip
 *  resolves string into an IP address
 *
 * to->family MUST be set (AF_UNSPEC, AF_INET, AF_INET6)
 * returns 1 on success, 0 on failure
 */
int addr_resolve_or_parse_ip(const char *address, IP *to)
{
    if (!addr_resolve(address, to))
        if (!addr_parse_ip(address, to))
            return 0;

    return 1;
};
