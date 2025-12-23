/* //last code before sunday
 * GraphColoringClustering.h
 *
 *  Created on: Nov 27, 2025
 *      Author: opp_env
 */

/*
 * GraphColoringClustering.h
 */

#ifndef __CLUSTERINGMANET_GRAPHCOLORINGCLUSTERING_H_
#define __CLUSTERINGMANET_GRAPHCOLORINGCLUSTERING_H_

#include <map>
#include <set>

#include "omnetpp.h"

#include "inet/common/InitStages.h"
#include "inet/common/packet/Packet.h"
#include "inet/transportlayer/contract/udp/UdpSocket.h"
#include "inet/networklayer/common/L3AddressResolver.h"

class GraphColoringClustering : public omnetpp::cSimpleModule, public inet::UdpSocket::ICallback
{
  public:
    enum Role {
        UNDECIDED    = 0,
        CLUSTER_HEAD = 1,
        MEMBER       = 2,
        GATEWAY      = 3
    };

  protected:
    // --- node state ---
    int nodeId = -1;
    int currentColor = -1;
    int role = UNDECIDED;
    int clusterId = -1;

    // --- timing parameters ---
    omnetpp::simtime_t helloInterval;
    omnetpp::simtime_t helloJitter;
    omnetpp::simtime_t neighborTimeout;
    omnetpp::simtime_t maintenanceInterval;
    omnetpp::simtime_t coloringJitter;

    // --- self messages ---
    omnetpp::cMessage *helloTimer = nullptr;
    omnetpp::cMessage *colorTimer = nullptr;
    omnetpp::cMessage *maintenanceTimer = nullptr;

    // --- UDP state ---
    inet::UdpSocket socket;
    inet::L3Address destAddress;
    int localPort = -1;
    int destPort  = -1;

    // --- visualization ---
    int lastDisplayColor = -1;

    // --- neighbor table ---
    struct NeighborInfo {
        int neighborId;
        int color;
        int role;
        int clusterId;
        omnetpp::simtime_t lastHeard;
    };

    std::map<int, NeighborInfo> neighborTable;

  protected:
    // multi-stage initialization (INET style)
    virtual int numInitStages() const override { return inet::NUM_INIT_STAGES; }
    virtual void initialize(int stage) override;
    virtual void handleMessage(omnetpp::cMessage *msg) override;
    virtual void finish() override;

    // timer handlers
    void handleHelloTimer();
    void handleColorTimer();
    void handleMaintenanceTimer();

    // UDP receive
    void handleUdpPacket(inet::Packet *pk);

    // NEW: UdpSocket callbacks
       virtual void socketDataArrived(inet::UdpSocket *socket, inet::Packet *pk) override;
       virtual void socketErrorArrived(inet::UdpSocket *socket, inet::Indication *indication) override {}
       virtual void socketClosed(inet::UdpSocket *socket) override {}

    // clustering helpers
    int  chooseGreedyColor() const;
    void pruneNeighbors();
    void updateDisplayColor();

    void recomputeRole();
    int  computeLocalMinColor() const;
    //bool hearsMultipleClusterHeads() const;   UNUSED
    bool hasSmallerIdSameColor() const;
};

#endif /* __CLUSTERINGMANET_GRAPHCOLORINGCLUSTERING_H_ */
