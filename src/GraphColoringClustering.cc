#include "GraphColoringClustering.h"
#include "inet/common/packet/Packet.h"
#include "omnetpp/cdisplaystring.h"
#include "inet/common/packet/chunk/ByteCountChunk.h"
using namespace omnetpp;
using namespace inet;

Define_Module(GraphColoringClustering);

// simple mapping: colorId -> color string
static const char *COLOR_MAP[] = { "red", "green", "blue", "yellow", "white",
        "orange", "black", "gray", "magenta", "cyan" };
static const int NUM_COLORS = sizeof(COLOR_MAP) / sizeof(COLOR_MAP[0]);

// A specific ID to mark packets we are holding back for jitter
const int KIND_DELAYED_FORWARD = 999;

static const char *ROLE_NAMES[] = { "U", "CH", "M", "GW" };

//----------------------------------------------------------
// Initialization (multi-stage)
//----------------------------------------------------------
void GraphColoringClustering::initialize(int stage) {
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {

        // 1. Get a pointer to the Top-Level Network Module (ClusteringNetwork)
        cModule *network = getSimulation()->getSystemModule();

        // 2. Read the parameter from that module
        numHosts = network->par("numHosts").intValue();

        // basic state & parameters
        nodeId = getParentModule() ? getParentModule()->getIndex() : getId();
        currentColor = -1;
        role = UNDECIDED;
        clusterId = -1;

        helloInterval = par("helloInterval");
        helloJitter = par("helloJitter");
        neighborTimeout = par("neighborTimeout");
        maintenanceInterval = par("maintenanceInterval");
        coloringInterval = par("coloringInterval");
        dataInterval = par("dataInterval");
        dataJitter = par("dataJitter");

        localPort = par("localPort");
        destPort = par("destPort");

        // (optional but recommended) sanity checks
        if (helloInterval < SIMTIME_ZERO)
            throw cRuntimeError("helloInterval must be >= 0s (is %s)",
                    helloInterval.str().c_str());
        if (maintenanceInterval <= SIMTIME_ZERO)
            throw cRuntimeError("maintenanceInterval must be > 0s (is %s)",
                    maintenanceInterval.str().c_str());
        if (coloringInterval < SIMTIME_ZERO)
            throw cRuntimeError("coloringInterval must be >= 0s (is %s)",
                    coloringInterval.str().c_str());

        helloTimer = new cMessage("helloTimer");
        //colorTimer = new cMessage("colorTimer");
        maintenanceTimer = new cMessage("maintenanceTimer");
        dataTimer = new cMessage("dataTimer");
        lastDisplayColor = -1;

        // ------------- SIGNALS -------------
        // Register the signal so OMNET++ knows it exists
        roleSignal = registerSignal("role");
        // Emit initial state (UNDECIDED)
        emit(roleSignal, role);

        // Register PDR signals
        pdrSentSignal = registerSignal("dataSent");
        pdrReceivedSignal = registerSignal("dataReceived");
        // Reset counters
        numDataSent = 0;
        numDataReceived = 0;

        // Register the new change signals
        chChangeSignal = registerSignal("chChange");
        gwChangeSignal = registerSignal("gwChange");
        memberChangeSignal = registerSignal("memberChange");

        delaySignal = registerSignal("delay");

        throughputSignal = registerSignal("throughputBits");

    } else if (stage == INITSTAGE_APPLICATION_LAYER) {
        // now the protocol stack (incl. UDP) exists and is registered

        socket.setOutputGate(gate("socketOut"));
        socket.bind(localPort);

        // tell UDP we want callbacks for this socket
        socket.setCallback(this);

        // allow broadcast if we ever use it
        socket.setBroadcast(true);

        destAddress = L3AddressResolver().resolve("224.0.0.1");
        socket.joinMulticastGroup(destAddress);

        // --- schedule timers (NO uniform(), no invalid ranges) ---
        // First HELLO: after helloInterval (can be 0s if you want it at t=0)
        simtime_t smallHelloJitter = nodeId * helloJitter;
        //scheduleAt(simTime() + helloInterval + smallHelloJitter, helloTimer);
        scheduleAt(simTime() + helloInterval + uniform(0, helloJitter.dbl()),
                helloTimer);

        // First coloring: after coloringJitter (0s means "immediately")
        //scheduleAt(simTime() + coloringInterval, colorTimer);

        // First maintenance: after maintenanceInterval
        scheduleAt(simTime() + maintenanceInterval, maintenanceTimer);

        // First data packet: after dataInterval * 2 (can make it 10s)
        simtime_t smallDataJitter = nodeId * dataJitter;
        scheduleAt(simTime() + dataInterval * 2 + uniform(0, dataJitter.dbl()),
                dataTimer);
    }
}

//----------------------------------------------------------
// Message handling
//----------------------------------------------------------
void GraphColoringClustering::handleMessage(cMessage *msg) {
    if (msg->isSelfMessage()) {
        // --- UPDATED DELAYED FORWARDING LOGIC ---
        if (msg->getKind() == KIND_DELAYED_FORWARD) {
            Packet *pk = check_and_cast<Packet*>(msg);

            // 1. Recover the intended destination from the parameter we set
            int nextHop = pk->par("nextHopId");

            // 2. Look up the IP address again (in case it changed, though unlikely in 10ms)
            auto it = neighborTable.find(nextHop);

            if (it != neighborTable.end()) {
                EV_INFO << "Node " << nodeId
                               << " processing DELAYED forward to " << nextHop
                               << " (" << it->second.ipAddress << ")\n";

                socket.sendTo(pk, it->second.ipAddress, destPort);
            } else {
                EV_WARN << "Node " << nodeId
                               << " delayed packet dropped. Neighbor "
                               << nextHop << " is no longer in table.\n";
                delete pk;
            }
            return;
        }
        // ----------------------------------------
        if (msg == helloTimer)
            handleHelloTimer();
        //else if (msg == colorTimer) handleColorTimer();
        else if (msg == maintenanceTimer)
            handleMaintenanceTimer();
        else if (msg == dataTimer)
            handleDataTimer();
        else {
            EV_WARN << "Unknown self-message " << msg->getName()
                           << ", deleting.\n";
            delete msg;
        }
    } else {
        // IMPORTANT: hand all incoming UDP/indication messages to UdpSocket
        socket.processMessage(msg);
    }
}

//----------------------------------------------------------
// Timer handlers
//----------------------------------------------------------
void GraphColoringClustering::handleHelloTimer() {

    // Create a UDP packet that will carry the HELLO information
    auto *pk = new Packet("HELLO");

    // Encode GCC state as packet parameters (small & simple)
    pk->addPar("senderId") = nodeId;
    pk->addPar("color") = currentColor;
    pk->addPar("role") = role;
    pk->addPar("clusterId") = clusterId;

    // add a small dummy payload so UDP doesn't see an EmptyChunk
    const auto &payload = makeShared<inet::ByteCountChunk>(inet::B(1)); // 1 byte is enough
    pk->insertAtBack(payload);

    socket.sendTo(pk, destAddress, destPort);

    // Periodic HELLO
    simtime_t jitter = nodeId * helloJitter;
    scheduleAt(simTime() + helloInterval + uniform(0, helloJitter.dbl()),
            helloTimer);
    // scheduleAt(simTime() + helloInterval + uniform(0, helloJitter), helloTimer);
}

void GraphColoringClustering::handleColorTimer() {
    // Collect neighbor colors
    std::set<int> usedColors;
    for (const auto &entry : neighborTable) {
        int c = entry.second.color;
        if (c >= 0)
            usedColors.insert(c);
    }

    // Check if we currently conflict with a higher-priority neighbor
    // (same color but smaller nodeId). If so, we must recolor.
    bool conflictWithHigherPrio = false;
    if (currentColor >= 0) {
        for (const auto &entry : neighborTable) {
            const NeighborInfo &n = entry.second;
            if (n.color == currentColor && n.neighborId < nodeId) {
                conflictWithHigherPrio = true;
                break;
            }
        }
    }

    int newColor = currentColor;

    // Initial coloring OR conflict resolution: pick smallest unused color.
    if (currentColor < 0 || conflictWithHigherPrio) {
        newColor = 0;
        while (usedColors.find(newColor) != usedColors.end())
            ++newColor;
    }
    // CH recovery: if nobody around uses color 0, try to become CH.
    // Any clashes are resolved next rounds by the "smaller nodeId wins" rule.
    else if (!neighborTable.empty() && usedColors.find(0) == usedColors.end()
            && currentColor != 0) {
        newColor = 0;
    }

    // NEW: Color compaction for non-CH nodes (do NOT steal color 0 here)
    // if a node with lower color id leaves!
    else if (currentColor > 0) {
        int candidate = 1; // start from 1 so CH color(0) stays special
        while (usedColors.count(candidate))
            ++candidate;

        // Only move "down" (compaction), not up
        if (candidate < currentColor)
            newColor = candidate;
    }

    if (newColor != currentColor) {
        EV_INFO << "Node " << nodeId << " changes color from " << currentColor
                       << " to " << newColor << "\n";
        currentColor = newColor;
    }

    updateDisplayColor();

    // Decide CH/MEMBER/GATEWAY using the *new* clustering semantics:
    // CH <=> color==0, clusterId = CH id, gateway by clusterId mismatch.
    //recomputeRole();

    // Periodic recoloring is required for self-healing under mobility/CH loss.
    //scheduleAt(simTime() + coloringInterval, colorTimer);
}

void GraphColoringClustering::handleMaintenanceTimer() {
    // Remove neighbors that timed out
    //bool changed = pruneNeighbors();
    pruneNeighbors();

    // Topology changes may affect roles
    handleColorTimer();
    recomputeRole();

    // If neighbors changed, trigger a near-immediate recolor to compact colors
    /*if (changed) {
     // small deterministic jitter to avoid synchronized recoloring
     simtime_t j = SimTime(nodeId * 0.001); // 1ms per node
     simtime_t t = simTime() + j;

     if (!colorTimer->isScheduled() || colorTimer->getArrivalTime() > t) {
     scheduleAt(t, colorTimer);
     }
     }*/

    scheduleAt(simTime() + maintenanceInterval, maintenanceTimer);
}

void GraphColoringClustering::handleDataTimer() {
    // ---------------------------------------------------------
    // 1. SELECT TARGET (Dynamic based on numHosts)
    // ---------------------------------------------------------

    // Safety check: need at least 2 nodes to send data
    if (numHosts <= 1) {
        EV_WARN << "Node " << nodeId << ": Not enough hosts to send data.\n";
        // Reschedule to keep the timer alive
        simtime_t jitter = nodeId * dataJitter;
        scheduleAt(simTime() + dataInterval + jitter, dataTimer);
        return;
    }

    // Pick a random target between 0 and numHosts-1
    int targetNode = intuniform(0, numHosts - 1);

    // Make sure we don't send to ourselves
    while (targetNode == nodeId) {
        targetNode = intuniform(0, numHosts - 1);
    }

    // ---------------------------------------------------------
    // 2. CREATE PACKET
    // ---------------------------------------------------------
    auto *pk = new Packet("DATA");

    // Standard Parameters
    pk->addPar("srcId") = nodeId;
    pk->addPar("seqNum") = mySeqNum;
    pk->addPar("ttl") = 10;                 // Max hops
    pk->addPar("destNodeId") = targetNode; // FINAL Destination
    pk->addPar("creationTime") = simTime().dbl();

    // Add payload (100 bytes)
    const auto &payload = makeShared<inet::ByteCountChunk>(inet::B(100));
    pk->insertAtBack(payload);

    // ---------------------------------------------------------
    // 3. SENDING LOGIC (Pure Unicast Hierarchy)
    // ---------------------------------------------------------
    bool sent = false;

    // --- CASE A: I am a MEMBER or GATEWAY (Source) ---
    // If I generate data, I send it to my CH (Uplink) so they can route it.
    // (Gateways act like Members when they are the *Source* of the data).
    if (role == MEMBER || role == GATEWAY) {

        // 1. Check if I have a valid CH
        if (clusterId != -1) {
            auto it = neighborTable.find(clusterId);

            if (it != neighborTable.end()) {
                inet::L3Address chIp = it->second.ipAddress;

                // Tag: "This is for my CH"
                pk->addPar("nextHopId") = clusterId;

                EV_INFO << "Node " << nodeId << " ("
                               << (role == MEMBER ? "Member" : "Gateway")
                               << ") sending UNICAST UPLINK to CH " << clusterId
                               << " (" << chIp << ") for Target " << targetNode
                               << "\n";

                socket.sendTo(pk, chIp, destPort);
                sent = true;
            } else {
                EV_WARN << "Node " << nodeId << " has clusterId " << clusterId
                               << " but CH not in neighbor table. Dropping.\n";
                delete pk;
                pk = nullptr;
                sent = false;
            }
        } else {
            EV_WARN << "Node " << nodeId
                           << " is Orphaned (No CH). Dropping packet.\n";
            delete pk;
            pk = nullptr;
            sent = false;
        }
    }
    // --- CASE B: I am a CLUSTER HEAD (Source) ---
    else if (role == CLUSTER_HEAD) {

        // 1. Check if the target is already my neighbor (Local Delivery)
        auto it = neighborTable.find(targetNode);

        if (it != neighborTable.end()) {
            // Found it locally! Direct delivery.
            pk->addPar("nextHopId") = targetNode;
            socket.sendTo(pk, it->second.ipAddress, destPort);
            sent = true;
            EV_INFO << "Node " << nodeId << " (CH) found Target " << targetNode
                           << " locally. Sending DIRECT UNICAST.\n";
        }
        // 2. Check Routing Table (Smart Unicast to Gateway)
        else if (backboneRoutingTable.find(targetNode)
                != backboneRoutingTable.end()) {
            int gwId = backboneRoutingTable[targetNode];

            // Verify the Gateway is still a valid neighbor
            auto gwIt = neighborTable.find(gwId);
            if (gwIt != neighborTable.end() && gwIt->second.role == GATEWAY) {
                pk->addPar("nextHopId") = gwId;
                socket.sendTo(pk, gwIt->second.ipAddress, destPort);
                sent = true;
                EV_INFO << "Node " << nodeId << " (CH) found CACHED route to "
                               << targetNode << " via GW " << gwId
                               << ". Sending Unicast.\n";
            } else {
                // Route is stale (GW died or moved), remove it and fall back to flood
                backboneRoutingTable.erase(targetNode);
                goto FALLBACK_FLOOD;
            }
        }
        // 3. Fallback: Flood All Gateways
        else {
            FALLBACK_FLOOD: ;
            EV_INFO << "Node " << nodeId
                           << " (CH) route unknown. Flooding all Gateways.\n";

            int gwCount = 0;
            for (auto &entry : neighborTable) {
                if (entry.second.role == GATEWAY) {
                    Packet *gwPk = pk->dup();
                    gwPk->addPar("nextHopId") = entry.first;
                    // Jittered send
                    simtime_t delay = uniform(0.001, 0.01);
                    gwPk->setKind(KIND_DELAYED_FORWARD);
                    scheduleAt(simTime() + delay, gwPk);
                    gwCount++;
                }
            }
            if (gwCount > 0) {
                sent = true;
                delete pk; // originals deleted because we sent dups
                pk = nullptr;
            } else {
                delete pk;
                pk = nullptr;
            }
        }
    }
    // --- CASE C: UNDECIDED ---
    else {
        EV_WARN << "Node " << nodeId << " is UNDECIDED. Cannot send data.\n";
        delete pk;
        pk = nullptr;
        sent = false;
    }

    // ---------------------------------------------------------
    // 4. BOOKKEEPING & RESCHEDULE
    // ---------------------------------------------------------
    if (sent) {
        // Log this packet so I don't process my own echo (if that were possible)
        seenDataPackets.insert( { nodeId, mySeqNum });
        mySeqNum++;

        // INCREMENT AND EMIT
        numDataSent++;
        emit(pdrSentSignal, 1); // Emitting '1' allows OMNeT++ to count incidents
    }

    // Schedule the NEXT data packet
    simtime_t jitter = nodeId * dataJitter;
    scheduleAt(simTime() + dataInterval + uniform(0, dataJitter.dbl()),
            dataTimer);
}

//----------------------------------------------------------
// UDP Recieve
//----------------------------------------------------------
void GraphColoringClustering::handleUdpPacket(Packet *pk) {
    // ---------------------------------------------------------
    // CASE 1: DATA PACKET HANDLING
    // ---------------------------------------------------------
    if (strcmp(pk->getName(), "DATA") == 0) {
        int src = pk->par("srcId");
        int seq = pk->par("seqNum");
        int ttl = pk->par("ttl");
        int targetNode = pk->par("destNodeId");
        int nextHop = pk->par("nextHopId"); // The "To:" field on the envelope

        auto tag = pk->getTag<inet::L3AddressInd>();
        inet::L3Address lastHopIp = tag->getSrcAddress();

        // --- DEBUGGING LOG ---
        EV_INFO << "DATA_DEBUG: Node " << nodeId << " received packet: "
                       << " [Src=" << src << " -> Dest=" << targetNode << "]"
                       << " (TTL=" << ttl << ")" << " (NextHop=" << nextHop
                       << ")\n";
        // ---------------------

        // =========================================================
        // STEP A: LEARNING (Route Caching)
        // =========================================================
        if (role == CLUSTER_HEAD) {
            // 1. Identify which neighbor sent this to me
            int lastHopId = -1;
            for (const auto &entry : neighborTable) {
                if (entry.second.ipAddress == lastHopIp) {
                    lastHopId = entry.first;
                    break;
                }
            }

            // 2. If it came from a GATEWAY, record the path!
            // Logic: "To reach the original sender 'src', I should route via 'lastHopId'"
            if (lastHopId != -1 && neighborTable[lastHopId].role == GATEWAY) {
                backboneRoutingTable[src] = lastHopId;
                // EV_DETAIL << "Node " << nodeId << " learned route: To " << src << " -> GW " << lastHopId << "\n";
            }
        }

        // =========================================================
        // STEP B: FILTERING & CHECKS
        // =========================================================

        // 1. FILTER: Is this packet meant for someone else?
        // If nextHop is specific (not -1) and it is NOT me, I ignore it.
        if (nextHop != -1 && nextHop != nodeId) {
            EV_DETAIL << "   -> DROP: Packet meant for Node " << nextHop
                             << ", not me.\n";
            delete pk;
            return;
        }

        // --- CHECK: DID THIS COME FROM MY CLUSTER HEAD? ---
        bool fromMyCH = false;
        if (role != UNDECIDED && clusterId != -1) {
            auto it = neighborTable.find(clusterId);
            if (it != neighborTable.end()) {
                if (it->second.ipAddress == lastHopIp) {
                    fromMyCH = true;
                }
            }
        }

        // 2. DUPLICATE CHECK (With Exception for Source-Gateway)
        bool isSeen = (seenDataPackets.find( { src, seq })
                != seenDataPackets.end());

        // EXCEPTION: If I am a Gateway, and I am the Source, and my CH sent it back to me,
        // I MUST process it so I can bridge it out to foreign neighbors.
        bool isMyPacketReturning =
                (src == nodeId && role == GATEWAY && fromMyCH);

        if (isSeen && !isMyPacketReturning) {
            EV_DETAIL << "   -> DROP: Duplicate packet.\n";
            delete pk;
            return;
        }

        // If it's new, mark it.
        // If it is my packet returning, it's already marked, which is fine.
        if (!isSeen) {
            seenDataPackets.insert( { src, seq });
        }

        // 3. DELIVERY CHECK: Am I the Final Destination?
        if (nodeId == targetNode) {
            EV_INFO << "Node " << nodeId << " (Target) RECEIVED DATA from Node "
                           << src << ". DELIVERY SUCCESSFUL!\n";

            // --- CALCULATE DELAY ---
            double creationTime = pk->par("creationTime").doubleValue();
            simtime_t delay = simTime() - creationTime;

            emit(delaySignal, delay.dbl()); // Record the delay
            // -----------------------
            // --- NEW: EMIT BITS FOR THROUGHPUT ---
            // getBitLength() returns the total size (header + payload)
            emit(throughputSignal, pk->getBitLength());
            // -------------------------------------

            // [ADD THIS]
            numDataReceived++;
            emit(pdrReceivedSignal, 1);
            delete pk;
            return;
        }

        // 4. MEMBER CHECK: Members do not route traffic.
        // If I am a Member and I wasn't the target (checked above), I drop it.
        if (role == MEMBER) {
            EV_DETAIL << "   -> DROP: Member node ignores non-target packet.\n";
            delete pk;
            return;
        }

        // 5. ROUTING LOGIC (Cluster Head or Gateway)
        if (ttl <= 0) {
            EV_WARN << "Node " << nodeId << ": TTL expired. Dropping packet.\n";
            delete pk;
            return;
        }

        // Create the packet to forward
        Packet *forwardPk = new Packet("DATA");
        forwardPk->addPar("srcId") = src;
        forwardPk->addPar("seqNum") = seq;
        forwardPk->addPar("ttl").setLongValue(ttl - 1);
        forwardPk->addPar("destNodeId") = targetNode;
        forwardPk->addPar("creationTime") =
                pk->par("creationTime").doubleValue();
        forwardPk->insertAtBack(makeShared<inet::ByteCountChunk>(inet::B(100)));

        inet::L3Address forwardIp = destAddress; // Default to Multicast (224.0.0.1)

        // --- ROLE SPECIFIC LOGIC ---
        // ====================================================================
        // A. CLUSTER HEAD LOGIC (Downlink or Forward to MY Gateways)
        // ====================================================================

        // --- CLUSTER HEAD LOGIC ---
        if (role == CLUSTER_HEAD) {

            // 1. CHECK LOCAL NEIGHBORS (Direct Delivery)
            auto it = neighborTable.find(targetNode);
            if (it != neighborTable.end()) {
                forwardPk->addPar("nextHopId") = targetNode;
                socket.sendTo(forwardPk, it->second.ipAddress, destPort);
                EV_INFO << "Node " << nodeId << " (CH) delivering locally to "
                               << targetNode << "\n";

                // We sent the template directly, so we are done.
                // Note: If you want to be safe with memory, you could set forwardPk = nullptr
                // but strictly speaking, socket.sendTo takes ownership or copies depending on version.
                // In standard INET, sendTo takes ownership.
            }

            // 2. CHECK ROUTING TABLE (Smart Unicast to Gateway)
            else if (backboneRoutingTable.find(targetNode)
                    != backboneRoutingTable.end()) {
                int gwId = backboneRoutingTable[targetNode];

                // Verify the Gateway is still a valid neighbor and IS a Gateway
                auto gwIt = neighborTable.find(gwId);
                if (gwIt != neighborTable.end()
                        && gwIt->second.role == GATEWAY) {

                    forwardPk->addPar("nextHopId") = gwId;

                    // Jittered Unicast to ONE Gateway
                    forwardPk->setKind(KIND_DELAYED_FORWARD);
                    simtime_t delay = uniform(0.001, 0.015); // Small jitter
                    scheduleAt(simTime() + delay, forwardPk);

                    EV_INFO << "Node " << nodeId
                                   << " (CH) forwarding to CACHED GW " << gwId
                                   << "\n";
                } else {
                    // Stale route (GW died or moved) -> Remove & Fallback
                    backboneRoutingTable.erase(targetNode);
                    goto CH_FORWARD_FLOOD;
                }
            }

            // 3. FALLBACK: FLOOD ALL GATEWAYS
            else {
                CH_FORWARD_FLOOD: ;
                EV_INFO << "Node " << nodeId
                               << " (CH) route unknown. Flooding all Gateways.\n";

                int gwCount = 0;
                for (auto &entry : neighborTable) {
                    if (entry.second.role == GATEWAY) {
                        // Create Copy
                        Packet *gwCopy = forwardPk->dup();

                        gwCopy->addPar("nextHopId") = entry.first;
                        gwCopy->setKind(KIND_DELAYED_FORWARD);

                        // Jittered Schedule
                        simtime_t delay = uniform(0.001, 0.015);
                        scheduleAt(simTime() + delay, gwCopy);

                        gwCount++;
                    }
                }

                // Delete the template (forwardPk) because we sent duplicates (or didn't send anything)
                delete forwardPk;
            }
        }
        // ====================================================================
        // B. GATEWAY LOGIC (The "Bridge")
        // ====================================================================
        else if (role == GATEWAY) {

            // We need to know: Did this come from MY Cluster Head?
            bool fromMyCH = false;
            if (clusterId != -1
                    && neighborTable.find(clusterId) != neighborTable.end()) {
                if (neighborTable[clusterId].ipAddress == lastHopIp) {
                    fromMyCH = true;
                }
            }

            if (fromMyCH) {
                // --- DIRECTION: OUTBOUND (CH -> GW -> Foreign GWs) ---
                EV_INFO << "Node " << nodeId
                               << " (GW) received from CH. Bridging OUT to foreign clusters.\n";

                bool sent = false;
                for (auto &entry : neighborTable) {
                    const NeighborInfo &n = entry.second;

                    // CRITERIA: Neighbor must be in a DIFFERENT cluster
                    // AND must be part of the backbone (Gateway or CH)
                    if (n.clusterId != clusterId
                            && (n.role == GATEWAY || n.role == CLUSTER_HEAD)) {

                        Packet *copy = forwardPk->dup();
                        copy->addPar("nextHopId") = n.neighborId;
                        copy->setKind(KIND_DELAYED_FORWARD);
                        simtime_t delay = uniform(0.001, 0.015);

                        // 5. Schedule it
                        scheduleAt(simTime() + delay, copy);
                        //socket.sendTo(copy, n.ipAddress, destPort);
                        sent = true;
                        EV_DETAIL
                                         << "   -> Forwarding to foreign backbone node "
                                         << n.neighborId << "\n";
                    }
                }
                if (!sent)
                    EV_DETAIL
                                     << "   -> No foreign neighbors found. Dead end.\n";
                delete forwardPk;
            } else {
                // --- DIRECTION: INBOUND (Foreign GW -> GW -> My CH) ---
                EV_INFO << "Node " << nodeId
                               << " (GW) received from Foreign neighbor. Bridging IN to CH.\n";

                if (clusterId != -1
                        && neighborTable.find(clusterId)
                                != neighborTable.end()) {
                    forwardPk->addPar("nextHopId") = clusterId;
                    socket.sendTo(forwardPk, neighborTable[clusterId].ipAddress,
                            destPort);
                } else {
                    EV_WARN << "   -> Orphaned Gateway (no CH). Dropping.\n";
                    delete forwardPk;
                }
            }
        }
        // ====================================================================
        delete pk; // Delete original packet
        return;
    }
    // --- CASE 2: HELLO PACKET (Your existing code) ---
    else {
        int sender = (int) pk->par("senderId");
        int neighCol = (int) pk->par("color");
        int neighRole = (int) pk->par("role");
        int neighCid = (int) pk->par("clusterId");
        // --- NEW: Extract IP Address ---
        auto tag = pk->getTag<inet::L3AddressInd>();
        inet::L3Address senderIp = tag->getSrcAddress();

        if (sender == nodeId) {
            delete pk;
            return;
        }

        EV_INFO << "Node " << nodeId << " received HELLO from " << sender
                       << " (color=" << neighCol << ", role=" << neighRole
                       << ", clusterId=" << neighCid << ")\n";

        NeighborInfo info;
        info.neighborId = sender;
        info.ipAddress = senderIp; // <--- Save it to the table
        info.color = neighCol;
        info.role = neighRole;
        info.clusterId = neighCid;
        info.lastHeard = simTime();

        neighborTable[sender] = info;

        // New info may change our role
        recomputeRole();

        delete pk;
    }
}

//----------------------------------------------------------
// UdpSocket Callbacks
//----------------------------------------------------------
void GraphColoringClustering::socketDataArrived(UdpSocket *socket, Packet *pk) {
    EV_INFO << "Node " << nodeId << " received packet " << pk->getName()
                   << " of length " << pk->getByteLength() << " bytes\n";

    // reuse your existing logic
    handleUdpPacket(pk);
}

//----------------------------------------------------------
// Coloring & neighbor maintenance
//----------------------------------------------------------
bool GraphColoringClustering::pruneNeighbors() {
    simtime_t now = simTime();
    std::vector<int> toRemove;

    for (const auto &entry : neighborTable) {
        if (now - entry.second.lastHeard > neighborTimeout) {
            toRemove.push_back(entry.first);
        }
    }

    for (int id : toRemove) {
        EV_INFO << "Node " << nodeId << " removing stale neighbor " << id
                       << "\n";
        neighborTable.erase(id);
    }
    return !toRemove.empty();
}

void GraphColoringClustering::updateDisplayColor() {
    if (!hasGUI())
        return;  // skip in Cmdenv

    if (currentColor == lastDisplayColor)
        return;  // nothing to do

    lastDisplayColor = currentColor;

    cModule *host = getParentModule();
    if (!host)
        return;

    omnetpp::cDisplayString &ds = host->getDisplayString();

    // 1. Update Color (Your existing logic)
    if (currentColor < 0) {
        ds.setTagArg("i", 1, "");
    } else {
        const char *col = COLOR_MAP[currentColor % NUM_COLORS];
        ds.setTagArg("i", 1, col);
    }

    // 2. Update Text Label (Role)
    /*const char *roleStr = ROLE_NAMES[role];

     ds.setTagArg("t", 0, roleStr);
     ds.setTagArg("t", 2, "black");
     ds.parse(ds.str());*/
}

//----------------------------------------------------------
// Decide role (CH / MEMBER / GATEWAY) based on colors and neighbors
//----------------------------------------------------------
void GraphColoringClustering::recomputeRole() {
    int oldRole = role;

    int oldClusterId = clusterId;

    // New semantics:
    //   - Cluster Head (CH)  <=> color == 0
    //   - CH's clusterId     = nodeId
    //   - Non-CH clusterId   = chosen CH id among 1-hop neighbors (lowest id)
    //   - Gateway            = hears any neighbor whose clusterId != my clusterId
    if (currentColor < 0) {
        role = UNDECIDED;
        clusterId = -1;
    } else if (currentColor == 0) {
        role = CLUSTER_HEAD;
        clusterId = nodeId;
    } else {
        // Join a CH we can hear (strict 1-hop attachment)
        int chosenChId = -1;
        for (const auto &entry : neighborTable) {
            const NeighborInfo &n = entry.second;
            //if (n.neighborId == nodeId) continue;
            if (n.color == 0) { // CH candidate under our rule
                if (chosenChId < 0 || n.neighborId < chosenChId)
                    chosenChId = n.neighborId;
            }
        }
        clusterId = chosenChId;

        if (clusterId < 0) {
            // Not clustered yet (e.g., before convergence). Keep it undecided.
            role = UNDECIDED;
            currentColor = -1;
        } else {
            // Gateway if we hear anyone from a different cluster
            bool hearsOtherCluster = false;
            for (const auto &entry : neighborTable) {
                const NeighborInfo &n = entry.second;
                //if (n.neighborId == nodeId) continue;
                if (n.clusterId >= 0 && n.clusterId != clusterId) {
                    hearsOtherCluster = true;
                    break;
                }
            }
            role = hearsOtherCluster ? GATEWAY : MEMBER;
        }
    }

    if (role != oldRole) {
        // 1. Remove myself from the OLD role count
        if (oldRole == CLUSTER_HEAD)
            emit(chChangeSignal, -1);
        else if (oldRole == GATEWAY)
            emit(gwChangeSignal, -1);
        else if (oldRole == MEMBER)
            emit(memberChangeSignal, -1);

        // 2. Add myself to the NEW role count
        if (role == CLUSTER_HEAD)
            emit(chChangeSignal, 1);
        else if (role == GATEWAY)
            emit(gwChangeSignal, 1);
        else if (role == MEMBER)
            emit(memberChangeSignal, 1);
        // Signal to the simulation
        emit(roleSignal, role);
        EV_INFO << "Node " << nodeId << " updates state: role " << oldRole
                       << " -> " << role << ", clusterId " << oldClusterId
                       << " -> " << clusterId << " (color=" << currentColor
                       << ")\n";
    } else if (clusterId != oldClusterId) {
        EV_INFO << "Node " << nodeId << " updates state: role " << oldRole
                       << " -> " << role << ", clusterId " << oldClusterId
                       << " -> " << clusterId << " (color=" << currentColor
                       << ")\n";

    }
}

void GraphColoringClustering::finish() {
    cancelAndDelete(helloTimer);
    //cancelAndDelete(colorTimer);
    cancelAndDelete(maintenanceTimer);
    cancelAndDelete(dataTimer);
}
