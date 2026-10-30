// IPv4 version of the direct connection network system for Nintendo Switch.
//
// src/pc/network/socket/socket.c uses a dual-stack IPv6 socket (IPv4 peers as
// IPv6-mapped addresses), but the Switch's BSD sockets only support IPv4. The
// packets are the same, so Switch and PC players can still connect to each other.
// Keep this in sync with socket.c and socket_linux.c, which are excluded on Switch.

#include "pc/network/socket/socket.h"
#include <stdio.h>
#include "pc/configfile.h"
#include "pc/debuglog.h"
#include "pc/djui/djui.h"
#include "platform_switch.h"

// How often a broken socket may be recreated (see ns_socket_update), in updates (frames)
#define SOCKET_RECOVERY_INTERVAL 30

static SOCKET sCurSocket = INVALID_SOCKET;
static struct sockaddr_in sAddr[MAX_PLAYERS] = { 0 };
static unsigned int sHostPort = 0;
static int sLastReceiveError = 0;
static u32 sUpdateCount = 0;
static u32 sLastRecoveryUpdate = 0;
static bool sRecovered = false;

char gGetHostName[MAX_CONFIG_STRING] = "";

SOCKET socket_initialize(void) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("socket failed with error %d", SOCKET_LAST_ERROR);
        return INVALID_SOCKET;
    }

    // set non-blocking mode
    int rc = fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    if (rc == (int)INVALID_SOCKET) {
        LOG_ERROR("fcntl failed with error: %d", rc);
        return INVALID_SOCKET;
    }

    LOG_INFO("socket initialized.");
    return sock;
}

void socket_shutdown(SOCKET socket) {
    if (socket == INVALID_SOCKET) { return; }
    int rc = closesocket(socket);
    if (rc == (int)SOCKET_ERROR) {
        LOG_ERROR("closesocket failed with error %d\n", SOCKET_LAST_ERROR);
    }
}

// Resolves a hostname to an IPv4 address (the first one found).
static void resolve_domain(struct sockaddr_in *addr) {
    // remove square brackets from configJoinIp, getaddrinfo doesn't like those
    if (configJoinIp[0] == '[') {
        for (int i = 0; i < MAX_CONFIG_STRING; i++) {
            if (configJoinIp[i] == '\0') { break; }
            if (configJoinIp[i] == ']') {
                configJoinIp[i] = '\0';
                memmove(&configJoinIp, &configJoinIp[1], MAX_CONFIG_STRING - 1);
                break;
            }
        }
    }

    struct addrinfo hints;
    struct addrinfo *result;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    int error = getaddrinfo(configJoinIp, NULL, &hints, &result);
    if (error != 0) {
        LOG_ERROR("getaddrinfo() failed with error code %i: %s", error, gai_strerror(error));
        return;
    }

    for (struct addrinfo *i = result; i != NULL; i = i->ai_next) {
        if (i->ai_addr->sa_family == AF_INET) {
            struct sockaddr_in *p = (struct sockaddr_in *)i->ai_addr;
            addr->sin_addr = p->sin_addr;
            char str[INET_ADDRSTRLEN];
            snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", inet_ntop(AF_INET, &p->sin_addr, str, sizeof(str)));
            break;
        }
    }
    freeaddrinfo(result);
}

static int socket_bind(SOCKET socket, unsigned int port) {
    struct sockaddr_in rxAddr;
    memset(&rxAddr, 0, sizeof(struct sockaddr_in));
    rxAddr.sin_family = AF_INET;
    rxAddr.sin_port = htons(port);
    rxAddr.sin_addr.s_addr = htonl(INADDR_ANY);

    int rc = bind(socket, (SOCKADDR *)&rxAddr, sizeof(rxAddr));
    if (rc != 0) {
        LOG_ERROR("bind failed with error %d", SOCKET_LAST_ERROR);
    }
    return rc;
}

static int socket_send(SOCKET socket, struct sockaddr_in* addr, u8* buffer, u16 bufferLength) {
    int rc = sendto(socket, (char*)buffer, bufferLength, 0, (struct sockaddr*)addr, sizeof(struct sockaddr_in));
    if (rc != SOCKET_ERROR) { return NO_ERROR; }

    int error = SOCKET_LAST_ERROR;
    if (error == SOCKET_EWOULDBLOCK) { return NO_ERROR; }

    LOG_ERROR("sendto failed with error: %d", error);
    return rc;
}

static int socket_receive(SOCKET socket, struct sockaddr_in* rxAddr, u8* buffer, u16 bufferLength, u16* receiveLength, u8* localIndex) {
    *receiveLength = 0;

    RX_ADDR_SIZE_TYPE rxAddrSize = sizeof(struct sockaddr_in);
    int rc = recvfrom(socket, (char*)buffer, bufferLength, 0, (struct sockaddr*)rxAddr, &rxAddrSize);

    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (memcmp(rxAddr, &sAddr[i], sizeof(struct sockaddr_in)) == 0) {
            *localIndex = i;
            break;
        }
    }

    if (rc == SOCKET_ERROR) {
        sLastReceiveError = SOCKET_LAST_ERROR;
        return SOCKET_ERROR;
    }

    *receiveLength = rc;
    return NO_ERROR;
}

static bool ns_socket_initialize(enum NetworkType networkType, UNUSED bool reconnecting) {
    // sanity check port
    unsigned int port = (networkType == NT_CLIENT) ? configJoinPort : configHostPort;
    if (port == 0) { port = DEFAULT_PORT; }

    // create a receiver socket to receive datagrams
    sCurSocket = socket_initialize();
    if (sCurSocket == INVALID_SOCKET) { return false; }

    if (networkType == NT_SERVER) {
        int reuse = 1;
        if (setsockopt(sCurSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0) {
            LOG_ERROR("setsockopt(SO_REUSEADDR) failed");
        }

        // bind the socket to any address and the specified port
        int rc = socket_bind(sCurSocket, port);
        if (rc != NO_ERROR) {
            LOG_ERROR("bind returned an error.");
            return false;
        }
        LOG_INFO("bound to port %u", port);
        sHostPort = port;
    } else if (networkType == NT_CLIENT) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(struct sockaddr_in));
        sAddr[0].sin_family = AF_INET;
        sAddr[0].sin_port = htons(port);
        resolve_domain(&addr);
        sAddr[0].sin_addr = addr.sin_addr;
        LOG_INFO("connecting to %s, port %u", configJoinIp, port);
        // copy hostname to be saved to config file
        snprintf(configJoinIp, MAX_CONFIG_STRING, "%s", gGetHostName);

        djui_connect_menu_open();

        gNetworkType = NT_CLIENT;
    }

    LOG_INFO("initialized");

    if (networkType == NT_CLIENT) {
        network_send_mod_list_request();
    }

    return true;
}

static s64 ns_socket_get_id(UNUSED u8 localId) {
    return 0;
}

static char* ns_socket_get_id_str(u8 localId) {
    if (localId == UNKNOWN_LOCAL_INDEX) { localId = 0; }
    static char id_str[INET_ADDRSTRLEN] = { 0 };
    inet_ntop(AF_INET, &sAddr[localId].sin_addr, id_str, sizeof(id_str));
    return id_str;
}

static void ns_socket_save_id(u8 localId, UNUSED s64 networkId) {
    SOFT_ASSERT(localId > 0);
    SOFT_ASSERT(localId < MAX_PLAYERS);
    sAddr[localId] = sAddr[0];
    LOG_INFO("saved addr for id %d", localId);
}

static void ns_socket_clear_id(u8 localId) {
    if (localId == 0) { return; }
    SOFT_ASSERT(localId < MAX_PLAYERS);
    memset(&sAddr[localId], 0, sizeof(struct sockaddr_in));
    LOG_INFO("cleared addr for id %d", localId);
}

static void* ns_socket_dup_addr(u8 localIndex) {
    void* address = malloc(sizeof(struct sockaddr_in));
    memcpy(address, &sAddr[localIndex], sizeof(struct sockaddr_in));
    return address;
}

static bool ns_socket_match_addr(void* addr1, void* addr2) {
    return !memcmp(addr1, addr2, sizeof(struct sockaddr_in));
}

// Horizon can leave a socket permanently failing with ENETDOWN (seen on hardware while hosting,
// with the internet connection still up), so a socket that fails with anything but "no data"
// is replaced by a new one on the same port. Clients are identified by their own address, so
// they keep working. A joining client's socket gets a new port, which the host may not accept.
static void ns_socket_recover(int error) {
    if (sRecovered && sUpdateCount - sLastRecoveryUpdate < SOCKET_RECOVERY_INTERVAL) { return; }
    sRecovered = true;
    sLastRecoveryUpdate = sUpdateCount;
    LOG_ERROR("recvfrom failed with error %d (internet request state %d), recreating the socket",
              error, switch_internet_request_state());

    socket_shutdown(sCurSocket);
    sCurSocket = socket_initialize();
    if (sCurSocket == INVALID_SOCKET || sHostPort == 0) { return; }
    int reuse = 1;
    setsockopt(sCurSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    if (socket_bind(sCurSocket, sHostPort) == NO_ERROR) {
        LOG_INFO("bound to port %u", sHostPort);
    }
}

static void ns_socket_update(void) {
    if (gNetworkType == NT_NONE) { return; }
    sUpdateCount++;
    if (sCurSocket == INVALID_SOCKET) {
        ns_socket_recover(0);
        return;
    }
    do {
        // receive packet
        u8 data[PACKET_LENGTH + 1];
        u16 dataLength = 0;
        u8 localIndex = UNKNOWN_LOCAL_INDEX;
        int rc = socket_receive(sCurSocket, &sAddr[0], data, PACKET_LENGTH + 1, &dataLength, &localIndex);
        SOFT_ASSERT(dataLength < PACKET_LENGTH);
        if (rc != NO_ERROR) {
            if (sLastReceiveError != SOCKET_EWOULDBLOCK && sLastReceiveError != SOCKET_ECONNRESET) {
                ns_socket_recover(sLastReceiveError);
            }
            break;
        }
        network_receive(localIndex, &sAddr[0], data, dataLength);
    } while (true);
}

static int ns_socket_send(u8 localIndex, void* address, u8* data, u16 dataLength) {
    if (localIndex != 0) {
        if (gNetworkType == NT_SERVER && gNetworkPlayers[localIndex].type != NPT_CLIENT) { return SOCKET_ERROR; }
        if (gNetworkType == NT_CLIENT && gNetworkPlayers[localIndex].type != NPT_SERVER) { return SOCKET_ERROR; }
    }

    struct sockaddr_in* userAddr = &sAddr[localIndex];
    if (localIndex == 0 && address != NULL) { userAddr = (struct sockaddr_in*)address; }

    int rc = socket_send(sCurSocket, userAddr, data, dataLength);
    if (rc) {
        LOG_ERROR("    localIndex: %d, packetType: %d, dataLength: %d", localIndex, data[0], dataLength);
    }
    return rc;
}

static void ns_socket_get_lobby_id(char* destination, u32 destLength) {
    snprintf(destination, destLength, "%s", "");
}

static void ns_socket_get_lobby_secret(char* destination, u32 destLength) {
    snprintf(destination, destLength, "%s", "");
}

static void ns_socket_shutdown(UNUSED bool reconnecting) {
    socket_shutdown(sCurSocket);
    sCurSocket = INVALID_SOCKET;
    sHostPort = 0;
    sRecovered = false;
    for (u16 i = 0; i < MAX_PLAYERS; i++) {
        memset(&sAddr[i], 0, sizeof(struct sockaddr_in));
    }
    LOG_INFO("shutdown");
}

struct NetworkSystem gNetworkSystemSocket = {
    .initialize       = ns_socket_initialize,
    .get_id           = ns_socket_get_id,
    .get_id_str       = ns_socket_get_id_str,
    .save_id          = ns_socket_save_id,
    .clear_id         = ns_socket_clear_id,
    .dup_addr         = ns_socket_dup_addr,
    .match_addr       = ns_socket_match_addr,
    .update           = ns_socket_update,
    .send             = ns_socket_send,
    .get_lobby_id     = ns_socket_get_lobby_id,
    .get_lobby_secret = ns_socket_get_lobby_secret,
    .shutdown         = ns_socket_shutdown,
    .requireServerBroadcast = true,
    .name             = "Socket",
};
