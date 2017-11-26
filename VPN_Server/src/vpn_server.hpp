#ifndef VPN_SERVER_HPP
#define VPN_SERVER_HPP

#include "client_parameters.hpp"
#include "tunnel_mgr.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <functional>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <linux/if_tun.h>

#include <ifaddrs.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <sys/time.h>

/**
 * @brief The VPNServer class<br>
 * Main application class that<br>
 * organizes a process of creating, removing and processing<br>
 * vpn tunnels, provides encrypting/decrypting of packets.<br>
 * To run the server loop call 'initConsolInput' method.<br>
 */
class VPNServer {
private:
    int                  argc;
    char**               argv;
    ClientParameters     cliParams;
    IPManager*           manager;
    std::string          port;
    TunnelManager*       tunMgr;
    std::recursive_mutex mutex;
    const int            TIMEOUT_LIMIT = 60000;
    const unsigned       default_values = 7;
    WOLFSSL_CTX*         ctx;

public:
    enum PacketType {
        ZERO_PACKET            = 0,
        CLIENT_WANT_CONNECT    = 1,
        CLIENT_WANT_DISCONNECT = 2
    };

    explicit VPNServer (int argc, char** argv) {
        this->argc = argc;
        this->argv = argv;
        parseArguments(argc, argv); // fill 'cliParams struct'

        manager = new IPManager(cliParams.virtualNetworkIp + '/' + cliParams.networkMask,
                                6); // IP pool init size
        tunMgr  = new TunnelManager;

        // Enable IP forwarding
        tunMgr->execTerminalCommand("echo 1 > /proc/sys/net/ipv4/ip_forward");

        /* In case if program was terminated by error: */
        tunMgr->cleanupTunnels();

        // Pick a range of private addresses and perform NAT over chosen network interface.
        std::string virtualLanAddress = cliParams.virtualNetworkIp + '/' + cliParams.networkMask;
        std::string physInterfaceName = cliParams.physInterface;

        // Delete previous rule if server crashed:
        std::string delPrevPostrouting
                = "iptables -t nat -D POSTROUTING -s " + virtualLanAddress +
                  " -o " + physInterfaceName + " -j MASQUERADE";
        tunMgr->execTerminalCommand(delPrevPostrouting);

        std::string postrouting
                = "iptables -t nat -A POSTROUTING -s " + virtualLanAddress +
                  " -o " + physInterfaceName + " -j MASQUERADE";
        tunMgr->execTerminalCommand(postrouting);
        initSsl(); // initialize ssl context
    }

    ~VPNServer() {
        // Clean all tunnels with prefix "vpn_"
        tunMgr->cleanupTunnels();
        // Disable IP Forwarding:
        tunMgr->execTerminalCommand("echo 0 > /proc/sys/net/ipv4/ip_forward");
        // Remove NAT rule from iptables:
        std::string virtualLanAddress = cliParams.virtualNetworkIp + '/' + cliParams.networkMask;
        std::string physInterfaceName = cliParams.physInterface;
        std::string postrouting
                = "iptables -t nat -D POSTROUTING -s " + virtualLanAddress +
                  " -o " + physInterfaceName + " -j MASQUERADE";
        tunMgr->execTerminalCommand(postrouting);

        wolfSSL_CTX_free(ctx);
        wolfSSL_Cleanup();

        delete manager;
        delete tunMgr;
    }

    /**
     * @brief initConsoleInput\r\n
     * Main method, waiting for user keyboard input.\r\n
     * If input string is 'exitpvn' - close all tunnels,\r\n
     * revert system settings, exit the application.\r\n
     */
    void initConsoleInput() {
        mutex.lock();
            std::cout << "\033[4;32mVPN Service is started (DTLS, ver.23.11.17)\033[0m"
                      << std::endl;
        mutex.unlock();

        std::thread t(&VPNServer::createNewConnection, this);
        t.detach();

        while(true) {
            std::this_thread::sleep_for(std::chrono::seconds(100));
        }
    }

    /**
     * @brief createNewConnection\r\n
     * Method creates new connection (tunnel)
     * waiting for client. When client is connected,
     * the new instance of this method will be runned
     * in another thread
     */
    void createNewConnection() {
        mutex.lock();
        // create ssl from sslContext:
        // run commands via unix terminal (needs sudo)
        in_addr_t serTunAddr    = manager->getAddrFromPool();
        in_addr_t cliTunAddr    = manager->getAddrFromPool();
        std::string serverIpStr = IPManager::getIpString(serTunAddr);
        std::string clientIpStr = IPManager::getIpString(cliTunAddr);
        size_t tunNumber        = tunMgr->getTunNumber();
        std::string tunStr      = "vpn_tun" + std::to_string(tunNumber);
        std::string tempTunStr = tunStr;
        int interface = 0; // Tun interface
        int sentParameters = -12345;
        int e = 0;
        // allocate the buffer for a single packet.
        char packet[32767];
        int timer = 0;
        bool isClientConnected = true;
        bool idle = true;
        int length = 0;
        int sentData = 0;
        std::pair<int, WOLFSSL*> tunnel;

        if(serTunAddr == 0 || cliTunAddr == 0) {
            TunnelManager::log("No free IP addresses. Tunnel will not be created.",
                               std::cerr);
            return;
        }

        //TunnelManager::log("Working with tunnel [" + tunStr + "] now..");
        tunMgr->createUnixTunnel(serverIpStr,
                                 clientIpStr,
                                 tunStr);
        // Get TUN interface.
        interface = get_interface(tunStr.c_str());

        // fill array with parameters to send:
        buildParameters(clientIpStr);
        mutex.unlock();

        // wait for a tunnel.
        while ((tunnel = get_tunnel(port.c_str())).first != -1
               &&
               tunnel.second != nullptr) {

            TunnelManager::log("New client connected to [" + tunStr + "]");

            /* if client is connected then run another instance of connection
             * in a new thread: */
            std::thread thr(&VPNServer::createNewConnection, this);
            thr.detach();
            
            // send the parameters several times in case of packet loss.
            for (int i = 0; i < 3; ++i) {
                sentParameters =
                    wolfSSL_send(tunnel.second, cliParams.parametersToSend,
                                 sizeof(cliParams.parametersToSend),
                                 MSG_NOSIGNAL);

                if(sentParameters < 0) {
                    TunnelManager::log("Error sending parameters: " +
                                    std::to_string(sentParameters));
                    e = wolfSSL_get_error(tunnel.second, 0);
                    printf("error = %d, %s\n", e, wolfSSL_ERR_reason_error_string(e));
                }
            }

            // we keep forwarding packets till something goes wrong.
            while (isClientConnected) {
                // assume that we did not make any progress in this iteration.
                idle = true;

                // read the outgoing packet from the input stream.
                length = read(interface, packet, sizeof(packet));
                if (length > 0) {
                    // write the outgoing packet to the tunnel.
                    sentData = wolfSSL_send(tunnel.second, packet, length, MSG_NOSIGNAL);
                    if(sentData < 0) {
                        TunnelManager::log("sentData < 0");
                        e = wolfSSL_get_error(tunnel.second, 0);
                        printf("error = %d, %s\n", e, wolfSSL_ERR_reason_error_string(e));
                    } /*else {
                        // TunnelManager::log("outgoing packet from interface to the tunnel.");
                    }*/

                    // there might be more outgoing packets.
                    idle = false;

                    // if we were receiving, switch to sending.
                    if (timer < 1)
                        timer = 1;
                }

                // read the incoming packet from the tunnel.
                length = wolfSSL_recv(tunnel.second, packet, sizeof(packet), 0);
                if (length == 0) {
                    TunnelManager::log(std::string() +
                                       "recv() length == " +
                                       std::to_string(length) +
                                       ". Breaking..",
                                       std::cerr);
                    break;
                }
                if (length > 0) {
                    // ignore control messages, which start with zero.
                    if (packet[0] != 0) {
                        // write the incoming packet to the output stream.
                        sentData = write(interface, packet, length);
                        if(sentData < 0) {
                            TunnelManager::log("write(interface, packet, length) < 0");
                        } else {
                            // TunnelManager::log("written the incoming packet to the output stream..");
                        }

                    } else {
                        TunnelManager::log("Recieved empty control msg from client");
                        if(packet[1] == CLIENT_WANT_DISCONNECT && length == 2) {
                            TunnelManager::log("WANT_DISCONNECT from client");
                            isClientConnected = false;
                        }
                    }

                    // there might be more incoming packets.
                    idle = false;

                    // if we were sending, switch to receiving.
                    if (timer > 0) {
                        timer = 0;
                    }
                }

                // if we are idle or waiting for the network, sleep for a
                // fraction of time to avoid busy looping.
                if (idle) {
                    std::this_thread::sleep_for(std::chrono::microseconds(100000));

                    // increase the timer. This is inaccurate but good enough,
                    // since everything is operated in non-blocking mode.
                    timer += (timer > 0) ? 100 : -100;

                    // we are receiving for a long time but not sending
                    if (timer < -10000) {  // -16000
                        // send empty control messages.
                        packet[0] = 0;
                        for (int i = 0; i < 3; ++i) {
                            sentData = wolfSSL_send(tunnel.second, packet, 1, MSG_NOSIGNAL);
                            if(sentData < 0) {
                                TunnelManager::log("sentData < 0");
                                e = wolfSSL_get_error(tunnel.second, 0);
                                printf("error = %d, %s\n", e, wolfSSL_ERR_reason_error_string(e));
                            } else {
                                TunnelManager::log("sent empty control packet");
                            }
                        }

                        // switch to sending.
                        timer = 1;
                    }

                    // we are sending for a long time but not receiving.
                    if (timer > TIMEOUT_LIMIT) {
                        TunnelManager::log("[" + tempTunStr + "]" +
                                           "Sending for a long time but"
                                           " not receiving. Breaking...");
                        break;
                    }
                }
            }
            TunnelManager::log("Client has been disconnected from tunnel [" +
                               tempTunStr + "]");

            //close(tunnel.first);
            // wolfSSL_set_fd(tunnel.second, 0);
            wolfSSL_shutdown(tunnel.second);
            wolfSSL_free(tunnel.second);
            //
            manager->returnAddrToPool(serTunAddr);
            manager->returnAddrToPool(cliTunAddr);
            tunMgr->closeTunNumber(tunNumber);
            return;
        }
        TunnelManager::log("Cannot create tunnels", std::cerr);
        throw std::runtime_error("Cannot create tunnels");
    }
    
    /**
     * @brief SetDefaultSettings
     * Set parameters if they were not set by
     * user via terminal arguments on startup
     * @param in_param - pointer on reference of parameter string to check
     * @param type     - index of parameters
     */
    void SetDefaultSettings(std::string *&in_param, const size_t& type) {
        if(!in_param->empty())
            return;
        
        std::string default_values[] = {
            "1400",
            "10.0.0.0", "8",
            "8.8.8.8",
            "0.0.0.0", "0",
            "eth0"
        };

        *in_param = default_values[type];    
    }

    /**
     * @brief parseArguments method
     * is parsing arguments from terminal
     * and fills ClientParameters structure
     * @param argc - arguments count
     * @param argv - arguments vector
     */
    void parseArguments(int argc, char** argv) {
        std::string* std_params[default_values] = {
            &cliParams.mtu,
            &cliParams.virtualNetworkIp,
            &cliParams.networkMask,
            &cliParams.dnsIp,
            &cliParams.routeIp,
            &cliParams.routeMask,
            &cliParams.physInterface
        };

        port = argv[1]; // port to listen

        if(atoi(port.c_str()) < 1 || atoi(port.c_str()) > 0xFFFF) {
            TunnelManager::log("Error: invalid number of port (" +
                               port +
                               "). Terminating . . .",
                               std::cerr);
            throw std::invalid_argument(
                        "Error: invalid number of port " + port);
        }

        for(int i = 2; i < argc; ++i) {
            switch (argv[i][1]) {
                case 'm':
                    cliParams.mtu = argv[i + 1];
                    break;
                case 'a':
                    cliParams.virtualNetworkIp = argv[i + 1];
                    cliParams.networkMask = argv[i + 2];
                    break;
                case 'd':
                    cliParams.dnsIp = argv[i + 1];
                    break;
                case 'r':
                    cliParams.routeIp = argv[i + 1];
                    cliParams.routeMask = argv[i + 2];
                    break;
                case 'i':
                    cliParams.physInterface = argv[i + 1];
                    break;       
            }
        }

        /* if there was no specific arguments,
         *  default settings will be set up
         */
        for(size_t i = 0; i < default_values; ++i)
            SetDefaultSettings(std_params[i], i);
    }

    /**
     * @brief buildParameters fills
     * 'parametersToSend' array with info
     *  which will be sent to client.
     * @param clientIp - IP-address string to send to client
     */
    void buildParameters(const std::string& clientIp) {

        int size = sizeof(cliParams.parametersToSend);
        // Here is parameters string formed:
        std::string paramStr = std::string() + "m," + cliParams.mtu +
                " a," + clientIp + ",32 d," + cliParams.dnsIp +
                " r," + cliParams.routeIp + "," + cliParams.routeMask;

        // fill parameters array:
        cliParams.parametersToSend[0] = 0; // control messages always start with zero
        memcpy(&cliParams.parametersToSend[1], paramStr.c_str(), paramStr.length());
        memset(&cliParams.parametersToSend[paramStr.length() + 1], ' ', size - (paramStr.length() + 1));
    }

    /**
     * @brief get_interface
     * Tries to open dev/net/tun interface
     * @param name - tunnel interface name (e.g. "tun0")
     * @return descriptor of interface
     */
    int get_interface(const char *name) {
        int interface = open("/dev/net/tun", O_RDWR | O_NONBLOCK);

        ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
        strncpy(ifr.ifr_name, name, sizeof(ifr.ifr_name));

        if (int status = ioctl(interface, TUNSETIFF, &ifr)) {
            TunnelManager::log("Cannot get TUN interface\nStatus is: " +
                               status,
                               std::cerr);
            throw std::runtime_error("Cannot get TUN interface\nStatus is: " +
                                     status);
        }

        return interface;
    }

    /**
     * @brief get_tunnel
     * Method creates listening datagram socket for income connection,
     * binds it to IP address and waiting for connection.
     * When client is connected, method initialize SSL session and
     * set socket to nonblocking mode.
     * @param port - port to listen
     * @return If success, pair with socket descriptor and
     * SSL session object pointer will be returned, otherwise
     * negative SD and nullptr.
     */
    std::pair<int, WOLFSSL*>
    get_tunnel(const char *port) {
        // we use an IPv6 socket to cover both IPv4 and IPv6.
        int tunnel = socket(AF_INET6, SOCK_DGRAM, 0);
        int flag = 1;
         // receive packets till the secret matches.
        char packet[1024];
        memset(packet, 0, sizeof(packet[0] * 1024));

        socklen_t addrlen;
        WOLFSSL* ssl;

        /* Create the WOLFSSL Object */
        if ((ssl = wolfSSL_new(ctx)) == NULL) {
            throw std::runtime_error("wolfSSL_new error.");
        }
        setsockopt(tunnel, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
        flag = 0;
        setsockopt(tunnel, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag));

        // accept packets received on any local address.
        sockaddr_in6 addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(atoi(port));

        // call bind(2) in a loop since Linux does not have SO_REUSEPORT.
        while (bind(tunnel, (sockaddr *)&addr, sizeof(addr))) {
            if (errno != EADDRINUSE) {
                return std::pair<int, WOLFSSL*>(-1, nullptr);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100000));
        }

        addrlen = sizeof(addr);
        int recievedLen = 0;
        do {
            recievedLen = 0;
            recievedLen = recvfrom(tunnel, packet, sizeof(packet), 0,
                                  (sockaddr *)&addr, &addrlen);
            /*
            TunnelManager::log("packet[0] == " +
                               std::to_string(packet[0]) +
                               ", packet[1] == " +
                               std::to_string(packet[1]) +
                               ", receivecLen == " + std::to_string(recievedLen));
            */
            if(recievedLen == 2
               && packet[0] == ZERO_PACKET
               && packet[1] == CLIENT_WANT_CONNECT)
                  break;

        } while (true);

        // connect to the client
        connect(tunnel, (sockaddr *)&addr, addrlen);

        // put the tunnel into non-blocking mode.
        fcntl(tunnel, F_SETFL, O_NONBLOCK);

        /* set the session ssl to client connection port */
        wolfSSL_set_fd(ssl, tunnel);
        wolfSSL_set_using_nonblock(ssl, 1);

        int acceptStatus = SSL_FAILURE;
        int tryCounter   = 1;

        // Try to accept ssl connection for 50 times:
        while( (acceptStatus = wolfSSL_accept(ssl)) != SSL_SUCCESS
              && tryCounter++ <= 50) {
            TunnelManager::log("wolfSSL_accept(ssl) != SSL_SUCCESS. Sleeping..");
            std::this_thread::sleep_for(std::chrono::microseconds(200000));
        }

        if(tryCounter >= 50) {
            wolfSSL_free(ssl);
            return get_tunnel(port);
        }

        return std::pair<int, WOLFSSL*>(tunnel, ssl);
    }

    /**
     * @brief initSsl
     * Initialize SSL library, load certificates and keys,
     * set up DTLS 1.2 protection type.
     * Terminates the application if even one of steps is failed.
     */
    void initSsl() {
        char caCertLoc[]   = "certs/ca_cert.pem";
        char servCertLoc[] = "certs/server-cert.pem";
        char servKeyLoc[]  = "certs/server-key.pem";
        /* Initialize wolfSSL */
        wolfSSL_Init();

        /* Set ctx to DTLS 1.2 */
        if ((ctx = wolfSSL_CTX_new(wolfDTLSv1_2_server_method())) == NULL) {
            throw std::runtime_error("wolfSSL_CTX_new error.");
        }
        /* Load CA certificates */
        if (wolfSSL_CTX_load_verify_locations(ctx, caCertLoc, 0) !=
                SSL_SUCCESS) {
            throw std::runtime_error(std::string() +
                                     "Error loading " +
                                     caCertLoc +
                                     "please check if the file exists");
        }
        /* Load server certificates */
        if (wolfSSL_CTX_use_certificate_file(ctx, servCertLoc, SSL_FILETYPE_PEM) !=
                                                                     SSL_SUCCESS) {
            throw std::runtime_error(std::string() +
                                     "Error loading " +
                                     servCertLoc +
                                     "please check if the file exists");
        }
        /* Load server Keys */
        if (wolfSSL_CTX_use_PrivateKey_file(ctx, servKeyLoc,
                    SSL_FILETYPE_PEM) != SSL_SUCCESS) {
            throw std::runtime_error(std::string() +
                                     "Error loading " +
                                     servKeyLoc +
                                     "please check if the file exists");
        }
    }

};

#endif // VPN_SERVER_HPP
