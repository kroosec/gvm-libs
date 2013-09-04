/* openvas-libraries/base
 * $Id$
 * Description: Implementation of API to handle Hosts objects
 *
 * Authors:
 * Hani Benhabiles <hani.benhabiles@greenbone.net>
 * Jan-Oliver Wagner <jan-oliver.wagner@greenbone.net>
 *
 * Copyright:
 * Copyright (C) 2013 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2,
 * or, at your option, any later version as published by the Free
 * Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file hosts.c
 * @brief Implementation of an API to handle Hosts objects
 *
 * This file contains all methods to handle Hosts collections (openvas_hosts_t)
 * and single hosts objects (openvas_host_t.)
 *
 * The module consequently uses glib datatypes.
 */

#include "openvas_hosts.h"

/* Static variables */

gchar *host_type_str[HOST_TYPE_MAX] = {
  [HOST_TYPE_NAME] = "Hostname",
  [HOST_TYPE_IPV4] = "IPv4",
  [HOST_TYPE_IPV6] = "IPv6",
  [HOST_TYPE_CIDR_BLOCK] = "IPv4 CIDR block",
  [HOST_TYPE_RANGE_SHORT] = "IPv4 short range",
  [HOST_TYPE_RANGE_LONG] = "IPv4 long range"
};

/* Function definitions */

/**
 * @brief Checks if a buffer points to a valid IPv4 address.
 * "192.168.11.1" is valid, "192.168.1.300" and "192.168.1.1e" are not.
 *
 * @param[in]   str Buffer to check in.
 *
 * @return 1 if valid IPv4 address, 0 otherwise.
 */
static int
is_ipv4_address (const char *str)
{
  struct sockaddr_in sa;

  return inet_pton(AF_INET, str, &(sa.sin_addr)) == 1;
}

/**
 * @brief Checks if a buffer points to a valid IPv6 address.
 * "0:0:0:0:0:0:0:1", "::1" and "::FFFF:192.168.13.55" are valid "::1g" is not.
 *
 * @param[in]   str Buffer to check in.
 *
 * @return 1 if valid IPv6 address, 0 otherwise.
 */
static int
is_ipv6_address (const char *str)
{
  struct sockaddr_in6 sa6;

  return inet_pton(AF_INET6, str, &(sa6.sin6_addr)) == 1;
}

/**
 * @brief Checks if a buffer points to an IPv4 CIDR-exprpessed block.
 * "192.168.12.3/24" is valid, "192.168.1.3/31" is not.
 *
 * @param[in]   str Buffer to check in.
 *
 * @return 1 if valid CIDR-expressed block, 0 otherwise.
 */
static int
is_cidr_block (const char *str)
{
  long block;
  char *addr_str, *block_str, *p;

  addr_str = g_strdup (str);
  block_str = strchr (addr_str, '/');
  if (block_str == NULL)
    {
      g_free (addr_str);
      return 0;
    }

  /* Separate the address from the block value. */
  *block_str = '\0';
  block_str++;

  if (!is_ipv4_address (addr_str) || !isdigit (*block_str))
    {
      g_free (addr_str);
      return 0;
    }

  p = NULL;
  block = strtol (block_str, &p, 10);
  g_free (addr_str);

  if (*p || block <= 0 || block > 30)
    return 0;

  return 1;
}

/**
 * @brief Gets the network block value from a CIDR-expressed block string.
 * For "192.168.1.1/24" it is 24.
 *
 * @param[in]   str     Buffer containing CIDR-expressed block.
 * @param[out]  block   Variable to store block value.
 *
 * @return -1 if error, 0 otherwise.
 */
static int
cidr_get_block (const char *str, unsigned int *block)
{
  if (str == NULL || block == NULL)
    return -1;

  if (sscanf (str, "%*[0-9.]/%2u", block)
      != 1)
    return -1;

  return 0;
}

/**
 * @brief Gets the IPv4 value from a CIDR-expressed block.
 * eg. For "192.168.1.10/24" it is "192.168.1.10".
 *
 * @param[in]   str     String containing CIDR-expressed block.
 * @param[out]  addr    Variable to store the IPv4 address value.
 *
 * @return -1 if error, 0 otherwise.
 */
static int
cidr_get_ip (const char *str, struct in_addr *addr)
{
  gchar *addr_str, *tmp;

  if (str == NULL || addr == NULL)
    return -1;

  addr_str = g_strdup (str);
  tmp = strchr (addr_str, '/');
  if (tmp == NULL)
    return -1;
  *tmp = '\0';

  if (inet_pton (AF_INET, addr_str, addr) != 1)
    return -1;

  g_free (addr_str);
  return 0;
}

/**
 * @brief Gets the first and last usable IPv4 addresses from a CIDR-expressed
 * block. eg. "192.168.1.0/24 would give 192.168.1.1 as first and 192.168.1.254
 * as last. Thus, it skips the network and broadcast addresses.
 *
 * @param[in]   str     Buffer containing CIDR-expressed block.
 * @param[out]  first   First IPv4 address in block.
 * @param[out]  last    Last IPv4 address in block.
 *
 * @return -1 if error, 0 else.
 */
static int
cidr_block_ips (const char *str, struct in_addr *first, struct in_addr *last)
{
  unsigned int block;

  if (str == NULL || first == NULL || last == NULL)
    return -1;

  /* Get IP and block values. */
  if (cidr_get_block (str, &block) == -1)
    return -1;
  if (cidr_get_ip (str, first) == -1)
    return -1;

  /* First IP: And with mask and increment. */
  first->s_addr &= htonl (0xffffffff ^ ((1 << (32 - block)) - 1));
  first->s_addr = htonl (ntohl (first->s_addr) + 1);

  /* Last IP: First IP + Number of usable hosts - 1. */
  last->s_addr = htonl (ntohl (first->s_addr) + (1 << (32 - block)) - 3);
  return 0;
}

/**
 * @brief Checks if a buffer points to a valid long range-expressed network.
 * "192.168.12.1-192.168.13.50" is valid.
 *
 * @param[in]   str Buffer to check in.
 *
 * @return 1 if valid long range-expressed network, 0 otherwise.
 */
static int
is_long_range_network (const char *str)
{
  char *first_str, *second_str;
  int ret;

  first_str = g_strdup (str);
  second_str = strchr (first_str, '-');
  if (second_str == NULL)
    {
      g_free (first_str);
      return 0;
    }

  /* Separate the addreses. */
  *second_str = '\0';
  second_str++;

  ret = is_ipv4_address (first_str) && is_ipv4_address (second_str);
  g_free (first_str);

  return ret;
}

/**
 * @brief Gets the first and last IPv4 addresses from a long range-expressed
 * network. eg. "192.168.1.1-192.168.2.40 would give 192.168.1.1 as first and
 * 192.168.2.40 as last.
 *
 * @param[in]   str     String containing long range-expressed network.
 * @param[out]  first   First IP address in block.
 * @param[out]  last    Last IP address in block.
 *
 * @return -1 if error, 0 else.
 */
static int
long_range_network_ips (const char *str, struct in_addr *first,
                        struct in_addr *last)
{
  char *first_str, *last_str;

  if (str == NULL || first == NULL || last == NULL)
    return -1;

  first_str = g_strdup (str);
  last_str = strchr (first_str, '-');
  if (last_str == NULL)
    {
      g_free (first_str);
      return -1;
    }

  /* Separate the two IPs. */
  *last_str = '\0';
  last_str++;

  if (inet_pton (AF_INET, first_str, first) != 1
      || inet_pton (AF_INET, last_str, last) != 1)
  {
    g_free (first_str);
    return -1;
  }

  g_free (first_str);
  return 0;
}

/**
 * @brief Checks if a buffer points to a valid short range-expressed network.
 * "192.168.11.1-50" is valid, "192.168.1.1-50e" and "192.168.1.1-300" are not.
 *
 * @param   str String to check in.
 *
 * @return 1 if str points to a valid short range-network, 0 otherwise.
 */
static int
is_short_range_network (const char *str)
{
  long end;
  char *ip_str, *end_str, *p;

  ip_str = g_strdup (str);
  end_str = strchr (ip_str, '-');
  if (end_str == NULL)
    {
      g_free (ip_str);
      return 0;
    }

  /* Separate the addreses. */
  *end_str = '\0';
  end_str++;

  if (!is_ipv4_address (ip_str) || !isdigit (*end_str))
    {
      g_free (ip_str);
      return 0;
    }

  p = NULL;
  end = strtol (end_str, &p, 10);
  g_free (ip_str);

  if (*p || end < 0 || end > 255)
    return 0;

  return 1;
}

/**
 * @brief Gets the first and last IPv4 addresses from a short range-expressed
 * network. "192.168.1.1-40 would give 192.168.1.1 as first and 192.168.1.40 as
 * last.
 *
 * @param[in]   str     String containing short range-expressed network.
 * @param[out]  first   First IP address in block.
 * @param[out]  last    Last IP address in block.
 *
 * @return -1 if error, 0 else.
 */
static int
short_range_network_ips (const char *str, struct in_addr *first,
                         struct in_addr *last)
{
  char *first_str, *last_str;
  int end;

  if (str == NULL || first == NULL || last == NULL)
    return -1;

  first_str = g_strdup (str);
  last_str = strchr (first_str, '-');
  if (last_str == NULL)
    {
      g_free (first_str);
      return -1;
    }

  /* Separate the two IPs. */
  *last_str = '\0';
  last_str++;
  end = atoi (last_str);

  /* Get the first IP */
  if (inet_pton (AF_INET, first_str, first) != 1)
  {
    g_free (first_str);
    return -1;
  }

  /* Get the last IP */
  last->s_addr = htonl ((ntohl (first->s_addr) & 0xffffff00) + end);

  g_free (first_str);
  return 0;
}

/**
 * @brief Checks if a buffer points to a valid hostname.
 * Valid characters include: Alphanumerics, dot (.), dash (-) and underscore (_)
 * up to 255 characters.
 *
 * @param[in]   str Buffer to check in.
 *
 * @return 1 if valid hostname, 0 otherwise.
 */
static int
is_hostname (const char *str)
{
  const char *h = str;

  while (*h && (isalnum (*h) || strchr ("-_.", *h)))
    h++;

  /* Valid string if no other chars, and length is 255 at most. */
  if (*h == '\0' && h - str < 256)
    return 1;

  return 0;
}

/**
 * @brief Determines the host type in a buffer.
 *
 * @param[in] str   Buffer that contains host definition, could a be hostname,
 *                  single IPv4 or IPv6, CIDR-expressed block etc,.
 *
 * @return Host_TYPE_*, -1 if error.
 */
static int
determine_host_type (const gchar *str_stripped)
{
  /*
   * We have a single element with no leading or trailing
   * white spaces. This element could represent different host
   * definitions: single IPs, host names, CIDR-expressed blocks,
   * range-expressed networks, IPv6 addresses.
   */

  /* Null or empty string. */
  if (str_stripped == NULL || *str_stripped == '\0')
    return -1;

  /* Check for regular single IPv4 address. */
  if (is_ipv4_address (str_stripped))
    return HOST_TYPE_IPV4;

  /* Check for regular single IPv6 address. */
  if (is_ipv6_address (str_stripped))
    return HOST_TYPE_IPV6;

  /* Check for regular IPv4 CIDR-expressed block like "192.168.12.0/24" */
  if (is_cidr_block (str_stripped))
    return HOST_TYPE_CIDR_BLOCK;

  /* Check for short range-expressed networks "192.168.12.5-40" */
  if (is_short_range_network (str_stripped))
    return HOST_TYPE_RANGE_SHORT;

  /* Check for long range-expressed networks "192.168.1.0-192.168.3.44" */
  if (is_long_range_network (str_stripped))
    return HOST_TYPE_RANGE_LONG;

  /* Check for hostname. */
  if (is_hostname (str_stripped))
    return HOST_TYPE_NAME;

  /* @todo: If everything else fails, fallback to hostname ? */
  return -1;
}

/**
 * @brief Creates a new openvas_host_t object.
 *
 * @return Pointer to new host object, NULL if creation fails.
 */
static openvas_host_t *
openvas_host_new ()
{
  openvas_host_t *host;

  host = g_malloc0 (sizeof (openvas_host_t));

  return host;
}

/**
 * @brief Frees the memory occupied by an openvas_host_t object.
 *
 * @param[in] host  Host to free.
 */
static void
openvas_host_free (gpointer host)
{
  openvas_host_t *h = host;
  if (h == NULL)
    return;

  /* If host of type hostname, free the name buffer, first. */
  if (h->type == HOST_TYPE_NAME)
    g_free (h->name);

  g_free (h);
}

/**
 * @brief Gives if two openvas_host structs have equal types and values.
 *
 * @param[in] host      First host object.
 * @param[in] host2     Second host object.
 *
 * @return 1 if the two hosts are equal, 0 otherwise.
 */
static int
openvas_host_equal (openvas_host_t *host, openvas_host_t *host2)
{
  if (!host || !host2 || host->type != host2->type)
    return 0;

  if (host->type == HOST_TYPE_IPV4)
    return host->addr.s_addr == host2->addr.s_addr;
  else if (host->type == HOST_TYPE_IPV6)
    return !memcmp (host->addr6.s6_addr, host2->addr6.s6_addr, 16);
  else if (host->type == HOST_TYPE_NAME)
    return !strcmp (host->name, host2->name);

  return 0;
}

/**
 * @brief Removes duplicate hosts values from an openvas_hosts_t structure.
 * Also resets the iterator current position.
 *
 * @param[in] hosts hosts collection from which to remove duplicates.
 */
static void
openvas_hosts_remove_duplicates (openvas_hosts_t *hosts)
{
  /**
   * @todo: Runtime complexity is O(N^2). If that ends up inadequate for very
   * large networks, investigate using hash tables (1 per type (ipv4, ipv6 and
   * hostnames) which would take more memory space but has ~O(N) complexity
   * instead.
   */
  GList *element = hosts->hosts;

  /* Iterate over list elements. */
  while (element)
    {
      openvas_host_t *host;
      GList *next = element->next;

      host = element->data;
      while (next)
        {
          openvas_host_t *host2 = next->data;

          if (openvas_host_equal (host, host2))
            {
              GList *tmp = next;
              next = next->next;

              /* Remove host and list element containing it. */
              openvas_host_free (host2);
              hosts->hosts = g_list_delete_link (hosts->hosts, tmp);
              /* Increment duplicates/invalid count. */
              hosts->removed++;
            }
          else
            next = next->next;
        }

      /* Go to next list element */
      element = element->next;
    }

  hosts->current = hosts->hosts;
}

/**
 * @brief Creates a new openvas_hosts_t structure and the associated hosts
 * objects from the provided hosts_str.
 *
 * @param[in] hosts_str The hosts string. A copy will be created of this within
 *                      the returned struct.
 *
 * @return NULL if error, otherwise, a hosts structure that should be released
 * using @ref openvas_hosts_free.
 */
openvas_hosts_t *
openvas_hosts_new (const gchar *hosts_str)
{
  openvas_hosts_t *hosts;
  gchar **host_element, **split;
  gchar *str;

  if (hosts_str == NULL)
    return NULL;

  hosts = g_malloc0 (sizeof (openvas_hosts_t));
  if (hosts == NULL)
    return NULL;

  hosts->orig_str = g_strdup (hosts_str);
  /* Normalize separator: Transform newlines into commas. */
  str = hosts->orig_str;
  while (*str)
    {
      if (*str == '\n') *str = ',';
      str++;
    }

  /* Split comma-separeted list into single host-specifications */
  split = g_strsplit (hosts->orig_str, ",", 0);

  /* first element of the splitted list */
  host_element = split;
  while (*host_element)
    {
      int host_type;
      gchar *stripped = g_strstrip (*host_element);

      if (stripped == NULL || *stripped == '\0')
        {
          host_element++;
          continue;
        }

      /* IPv4, hostname, IPv6, collection (short/long range, cidr block) etc,. ? */
      /* -1 if error. */
      host_type = determine_host_type (stripped);

      switch (host_type)
        {
          case HOST_TYPE_NAME:
          case HOST_TYPE_IPV4:
          case HOST_TYPE_IPV6:
            {
              /* New host. */
              openvas_host_t *host = openvas_host_new ();
              host->type = host_type;
              if (host_type == HOST_TYPE_NAME)
                host->name = g_strdup (stripped);
              else if (host_type == HOST_TYPE_IPV4)
                inet_pton (AF_INET, stripped, &host->addr);
              else if (host_type == HOST_TYPE_IPV6)
                inet_pton (AF_INET6, stripped, &host->addr6);
              /* Prepend to list of hosts. */
              hosts->hosts = g_list_prepend (hosts->hosts, host);
              break;
            }
          case HOST_TYPE_CIDR_BLOCK:
          case HOST_TYPE_RANGE_SHORT:
          case HOST_TYPE_RANGE_LONG:
            {
              struct in_addr first, last;
              uint32_t current;
              int (*ips_func) (const char *, struct in_addr *, struct in_addr *);

              if (host_type == HOST_TYPE_CIDR_BLOCK)
                ips_func = cidr_block_ips;
              else if (host_type == HOST_TYPE_RANGE_SHORT)
                ips_func = short_range_network_ips;
              else if (host_type == HOST_TYPE_RANGE_LONG)
                ips_func = long_range_network_ips;

              if (ips_func (stripped, &first, &last) == -1)
                break;

              /* Make sure that first actually comes before last */
              if (ntohl (first.s_addr) > ntohl (last.s_addr))
                {
                  fprintf (stderr, "ERROR - %s: Inversed limits.\n", stripped);
                  break;
                }

              /* Add addresses from first to last as single hosts. */
              current = first.s_addr;
              while (ntohl (current) <= ntohl (last.s_addr))
                {
                  openvas_host_t *host = openvas_host_new ();
                  host->type = HOST_TYPE_IPV4;
                  host->addr.s_addr = current;
                  hosts->hosts = g_list_prepend (hosts->hosts, host);
                  /* Next IP address. */
                  current = htonl (ntohl (current) + 1);
                }
              break;
            }
          case -1:
          default:
            hosts->removed++;
            fprintf (stderr, "ERROR - %s: Invalid host string.\n", stripped);
            break;
        }
      host_element++; /* move on to next element of splitted list */
    }

  /* Reverse list, as we were prepending (for performance) to the list. */
  hosts->hosts = g_list_reverse (hosts->hosts);

  /* Remove duplicated values. */
  openvas_hosts_remove_duplicates (hosts);

  /* Set current to start of hosts list. */
  hosts->current = hosts->hosts;

  g_strfreev (split);
  return hosts;
}

/**
 * @brief Gets the next openvas_host_t from a openvas_hosts_t structure. The
 * state of iteration is kept internally within the openvas_hosts structure.
 *
 * @param[in]   hosts     openvas_hosts_t structure to get next host from.
 *
 * @return Pointer to host. NULL if error or end of hosts.
 */
openvas_host_t *
openvas_hosts_next (openvas_hosts_t *hosts)
{
  openvas_host_t *next;

  if (hosts == NULL || hosts->current == NULL)
    return NULL;

  next = hosts->current->data;
  hosts->current = g_list_next (hosts->current);

  return next;
}

/**
 * @brief Frees memory occupied by an openvas_hosts_t structure.
 *
 * @param[in] hosts The hosts collection to free.
 *
 */
void
openvas_hosts_free (openvas_hosts_t * hosts)
{
  if (hosts == NULL)
    return;

  if (hosts->orig_str)
    g_free (hosts->orig_str);

  g_list_free_full (hosts->hosts, openvas_host_free);
  g_free (hosts);
}

/**
 * @brief Randomizes the order of the hosts objects in the collection.
 * Not to be used while iterating over the single hosts as it resets the
 * iterator.
 *
 * @param[in] hosts The hosts collection to shuffle.
 */
void
openvas_hosts_shuffle (openvas_hosts_t * hosts)
{
  int count;
  GList *new_list;
  GRand *rand;

  count = openvas_hosts_count (hosts);
  new_list = NULL;

  rand = g_rand_new ();

  while (count)
    {
      GList *element;

      /* Get element from random position [0, count[. */
      element = g_list_nth (hosts->hosts, g_rand_int_range (rand, 0, count));
      /* Remove it. */
      hosts->hosts = g_list_remove_link (hosts->hosts, element);
      /* Insert it in new list */
      new_list = g_list_concat (element, new_list);
      count--;
    }
  hosts->hosts = new_list;
  hosts->current = hosts->hosts;

  g_rand_free (rand);
}

/**
 * @brief Gets the count of single hosts objects in a hosts collection.
 *
 * @param[in] hosts The hosts collection to count hosts of.
 *
 * @return The number of single hosts.
 */
unsigned int
openvas_hosts_count (const openvas_hosts_t * hosts)
{
  return hosts ? g_list_length (hosts->hosts) : 0;
}

/**
 * @brief Gets the count of single values in hosts string that were removed
 * (duplicates / invalid.)
 *
 * @param[in] hosts The hosts collection.
 *
 * @return The number of removed values.
 */
unsigned int
openvas_hosts_removed (const openvas_hosts_t * hosts)
{
    return hosts ? hosts->removed : 0;
}

/**
 * @brief Gets a host object's type.
 *
 * @param[in] host  The host object.
 *
 * @return Host type.
 */
int
openvas_host_type (const openvas_host_t *host)
{
  return host ? host->type : -1;
}

/**
 * @brief Gets a host's type in printable format.
 *
 * @param[in] host  The host object.
 *
 * @return String representing host type. Statically allocated, thus, not to be
 * freed.
 */
gchar *
openvas_host_type_str (const openvas_host_t *host)
{
  if (host == NULL)
    return NULL;

  return host_type_str[host->type];
}

/**
 * @brief Gets a host's value in printable format.
 *
 * @param[in] host  The host object.
 *
 * @return String representing host value. To be freed with g_free().
 */
gchar *
openvas_host_value_str (const openvas_host_t *host)
{
  if (host == NULL)
    return NULL;

  switch (host->type)
    {
      case HOST_TYPE_NAME:
        return g_strdup (host->name);
        break;
      case HOST_TYPE_IPV4:
      case HOST_TYPE_IPV6:
        /* Handle both cases using inet_ntop(). */
        {
          int family, size;
          gchar *str;
          const void *srcaddr;

          if (host->type == HOST_TYPE_IPV4)
            {
              family = AF_INET;
              size = INET_ADDRSTRLEN;
              srcaddr = &host->addr;
            }
          else
            {
              family = AF_INET6;
              size = INET6_ADDRSTRLEN;
              srcaddr = &host->addr6;
            }

          str = g_malloc0 (size);
          if (str == NULL)
            {
              perror ("g_malloc0");
              return NULL;
            }
          if (inet_ntop (family, srcaddr, str, size) == NULL)
            {
              perror ("inet_ntop");
              g_free (str);
              return NULL;
            }
          return str;
        }
      default:
       return g_strdup ("Erroneous host type: Should be Hostname/IPv4/IPv6.");
    }
}

/**
 * @brief Resolves a host object's name to an IPv4 or IPv6 address. Host object
 * should be of type HOST_TYPE_NAME.
 *
 * @param[in] host      The host object whose name to resolve.
 * @param[in] dst       Buffer to store resolved address. Size must be at least
 *                      4 bytes for AF_INET and 16 bytes for AF_INET6.
 * @param[in] family    Either AF_INET or AF_INET6.
 *
 * @return -1 if error, 0 otherwise.
 */
int
openvas_host_resolve (const openvas_host_t *host, void *dst, int family)
{
  struct addrinfo hints, *info, *p;

  if (host == NULL || dst == NULL || host->type != HOST_TYPE_NAME
      || (family != AF_INET && family != AF_INET6))
    return 0;

  bzero (&hints, sizeof (hints));
  hints.ai_family = family;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;
  if ((getaddrinfo (host->name, NULL, &hints, &info)) != 0)
    return -1;

  p = info;
  while (p)
    {
      if (p->ai_family == family)
        {
          if (family == AF_INET)
            {
              struct sockaddr_in *addrin = (struct sockaddr_in *) p->ai_addr;
              memcpy (dst, &(addrin->sin_addr), sizeof (struct in_addr));
            }
          else if (family == AF_INET6)
            {
              struct sockaddr_in6 *addrin = (struct sockaddr_in6 *) p->ai_addr;
              memcpy (dst, &(addrin->sin6_addr), sizeof (struct in6_addr));
            }
          break;
        }

      p = p->ai_next;
    }

  freeaddrinfo (info);
  return 0;
}

/**
 * @brief Gives an IPv4-mapped IPv6 address.
 * eg. 192.168.10.20 would map to ::ffff:192.168.10.20.
 *
 * @param[in]  ip4  IPv4 address to map.
 * @param[out] ip6  Buffer to store the IPv6 address.
 */
static void
ipv4_mapped_ipv6 (const struct in_addr *ip4, struct in6_addr *ip6)
{
  if (ip4 == NULL || ip6 == NULL)
    return;

  ip6->s6_addr32[0] = 0;
  ip6->s6_addr32[1] = 0;
  ip6->s6_addr32[2] = htonl (0xffff);
  memcpy (&ip6->s6_addr32[3], ip4, sizeof (struct in_addr));
}

/**
 * @brief Gives a host object's value as an IPv6 address.
 * If the host type is hostname, it resolves the IPv4 address then gives an
 * IPv4-mapped IPv6 address (eg. ::ffff:192.168.1.1 .)
 * If the host type is IPv4, it gives an IPv4-mapped IPv6 address.
 * If the host's type is IPv6, it gives the value directly.
 *
 * @param[in]  host     The host object whose value to get as IPv6.
 * @param[out] ip6      Buffer to store the IPv6 address.
 *
 * @return -1 if error, 0 otherwise.
 */
int
openvas_host_addr6 (const openvas_host_t *host, struct in6_addr *ip6)
{
  if (host == NULL || ip6 == NULL)
    return -1;

  switch (openvas_host_type (host))
    {
      case HOST_TYPE_IPV6:
        memcpy (ip6, &host->addr6, sizeof (struct in6_addr));
        return 0;

      case HOST_TYPE_IPV4:
        ipv4_mapped_ipv6 (&host->addr, ip6);
        return 0;

      case HOST_TYPE_NAME:
        {
          struct in_addr ip4;

          if (openvas_host_resolve (host, &ip4, AF_INET) == -1)
            return -1;

          ipv4_mapped_ipv6 (&ip4, ip6);
          return 0;
        }

      default:
        return -1;
    }
}

