#include "GraphColoringClustering.h"

using namespace omnetpp;

Define_Module(GraphColoringClustering);

void GraphColoringClustering::initialize()
{
    // Node ID: use parent index if available, otherwise module ID
    nodeId = getParentModule() ? getParentModule()->getIndex() : getId();

    currentColor = -1;     // uncolored
    role        = UNDECIDED;
    clusterId   = -1;

    // Read parameters
    helloInterval       = par("helloInterval");
    neighborTimeout     = par("neighborTimeout");
    maintenanceInterval = par("maintenanceInterval");
    coloringJitter      = par("coloringJitter");

    // Create timers
    helloTimer       = new cMessage("helloTimer");
    colorTimer       = new cMessage("colorTimer");
    maintenanceTimer = new cMessage("maintenanceTimer");

    // Schedule timers with small random offsets
    scheduleAt(simTime() + uniform(0, helloInterval.dbl()), helloTimer);
    scheduleAt(simTime() + uniform(0, coloringJitter.dbl()), colorTimer);
    scheduleAt(simTime() + maintenanceInterval, maintenanceTimer);
}

void GraphColoringClustering::handleMessage(cMessage *msg)
{
    if (msg->isSelfMessage()) {
        if (msg == helloTimer) {
            handleHelloTimer();
        }
        else if (msg == colorTimer) {
            handleColorTimer();
        }
        else if (msg == maintenanceTimer) {
            handleMaintenanceTimer();
        }
        else {
            EV_WARN << "Unknown self-message: " << msg->getName() << "\n";
            delete msg;
        }
    }
    else {
        // Message from other node: should be HelloMessage
        HelloMessage *hello = dynamic_cast<HelloMessage *>(msg);
        if (hello != nullptr) {
            handleHelloMessage(hello);
        }
        else {
            EV_WARN << "Received unknown message type, dropping.\n";
            delete msg;
        }
    }
}

void GraphColoringClustering::handleHelloTimer()
{
    // Create HELLO
    auto *hello = new HelloMessage("HELLO");
    hello->setSenderId(nodeId);
    hello->setColor(currentColor);
    hello->setRole(role);

    // Only send if 'out' gate is actually connected
    if (gate("out")->isConnected()) {
        send(hello, "out");
    }
    else {
        EV_WARN << "Node " << nodeId
                << ": gcc.out gate not connected, dropping HELLO\n";
        delete hello;
    }

    // Reschedule
    scheduleAt(simTime() + helloInterval, helloTimer);
}

void GraphColoringClustering::handleColorTimer()
{
    int newColor = chooseGreedyColor();

    if (newColor != currentColor) {
        EV_INFO << "Node " << nodeId << " changes color from "
                << currentColor << " to " << newColor << "\n";
        currentColor = newColor;
    }

    // In this simple GCC model, clusterId = color
    clusterId = currentColor;

    // After color is set, recompute CH / MEMBER / GATEWAY role
    recomputeRole();

    // We do not automatically reschedule colorTimer - recoloring is triggered
    // by maintenance or topology changes if you decide to do so.
}


void GraphColoringClustering::handleMaintenanceTimer()
{
    // Remove neighbors that timed out
    pruneNeighbors();

    // Topology changes may affect roles
    recomputeRole();

    scheduleAt(simTime() + maintenanceInterval, maintenanceTimer);
}

void GraphColoringClustering::handleHelloMessage(HelloMessage *hello)
{
    int sender = hello->getSenderId();
    int color  = hello->getColor();
    int r      = hello->getRole();

    NeighborInfo info;
    info.neighborId = sender;
    info.color      = color;
    info.role       = r;
    info.lastHeard  = simTime();

    auto it = neighborTable.find(sender);
    if (it == neighborTable.end()) {
        neighborTable[sender] = info;
    } else {
        it->second = info;
    }

    // Neighbor info updated; roles may change
    recomputeRole();

    delete hello;
}

int GraphColoringClustering::chooseGreedyColor() const
{
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


void GraphColoringClustering::pruneNeighbors()
{
    simtime_t now = simTime();
    std::vector<int> toRemove;

    for (const auto &entry : neighborTable) {
        if (now - entry.second.lastHeard > neighborTimeout) {
            toRemove.push_back(entry.first);
        }
    }

    for (int id : toRemove) {
        EV_INFO << "Node " << nodeId << " removing stale neighbor " << id << "\n";
        neighborTable.erase(id);
    }
}

// Minimum color among this node + its neighbors
int GraphColoringClustering::computeLocalMinColor() const
{
    int minColor = -1;

    if (currentColor >= 0)
        minColor = currentColor;

    for (const auto &entry : neighborTable) {
        int c = entry.second.color;
        if (c < 0) continue;
        if (minColor < 0 || c < minColor)
            minColor = c;
    }

    return minColor;
}

// Does this node hear 2 or more distinct cluster heads?
bool GraphColoringClustering::hearsMultipleClusterHeads() const
{
    std::set<int> chColors;

    for (const auto &entry : neighborTable) {
        const NeighborInfo &n = entry.second;
        if (n.role == CLUSTER_HEAD && n.color >= 0) {
            chColors.insert(n.color);
            if (chColors.size() >= 2)
                return true;
        }
    }
    return false;
}

// Decide role (CH / MEMBER / GATEWAY) based on colors and neighbors
void GraphColoringClustering::recomputeRole()
{
    int oldRole = role;

    if (currentColor < 0) {
        role = UNDECIDED;
        clusterId = -1;
    }
    else {
        clusterId = currentColor;

        int minColor = computeLocalMinColor();
        bool isCH = (currentColor >= 0 && currentColor == minColor);

        if (isCH) {
            role = CLUSTER_HEAD;
        }
        else {
            if (hearsMultipleClusterHeads()) {
                role = GATEWAY;
            } else {
                role = MEMBER;
            }
        }
    }

    if (role != oldRole) {
        EV_INFO << "Node " << nodeId << " changes role from "
                << oldRole << " to " << role
                << " (color=" << currentColor
                << ", clusterId=" << clusterId << ")\n";
    }
}

void GraphColoringClustering::finish()
{
    cancelAndDelete(helloTimer);
    cancelAndDelete(colorTimer);
    cancelAndDelete(maintenanceTimer);
}
