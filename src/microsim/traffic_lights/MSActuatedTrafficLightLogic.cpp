/****************************************************************************/
// Eclipse SUMO, Simulation of Urban MObility; see https://eclipse.org/sumo
// Copyright (C) 2001-2019 German Aerospace Center (DLR) and others.
// This program and the accompanying materials
// are made available under the terms of the Eclipse Public License v2.0
// which accompanies this distribution, and is available at
// http://www.eclipse.org/legal/epl-v20.html
// SPDX-License-Identifier: EPL-2.0
/****************************************************************************/
/// @file    MSActuatedTrafficLightLogic.cpp
/// @author  Daniel Krajzewicz
/// @author  Julia Ringel
/// @author  Jakob Erdmann
/// @author  Michael Behrisch
/// @author  Laura Bieker
/// @date    Sept 2002
/// @version $Id$
///
// An actuated (adaptive) traffic light logic
/****************************************************************************/


// ===========================================================================
// included modules
// ===========================================================================
#include <config.h>

#include <cassert>
#include <utility>
#include <vector>
#include <bitset>
#include <microsim/MSEventControl.h>
#include <microsim/output/MSDetectorControl.h>
#include <microsim/output/MSInductLoop.h>
#include <microsim/MSGlobals.h>
#include <microsim/MSNet.h>
#include "MSTrafficLightLogic.h"
#include "MSActuatedTrafficLightLogic.h"
#include <microsim/MSLane.h>
#include <netload/NLDetectorBuilder.h>
#include <utils/common/StringUtils.h>


// ===========================================================================
// parameter defaults definitions
// ===========================================================================
#define DEFAULT_MAX_GAP "3.0"
#define DEFAULT_PASSING_TIME "1.9"
#define DEFAULT_DETECTOR_GAP "2.0"

#define DEFAULT_LENGTH_WITH_GAP 7.5


// ===========================================================================
// method definitions
// ===========================================================================
MSActuatedTrafficLightLogic::MSActuatedTrafficLightLogic(MSTLLogicControl& tlcontrol,
        const std::string& id, const std::string& programID,
        const Phases& phases,
        int step, SUMOTime delay,
        const std::map<std::string, std::string>& parameter,
        const std::string& basePath) :
    MSSimpleTrafficLightLogic(tlcontrol, id, programID, TLTYPE_ACTUATED, phases, step, delay, parameter) {

    myMaxGap = StringUtils::toDouble(getParameter("max-gap", DEFAULT_MAX_GAP));
    myPassingTime = StringUtils::toDouble(getParameter("passing-time", DEFAULT_PASSING_TIME)); // passing-time seems obsolete... (Leo)
    myDetectorGap = StringUtils::toDouble(getParameter("detector-gap", DEFAULT_DETECTOR_GAP));
    myShowDetectors = StringUtils::toBool(getParameter("show-detectors", "false"));
    myFile = FileHelpers::checkForRelativity(getParameter("file", "NUL"), basePath);
    myFreq = TIME2STEPS(StringUtils::toDouble(getParameter("freq", "300")));
    myVehicleTypes = getParameter("vTypes", "");
}

MSActuatedTrafficLightLogic::~MSActuatedTrafficLightLogic() { }

void
MSActuatedTrafficLightLogic::init(NLDetectorBuilder& nb) {
    MSTrafficLightLogic::init(nb);
    assert(myLanes.size() > 0);
    bool warn = true; // warn only once

    // Detector position should be computed based on road speed. If the position
    // is quite far away and the minDur is short this may cause the following
    // problems:
    //
    // 1)  high flow failure: 
    // In a standing queue, no vehicle touches the detector. 
    // By the time the queue advances, the detector gap has been exceeded and the phase terminates prematurely
    //
    // 2) low flow failure
    // The standing queue is fully between stop line and detector and there are no further vehicles. 
    // The minDur is too short to let all vehicles pass
    //
    // Problem 2) is not so critical because there is less potential for
    // jamming in a low-flow situation. In contrast, problem 1) should be
    // avoided as it has big jamming potential. We compute an upper bound for the
    // detector disatnce to avoid it


    // change values for setting the loops and lanestate-detectors, here
    //SUMOTime inductLoopInterval = 1; //
    LaneVectorVector::const_iterator i2;
    LaneVector::const_iterator i;
    // build the induct loops
    double maxDetectorGap = 0;
    for (i2 = myLanes.begin(); i2 != myLanes.end(); ++i2) {
        const LaneVector& lanes = *i2;
        for (i = lanes.begin(); i != lanes.end(); i++) {
            MSLane* lane = (*i);
            if (noVehicles(lane->getPermissions())) {
                // do not build detectors on green verges or sidewalks
                continue;
            }
            if (myInductLoops.find(lane) != myInductLoops.end()) {
                // only build one detector per lane
                continue;
            }
            const SUMOTime minDur = getMinimumMinDuration(lane);
            if (minDur == std::numeric_limits<SUMOTime>::max()) {
                // only build detector if this lane is relevant for an actuated phase
                continue;
            }
            double length = lane->getLength();
            double speed = lane->getSpeedLimit();
            double inductLoopPosition = MIN2(
                    myDetectorGap * speed,
                    (STEPS2TIME(minDur) - 1) * DEFAULT_LENGTH_WITH_GAP);

            // check whether the lane is long enough
            double ilpos = length - inductLoopPosition;
            if (ilpos < 0) {
                ilpos = 0;
            }
            // Build the induct loop and set it into the container
            std::string id = "TLS" + myID + "_" + myProgramID + "_InductLoopOn_" + lane->getID();
            myInductLoops[lane] = nb.createInductLoop(id, lane, ilpos, myVehicleTypes, myShowDetectors);
            MSNet::getInstance()->getDetectorControl().add(SUMO_TAG_INDUCTION_LOOP, myInductLoops[lane], myFile, myFreq);
            maxDetectorGap = MAX2(maxDetectorGap, length - ilpos);

            if (warn && floor(floor(inductLoopPosition / DEFAULT_LENGTH_WITH_GAP) * myPassingTime) > STEPS2TIME(minDur)) {
                // warn if the minGap is insufficient to clear vehicles between stop line and detector
                WRITE_WARNING("At actuated tlLogic '" + getID() + "', minDur " + time2string(minDur) + " is too short for a detector gap of " + toString(inductLoopPosition) + "m.");
                warn = false;
            }
        }
    }
}


SUMOTime
MSActuatedTrafficLightLogic::getMinimumMinDuration(MSLane* lane) const {
    SUMOTime result = std::numeric_limits<SUMOTime>::max();
    for (const MSPhaseDefinition* phase : myPhases) {
        const std::string& state = phase->getState();
        for (int i = 0; i < (int)state.size(); i++)  {
            if (state[i] == LINKSTATE_TL_GREEN_MAJOR || state[i] == LINKSTATE_TL_GREEN_MINOR) {
                for (MSLane* cand: getLanesAt(i)) {
                    if (lane == cand) {
                        if (phase->minDuration != phase->maxDuration) {
                            result = MIN2(result, phase->minDuration);
                        }
                    }
                }
            }
        }
    }
    return result;
}


// ------------ Switching and setting current rows
SUMOTime
MSActuatedTrafficLightLogic::trySwitch() {
    // checks if the actual phase should be continued
    // @note any vehicles which arrived during the previous phases which are now waiting between the detector and the stop line are not
    // considere here. RiLSA recommends to set minDuration in a way that lets all vehicles pass the detector
    const double detectionGap = gapControl();
    if (detectionGap < std::numeric_limits<double>::max()) {
        return duration(detectionGap);
    }
    // increment the index to the current phase
    myStep++;
    assert(myStep <= (int)myPhases.size());
    if (myStep == (int)myPhases.size()) {
        myStep = 0;
    }
    //stores the time the phase started
    myPhases[myStep]->myLastSwitch = MSNet::getInstance()->getCurrentTimeStep();
    // set the next event
    return getCurrentPhaseDef().minDuration;
}


// ------------ "actuated" algorithm methods
SUMOTime
MSActuatedTrafficLightLogic::duration(const double detectionGap) const {
    assert(getCurrentPhaseDef().isGreenPhase());
    assert((int)myPhases.size() > myStep);
    const SUMOTime actDuration = MSNet::getInstance()->getCurrentTimeStep() - myPhases[myStep]->myLastSwitch;
    // ensure that minimum duration is kept
    SUMOTime newDuration = getCurrentPhaseDef().minDuration - actDuration;
    // try to let the last detected vehicle pass the intersection (duration must be positive)
    newDuration = MAX3(newDuration, TIME2STEPS(myDetectorGap - detectionGap), SUMOTime(1));
    // cut the decimal places to ensure that phases always have integer duration
    if (newDuration % 1000 != 0) {
        const SUMOTime totalDur = newDuration + actDuration;
        newDuration = (totalDur / 1000 + 1) * 1000 - actDuration;
    }
    // ensure that the maximum duration is not exceeded
    newDuration = MIN2(newDuration, getCurrentPhaseDef().maxDuration - actDuration);
    return newDuration;
}


double
MSActuatedTrafficLightLogic::gapControl() {
    //intergreen times should not be lenghtend
    assert((int)myPhases.size() > myStep);
    double result = std::numeric_limits<double>::max();
    if (!getCurrentPhaseDef().isGreenPhase()) {
        return result; // end current phase
    }

    // Checks, if the maxDuration is kept. No phase should longer send than maxDuration.
    SUMOTime actDuration = MSNet::getInstance()->getCurrentTimeStep() - myPhases[myStep]->myLastSwitch;
    if (actDuration >= getCurrentPhaseDef().maxDuration) {
        return result; // end current phase
    }

    // now the gapcontrol starts
    const std::string& state = getCurrentPhaseDef().getState();
    for (int i = 0; i < (int) state.size(); i++)  {
        if (state[i] == LINKSTATE_TL_GREEN_MAJOR || state[i] == LINKSTATE_TL_GREEN_MINOR) {
            const std::vector<MSLane*>& lanes = getLanesAt(i);
            for (LaneVector::const_iterator j = lanes.begin(); j != lanes.end(); j++) {
                if (myInductLoops.find(*j) == myInductLoops.end()) {
                    continue;
                }
                if (!MSGlobals::gUseMesoSim) { // why not check outside the loop? (Leo)
                    const double actualGap = static_cast<MSInductLoop*>(myInductLoops.find(*j)->second)->getTimeSinceLastDetection();
                    if (actualGap < myMaxGap) {
                        result = MIN2(result, actualGap);
                    }
                }
            }
        }
    }
    return result;
}



/****************************************************************************/

