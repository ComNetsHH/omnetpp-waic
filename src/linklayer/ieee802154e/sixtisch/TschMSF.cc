/*
 * Minimal Scheduling Function Implementation (6TiSCH WG Draft).
 *
 * Copyright (C) 2021  Institute of Communication Networks (ComNets),
 *                     Hamburg University of Technology (TUHH)
 *           (C) 2021  Yevhenii Shudrenko
 *           (C) 2019  Leo Krüger
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "TschMSF.h"
#include "RplDefs.h"
#include "Tsch6tischComponents.h"
#include "../Ieee802154eMac.h"
#include <omnetpp.h>
#include <random>
#include <algorithm>

using namespace tsch;
using namespace std;

Define_Module(TschMSF);

TschMSF::TschMSF() :
    rplParentId(0),
    tsch6pRtxThresh(3),
    numInconsistenciesDetected(0),
    numLinkResets(0),
    hasStarted(false),
    pHousekeepingDisabled(false),
    hasOverlapping(false),
    isSink(false),
    num6pAddSent(0),
    delayed6pReq(nullptr),
    numFailedTracked6p(0),
    numClearRcv(0)
{
}
TschMSF::~TschMSF() {
}

void TschMSF::initialize(int stage) {
    if (stage == 0) {
        EV_DETAIL << "MSF initializing" << endl;
        pNumChannels = getParentModule()->par("nbRadioChannels").intValue();
        pTschLinkInfo = (TschLinkInfo*) getParentModule()->getSubmodule("linkinfo");
        pTimeout = par("timeout").intValue();
        pMaxNumCells = par("maxNumCells");
        pMaxNumTx = par("maxNumTx");
        pLimNumCellsUsedHigh = par("upperCellUsageLimit");
        pLimNumCellsUsedLow = par("lowerCellUsageLimit");
        pRelocatePdrThres = par("relocatePdrThresh");
        pCellListRedundancy = par("cellListRedundancy").intValue();
        pNumMinimalCells = par("numMinCells").intValue();
        isDisabled = par("disable").boolValue();
        pCellIncrement = par("cellsToAdd").intValue();
        pSend6pDelayed = par("send6pDelayed").boolValue();
        pHousekeepingPeriod = par("housekeepingPeriod").intValue();
        pHousekeepingDisabled = par("disableHousekeeping").boolValue();
        internalEvent = new cMessage("SF internal event", UNDEFINED);
        queueUtilization = registerSignal("queueUtilization");
        failed6pAdd = registerSignal("failed6pAdd");
    } else if (stage == 5) {
        interfaceModule = dynamic_cast<InterfaceTable *>(getParentModule()->getParentModule()->getParentModule()->getParentModule()->getSubmodule("interfaceTable", 0));
        pNodeId = interfaceModule->getInterface(1)->getMacAddress().getInt();
        pSlotframeLength = getModuleByPath("^.^.schedule")->par("macSlotframeSize").intValue();
        pTsch6p = (Tsch6topSublayer*) getParentModule()->getSubmodule("sixtop");
        mac = check_and_cast<Ieee802154eMac*>(getModuleByPath("^.^.mac"));
        mac->subscribe(POST_MODEL_CHANGE, this);
        mac->subscribe(mac->pktRecFromUpperSignal, this);

        schedule = check_and_cast<TschSlotframe*>(getModuleByPath("^.^.schedule"));

        if (isDisabled)
            return;

        hostNode = getModuleByPath("^.^.^.^.");
        showTxCells = par("showDedicatedTxCells").boolValue();
        showQueueUtilization = par("showQueueUtilization").boolValue();
        showTxCellCount = par("showTxCellCount").boolValue();

        /** Schedule minimal cells for broadcast control traffic [RFC8180, 4.1] */
        scheduleMinimalCells(pNumMinimalCells, pSlotframeLength);

        /** And an auto RX cell for communication with neighbors [IETF MSF draft, 3] */
        scheduleAutoRxCell(interfaceModule->getInterface(1)->getMacAddress().formInterfaceIdentifier());

        rpl = hostNode->getSubmodule("rpl");
        if (!rpl) {
            EV_WARN << "RPL module not found" << endl;
            return;
        }

        isSink = rpl->par("isRoot");

        WATCH(numInconsistenciesDetected);
        WATCH(pMaxNumCells);
        WATCH(util);
        WATCH(rplParentId);
        WATCH(numLinkResets);
        WATCH(num6pAddSent);
        WATCH(numFailedTracked6p);
        WATCH(numClearRcv);

        rpl->subscribe("parentChanged", this);
        rpl->subscribe("rankUpdated", this);
    }
}

void TschMSF::scheduleAutoRxCell(InterfaceToken euiAddr) {
    if (!pNumChannels)
        return;
    auto ctrlMsg = new tsch6topCtrlMsg();
    ctrlMsg->setDestId(pNodeId);
    ctrlMsg->setCellOptions(MAC_LINKOPTIONS_RX | MAC_LINKOPTIONS_SRCAUTO);

    // TODO: check this saxHash in more detail, often produces overlapping cells
//    autoRxCellslotOffset = 1 + saxHash(pSlotframeLen - 1, euiAddr);
//    uint32_t autoRxCellchanOffset = saxHash(16, euiAddr);
    std::vector<cellLocation_t> cellList;
//    cellList.push_back({autoRxCellslotOffset, autoRxCellchanOffset});

    autoRxCell = {euiAddr.low() % pSlotframeLength, euiAddr.low() % pNumChannels}; // heuristics
    cellList.push_back(autoRxCell);
    ctrlMsg->setNewCells(cellList);
    EV_DETAIL << "Scheduling auto RX cell at " << cellList << endl;

    // Check if auto RX overlaps with minimal cell
    auto overlappingMinCells = pTschLinkInfo->getMinimalCells(autoRxCell.timeOffset);
    if (overlappingMinCells.size()) {
        EV_DETAIL << "Auto RX cell conflicts with minimal cell at " << autoRxCell.timeOffset
                << " slotOffset, deleting this minimal cell to avoid scheduling issues" << endl;
        ctrlMsg->setDeleteCells(overlappingMinCells);
    }

    pTsch6p->updateSchedule(*ctrlMsg);
    delete ctrlMsg;
}

void TschMSF::scheduleAutoCell(uint64_t neighbor) {
    EV_DETAIL << "Trying to schedule an auto cell to " << MacAddress(neighbor) << endl;

    // Although this is always supposed to be a single cell,
    // implementation-wise it's easier to keep it as vector
    auto sharedCells = pTschLinkInfo->getSharedCellsWith(neighbor);

    if (sharedCells.size()) {
        auto sharedCellLoc = sharedCells.back();
        if (schedule->getLinkByCellCoordinates(sharedCellLoc.timeOffset, sharedCellLoc.channelOffset))
        {
            EV_DETAIL << "Already scheduled, aborting" << endl;
            return;
        }
        EV_WARN << "Shared auto cell found in TschLinkInfo but missing in schedule!" << endl;
    }

    auto ctrlMsg = new tsch6topCtrlMsg();
    ctrlMsg->setDestId(neighbor);
    ctrlMsg->setCellOptions(MAC_LINKOPTIONS_TX | MAC_LINKOPTIONS_SHARED | MAC_LINKOPTIONS_SRCAUTO);

    auto neighborIntfIdent = MacAddress(neighbor).formInterfaceIdentifier();

    uint32_t nbruid = neighborIntfIdent.low();

    std::vector<cellLocation_t> cellList;
//    cellList.push_back({1 + saxHash(pSlotframeLen - 1, neighborIntfIdent), saxHash(16, neighborIntfIdent)});

    auto slotOffset = nbruid % pSlotframeLength;
    cellList.push_back({slotOffset, nbruid % pNumChannels});
    ctrlMsg->setNewCells(cellList);

    EV_DETAIL << "Scheduling auto TX cell at " << cellList.back() << " to " << MacAddress(neighbor) << endl;

    pTschLinkInfo->addLink(neighbor, false, 0, 0);
    pTschLinkInfo->addCell(neighbor, cellList.back(), MAC_LINKOPTIONS_TX | MAC_LINKOPTIONS_SHARED | MAC_LINKOPTIONS_SRCAUTO);
    pTsch6p->updateSchedule(*ctrlMsg);
    delete ctrlMsg;
}

void TschMSF::finish() {
    if (rplParentId) {
        recordScalar("numUplinkCells", (int) pTschLinkInfo->getDedicatedCells(rplParentId).size());
    }
    recordScalar("numInconsistenciesDetected", numInconsistenciesDetected);
    recordScalar("numLinkResets", numLinkResets);
    if (par("trackFailed6pAddByNum").intValue() > 0)
        recordScalar("tracked6pFailed", numFailedTracked6p);
}

void TschMSF::start() {
    Enter_Method_Silent();

    if (isDisabled)
        return;

    tsch6topCtrlMsg* msg = new tsch6topCtrlMsg();
    msg->setKind(DO_START);

    scheduleAt(simTime() + SimTime(par("startTime").doubleValue()), msg);

    if (par("estimateQueueUtil").boolValue())
        scheduleAt(simTime() + par("queueEstimationPeriod"), new cMessage("", QUEUE_ESTIMATION));
}

std::string TschMSF::printCellUsage(std::string neighborMac, double usage) {
    std::string usageInfo = std::string(" Usage to/from neighbor ") + neighborMac
            + std::string(usage >= pLimNumCellsUsedHigh ? " exceeds upper" : "")
            + std::string(usage <= pLimNumCellsUsedLow ? " below lower" : "")
            + std::string((usage < pLimNumCellsUsedHigh && usage > pLimNumCellsUsedLow) ? " within" : "")
            + std::string(" limit with ") + std::to_string(usage);

    return usageInfo;
//    std::cout << pNodeId << usageInfo << endl;
}

bool TschMSF::slOfScheduled(offset_t slOf) {
    auto links = schedule->getLinks();
    for (auto l : links)
        if (slOf == l->getSlotOffset())
            return true;
    return false;
}

void TschMSF::handleMaxCellsReached(cMessage* msg) {
    auto neighborMac = (MacAddress*) msg->getContextPointer();
    auto neighborId = neighborMac->getInt();

    // we might get a notification from our own mac
    if (neighborId == pNodeId)
        return;

    EV_DETAIL << "MAX_NUM_CELLS reached, assessing cell usage with " << *neighborMac << ":"
            << endl << "Currently scheduled cells: \n" << pTschLinkInfo->getCells(neighborId) << endl;

    double usage = (double) nbrStatistic[neighborId].NumCellsUsed / nbrStatistic[neighborId].NumCellsElapsed;

    EV_DETAIL << printCellUsage(neighborMac->str(), usage) << endl;

    if (usage >= pLimNumCellsUsedHigh && !isLossyLink())
        addCells(neighborId, pCellIncrement, MAC_LINKOPTIONS_TX, pSend6pDelayed ? uniform(1, 3) : 0);
    if (usage <= pLimNumCellsUsedLow)
        deleteCells(neighborId, 1);

    // reset values
    nbrStatistic[neighborId].NumCellsUsed = 0; //intrand(pMaxNumCells >> 1);
    nbrStatistic[neighborId].NumCellsElapsed = 0;

    delete neighborMac;
}

bool TschMSF::isLossyLink() {
    return mac->par("pLinkCollision").doubleValue() > 0
            && simTime().dbl() > mac->par("lossyLinkTimeout").doubleValue();
}

void TschMSF::addCells(SfControlInfo *retryInfo)
{
    auto numCells = retryInfo->getNumCells();
    auto nodeId = retryInfo->getNodeId();
    if (!pTschLinkInfo->inTransaction(nodeId)) {
        EV_DETAIL << "Seems we can send 6P ADD" << endl;
        addCells(nodeId, numCells, retryInfo->getCellOptions());
        return;
    }

    EV_WARN << "Can't add " << numCells << " to " << MacAddress(nodeId)
            << " currently in transaction with this node" << endl;
    retryInfo->incRtxCtn();
    if (retryInfo->getRtxCtn() > par("maxRetries").intValue()) {
        EV_DETAIL << "Maximum number of retransmits attempted, dropping packet" << endl;
        delete retryInfo;
        return;
    }
    auto backoff = pow(4, retryInfo->getRtxCtn());
    auto retryMsg = new cMessage("Retry self-msg", SEND_6P_REQ);
    retryMsg->setControlInfo(retryInfo);
    auto timeout = simTime() + SimTime(backoff, SIMTIME_S);
    scheduleAt(timeout, retryMsg);
    EV_DETAIL << "Another transmission attempt scheduled at " << timeout
            << ", " << retryInfo->getRtxCtn() << " retry" << endl;
}


void TschMSF::handleDoStart(cMessage* msg) {
    hasStarted = true;
    EV_DETAIL << "MSF has started" << endl;

    if (!pHousekeepingDisabled)
        scheduleAt(simTime() + SimTime(par("housekeepingStart"), SIMTIME_S), new tsch6topCtrlMsg("", HOUSEKEEPING));

    /** Get all nodes that are within communication range of @p nodeId.
    *   Note that this only works if all nodes have been initialized (i.e.
    *   maybe not in init() step 1!) **/
    neighbors = mac->getNeighborsInRange();
}

void TschMSF::handleHousekeeping(cMessage* msg) {
    EV_DETAIL << "Performing housekeeping: " << endl;
    scheduleAt(simTime()+ uniform(0.5, 1.25) * SimTime(pHousekeepingPeriod, SIMTIME_S), msg);

    // iterate over all neighbors
    for (auto const& neighbourId : neighbors) {
        std::map<cellLocation_t, double> pdrStat;

        // calc cell PDR per neighbor
        for (auto & cell :pTschLinkInfo->getCells(neighbourId)) {
            auto slotOffset = std::get<0>(cell).timeOffset;
            auto channelOffset = std::get<0>(cell).channelOffset;
            cellLocation_t cellLoc = {slotOffset, channelOffset};

            auto it = cellStatistic.find(cellLoc);
            if (it == cellStatistic.end())
                continue;

            double cellPdr = static_cast<double>(std::get<1>(*it).NumTxAck)
                    / static_cast<double>(std::get<1>(*it).NumTx);
            pdrStat.insert({cellLoc, cellPdr});
//            EV_DETAIL << "Calculated cell " << cellLoc << " pdr = " << cellPdr << endl;
//            EV_DETAIL << "PDR stat:c " << pdrStat << endl;
        }

        if (pdrStat.size() <= 1)
            continue;

        auto maxPdr = std::max_element(pdrStat.begin(), pdrStat.end(),
                [] (decltype(pdrStat)::value_type a, decltype(pdrStat)::value_type b)
            {
                return a.second < b.second;
            }
        );

//        if (!std::isnan(maxPdr->second))
//            std::cout << "neighbor " << entry.first << " has max pdr " << maxPdr->second << " at "
//                    << maxPdr->first << endl;

        for (auto & cellPdr: pdrStat) {
            if (cellPdr.first == maxPdr->first)
                continue;

//            if (!std::isnan(cellPdr.second))
//                std::cout << " and " << cellPdr.second << " at " << cellPdr.first << endl;

            if ((maxPdr->second - cellPdr.second) > pRelocatePdrThres)
                relocateCells(neighbourId, cellPdr.first);
        }
    }

}


void TschMSF::relocateCells(uint64_t neighbor, cellLocation_t cell) {
    std::vector<cellLocation_t> cellList = { cell };
    relocateCells(neighbor, cellList);
}

void TschMSF::relocateCells(uint64_t neighborId, std::vector<cellLocation_t> relocCells) {
    if (!relocCells.size()) {
        EV_DETAIL << "No cells provided to relocate" << endl;
        return;
    }

    if (pTschLinkInfo->inTransaction(neighborId)) {
        EV_WARN << "Can't relocate cells, currently in another transaction with this node" << endl;
        return;
    }

    EV_DETAIL << "Relocating cell(s) with " << MacAddress(neighborId) << " : " << relocCells << endl;

    auto availableSlots = getAvailableSlotsInRange(0, pSlotframeLength);

    if ((int) availableSlots.size() < (int) relocCells.size()) {
        EV_WARN << "Not enough free slot offsets to relocate currently scheduled cells" << endl;
        return;
    }

    std::vector<cellLocation_t> candidateCells = {};
    for (auto slof: availableSlots)
        candidateCells.push_back({slof, (offset_t) intrand(pNumChannels) });

    if (availableSlots.size() > relocCells.size() + pCellListRedundancy)
        candidateCells = pickRandomly(candidateCells, relocCells.size() + pCellListRedundancy);

    EV_DETAIL << "Selected candidate cell list to accommodate relocated cells: " << candidateCells << endl;

    for (auto cc : candidateCells)
        reservedTimeOffsets[neighborId].push_back(cc.timeOffset);

    pTsch6p->sendRelocationRequest(neighborId, MAC_LINKOPTIONS_TX, relocCells.size(), relocCells, candidateCells, pTimeout);
}

void TschMSF::estimateQueueUtilization() {
    if (!rplParentId)
        return;

//    util = mac->getQueueUtilization(MacAddress(rplParentId));

//    auto currentQueueSize = std::get<1>(mac->getQueueSizes(MacAddress(rplParentId), {0}).back());
    util = mac->getQueueUtilization(MacAddress(rplParentId), 0);
    emit(queueUtilization, util);

    if (util > par("queueUtilUpperThresh").doubleValue())
        addCells(rplParentId, 1);
}

void TschMSF::handleMessage(cMessage* msg) {
    Enter_Method_Silent();

    if (!msg->isSelfMessage()) {
        delete msg;
        return;
    }

    switch (msg->getKind()) {
        case REACHED_MAXNUMCELLS: {
            handleMaxCellsReached(msg);
            break;
        }
        case QUEUE_ESTIMATION: {
            estimateQueueUtilization();
            auto nextTimestamp = simTime() + par("queueEstimationPeriod");
            EV_DETAIL << "Next queue estimation event on " << nextTimestamp << endl;
            scheduleAt(nextTimestamp, msg);
            return;
        }
        case DO_START: {
            handleDoStart(msg);
            break;
        }
        case HOUSEKEEPING: {
            handleHousekeeping(msg);
            break;
        }
        case SEND_6P_REQ: {
            auto ctrlInfo = check_and_cast<SfControlInfo*> (msg->getControlInfo());

            // If this 6P request needs to be retransmitted, this check would means
            // that the first attempt has already failed and we have to retry more
            if (ctrlInfo->getRtxCtn())
                addCells(ctrlInfo);
            else
                // This one just plainly sends the message out, without scheduling a retransmit attempt
                send6topRequest(ctrlInfo);
            delayed6pReq = nullptr;
            return;
        }
        case DELAY_TEST: {
            long *rankPtr = (long*) msg->getContextPointer();
            EV_DETAIL << "Received DELAY_TEST self-msg, rank - " << *rankPtr << endl;

 //            handleRplRankUpdate(*rankPtr, numHosts);

            auto network = hostNode->getParentModule();
            // lambda is assumed to be the same for all hosts
            handleRplRankUpdate(rplRank, network->par("numHosts"), network->par("lambda").doubleValue());

            break;
        }
        default: EV_ERROR << "Unknown message received: " << msg << endl;
    }
    delete msg;
}

void TschMSF::handleRplRankUpdate(long rank, int numHosts, double lambda) {
    EV_DETAIL << "Trying to schedule TX cells according to our rank - "
            << rank << " and total number of hosts - " << numHosts << endl;

    if (!rplParentId) {
        EV_WARN << "RPL parent MAC unknown, aborting" << endl;
        return;
    }

    auto currentTxCells = pTschLinkInfo->getDedicatedCells(rplParentId);
    EV_DETAIL << "Found " << currentTxCells.size() << " cells already scheduled with PP: "
            << currentTxCells << endl;

    auto val = 0.001 + lambda * (numHosts + 2 - rank);
    auto numCellsRequired = ceil(val) - (int) currentTxCells.size();

    EV_DETAIL << "Num cells required to schedule - " << numCellsRequired << endl;
    /**
     * "numHosts - rank + 3" corresponds to the number of cells required to handle the traffic
     * from the descendant nodes (numHosts - rank) as well as the node itself (+1).
     * Additional +1 to account for the fact, that node closest to the sink are of rank 2 rather than 1.
     * And another +1 to have service rate > arrival rate for stability condition.
     *
     * 0.001 is added to account for cases lambda = mu
     *
     * Each node is assumed to be M/M/1 system with incoming rate of 1 packet per slotframe
     */
    auto ctrlInfo = new SfControlInfo(rplParentId);
    ctrlInfo->set6pCmd(CMD_ADD);
    ctrlInfo->setNumCells(numCellsRequired);
    ctrlInfo->setCellOptions(MAC_LINKOPTIONS_TX);
    addCells(ctrlInfo);
}



void TschMSF::send6topRequest(SfControlInfo *ctrlInfo) {
    auto sixTopCmd = ctrlInfo->get6pCmd();
    auto nodeId = ctrlInfo->getNodeId();
    auto cellOp = ctrlInfo->getCellOptions();
    auto numCells = ctrlInfo->getNumCells();

    switch (sixTopCmd) {
        case CMD_ADD: {
            addCells(nodeId, numCells);
            break;
        }
        case CMD_DELETE: {
            deleteCells(nodeId, numCells);
            break;
        }
        case CMD_CLEAR: {
            pTsch6p->sendClearRequest(nodeId, pTimeout);
            break;
        }
        case CMD_RELOCATE: {
            relocateCells(nodeId, ctrlInfo->getCellList());
            break;
        }
        default:
            std::string errMsg = "Cannot send out delayed 6P request, unknown 6P command type - "
                    + std::to_string(sixTopCmd);
            throw cRuntimeError(errMsg.c_str());
    }
}


void TschMSF::scheduleMinimalCells(int numMinimalCells, int slotframeLength) {
    if (numMinimalCells > slotframeLength) {
        std::ostringstream out;
        out << "More minimal cells requested (" << numMinimalCells << ") than the slotframe length (" << slotframeLength << ")" << endl;

        throw cRuntimeError(out.str().c_str());
    }

    EV_DETAIL << "Scheduling " << numMinimalCells << " minimal cells for broadcast messages: " << endl;
    auto ctrlMsg = new tsch6topCtrlMsg();
    auto macBroadcast = inet::MacAddress::BROADCAST_ADDRESS.getInt();
    ctrlMsg->setDestId(macBroadcast);
    auto cellOpts = MAC_LINKOPTIONS_TX | MAC_LINKOPTIONS_RX | MAC_LINKOPTIONS_SHARED;
    ctrlMsg->setCellOptions(cellOpts);

    std::vector<cellLocation_t> cellList = {};

    int minCellPeriod = floor(slotframeLength / numMinimalCells);
    EV_DETAIL << "Minimal cell period - " << minCellPeriod << endl;

    for (auto i = 0; i < numMinimalCells; i++)
        cellList.push_back({(offset_t) (i * minCellPeriod), 0});

    ctrlMsg->setNewCells(cellList);
    pTschLinkInfo->addLink(macBroadcast, false, 0, 0);
    for (auto cell : cellList)
        pTschLinkInfo->addCell(macBroadcast, cell, cellOpts);

    EV_DETAIL << cellList << endl;

    pTsch6p->updateSchedule(*ctrlMsg);
    delete ctrlMsg;
}



void TschMSF::removeAutoTxCell(uint64_t neighbor) {
    std::vector<cellLocation_t> cellList;

    // TODO: Check correctness of this auto cell search
    for (auto &cell: pTschLinkInfo->getCells(neighbor))
        if (std::get<1>(cell) != 0xFF && getCellOptions_isAUTO(std::get<1>(cell)))
            cellList.push_back(std::get<0>(cell));

    if (cellList.size()) {
        EV_DETAIL << "Removing auto TX cell " << cellList << " to neighbor " << MacAddress(neighbor) << endl;
        auto ctrlMsg = new tsch6topCtrlMsg();
        ctrlMsg->setDestId(neighbor);
        pTschLinkInfo->deleteCells(neighbor, cellList, MAC_LINKOPTIONS_TX | MAC_LINKOPTIONS_SHARED | MAC_LINKOPTIONS_SRCAUTO);
        ctrlMsg->setDeleteCells(cellList);
        pTsch6p->updateSchedule(*ctrlMsg);
        delete ctrlMsg;
    }
}

tsch6pSFID_t TschMSF::getSFID() {
    Enter_Method_Silent();
    return pSFID;
}

std::vector<cellLocation_t> TschMSF::pickRandomly(std::vector<cellLocation_t> inputVec, int numRequested)
{
    if ((int) inputVec.size() < numRequested)
        return {};

    std::random_device rd;
    std::mt19937 e{rd()};

    std::shuffle(inputVec.begin(), inputVec.end(), e);

    std::vector<cellLocation_t> picked = {};
    for (auto i = 0; i < numRequested; i++)
        picked.push_back(inputVec[i]);

//    std::copy(slotOffsets.begin(), slotOffsets.begin() + numSlots, picked.begin());
    return picked;
}

bool TschMSF::slotOffsetAvailable(offset_t slOf) {
    return !pTschLinkInfo->timeOffsetScheduled(slOf)
            && slOf != autoRxCell.timeOffset
            && !slotOffsetReserved(slOf)
            && !slOfScheduled(slOf);
}

std::vector<offset_t> TschMSF::getAvailableSlotsInRange(int start, int end) {
    std::vector<offset_t> slots = {};

    if (start < 0 || end < 0) {
        EV_WARN << "Requested slot offsets in invalid range";
        return slots;
    }

    for (int slOf = start; slOf < end; slOf++)
        if (slotOffsetAvailable( (offset_t) slOf))
            slots.push_back((offset_t) slOf);

    EV_DETAIL << "\nIn range (" << start << ", " << end << ") found available slot offsets: " << slots << endl;
    return slots;
}

std::vector<offset_t> TschMSF::getAvailableSlotsInRange(int slOffsetEnd) {
    return getAvailableSlotsInRange(0, slOffsetEnd);
}

int TschMSF::createCellList(uint64_t destId, std::vector<cellLocation_t> &cellList, int numCells)
{
    Enter_Method_Silent();

    if (cellList.size()) {
        EV_WARN << "cellList should be an empty vector" << endl;
        return -EINVAL;
    }
    if (!reservedTimeOffsets[destId].empty()) {
        EV_ERROR << "reservedTimeOffsets should be empty when creating new cellList,"
                << " is another transaction still in progress?" << endl;
        return -EINVAL;
    }

    std::vector<offset_t> availableSlots = getAvailableSlotsInRange(pSlotframeLength);

    if (!availableSlots.size()) {
        EV_DETAIL << "No available cells found" << endl;
        return -ENOSPC;
    }

    if ((int) availableSlots.size() < numCells) {
        EV_DETAIL << "More cells requested than total available, returning "
                << availableSlots.size() << " cells" << endl;
        for (auto s : availableSlots) {
            cellList.push_back({s, (offset_t) intrand(pNumChannels)});
            reservedTimeOffsets[destId].push_back(s);
        }

        return -EFBIG;
    }

    // Fill cell list with all available slot offsets and random channel offset
    for (auto sl : availableSlots)
        cellList.push_back({sl, (offset_t) intrand(pNumChannels)});

    EV_DETAIL << "Initialized cell list: " << cellList << endl;

    // Select only required number of cells from cell list
    cellList = pickRandomly(cellList, numCells);
    EV_DETAIL << "After picking required number of cells (" << numCells << "): " << cellList << endl;

    // Block selected slot offsets until 6P transaction finishes
    for (auto c : cellList)
        reservedTimeOffsets[destId].push_back(c.timeOffset);

    return 0;
}

int TschMSF::pickCells(uint64_t destId, std::vector<cellLocation_t> &cellList,
                        int numCells, bool isRX, bool isTX, bool isSHARED)
{
    Enter_Method_Silent();

    if (cellList.size() == 0)
        return -EINVAL;

    EV_DETAIL << "Picking cells from list: " << cellList << endl;

    std::vector<cellLocation_t> pickedCells = {};

    for (auto cell : cellList) {
        EV_DETAIL << "proposed cell " << cell;
        if (slotOffsetAvailable(cell.timeOffset))
        {
            EV_DETAIL << " available" << endl;
            /* cell is still available, pick it. */
            pickedCells.push_back(cell);
        } else
            EV_DETAIL << " unavailable" << endl;
    }
    cellList.clear();
    for (auto i = 0; i < numCells && i < (int) pickedCells.size(); i++) {
        cellList.push_back(pickedCells[i]);
        reservedTimeOffsets[destId].push_back(pickedCells[i].timeOffset);
    }

    EV_DETAIL << "Cell list after picking: " << cellList << endl;

    if (!pickedCells.size())
        return -ENOSPC;

    if ((int) pickedCells.size() < numCells)
        return -EFBIG;

    return 0;
}

void TschMSF::clearCellStats(std::vector<cellLocation_t> cellList) {
    for (auto &cell : cellList) {
        auto cand = cellStatistic.find(cell);
        if (cand != cellStatistic.end())
            cellStatistic.erase(cand);
    }
}

bool TschMSF::checkOverlapping() {
    auto numLinks = schedule->getNumLinks();
    std::vector<offset_t> dedicatedSlOffsets = {};
    for (auto i = 0; i < numLinks; i++) {
        auto link = schedule->getLink(i);
        if (link->getAddr() != MacAddress::BROADCAST_ADDRESS && !link->isShared()) {
            auto slOf = link->getSlotOffset();
            auto nodeId = link->getAddr().getInt();
            if (std::count(dedicatedSlOffsets.begin(), dedicatedSlOffsets.end(), slOf)) {
//                handleInconsistency(link->getAddr().getInt(), 0);
                return true;
            }
            else
                dedicatedSlOffsets.push_back(slOf);
        }
    }
    return false;
}


void TschMSF::refreshDisplay() const {
    if (!rplParentId)
        return;

    if (showTxCells || showTxCellCount) {
        std::vector<cellLocation_t> txCells = {};
        txCells = pTschLinkInfo->getDedicatedCells(rplParentId);

        std::ostringstream outCells;

        // Paint dedicated TX cell(s) in red if overlapping cells are detected
        if (hasOverlapping)
            hostNode->getDisplayString().setTagArg("t", 2, "#fc2803");

        if (showTxCellCount)
            outCells << "TX cells: " << txCells.size();
        else {
            std::sort(txCells.begin(), txCells.end(),
                [](const cellLocation_t c1, const cellLocation_t c2) { return c1.timeOffset < c2.timeOffset; });
            outCells << txCells;
        }

        hostNode->getDisplayString().setTagArg(showQueueUtilization ? "tt" : "t", 0, outCells.str().c_str());
    }

    if (showQueueUtilization) {
        std::ostringstream outUtil;
        outUtil << mac->getQueueUtilization(MacAddress(rplParentId));
        hostNode->getDisplayString().setTagArg("t", 0, outUtil.str().c_str()); // show queue utilization above nodes
    }

}

void TschMSF::handleSuccessResponse(uint64_t sender, tsch6pCmd_t cmd, int numCells,
        std::vector<cellLocation_t> cellList)
{
    if (pTschLinkInfo->getLastKnownType(sender) != MSG_RESPONSE)
        return;

    switch (cmd) {
        case CMD_ADD: {
            // No cells were added if 6P SUCCESS responds with an empty CELL_LIST
            if (!cellList.size()) {
                EV_DETAIL << "Seems ADD to " << MacAddress(sender) << " failed" << endl;

                if (num6pAddSent == par("trackFailed6pAddByNum").intValue())
                    numFailedTracked6p++;

                return;
            }

            // if we successfully scheduled dedicated TX we don't need an auto, shared TX cell anymore
            removeAutoTxCell(sender);

            // FIXME: magic numbers
//            pMaxNumCells = pow(2, (int) pTschLinkInfo->getDedicatedCells(sender).size()) * par("maxNumCells").intValue();

            // FIXME: commented out for Lukas's evaluation
//            pMaxNumCells = 2 * ((int) pTschLinkInfo->getDedicatedCells(sender).size()) + par("maxNumCells").intValue();

            EV_DETAIL << "6P ADD succeeded, " << cellList << "cells added" << endl;
            break;
        }
        case CMD_DELETE: {
            // remove cell statistics for cells that have been deleted
            clearCellStats(cellList);
            break;
        }
        case CMD_RELOCATE: {
            handleSuccessRelocate(sender, cellList);
            break;
        }
        case CMD_CLEAR: {
            clearCellStats(cellList);
            clearScheduleWithNode(sender);
            pTschLinkInfo->resetLink(sender, MSG_RESPONSE);
            numClearRcv++;
            break;
        }
        default: EV_DETAIL << "Unsupported 6P command" << endl;
    }

}

void TschMSF::handleSuccessRelocate(uint64_t sender, std::vector<cellLocation_t> cellList) {
    if (!cellList.size())
        EV_WARN << "Seems RELOCATE failed, worth retrying?" << endl;
    else
        EV_DETAIL << cellList.size() << " successfully relocated" << endl;
}

void TschMSF::clearScheduleWithNode(uint64_t sender)
{
    auto ctrlMsg = new tsch6topCtrlMsg();
    ctrlMsg->setDestId(sender);
    std::vector<cellLocation_t> deletable = pTschLinkInfo->getCellLocations(sender);
    EV_DETAIL << "Clearing schedule with " << MacAddress(sender) << endl;

    if (deletable.size()) {
        EV_DETAIL << "Found cells to delete: " << deletable << endl;
        ctrlMsg->setDeleteCells(deletable);
        pTsch6p->updateSchedule(*ctrlMsg);
    } else
        delete ctrlMsg;

    scheduleAutoCell(sender); // Try to schedule an auto cell (if needed) for remaining packets in the queue
}


/** Hacky way to free cells reserved to @param sender when link-layer ACK is received from it */
void TschMSF::handleResponse(uint64_t sender, tsch6pReturn_t code, int numCells, std::vector<cellLocation_t> *cellList)
{
    reservedTimeOffsets[sender].clear();
    return;
}

void TschMSF::handleResponse(uint64_t sender, tsch6pReturn_t code, int numCells, std::vector<cellLocation_t> cellList)
{
    Enter_Method_Silent();

    auto lastKnownCmd = pTschLinkInfo->getLastKnownCommand(sender);
    EV_DETAIL << "From " << MacAddress(sender) << " received " << code << " for "
            << lastKnownCmd << " with cell list: " << cellList << endl;

    // free the cells that were reserved during the transaction
    reservedTimeOffsets[sender].clear();

    switch (code) {
        case RC_SUCCESS: {
            handleSuccessResponse(sender, lastKnownCmd, numCells, cellList);
            break;
        }
        // Handle all other return codes (RC_RESET, RC_ERROR, RC_VERSION, RC_SFID, ...) as a generic error
        default: {
            clearCellStats(cellList);
            clearScheduleWithNode(sender);
            pTschLinkInfo->resetLink(sender, MSG_RESPONSE);
            numLinkResets++;
        }
    }
}

void TschMSF::handleInconsistency(uint64_t destId, uint8_t seqNum) {
    Enter_Method_Silent();
    EV_WARN << "Inconsistency detected" << endl;
    /* Free already reserved cells to avoid race conditions */
    reservedTimeOffsets[destId].clear();
    numInconsistenciesDetected++;

    pTsch6p->sendClearRequest(destId, pTimeout);
}

int TschMSF::getTimeout() {
    Enter_Method_Silent();

    return pTimeout;
}

void TschMSF::addCells(uint64_t nodeId, int numCells, uint8_t cellOptions, int delay) {
    if (numCells < 1) {
        EV_WARN << "Invalid number of cells requested - " << numCells << endl;
        return;
    }

    EV_DETAIL << "Trying to add " << numCells << " cell(s) to " << MacAddress(nodeId) << endl;

    if (delay > 0) {
        // already in progress
        if (delayed6pReq) {
            EV_DETAIL << "Detected another delayed 6P request already in progress" << endl;
            return;
        }

        delayed6pReq = new cMessage("SEND_6P_DELAYED", SEND_6P_REQ);
        auto ctrlInfo = new SfControlInfo(nodeId);
        ctrlInfo->set6pCmd(CMD_ADD);
        ctrlInfo->setNumCells(numCells);
        ctrlInfo->setCellOptions(cellOptions);

        delayed6pReq->setControlInfo(ctrlInfo);
        scheduleAt(simTime() + SimTime(delay, SIMTIME_S), delayed6pReq);
        EV_DETAIL << "6P ADD will be sent out after " << delay << " seconds timeout" << endl;
        return;
    }

    if (pTschLinkInfo->inTransaction(nodeId)) {
        EV_WARN << "Can't add cells, currently in another transaction with this node" << endl;
        return;
    }

    std::vector<cellLocation_t> cellList = {};
    createCellList(nodeId, cellList, numCells + pCellListRedundancy);

    if (!cellList.size())
        EV_DETAIL << "No cells could be added to cell list, aborting ADD" << endl;
    else {
        if (pTsch6p->sendAddRequest(nodeId, cellOptions, numCells, cellList, pTimeout))
            num6pAddSent++;
    }
}

void TschMSF::deleteCells(uint64_t nodeId, int numCells) {
    if (numCells <= 0) {
        EV_WARN << "Invalid number of cells requested to delete" << endl;
        return;
    }

    std::vector<cellLocation_t> dedicated = pTschLinkInfo->getDedicatedCells(nodeId);

    if (!dedicated.size()) {
        EV_DETAIL << "No dedicated cells found" << endl;
        // If neighbor is not a parent or we are the sink, no need to hold onto this auto cell
        if (nodeId != rplParentId || isSink)
            removeAutoTxCell(nodeId);
        return;
    }

    if (((int) dedicated.size()) - numCells < 1) {
        EV_WARN << "Cannot delete last dedicated cell" << endl;
        return;
    }

    std::vector<cellLocation_t> cellList;

    if (numCells == 1)
        cellList.push_back(dedicated.back());
    else
        cellList = pickRandomly(dedicated, numCells);

    EV_DETAIL  << "Chosen for deletion: " << cellList << endl;
    pTsch6p->sendDeleteRequest(nodeId, MAC_LINKOPTIONS_TX, numCells, cellList, pTimeout);
}

bool TschMSF::slotOffsetReserved(offset_t slOf) {
    std::map<uint64_t, std::vector<offset_t>>::iterator nodes;

    for (nodes = reservedTimeOffsets.begin(); nodes != reservedTimeOffsets.end(); ++nodes) {
        if (slotOffsetReserved(nodes->first, slOf))
            return true;
    }

    return false;
}

bool TschMSF::slotOffsetReserved(uint64_t nodeId, offset_t slOf) {
    std::vector<offset_t> slOffsets = reservedTimeOffsets[nodeId];
    return std::find(slOffsets.begin(), slOffsets.end(), slOf) != slOffsets.end();
}

void TschMSF::receiveSignal(cComponent *src, simsignal_t id, cObject *value, cObject *details)
{
    Enter_Method_Silent();

    std::string signalName = getSignalName(id);
//    EV_DETAIL << "got signal " << signalName;

    static std::vector<std::string> namesNeigh = {}; //{"nbSlot-neigh-", "nbTxFrames-neigh-", "nbRxFrames-neigh-"};
    static std::vector<std::string> namesLink = {"nbSlot-link-", "nbTxFrames-link-", "nbRecvdAcks-link-"};

    // this is a hint we should check for new dynamic signals
    if (dynamic_cast<cPostParameterChangeNotification *>(value)) {
        // consider all neighbors we already know
        for (auto const& neigbhor : neighbors) {
            // subscribe to all neighbor-related signals
            auto macStr = MacAddress(neigbhor).str();
            for (auto & name: namesNeigh) {
                auto fullname = name + macStr;
                if (src->isSubscribed(fullname.c_str(), this) == false)
                    src->subscribe(fullname.c_str(), this);
            }

            // subscribe to all link related signals
            for (auto & cell :pTschLinkInfo->getCells(neigbhor)) {
                auto slotOffset = std::get<0>(cell).timeOffset;
                auto channelOffset = std::get<0>(cell).channelOffset;

                for (auto & name: namesLink) {
                    std::stringstream fullname;
                    fullname << name << "0." << slotOffset << "." << channelOffset;
                    if (src->isSubscribed(fullname.str().c_str(), this) == false)
                        src->subscribe(fullname.str().c_str(), this);
                }
            }
        }
    }
}

void TschMSF::handleParentChangedSignal(uint64_t newParentId) {
    EV_DETAIL << "RPL parent changed to " << MacAddress(newParentId) << endl;

    // If RPL parent hasn't been set, we've just joined the DODAG
    if (!rplParentId) {
        rplParentId = newParentId;
        addCells(rplParentId, par("initialNumCells").intValue());
        return;
    }

    auto txCells = pTschLinkInfo->getDedicatedCells(rplParentId);
    EV_DETAIL << "Dedicated TX cells currently scheduled with PP: " << txCells << endl;

    /** Clear all negotiated cells and state information with now former PP */
//    clearScheduleWithNode(rplParentId);
//    pTschLinkInfo->abortTransaction(rplParentId);
//    reservedTimeOffsets[rplParentId].clear();
    pTsch6p->sendClearRequest(rplParentId, pTimeout);

    rplParentId = newParentId;

    /** and schedule the same amount of cells (or at least 1) with the new parent */
    addCells(newParentId, (int) txCells.size() > 0 ? (int) txCells.size() : par("initialNumCells").intValue());
}

void TschMSF::handlePacketEnqueued(uint64_t dest) {
    if (MacAddress(dest) == MacAddress::BROADCAST_ADDRESS)
        return;

    auto txCells = pTschLinkInfo->getCellsByType(dest, MAC_LINKOPTIONS_TX);

    EV_DETAIL << "Received MAC notification for a packet "
            << MacAddress(dest) << ",\ncells scheduled to this neighbor: " << txCells << endl;

    bool scheduleInconsistency = false;

    for (auto cell : txCells) {
        if (!schedule->getLinkByCellCoordinates(cell.timeOffset, cell.channelOffset)) {
            EV_WARN << "TX cell at [ " << cell.timeOffset << ", "
                    << cell.channelOffset << " ] missing from the schedule!" << endl;
            scheduleInconsistency = true;
        }
    }

    // Heuristic, checking if there's a dedicated cell to preferred parent whenever a packet is enqueued
    if (rplParentId == dest || par("downlinkDedicated").boolValue()) {
        auto dedicatedCells = pTschLinkInfo->getDedicatedCells(dest);

        if (!dedicatedCells.size() && !pTschLinkInfo->inTransaction(dest)) {
            EV_DETAIL << "No dedicated TX cell found to this node, and " <<
                    "we are currently not in transaction with it, attempting to add one TX cell" << endl;
            // heuristics - do not send 6P ADD request immediately, cause simultaneous
            // bi-directional transactions are not yet supported
            addCells(dest, 1, MAC_LINKOPTIONS_TX, uniform(2, 5)); // FIXME: magic numbers
        }
    }

    // If the node's just a neighbor, schedule an auto cell if there's no cell at all
    if (!txCells.size() || scheduleInconsistency)
        scheduleAutoCell(dest);
}

void TschMSF::receiveSignal(cComponent *src, simsignal_t id, unsigned long value, cObject *details)
{
    Enter_Method_Silent();
    if (!hasStarted)
        return;

    std::string signalName = getSignalName(id);
    EV_DETAIL << "Got signal - " << signalName << endl;
}

void TschMSF::receiveSignal(cComponent *src, simsignal_t id, long value, cObject *details)
{
    Enter_Method_Silent();

    if (!hasStarted)
        return;

    std::string signalName = getSignalName(id);
    EV_DETAIL << "Got signal - " << signalName << endl;

    if (std::strcmp(signalName.c_str(), "parentChanged") == 0) {
        auto rplControlInfo = (RplGenericControlInfo*) details;
        handleParentChangedSignal(rplControlInfo->getNodeId());
        return;
    }

    if (id == mac->pktRecFromUpperSignal) {
        auto macCtrlInfo = (Ieee802154eMac::MacGenericInfo*) details;
        handlePacketEnqueued(macCtrlInfo->getNodeId());
        return;
    }


    if (std::strcmp(signalName.c_str(), "rankUpdated") == 0 && par("handleRankUpdates")) {
        auto selfMsg = new cMessage("", DELAY_TEST);
        long updatedRank = value;
        rplRank = value;
        EV_DETAIL << "Set RPL rank inside SF to " << rplRank << endl;
        selfMsg->setContextPointer((long*) &updatedRank);
        // FIXME: Magic numbers
        scheduleAt(simTime() + SimTime(20, SIMTIME_S), selfMsg);
//        handleRplRankUpdate(value, numHosts);
        return;
    }


    std::string statisticStr;
    std::string scopeStr;

    auto pos = signalName.find('-'), pos2 = pos;

    if (pos != std::string::npos) {
        statisticStr = signalName.substr(0, pos);

        pos2 = signalName.find('-', pos + 1);
        if (pos2 != std::string::npos)
            scopeStr = signalName.substr(pos+1, pos2-pos-1);
        else
            return; //error
    } else
        return; //error

    if (scopeStr != "link")
        return;

    std::string linkStr = signalName.substr(pos2+1, signalName.size());
    pos = linkStr.find('.');
    pos2 = linkStr.find('.', pos + 1);
    offset_t slotOffset = std::stoi(linkStr.substr(pos + 1, pos2-pos-1));
    offset_t channelOffset = std::stoi(linkStr.substr(pos2 + 1, linkStr.size()-pos-1));
    cellLocation_t cell = {slotOffset, channelOffset};

    auto neighbor = pTschLinkInfo->getNodeOfCell(cell);
    auto options = pTschLinkInfo->getCellOptions(neighbor, cell);

    if (neighbor == 0) {
        EV_WARN << "could not find neighbor for cell " << linkStr << endl;
        return;
    }

    if (options != 0xFF && getCellOptions_isTX(options))
        updateNeighborStats(neighbor, statisticStr);
}

void TschMSF::updateCellTxStats(cellLocation_t cell, std::string statType) {
    EV_DETAIL << "Updating cell Tx stats" << endl;
    if (cellStatistic.find(cell) == cellStatistic.end())
        cellStatistic.insert({cell, {0, 0, 0}});

    if (statType == "nbTxFrames") {
        cellStatistic[cell].NumTx++;
        if (cellStatistic[cell].NumTx >= pMaxNumTx) {
            cellStatistic[cell].NumTx =  cellStatistic[cell].NumTx / 2;
            cellStatistic[cell].NumTxAck = cellStatistic[cell].NumTxAck / 2;
        }
    } else if (statType == "nbRecvdAcks")
        cellStatistic[cell].NumTxAck++;
}

void TschMSF::updateNeighborStats(uint64_t neighbor, std::string statType) {
    if (nbrStatistic.find(neighbor) == nbrStatistic.end())
        nbrStatistic.insert({neighbor, {0, 0}});

    if (statType == "nbSlot") {
        nbrStatistic[neighbor].NumCellsElapsed++;
        EV_DETAIL << "NumCellsElapsed for " << MacAddress(neighbor) << " now at " << +(nbrStatistic[neighbor].NumCellsElapsed) << endl;
    } else if (statType == "nbTxFrames") {
        nbrStatistic[neighbor].NumCellsUsed++;
        EV_DETAIL << "NumCellsUsed for " << MacAddress(neighbor) << " now at " << +(nbrStatistic[neighbor].NumCellsUsed) << endl;
    }

    if (nbrStatistic[neighbor].NumCellsElapsed >= pMaxNumCells) {
        cMessage* selfMsg = new cMessage();
        selfMsg->setKind(REACHED_MAXNUMCELLS);
        selfMsg->setContextPointer(new MacAddress(neighbor));
        scheduleAt(simTime(), selfMsg);
    }
}

uint32_t TschMSF::saxHash(int maxReturnVal, InterfaceToken EUI64addr)
{
    uint32_t l_bit = 1, r_bit = 1; // TODO choose wisely

    uint64_t EUI64addr64 = (((uint64_t) EUI64addr.normal()) << 32) | EUI64addr.low();
    auto c = static_cast<char*>(static_cast<void*>(&EUI64addr64));

    // step 1
    uint32_t i = 0; // byte index of EUI64addr
    uint32_t h = 0; // intermediate hash value = h0

    do {
        // step 2
        uint32_t sum = (h << l_bit) + (h >> r_bit) + ((uint8_t) c[i]);
        h = (sum ^ h) % maxReturnVal;
        i++;
    } while (i < sizeof(uint64_t));

    return h;
}
