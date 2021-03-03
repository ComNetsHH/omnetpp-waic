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

#include "../Ieee802154eMac.h"
#include "Rpl.h"
#include "Tsch6tischComponents.h"
#include <omnetpp.h>
#include <random>
#include <algorithm>

using namespace tsch;

Define_Module(TschMSF);

TschMSF::TschMSF() :
    rplParentId(0),
    hasStarted(false)
{
}

TschMSF::~TschMSF() {
}

void TschMSF::initialize(int stage) {
    if (stage == 0) {
        EV_DETAIL << "CLSF initializing" << endl;
        pNumChannels = getParentModule()->par("nbRadioChannels").intValue();
        pTschLinkInfo = (TschLinkInfo*) getParentModule()->getSubmodule("linkinfo");
        pTimeout = par("timeout").intValue();
        pMaxNumCells = par("maxNumCells");
        pMaxNumTx = par("MAX_NUMTX");
        pLimNumCellsUsedHigh = par("upperCellUsageLimit");
        pLimNumCellsUsedLow = par("lowerCellUsageLimit");
        pRelocatePdrThres = par("RELOCATE_PDRTHRES");
        pCellListRedundancy = par("cellListRedundancy").intValue();
        pNumMinimalCells = par("numMinCells").intValue();
        disable = par("disable").boolValue();
        s_InitialScheduleComplete = registerSignal("initial_schedule_complete");
        rplParentChangedSignal = registerSignal("rplParentChanged");
    } else if (stage == 5) {
        interfaceModule = dynamic_cast<InterfaceTable *>(getParentModule()->getParentModule()->getParentModule()->getParentModule()->getSubmodule("interfaceTable", 0));
        pNodeId = interfaceModule->getInterface(1)->getMacAddress().getInt();
        pSlotframeLength = getModuleByPath("^.^.schedule")->par("macSlotframeSize").intValue();
        pTsch6p = (Tsch6topSublayer*) getParentModule()->getSubmodule("sixtop");
        mac = check_and_cast<Ieee802154eMac*>(getModuleByPath("^.^.mac")); // TODO this is very ugly
        mac->subscribe(POST_MODEL_CHANGE, this);
        mac->subscribe("pktEnqueued", this);

        if (disable)
            return;

        hostNode = getModuleByPath("^.^.^.^.");

        /** Schedule minimal cells for broadcast control traffic [RFC8180, 4.1] */
        scheduleMinimalCells();
        /** And an auto RX cell for communication with neighbors [IETF MSF draft, 3] */
        scheduleAutoRxCell(interfaceModule->getInterface(1)->getMacAddress().formInterfaceIdentifier());

        rpl = hostNode->getSubmodule("rpl");
        if (!rpl) {
            EV_WARN << "RPL module not found, associated operations disabled" << endl;
            return;
        }

        rpl->subscribe("parentChanged", this);
    }
}

void TschMSF::refreshDisplay() const {
    if (!rplParentId
            || !pTschLinkInfo->getDedicatedCells(rplParentId).size()
            || !par("showDedicatedTxCells").boolValue())
        return;

    auto txCells = pTschLinkInfo->getDedicatedCells(rplParentId);

    std::sort(txCells.begin(), txCells.end(),
            [](const cellLocation_t c1, const cellLocation_t c2) { return c1.timeOffset < c2.timeOffset; });

    std::ostringstream out;
    out << txCells;

    hostNode->getDisplayString().setTagArg("t", 0, out.str().c_str());
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

    // Auto RX cell conflicts with one of the minimal cells, remove the latter
    if (autoRxCell.timeOffset < pNumMinimalCells) {
        EV_DETAIL << "Auto RX cell conflicts with minimal cell at " << autoRxCell.timeOffset << " slotOffset" << endl;
        ctrlMsg->setDeleteCells(pTschLinkInfo->getMinimalCellLocations(autoRxCell.timeOffset));
    }

    pTsch6p->updateSchedule(*ctrlMsg);
    initialScheduleComplete[pNodeId] = true;
    delete ctrlMsg;
}

void TschMSF::scheduleAutoCell(uint64_t neighbor) {
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

    if (slotOffset < pNumMinimalCells) {
        if (!par("allowMinimalCellsOverride")) {
            EV_DETAIL << "Auto TX collides with minimal cell at " << slotOffset
                    << " slotOffset, but overriding minimal cells is disabled" << endl;
            return;
        }

        ctrlMsg->setDeleteCells(pTschLinkInfo->getMinimalCellLocations(slotOffset));
        EV_DETAIL << "Deleting minimal cell (due to conflict with an auto TX) at " << slotOffset << " slot offset" << endl;
    }


    EV_DETAIL << "Scheduling auto TX cell at " << cellList << " to neighbor - " << MacAddress(neighbor) << endl;

    pTschLinkInfo->addLink(neighbor, false, 0, 0);
    pTschLinkInfo->addCell(neighbor, cellList[0], MAC_LINKOPTIONS_TX | MAC_LINKOPTIONS_SHARED | MAC_LINKOPTIONS_SRCAUTO);
    pTsch6p->updateSchedule(*ctrlMsg);
    delete ctrlMsg;
}

void TschMSF::finish() {
    for (auto const& entry : nbrStatistic)
        std::cout << MacAddress(entry.first).str() << " elapsed "
            << +(entry.second.NumCellsElapsed) << " used " << +(entry.second.NumCellsUsed) << endl;
}

void TschMSF::start() {
    Enter_Method_Silent();

    if (disable)
        return;

    tsch6topCtrlMsg* msg = new tsch6topCtrlMsg();
    msg->setKind(DO_START);

    scheduleAt(simTime() + SimTime(par("startTime").doubleValue()), msg);
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

void TschMSF::handleMaxCellsReached(cMessage* msg) {
    auto neighborMac = (MacAddress*) msg->getContextPointer();
    auto neighborId = neighborMac->getInt();

    // we might get a notification from our own mac
    if (neighborId == pNodeId)
        return;

    EV_DETAIL << "MAX_NUM_CELLS reached, assessing cell usage with " << *neighborMac << ":"
            << endl << "Currently scheduled cells: \n" << pTschLinkInfo->getCells(neighborId) << endl;

    if (neighborId == rplParentId
            && !pTschLinkInfo->getDedicatedCells(rplParentId).size()
            && !pTschLinkInfo->inTransaction(rplParentId))
    {
        addCells(rplParentId, 1);
    }

    double usage = (double) nbrStatistic[neighborId].NumCellsUsed / nbrStatistic[neighborId].NumCellsElapsed;

    EV_DETAIL << printCellUsage(neighborMac->str(), usage) << endl;

    if (usage >= pLimNumCellsUsedHigh)
        addCells(neighborId, par("cellBandwidthIncrement").intValue());
    if (usage <= pLimNumCellsUsedLow && par("allowCellRemoval").boolValue())
        deleteCells(neighborId, 1);

    // reset values
    nbrStatistic[neighborId].NumCellsUsed = 0; //intrand(pMaxNumCells >> 1);
    nbrStatistic[neighborId].NumCellsElapsed = 0;

    delete neighborMac;
}

void TschMSF::handleDoStart(cMessage* msg) {
    hasStarted = true;
    EV_DETAIL << "MSF has started" << endl;

    if (!par("disableHousekeeping").boolValue())
        scheduleAt(simTime() + SimTime(par("HOUSEKEEPINGCOLLISION_PERIOD").intValue()), new tsch6topCtrlMsg("", HOUSEKEEPING));

    /** Get all nodes that are within communication range of @p nodeId.
    *   Note that this only works if all nodes have been initialized (i.e.
    *   maybe not in init() step 1!) **/
    neighbors = pTsch6p->getNeighborsInRange(pNodeId, mac);

    EV_DETAIL << "neighbors: " << endl;
    for (auto nbr: neighbors)
        EV_DETAIL << nbr << ", ";

    // By MSF draft we shouldn't really schedule an auto cell to each neighbor at startup,
    // keeping this as an option for compatibility reasons
    if (!par("autoCellOnDemand").boolValue()) {
        for (auto & neighbor : neighbors) {
            scheduleAutoCell(neighbor);
            initialScheduleComplete[neighbor] = true;
        }
        return;
    }
}

void TschMSF::handleHouskeeping(cMessage* msg) {
    EV_DETAIL << "Performing housekeeping: " << endl;
    scheduleAt(simTime()+ SimTime(par("HOUSEKEEPINGCOLLISION_PERIOD").intValue()), msg);

    // iterate over all neighbors
    for (auto const& entry : initialScheduleComplete) {
        std::map<cellLocation_t, double> pdrStat;

        // calc cell PDR per neighbor
        for (auto & cell :pTschLinkInfo->getCells(entry.first)) {
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

            // TODO: implement this
//            if ((maxPdr->second - cellPdr.second) > pRelocatePdrThres)
//                relocateCell(cellPdr.first, cellPdr.second, maxPdr->second);
        }
    }
}

bool TschMSF::hasDedicatedCell() {
    return rplParentId && pTschLinkInfo->getDedicatedCells(rplParentId).size() > 0;
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
        case DO_START: {
            handleDoStart(msg);
            break;
        }
        case HOUSEKEEPING: {
            handleHouskeeping(msg);
            return;
        }
        default: EV_ERROR << "Unknown message received: " << msg << endl;
    }
    delete msg;
}

void TschMSF::scheduleMinimalCells() {
    EV_DETAIL << "Scheduling minimal cells for broadcast messages: " << endl;
    auto ctrlMsg = new tsch6topCtrlMsg();
    auto macBroadcast = inet::MacAddress::BROADCAST_ADDRESS.getInt();
    ctrlMsg->setDestId(macBroadcast);
    auto cellOpts = MAC_LINKOPTIONS_TX | MAC_LINKOPTIONS_RX | MAC_LINKOPTIONS_SHARED;
    ctrlMsg->setCellOptions(cellOpts);

    std::vector<cellLocation_t> cellList = {};
    for (offset_t i = 0; i < pNumMinimalCells; i++)
        cellList.push_back({i, 0});

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
    if (inputVec.size() < numRequested)
        return {};

    // shuffle the array
    std::random_device rd;
    std::mt19937 e{rd()};

    std::shuffle(inputVec.begin(), inputVec.end(), e);

    // copy the picked ones
    std::vector<cellLocation_t> picked = {};
    for (auto i = 0; i < numRequested; i++)
        picked.push_back(inputVec[i]);

//    std::copy(slotOffsets.begin(), slotOffsets.begin() + numSlots, picked.begin());
    return picked;
}

bool TschMSF::slotOffsetAvailable(offset_t slOf) {
    return !pTschLinkInfo->timeOffsetScheduled(slOf) && slOf != autoRxCell.timeOffset && !slotOffsetReserved(slOf);
}

std::vector<offset_t> TschMSF::getAvailableSlotsInRange(offset_t start, offset_t end) {
    std::vector<offset_t> slots = {};

    for (auto slOf = start; slOf < end; slOf++) {
        if (slotOffsetAvailable(slOf))
            slots.push_back(slOf);
    }

    EV_DETAIL << "In range (" << start << ", " << end << ") found available slot offsets: " << slots << endl;
    return slots;
}


std::vector<offset_t> TschMSF::getAvailableSlotsInRange(int slOffsetEnd) {
    return getAvailableSlotsInRange(pNumMinimalCells + 1, slOffsetEnd);
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

    if (!availableSlots.size())
        return -ENOSPC;

    if (availableSlots.size() < numCells) {
        EV_DETAIL << "More cells requested than total available, returning "
                << availableSlots.size() << " cells" << endl;
        for (auto s : availableSlots) {
            cellList.push_back({s, (offset_t) intrand(pNumChannels)});
            reservedTimeOffsets[destId].push_back(s);
        }

        return -EFBIG;
    }

    // Fill cell list with all available slot offsets and random/cross-layer channel offset
    for (auto sl : availableSlots)
        cellList.push_back({sl, (offset_t) intrand(pNumChannels)});

    EV_DETAIL << "Initialized cell list: " << cellList << endl;

    // Select only required number of cells from cell list
    cellList = pickRandomly(cellList, numCells);
    EV_DETAIL << "After picking required number of cells: " << cellList << endl;
    // Block selected cells' slot offsets until 6P transaction finishes
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

    // TODO: refactor this
    for (auto cell : cellList) {
        unsigned int slOf = cell.timeOffset;
        EV_DETAIL << "proposed cell " << cell;
        if (slotOffsetAvailable(slOf))
        {
            EV_DETAIL << " available" << endl;
            /* cell is still available, pick it. */
            pickedCells.push_back(cell);

        } else
            EV_DETAIL << " unavailable" << endl;
    }
    cellList.clear();
    for (auto i = 0; i < numCells && i < pickedCells.size(); i++)
        cellList.push_back(pickedCells[i]);

    EV_DETAIL << "Cell list after picking: " << cellList << endl;

    if (pickedCells.size() == 0)
        return -ENOSPC;

    if (pickedCells.size() < numCells)
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

void TschMSF::handleResponse(uint64_t sender, tsch6pReturn_t code, int numCells, std::vector<cellLocation_t> *cellList)
{
    EV_WARN << "Deprecated handleResponse() invoked" << endl;
    return;
}

void TschMSF::handleSuccessResponse(uint64_t sender, tsch6pCmd_t cmd, int numCells, std::vector<cellLocation_t> cellList)
{
    if (pTschLinkInfo->getLastKnownType(sender) != MSG_RESPONSE)
        return;

    switch (cmd) {
        case CMD_ADD: {
            // No cells were added => failed transaction
            // likely neighbor's schedule is quite busy, or proposed cell list is too small
            if (!cellList.size()) {
                EV_DETAIL << "6P ADD to " << MacAddress(sender) << " failed" << endl;
                // We may have to retry if there's a need for good communication to this neighbor
                break;
            }

            removeAutoTxCell(sender);

            EV_DETAIL << "Added cells: " << cellList << endl;
            emit(s_InitialScheduleComplete, (unsigned long) sender);
            break;
        }
        case CMD_DELETE: {
            // remove cell statistics for cells that have been deleted
            clearCellStats(cellList);
            break;
        }
        case CMD_RELOCATE: {
            if (!cellList.size())
                EV_DETAIL << "Failed to relocate: " << cellList << endl;
            break;
        }
        case CMD_CLEAR: {
            clearCellStats(cellList);
            clearScheduleWithNode(sender);
            break;
        }
        default: EV_DETAIL << "Unsupported 6P command" << endl;
    }

}

void TschMSF::clearScheduleWithNode(uint64_t sender, bool clearAutoCells)
{
    auto ctrlMsg = new tsch6topCtrlMsg();
    ctrlMsg->setDestId(sender);
    std::vector<cellLocation_t> deletable = {};
    EV_DETAIL << "Clearing schedule with " << MacAddress(sender) << endl;

    // Clearing cells scheduled with sender
    // (except for auto-cells preserving minimal connectivity) from local schedule
    for (auto &cell : pTschLinkInfo->getCells(sender))
        if (!getCellOptions_isAUTO(std::get<1>(cell)) || clearAutoCells)
            deletable.push_back(std::get<0>(cell));

    if (deletable.size()) {
        EV_DETAIL << "Found cells to delete: " << deletable << endl;
        ctrlMsg->setDeleteCells(deletable);
        pTsch6p->updateSchedule(*ctrlMsg);
    } else
        delete ctrlMsg;

//    delete ctrlMsg;
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
        // Clear all state info with unreliable neighbor
        case RC_RESET: {
            clearCellStats(cellList);
            clearScheduleWithNode(sender);
            pTschLinkInfo->resetLink(sender, MSG_RESPONSE);
            break;
        }
        // Handle all other return codes (RC_ERROR, RC_VERSION, RC_SFID, ...) as a generic error
        default: {
            clearCellStats(cellList);
            clearScheduleWithNode(sender);
            pTschLinkInfo->resetLink(sender, MSG_RESPONSE);
        }
    }
}

void TschMSF::handleInconsistency(uint64_t destId, uint8_t seqNum) {
    Enter_Method_Silent();
    EV_WARN << "Inconsistency detected" << endl;
    /* Free already reserved cells to avoid race conditions */
    reservedTimeOffsets[destId].clear();

    pTsch6p->sendClearRequest(destId, pTimeout);
}

int TschMSF::getTimeout() {
    Enter_Method_Silent();

    return pTimeout;
}

bool TschMSF::validSlotOffsets(SlotframeChunk chunk) {
    return !(chunk.start <= 0 || chunk.end < 1);
}

void TschMSF::addCells(uint64_t nodeId, int numCells, uint8_t cellOptions) {
    if (numCells < 1) {
        EV_WARN << "Invalid number of cells requested - " << numCells << endl;
        return;
    }

    EV_DETAIL << "Trying to add " << numCells << " cell(s) to " << MacAddress(nodeId) << endl;

    if (pTschLinkInfo->inTransaction(nodeId)) {
        EV_WARN << "Can't add cells, currently in another transaction with this node" << endl;
        return;
    }

    std::vector<cellLocation_t> cellList = {};
    createCellList(nodeId, cellList, numCells + pCellListRedundancy);

    if (!cellList.size())
        EV_DETAIL << "No cells could be added to cell list, aborting ADD" << endl;
    else
        pTsch6p->sendAddRequest(nodeId, cellOptions, numCells, cellList, pTimeout);
}

void TschMSF::deleteCells(uint64_t nodeId, int numCells)
{
    std::vector<cellLocation_t> scheduledCells = pTschLinkInfo->getDedicatedCells(nodeId);

    if (nodeId == rplParentId) {
        EV_DETAIL << "Deleting " << numCells << " cell(s) to preferred parent, currently scheduled: "
                << scheduledCells << endl;
        if (scheduledCells.size() - numCells < 1) {
            EV_DETAIL << "Cannot delete last dedicated cell with preferred parent" << endl;
            return;
        }
    }

    // If on-demand scheduling of auto cells enabled (default by IETF MSF draft, 3, p.5)
    // we should also remove auto cells if they're not used
    if (!scheduledCells.size() && par("autoCellOnDemand").boolValue()) {
        EV_DETAIL << "No dedicated cells found, deleting auto" << endl;
        clearScheduleWithNode(nodeId, true);
        return;
    }

    if (scheduledCells.size() - numCells < 0) {
        EV_WARN << "Cannot delete more cells than scheduled" << endl;
        return;
    }

    auto deletable = pickRandomly(scheduledCells, numCells);
    EV_DETAIL << "Currently scheduled cells with this node: " << scheduledCells
            << ",\nchosen for deletion: " << deletable << endl;
    pTsch6p->sendDeleteRequest(nodeId, MAC_LINKOPTIONS_TX, numCells, deletable, pTimeout);
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
        for (auto const& entry : initialScheduleComplete) {
            // subscribe to all neighbor related signals
            auto macStr = MacAddress(entry.first).str();
            for (auto & name: namesNeigh) {
                auto fullname = name + macStr;
                if (src->isSubscribed(fullname.c_str(), this) == false)
                    src->subscribe(fullname.c_str(), this);
            }

            // subscribe to all link related signals
            for (auto & cell :pTschLinkInfo->getCells(entry.first)) {
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

    // Just joined the DODAG, schedule dedicated TX
    if (!rplParentId) {
        rplParentId = newParentId;
        addCells(rplParentId, 1);
        return;
    }

    auto txCells = pTschLinkInfo->getDedicatedCells(rplParentId);
    EV_DETAIL << "Dedicated TX cells currently scheduled with PP: " << txCells << endl;

    /** Clear all negotiated cells with now former PP */
    clearScheduleWithNode(rplParentId);

    rplPreviousParentId = rplParentId; // save for future checks
    rplParentId = newParentId;

    /** Clear all negotiated state with now former PP */
    pTschLinkInfo->abortTransaction(rplPreviousParentId);
    reservedTimeOffsets[rplPreviousParentId].clear();
    pTsch6p->sendClearRequest(rplPreviousParentId, pTimeout);

    if (txCells.size())
        /** and schedule the same amount of cells with the new parent */
        addCells(newParentId, txCells.size());
}

void TschMSF::handlePacketEnqueued(uint64_t dest) {
    auto cells = pTschLinkInfo->getCells(dest);
    EV_DETAIL << "Received MAC notification for a packet enqueued to "
            << MacAddress(dest) << ", cells scheduled to this neighbor:" << cells << endl;
    if (!cells.size() && par("autoCellOnDemand").boolValue())
        scheduleAutoCell(dest);
}

void TschMSF::receiveSignal(cComponent *src, simsignal_t id, long value, cObject *details)
{
    Enter_Method_Silent();

    if (!hasStarted)
        return;

    std::string signalName = getSignalName(id);
    EV_DETAIL << "Got signal - " << signalName << endl;

    if (std::strcmp(signalName.c_str(), "parentChanged") == 0) {
        handleParentChangedSignal(value);
        return;
    }

    if (std::strcmp(signalName.c_str(), "pktEnqueued") == 0) {
        handlePacketEnqueued(value);
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

    if (!getCellOptions_isAUTO(options))
        updateCellTxStats(cell, statisticStr);

    if (options != 0xFF && getCellOptions_isTX(options))
        updateNeighborStats(neighbor, statisticStr);
}

void TschMSF::updateCellTxStats(cellLocation_t cell, std::string statType) {
    EV_DETAIL << "Updating cell Tx stats" << endl;
    if (cellStatistic.find(cell) == cellStatistic.end())
        cellStatistic.insert({cell, {0, 0}});

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
