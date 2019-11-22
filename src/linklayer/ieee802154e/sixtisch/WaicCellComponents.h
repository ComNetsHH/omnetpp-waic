/*
 * TODO: license foo
 */

/**
 * @file WaicCellComponents.h
 * @author Lotte Steenbrink <lotte.steenbrink@tuhh.de>
 *
 * @brief Constants and data structures for WAIC MAC Cell code
 */

#ifndef __WAIC_WAICCELLCOMPONENTS_H_
#define __WAIC_WAICCELLCOMPONENTS_H_

#include <vector>
#include <map>
#include <cstdint>

typedef unsigned int offset_t;

/** The location of a cell, defined by <timeOffset, channelOffset> */
struct cellLocation_t {
    offset_t timeOffset;
    offset_t channelOffset;

    bool operator==(const cellLocation_t& other) const {
        return (timeOffset == other.timeOffset) &&
                (channelOffset == other.channelOffset);
    }
    bool operator<(const cellLocation_t& other) const {
        return (timeOffset < other.timeOffset);
    }
};

/* Bit masks for link options (see fig. 7-54 of the IEEE802.15.4e standard) */
enum macLinkOption_t {
    MAC_LINKOPTIONS_TX       = 0x01, /**< The link is a TX link */
    MAC_LINKOPTIONS_RX       = 0x02, /**< The link is a RX link */
    MAC_LINKOPTIONS_SHARED   = 0x04, /**< The link is a Shared link (can be
                                           combined with MAC_LINKOPTIONS_TX) */
    MAC_LINKOPTIONS_TIMEKEEPING = 0x08, /**< The link is to be used for clock
                                           synchronization. Shall be set to 1
                                           for MAC_LINKOPTIONS_RX. */
    MAC_LINKOPTIONS_PRIORITY = 0x10, /** The link is a priority channel access,
                                           as defined in section 6.2.5.2 of the
                                           IEEE802.15.4e standard*/
    MAC_LINKOPTIONS_NONE = 0x23
};

/**
 * @return             true if the TX bit in @p cellOptions is set,
 *                     false otherwise
 */
inline bool getCellOptions_isTX(uint8_t cellOptions) {
    return cellOptions & MAC_LINKOPTIONS_TX;
}

/**
 * @return             true if the RX bit in @p cellOptions is set,
 *                     false otherwise
 */
inline bool getCellOptions_isRX(uint8_t cellOptions) {
    return cellOptions & MAC_LINKOPTIONS_RX;
}

/**
 * @return             true if the SHARED bit in @p cellOptions is set,
 *                     false otherwise
 */
inline bool getCellOptions_isSHARED(uint8_t cellOptions) {
    return cellOptions & MAC_LINKOPTIONS_SHARED;
}

typedef enum {
    INTERFERENCE_PROBABILITY /**< the probability that a link is interfered with. */
} metricType;

/**
 * The metric that denotes the predicted/measured quality of a cell.
 */
class WaicCellMetric {
public:
    metricType type;
    /*TODO: use uint between 0 and 100 instead? */
    float value;
};

#endif /* __WAIC_WAICCELLCOMPONENTS_H_ */
