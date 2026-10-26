#if defined(WOWDECOMP_ARCH_32)

#include <winsock2.h>
#include <windows.h>
#include "WoW/Source/WowServices/WowConnectionNet.h"

// VERIFY: 55df08d6 auto
// OFFSET: 0x004697E0
void WowConnectionNet::PlatformChangeState(WowConnection* connection, WOW_CONN_STATE state) {
    long networkEvents = 0;

    switch (connection->m_connState) {
        case WOWC_CONNECTING:
            networkEvents = FD_CONNECT | FD_CLOSE | FD_WRITE;
            break;

        case WOWC_LISTENING:
            networkEvents = FD_ACCEPT;
            break;

        case WOWC_CONNECTED:
            networkEvents = connection->m_sendList.Head() || connection->m_notifyWriteReady ? FD_READ | FD_WRITE | FD_CLOSE : FD_READ | FD_CLOSE;
            break;
    }

    if (connection->m_event && connection->m_sock >= 0) {
        WSAEventSelect(connection->m_sock, connection->m_event, networkEvents);
    }

    SetEvent(this->m_event);
}

// VERIFY: acc51124 manual
// OFFSET: 0x00469860
void WowConnectionNet::PlatformRun() {
    this->m_networkStarted = true;

    DWORD timeout = 500;

    if (this->m_stop) {
        return;
    }

    do {
        WowConnection* connections[MAXIMUM_WAIT_OBJECTS];
        HANDLE handles[MAXIMUM_WAIT_OBJECTS];

        // Not stock: stock read each connection's send list, write-ready flag and lag-test list here under
        // m_connectionsLock only, racing Send/DoWrites (they guard them with the connection's m_lock). Taking m_lock
        // inside m_connectionsLock would invert DoDisconnect's order (m_lock, then Remove), so collect referenced
        // candidates under m_connectionsLock first, then select events per connection under its own m_lock.
        WowConnection* candidates[MAXIMUM_WAIT_OBJECTS];
        DWORD numCandidates = 0;

        this->m_connectionsLock.Enter();

        for (WowConnection* connection = this->m_connections.Head(); connection && numCandidates < MAXIMUM_WAIT_OBJECTS - 1; connection = this->m_connections.Next(connection)) {
            if (connection->m_serviceCount) {
                continue;
            }

            connection->m_refCount.Increment();
            candidates[numCandidates++] = connection;
        }

        this->m_connectionsLock.Leave();

        connections[0] = nullptr;
        handles[0] = this->m_event;
        DWORD count = 1;

        for (DWORD c = 0; c < numCandidates; c++) {
            WowConnection* connection = candidates[c];
            HANDLE event = connection->m_event;
            bool wait = true;

            connection->m_lock.Enter();

            switch (connection->m_connState) {
                case WOWC_CONNECTING:
                    WSAEventSelect(connection->m_sock, event, FD_CONNECT | FD_CLOSE);
                    break;

                case WOWC_LISTENING:
                    WSAEventSelect(connection->m_sock, event, FD_ACCEPT);
                    break;

                case WOWC_CONNECTED:
                    WSAEventSelect(connection->m_sock, event, (connection->m_sendList.Head() || connection->m_notifyWriteReady ? FD_WRITE : 0) | FD_READ | FD_CLOSE);
                    break;

                case WOWC_DISCONNECTING:
                    timeout = 0;
                    break;

                default:
                    wait = false;
                    break;
            }

            if (wait && connection->m_lagTestRecvList.Head()) {
                timeout = 0;
            }

            connection->m_lock.Leave();

            if (wait) {
                handles[count] = event;
                connections[count] = connection;
                count++;
            } else {
                connection->Release();
            }
        }

        DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout);

        if (result < count) {
            if (result == WAIT_OBJECT_0) {
                ResetEvent(this->m_event);
            }

            for (DWORD i = 1; i < count; i++) {
                WowConnection* connection = connections[i];

                WSANETWORKEVENTS networkEvents;
                WSAEnumNetworkEvents(connection->m_sock, handles[i], &networkEvents);

                uint32_t flags = 0;

                if (networkEvents.lNetworkEvents & (FD_READ | FD_ACCEPT | FD_CLOSE)) {
                    flags = 2;
                }

                if (networkEvents.lNetworkEvents & (FD_WRITE | FD_CONNECT | FD_CLOSE)) {
                    flags |= 1;
                }

                if (connection->m_connState == WOWC_DISCONNECTING) {
                    flags |= 8;
                }

                if (WowConnection::s_lagTestDelayMin || WowConnection::s_lagTestDelayMax) {
                    flags |= 3;
                    timeout = 10;
                } else {
                    timeout = 500;
                }

                if (flags) {
                    this->SignalWorker(connection, flags);
                }
            }
        }

        for (DWORD i = 1; i < count; i++) {
            connections[i]->Release();
        }
    } while (!this->m_stop);
}

// VERIFY: 10cfb4a7 auto
// OFFSET: 0x004696E0
void WowConnectionNet::PlatformInit(bool useEngine) {
    WSADATA data;

    if (!WSAStartup(MAKEWORD(2, 2), &data) && data.wVersion == MAKEWORD(2, 2)) {
        this->m_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    }
}

// VERIFY: a9bf6993 auto
// OFFSET: 0x00469740
void WowConnectionNet::PlatformDestruct(WowConnection* connection) {
    if (connection->m_event) {
        WSACloseEvent(connection->m_event);
        connection->m_event = nullptr;
    }
}

// VERIFY: 2fa3865c auto
// OFFSET: 0x00469770
void WowConnectionNet::PlatformWorkerReady() {
    SetEvent(this->m_event);
}

// VERIFY: 20a6eddf auto
// OFFSET: 0x00469780
void WowConnectionNet::PlatformAdd(WowConnection* connection) {
    if (!connection->m_event) {
        connection->m_event = WSACreateEvent();
    }

    BOOL noDelay = TRUE;
    setsockopt(connection->m_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

    SetEvent(this->m_event);
}

// VERIFY: 0881760b auto
// OFFSET: 0x004697D0
void WowConnectionNet::PlatformRemove(WowConnection* connection) {
    SetEvent(this->m_event);
}

// VERIFY: 9306efd9 manual
// OFFSET: 0x005EEB70
void WowConnectionNet::PlatformStop() {
}

// VERIFY: ed77cfe3 auto
// OFFSET: 0x00469730
void WowConnectionNet::PlatformDestroy() {
    WSACleanup();
}

#endif
