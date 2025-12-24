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

//----------------------------------------------------------
// Initialization (multi-stage)
//----------------------------------------------------------
void GraphColoringClustering::initialize(int stage) {
    cSimpleModule::initialize(stage);

    if (stage == INITSTAGE_LOCAL) {
        // basic state & parameters
        nodeId = getParentModule() ? getParentModule()->getIndex() : getId();
        currentColor = -1;
        role = UNDECIDED;
        clusterId = -1;

        helloInterval = par("helloInterval");
        helloJitter = par("helloJitter");
        neighborTimeout = par("neighborTimeout");
        maintenanceInterval = par("maintenanceInterval");
        coloringJitter = par("coloringJitter");

        localPort = par("localPort");
        destPort = par("destPort");

        // (optional but recommended) sanity checks
        if (helloInterval < SIMTIME_ZERO)
            throw cRuntimeError("helloInterval must be >= 0s (is %s)",
                    helloInterval.str().c_str());
        if (maintenanceInterval <= SIMTIME_ZERO)
            throw cRuntimeError("maintenanceInterval must be > 0s (is %s)",
                    maintenanceInterval.str().c_str());
        if (coloringJitter < SIMTIME_ZERO)
            throw cRuntimeError("coloringJitter must be >= 0s (is %s)",
                    coloringJitter.str().c_str());

        helloTimer = new cMessage("helloTimer");
        colorTimer = new cMessage("colorTimer");
        maintenanceTimer = new cMessage("maintenanceTimer");
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
        simtime_t jitter = nodeId * helloJitter;
        scheduleAt(simTime() + helloInterval + jitter, helloTimer);
        //scheduleAt(simTime() + helloInterval + uniform(0, helloJitter), helloTimer);

        // First coloring: after coloringJitter (0s means "immediately")
        scheduleAt(simTime() + coloringJitter, colorTimer);

        // First maintenance: after maintenanceInterval
        scheduleAt(simTime() + maintenanceInterval, maintenanceTimer);
    }
}

//----------------------------------------------------------
// Message handling
//----------------------------------------------------------
void GraphColoringClustering::handleMessage(cMessage *msg) {
    if (msg->isSelfMessage()) {
        if (msg == helloTimer)
            handleHelloTimer();
        else if (msg == colorTimer)
            handleColorTimer();
        else if (msg == maintenanceTimer)
            handleMaintenanceTimer();
        else {
            EV_WARN << "Unknown self-message " << msg->getName()
                           << ", deleting.\n";
            delete msg;
        }
    } else {
        // IMPORTANT: hand all incoming UDP/indication messages to UdpSocket
        socket.processMessage(msg);
    }
    //OLD!!!!
    /*if (msg->isSelfMessage()) {
     if (msg == helloTimer)
     handleHelloTimer();
     else if (msg == colorTimer)
     handleColorTimer();
     else if (msg == maintenanceTimer)
     handleMaintenanceTimer();
     else {
     EV_WARN << "Unknown self-message " << msg->getName()
     << ", deleting.\n";
     delete msg;
     }
     } else if (msg->arrivedOn("socketIn")) {
     EV_INFO << "message arrived!!!!!!!!!" << "\n";
     auto *pk = check_and_cast<Packet*>(msg);
     handleUdpPacket(pk);
     } else {
     EV_WARN << "Message arrived on unexpected gate, deleting.\n";
     delete msg;
     }*/
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
    scheduleAt(simTime() + helloInterval + jitter, helloTimer);
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

        updateDisplayColor();
    }

    // Decide CH/MEMBER/GATEWAY using the *new* clustering semantics:
    // CH <=> color==0, clusterId = CH id, gateway by clusterId mismatch.
    recomputeRole();

    // Periodic recoloring is required for self-healing under mobility/CH loss.
    scheduleAt(simTime() + coloringJitter, colorTimer);
}

void GraphColoringClustering::handleMaintenanceTimer() {
    // Remove neighbors that timed out
    bool changed = pruneNeighbors();
    pruneNeighbors();

    // Topology changes may affect roles
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

// new helper
void GraphColoringClustering::handleUdpPacket(Packet *pk) {
    int sender = (int) pk->par("senderId");
    int neighCol = (int) pk->par("color");
    int neighRole = (int) pk->par("role");
    int neighCid = (int) pk->par("clusterId");

    //if (sender == nodeId) {
    //    delete pk;
    //    return;
    //}

    EV_INFO << "Node " << nodeId << " received HELLO from " << sender
                   << " (color=" << neighCol << ", role=" << neighRole
                   << ", clusterId=" << neighCid << ")\n";

    NeighborInfo info;
    info.neighborId = sender;
    info.color = neighCol;
    info.role = neighRole;
    info.clusterId = neighCid;
    info.lastHeard = simTime();

    neighborTable[sender] = info;

    // New info may change our role
    recomputeRole();

    delete pk;
}

void GraphColoringClustering::socketDataArrived(UdpSocket *socket, Packet *pk) {
    EV_INFO << "Node " << nodeId << " received packet " << pk->getName()
                   << " of length " << pk->getByteLength() << " bytes\n";

    // reuse your existing logic
    handleUdpPacket(pk);
}

//----------------------------------------------------------
// Coloring & neighbor maintenance
//----------------------------------------------------------
int GraphColoringClustering::chooseGreedyColor() const {
    // Collect neighbor colors
    std::set<int> usedColors;
    for (const auto &entry : neighborTable) {
        int c = entry.second.color;
        if (c >= 0) {
            usedColors.insert(c);
        }
    }

    // Greedy: smallest non-used integer >= 0
    int candidate = 0;
    while (usedColors.find(candidate) != usedColors.end()) {
        ++candidate;
    }
    return candidate;
}

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

    if (currentColor < 0)
        return;  // uncolored

    if (currentColor == lastDisplayColor)
        return;  // nothing to do

    lastDisplayColor = currentColor;

    cModule *host = getParentModule();
    if (!host)
        return;

    omnetpp::cDisplayString &ds = host->getDisplayString();

    const char *col = COLOR_MAP[currentColor % NUM_COLORS];

    // tint the icon
    ds.setTagArg("i", 1, col);

    // (optional) also color the background instead/too:
    // ds.setTagArg("b", 0, col);
    // ds.setTagArg("b", 1, "oval");
}

// Minimum color among this node + its neighbors
int GraphColoringClustering::computeLocalMinColor() const {
    int minColor = -1;

    if (currentColor >= 0)
        minColor = currentColor;

    for (const auto &entry : neighborTable) {
        int c = entry.second.color;
        if (c < 0)
            continue;
        if (minColor < 0 || c < minColor)
            minColor = c;
    }

    return minColor;
}

bool GraphColoringClustering::hasSmallerIdSameColor() const {
    for (const auto &entry : neighborTable) {
        const NeighborInfo &n = entry.second;
        if (n.color == currentColor && n.neighborId < nodeId)
            return true;
    }
    return false;
}

// Decide role (CH / MEMBER / GATEWAY) based on colors and neighbors
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
    cancelAndDelete(colorTimer);
    cancelAndDelete(maintenanceTimer);
}

