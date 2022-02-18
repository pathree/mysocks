#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

char* sock_ntop(const struct sockaddr* sa, char str[], int strsize, int* port) {
  switch (sa->sa_family) {
    case AF_INET: {
      struct sockaddr_in* sin = (struct sockaddr_in*)sa;

      if (inet_ntop(AF_INET, &sin->sin_addr, str, strsize) == NULL)
        return (NULL);

      if (port) *port = ntohs(sin->sin_port);

      return (str);
    }

    case AF_INET6: {
      struct sockaddr_in6* sin6 = (struct sockaddr_in6*)sa;

      if (inet_ntop(AF_INET6, &sin6->sin6_addr, str, strsize) == NULL)
        return (NULL);

      static char v6prefix[] = "::ffff:";
      static int v6prefix_len = sizeof("::ffff:") - 1;

      if (memcmp(str, v6prefix, v6prefix_len) == 0)
        memmove(str, str + v6prefix_len, strlen(str + v6prefix_len) + 1);

      if (port) *port = ntohs(sin6->sin6_port);

      return (str);
    }

    case AF_UNIX: {
      struct sockaddr_un* unp = (struct sockaddr_un*)sa;

      /* OK to have no pathname bound to the socket: happens on
             every connect() unless client calls bind() first. */
      if (unp->sun_path[0] == 0)
        strcpy(str, "(no pathname bound)");
      else
        snprintf(str, strsize, "%s", unp->sun_path);
      return (str);
    }

    default:
      snprintf(str, strsize, "sock_ntop: unknown AF_xxx: %d", sa->sa_family);
      return (str);
  }
  return (NULL);
}