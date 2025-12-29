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
        scheduleAt(simTime() + helloInterval + uniform(0, helloJitter.dbl()), helloTimer);

        // First coloring: after coloringJitter (0s means "immediately")
        //scheduleAt(simTime() + coloringInterval, colorTimer);

        // First maintenance: after maintenanceInterval
        scheduleAt(simTime() + maintenanceInterval, maintenanceTimer);

        // First data packet: after dataInterval * 2 (can make it 10s)
        simtime_t smallDataJitter = nodeId * dataJitter;
        scheduleAt(simTime() + dataInterval * 2 + uniform(0, dataJitter.dbl()), dataTimer);
    }
}

//----------------------------------------------------------
// Message handling
//----------------------------------------------------------
void GraphColoringClustering::handleMessage(cMessage *msg) {
    if (msg->isSelfMessage()) {
        // --- NEW BLOCK START ---
        if (msg->getKind() == KIND_DELAYED_FORWARD) {
            // This is a packet we scheduled earlier. Now we send it.
            // We must cast it back to a Packet*
            Packet *pk = check_and_cast<Packet*>(msg);

            EV_INFO << "Node " << nodeId << " processing DELAYED forward for "
                           << pk->getName() << "\n";

            // Send it to the multicast address
            socket.sendTo(pk, destAddress, destPort);
            return;
        }
        // --- NEW BLOCK END ---
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
    scheduleAt(simTime() + helloInterval + uniform(0, helloJitter.dbl()), helloTimer);
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
            // Reschedule anyway to keep the timer alive for later
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
        pk->addPar("ttl") = 6;           // Max hops
        pk->addPar("destNodeId") = targetNode; // FINAL Destination

        // Add payload (100 bytes)
        const auto &payload = makeShared<inet::ByteCountChunk>(inet::B(100));
        pk->insertAtBack(payload);

        // ---------------------------------------------------------
        // 3. SENDING LOGIC (Uplink vs. Search)
        // ---------------------------------------------------------
        bool sent = false;

        // --- CASE A: I am a Simple MEMBER ---
        // I should send this ONLY to my Cluster Head (Uplink).
        if (role == MEMBER && clusterId != -1) {

            // Look up my CH in the neighbor table to find their IP
            auto it = neighborTable.find(clusterId);

            if (it != neighborTable.end()) {
                inet::L3Address chIp = it->second.ipAddress;

                // Logical Tag: "This is for my CH"
                pk->addPar("nextHopId") = clusterId;

                EV_INFO << "Node " << nodeId << " (Member) sending UNICAST UPLINK to CH "
                        << clusterId << " (" << chIp << ") for Target " << targetNode << "\n";

                // Send Unicast to CH's IP
                socket.sendTo(pk, chIp, destPort);
                sent = true;
            }
            else {
                // Error: I have a clusterId, but I don't see the CH in my table anymore
                EV_WARN << "Node " << nodeId << " (Member) cannot find IP for CH "
                        << clusterId << ". Dropping packet.\n";
                delete pk;
                sent = false;
            }
        }
        // --- CASE B: I am a CLUSTER HEAD ---
            else if (role == CLUSTER_HEAD) {
                // OPTIMIZATION: Check if the target is already my neighbor!
                auto it = neighborTable.find(targetNode);

                if (it != neighborTable.end()) {
                    // Found it locally! Direct delivery.
                    pk->addPar("nextHopId") = targetNode;
                    socket.sendTo(pk, it->second.ipAddress, destPort); // Unicast Downlink
                    sent = true;
                    EV_INFO << "Node " << nodeId << " (CH) found Target " << targetNode
                            << " locally. Sending DIRECT UNICAST.\n";
                } else {
                    // Not found. Start Backbone Search.
                    pk->addPar("nextHopId") = -1;
                    socket.sendTo(pk, destAddress, destPort); // Multicast Search
                    sent = true;
                    EV_INFO << "Node " << nodeId << " (CH) of target Node " << targetNode << " not local. Flooding Backbone.\n";
                }
            }
            // --- CASE C: GATEWAY / UNDECIDED ---
            else {
                // Gateways just start the search immediately
                pk->addPar("nextHopId") = -1;
                socket.sendTo(pk, destAddress, destPort);
                sent = true;
            }

        // ---------------------------------------------------------
        // 4. BOOKKEEPING & RESCHEDULE
        // ---------------------------------------------------------
        if (sent) {
            // Log this packet so I don't process my own echo
            seenDataPackets.insert( { nodeId, mySeqNum });
            mySeqNum++;
        }

        // Schedule the NEXT data packet
        simtime_t jitter = nodeId * dataJitter;
        scheduleAt(simTime() + dataInterval * 2 + uniform(0, dataJitter.dbl()), dataTimer);
    //OLD IMPLEMENTATION
    /*// 1. Create generic packet
    auto *pk = new Packet("DATA");

    // 2. Add parameters manually
    pk->addPar("srcId") = nodeId;
    pk->addPar("seqNum") = mySeqNum;
    pk->addPar("ttl") = 5;

    // 3. Mark as seen
    seenDataPackets.insert( { nodeId, mySeqNum });
    mySeqNum++;

    // add a small dummy payload so UDP doesn't see an EmptyChunk
    const auto &payload = makeShared<inet::ByteCountChunk>(inet::B(100)); // 1 byte is enough
    pk->insertAtBack(payload);

    // 4. Send Broadcast
    socket.sendTo(pk, destAddress, destPort);

    EV_INFO << "Node " << nodeId << " generated DATA packet seq "
                   << (mySeqNum - 1) << "\n";

    // 5. Schedule NEXT packet using dataInterval
    simtime_t jitter = nodeId * dataJitter;
    scheduleAt(simTime() + dataInterval + jitter, dataTimer); // <--- Reschedule renamed timer*/
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

            // --- DEBUGGING LOG ---
                    EV_INFO << "DATA_DEBUG: Node " << nodeId << " received packet: "
                            << " [Src=" << src
                            << " -> Dest=" << targetNode << "]"
                            << " (TTL=" << ttl << ")"
                            << " (NextHop=" << nextHop << ")\n";
                    // ---------------------

            // 1. FILTER: Is this packet meant for someone else?
            // If nextHop is specific (not -1) and it is NOT me, I ignore it.
            if (nextHop != -1 && nextHop != nodeId) {
                EV_DETAIL << "   -> DROP: Packet meant for Node " << nextHop << ", not me.\n";
                 delete pk;
                 return;
            }

            // 2. DUPLICATE CHECK: Have I processed this before?
            if (seenDataPackets.find({src, seq}) != seenDataPackets.end()) {
                EV_DETAIL << "   -> DROP: Duplicate packet.\n";
                delete pk;
                return;
            }
            seenDataPackets.insert({src, seq});

            // 3. DELIVERY CHECK: Am I the Final Destination?
            if (nodeId == targetNode) {
                EV_INFO << "Node " << nodeId << " (Target) RECEIVED DATA from Node " << src
                        << ". DELIVERY SUCCESSFUL!\n";
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
            forwardPk->insertAtBack(makeShared<inet::ByteCountChunk>(inet::B(100)));

            bool shouldForward = false;
            inet::L3Address forwardIp = destAddress; // Default to Multicast (224.0.0.1)

            // --- ROLE SPECIFIC LOGIC ---

            if (role == CLUSTER_HEAD) {
                // CHECK: Is the target in my local neighbor table?
                auto it = neighborTable.find(targetNode);

                if (it != neighborTable.end()) {
                    // FOUND LOCALLY -> UNICAST DOWNLINK
                    inet::L3Address targetIp = it->second.ipAddress;

                    forwardPk->addPar("nextHopId") = targetNode; // Tag specifically for target
                    forwardIp = targetIp;                        // Use Real IP Unicast
                    shouldForward = true;

                    EV_INFO << "Node " << nodeId << " (CH) found Target " << targetNode
                            << " locally. Sending UNICAST to " << targetIp << "\n";
                }
                else {
                    // NOT FOUND -> BACKBONE FLOOD
                    forwardPk->addPar("nextHopId") = -1; // -1 = Search Mode
                    forwardIp = destAddress;             // Multicast
                    shouldForward = true;

                    EV_INFO << "Node " << nodeId << " (CH) target not here. Flooding Backbone.\n";
                }
            }
            else if (role == GATEWAY) {
                // GATEWAY -> RELAY THE SEARCH
                forwardPk->addPar("nextHopId") = -1; // Keep it as Search
                forwardIp = destAddress;             // Multicast
                shouldForward = true;

                EV_INFO << "   -> FORWARD (GW): Relaying Backbone Search (TTL=" << (ttl-1) << ")\n";
            }

            // 6. EXECUTE FORWARDING (With Jitter)
            if (shouldForward) {
                // We use the delayed mechanism to avoid collisions
                forwardPk->setKind(KIND_DELAYED_FORWARD);

                // CRITICAL: We need to know *where* to send it when the timer expires.
                // Since we can't easily attach the IP to the message object itself without a custom class,
                // we will cheat slightly: If it's unicast, we send immediately.
                // If it's broadcast, we use jitter.

                if (forwardIp.isMulticast()) {
                     scheduleAt(simTime() + uniform(0, 0.01), forwardPk);
                } else {
                     // Unicast is safe to send immediately (MAC handles contention)
                     socket.sendTo(forwardPk, forwardIp, destPort);
                }
            } else {
                delete forwardPk;
            }

            delete pk; // Delete original packet
            return;
        }
            //OLD!!!!
    // --- CASE 1: DATA PACKET (The Forwarding Filter) ---
    /*if (strcmp(pk->getName(), "DATA") == 0) {
        int src = pk->par("srcId");
        int seq = pk->par("seqNum");
        int ttl = pk->par("ttl");

        // 1. Duplicate Check
        if (seenDataPackets.find( { src, seq }) != seenDataPackets.end()) {
            // Optional: Log duplicates if you want to see how "noisy" the network is
            EV_DETAIL << "Node " << nodeId << " dropped DUPLICATE Data from "
                             << src << " (Seq " << seq << ")\n";
            delete pk;
            return;
        }
        seenDataPackets.insert( { src, seq });

        // --- ENHANCED LOGGING HERE ---
        // Shows: Who I am (ID, Role, Cluster), Who sent it, and Packet Details
        EV_INFO << "Node " << nodeId << " [Role=" << role << ", Cluster="
                       << clusterId << "]" << " RECEIVED Data from Node " << src
                       << " (Seq=" << seq << ", TTL=" << ttl << ")\n";

        // 2. THE FILTER: MEMBERS STOP HERE
        if (role == MEMBER || role == UNDECIDED) {
            EV_INFO
                           << "   -> STOP: I am a MEMBER/UNDECIDED. Consumed packet. NOT forwarding.\n";
            delete pk;
            return;
        }

        // 3. TTL Check
        if (ttl <= 0) {
            EV_WARN << "   -> DROP: TTL expired.\n";
            delete pk;
            return;
        }

        // 4. Forwarding (I am CH or Gateway)
        EV_INFO
                       << "   -> FORWARD: I am Backbone (CH/GW). Scheduling forward with jitter.\n";

        Packet *forwardPk = new Packet("DATA");

        // Copy parameters
        forwardPk->addPar("srcId") = src;
        forwardPk->addPar("seqNum") = seq;
        forwardPk->addPar("ttl").setLongValue(ttl - 1);

        // Add payload
        const auto &payload = makeShared<inet::ByteCountChunk>(inet::B(100));
        forwardPk->insertAtBack(payload);

        // --- NEW LOGIC: JITTER ---

        // 1. Mark this packet so handleMessage knows it's a forward
        forwardPk->setKind(KIND_DELAYED_FORWARD);

        // 2. Pick a random delay (e.g., between 0ms and 10ms)
        // 10ms (0.01s) is usually enough to let the MAC layer breathe
        simtime_t forwardJitter = uniform(0, 0.01);

        // 3. Schedule the packet to "arrive" at ourselves after the delay
        scheduleAt(simTime() + forwardJitter, forwardPk);

        delete pk; // Delete the incoming packet as usual
        return;
    }*/

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

    if (role != oldRole || clusterId != oldClusterId) {
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
