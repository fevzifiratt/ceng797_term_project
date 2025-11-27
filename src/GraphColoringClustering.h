/*
 * GraphColoringClustering.h
 *
 *  Created on: Nov 27, 2025
 *      Author: opp_env
 */

#ifndef __GRAPH_COLORING_CLUSTERING_H_
#define __GRAPH_COLORING_CLUSTERING_H_

#include<map>
#include<vector>
#include<set>
#include<string>
#include "omnetpp.h"
#include "HelloMessage_m.h"

class GraphColoringClustering: public omnetpp::cSimpleModule {
public:
    enum Role {
        UNDECIDED = 0, CLUSTER_HEAD = 1, MEMBER = 2, GATEWAY = 3
    };
protected:
    // Node identity and state
    int nodeId;
    int currentColor;  // greedy color
    int role;          // UNDECIDED / CH / MEMBER / GATEWAY
    int clusterId;     // cluster identifier (here: same as currentColor)

    // Parameters
    omnetpp::simtime_t helloInterval;
    omnetpp::simtime_t neighborTimeout;
    omnetpp::simtime_t maintenanceInterval;
    omnetpp::simtime_t coloringJitter;

    // Timers (self-messages)
    omnetpp::cMessage *helloTimer;
    omnetpp::cMessage *colorTimer;
    omnetpp::cMessage *maintenanceTimer;

    // Neighbor info struct
    struct NeighborInfo {
        int neighborId;
        int color;
        int role;
        omnetpp::simtime_t lastHeard;
    };

    // neighborId -> NeighborInfo
    std::map<int, NeighborInfo> neighborTable;

protected:
    virtual void initialize() override;
    virtual void handleMessage(omnetpp::cMessage *msg) override;
    virtual void finish() override;

    // Timer handlers
    void handleHelloTimer();
    void handleColorTimer();
    void handleMaintenanceTimer();
    void handleHelloMessage(HelloMessage *hello);

    // Coloring / clustering helpers
    int chooseGreedyColor() const;
    void pruneNeighbors();

    void recomputeRole();                    // decide CH / MEMBER / GATEWAY
    int computeLocalMinColor() const;     // minimum color in 1-hop neighborhood
    bool hearsMultipleClusterHeads() const; // detect GATEWAY condition
};

#endif /* __GRAPH_COLORING_CLUSTERING_H_ */
