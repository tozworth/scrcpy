// Modified by appcpy: concurrent clients must not share a Windows listener.
#include "util/net.h"

#include <assert.h>

int main(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
#ifdef _WIN32
    assert(net_init());
    sc_socket first = net_socket();
    sc_socket second = net_socket();
    sc_socket other = net_socket();
    assert(first != SC_SOCKET_NONE);
    assert(second != SC_SOCKET_NONE);
    assert(other != SC_SOCKET_NONE);

    // Let Windows choose a free port, avoiding other programs and test runs.
    assert(net_listen(first, IPV4_LOCALHOST, 0, 1));
    struct sockaddr_in address;
    int size = sizeof(address);
    assert(!getsockname(first->socket, (struct sockaddr *) &address, &size));
    uint16_t port = ntohs(address.sin_port);
    assert(port);

    // With SO_REUSEADDR, both listens succeed on Windows and ADB reverse
    // connections can reach the wrong scrcpy client (audio/video/control).
    assert(!net_listen(second, IPV4_LOCALHOST, port, 1));
    assert(net_close(second));

    // A concurrent client can still listen on another port.
    assert(net_listen(other, IPV4_LOCALHOST, 0, 1));
    size = sizeof(address);
    assert(!getsockname(other->socket, (struct sockaddr *) &address, &size));
    assert(ntohs(address.sin_port) != port);
    assert(net_close(other));
    assert(net_close(first));

    // Closing a listener frees its port for a later session.
    sc_socket replacement = net_socket();
    assert(replacement != SC_SOCKET_NONE);
    assert(net_listen(replacement, IPV4_LOCALHOST, port, 1));
    assert(net_close(replacement));
    net_cleanup();
#endif
    return 0;
}
