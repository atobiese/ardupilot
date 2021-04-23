#include <AP_HAL/AP_HAL.h>

#include "AP_NavEKF3.h"
#include "AP_NavEKF3_core.h"
#include <AP_DAL/AP_DAL.h>

// Check basic filter health metrics and return a consolidated health status
bool NavEKF3_core::healthy(void) const
{
    uint16_t faultInt;
    getFilterFaults(faultInt);
    if (faultInt > 0) {
        return false;
    }
    if (velTestRatio > 1 && posTestRatio > 1 && hgtTestRatio > 1) {
        // all three metrics being above 1 means the filter is
        // extremely unhealthy.
        return false;
    }
    // Give the filter a second to settle before use
    if ((imuSampleTime_ms - ekfStartTime_ms) < 1000 ) {
        return false;
    }
    // position and height innovations must be within limits when on-ground and in a static mode of operation
    float horizErrSq = sq(innovVelPos[3]) + sq(innovVelPos[4]);
    if (onGround && (PV_AidingMode == AID_NONE) && ((horizErrSq > 1.0f) || (fabsf(hgtInnovFiltState) > 1.0f))) {
        return false;
    }

    // all OK
    return true;
}

// Return a consolidated error score where higher numbers represent larger errors
// Intended to be used by the front-end to determine which is the primary EKF
float NavEKF3_core::errorScore() const
{
    float score = 0.0f;
    if (tiltAlignComplete && yawAlignComplete) {
        // Check GPS fusion performance
        score = MAX(score, 0.5f * (velTestRatio + posTestRatio));
        // Check altimeter fusion performance
        score = MAX(score, hgtTestRatio);
        // Check airspeed fusion performance - only when we are using at least 2 airspeed sensors so we can switch lanes with 
        // a better one. This only comes into effect for a forward flight vehicle. A sensitivity factor of 0.3 is added to keep the
        // EKF less sensitive to innovations arising due events like strong gusts of wind, thus, prevent reporting high error scores
        if (assume_zero_sideslip()) {
            const auto *arsp = dal.airspeed();
            if (arsp != nullptr && arsp->get_num_sensors() >= 2 && (frontend->_affinity & EKF_AFFINITY_ARSP)) {
                score = MAX(score, 0.3f * tasTestRatio);
            }
        }
        // Check magnetometer fusion performance - need this when magnetometer affinity is enabled to override the inherent compass
        // switching mechanism, and instead be able to move to a better lane
        if (frontend->_affinity & EKF_AFFINITY_MAG) {
            score = MAX(score, 0.3f * (magTestRatio.x + magTestRatio.y + magTestRatio.z));
        }
    }
    return score;
}

#if EK3_FEATURE_BODY_ODOM
// return data for debugging body frame odometry fusion
uint32_t NavEKF3_core::getBodyFrameOdomDebug(Vector3f &velInnov, Vector3f &velInnovVar)
{
    velInnov.x = innovBodyVel[0];
    velInnov.y = innovBodyVel[1];
    velInnov.z = innovBodyVel[2];
    velInnovVar.x = varInnovBodyVel[0];
    velInnovVar.y = varInnovBodyVel[1];
    velInnovVar.z = varInnovBodyVel[2];
    return MAX(bodyOdmDataDelayed.time_ms,wheelOdmDataDelayed.time_ms);
}
#endif // EK3_FEATURE_BODY_ODOM

// provides the height limit to be observed by the control loops
// returns false if no height limiting is required
// this is needed to ensure the vehicle does not fly too high when using optical flow navigation
bool NavEKF3_core::getHeightControlLimit(float &height) const
{
    // only ask for limiting if we are doing optical flow navigation
    if (frontend->sources.useVelXYSource(AP_NavEKF_Source::SourceXY::OPTFLOW) && (PV_AidingMode == AID_RELATIVE) && flowDataValid) {
        // If are doing optical flow nav, ensure the height above ground is within range finder limits after accounting for vehicle tilt and control errors
        const auto *_rng = dal.rangefinder();
        if (_rng == nullptr) {
            // we really, really shouldn't be here.
            return false;
        }
        height = MAX(float(_rng->max_distance_cm_orient(ROTATION_PITCH_270)) * 0.007f - 1.0f, 1.0f);
        // If we are are not using the range finder as the height reference, then compensate for the difference between terrain and EKF origin
        if (frontend->sources.getPosZSource() != AP_NavEKF_Source::SourceZ::RANGEFINDER) {
            height -= terrainState;
        }
        return true;
    } else {
        return false;
    }
}


// return the Euler roll, pitch and yaw angle in radians
void NavEKF3_core::getEulerAngles(Vector3f &euler) const
{
    outputDataNew.quat.to_euler(euler.x, euler.y, euler.z);
    euler = euler - dal.get_trim();
}

// return body axis gyro bias estimates in rad/sec
void NavEKF3_core::getGyroBias(Vector3f &gyroBias) const
{
    if (dtEkfAvg < 1e-6f) {
        gyroBias.zero();
        return;
    }
    gyroBias = stateStruct.gyro_bias / dtEkfAvg;
}

// return accelerometer bias in m/s/s
void NavEKF3_core::getAccelBias(Vector3f &accelBias) const
{
    if (!statesInitialised) {
        accelBias.zero();
        return;
    }
    accelBias = stateStruct.accel_bias / dtEkfAvg;
}

// return the transformation matrix from XYZ (body) to NED axes
void NavEKF3_core::getRotationBodyToNED(Matrix3f &mat) const
{
    outputDataNew.quat.rotation_matrix(mat);
    mat = mat * dal.get_rotation_vehicle_body_to_autopilot_body();
}

// return the quaternions defining the rotation from NED to XYZ (body) axes
void NavEKF3_core::getQuaternion(Quaternion& ret) const
{
    ret = outputDataNew.quat;
}

// return the amount of yaw angle change due to the last yaw angle reset in radians
// returns the time of the last yaw angle reset or 0 if no reset has ever occurred
uint32_t NavEKF3_core::getLastYawResetAngle(float &yawAng) const
{
    yawAng = yawResetAngle;
    return lastYawReset_ms;
}

// return the amount of NE position change due to the last position reset in metres
// returns the time of the last reset or 0 if no reset has ever occurred
uint32_t NavEKF3_core::getLastPosNorthEastReset(Vector2f &pos) const
{
    pos = posResetNE;
    return lastPosReset_ms;
}

// return the amount of vertical position change due to the last vertical position reset in metres
// returns the time of the last reset or 0 if no reset has ever occurred
uint32_t NavEKF3_core::getLastPosDownReset(float &posD) const
{
    posD = posResetD;
    return lastPosResetD_ms;
}

// return the amount of NE velocity change due to the last velocity reset in metres/sec
// returns the time of the last reset or 0 if no reset has ever occurred
uint32_t NavEKF3_core::getLastVelNorthEastReset(Vector2f &vel) const
{
    vel = velResetNE;
    return lastVelReset_ms;
}

// return the NED wind speed estimates in m/s (positive is air moving in the direction of the axis)
// returns true if wind state estimation is active
bool NavEKF3_core::getWind(Vector3f &wind) const
{
    wind.x = stateStruct.wind_vel.x;
    wind.y = stateStruct.wind_vel.y;
    wind.z = 0.0f; // currently don't estimate this
    return !inhibitWindStates;
}

// return the NED velocity of the body frame origin in m/s
//
void NavEKF3_core::getVelNED(Vector3f &vel) const
{
    // correct for the IMU position offset (EKF calculations are at the IMU)
    vel = outputDataNew.velocity + velOffsetNED;
}

// return estimate of true airspeed vector in body frame in m/s
// returns false if estimate is unavailable
bool NavEKF3_core::getAirSpdVec(Vector3f &vel) const
{
    if (inhibitWindStates || PV_AidingMode == AID_NONE) {
        return false;
    }
    vel = outputDataNew.velocity + velOffsetNED;
    vel.x -= stateStruct.wind_vel.x;
    vel.y -= stateStruct.wind_vel.y;
    Matrix3f Tnb; // rotation from nav to body frame
    outputDataNew.quat.inverse().rotation_matrix(Tnb);
    vel = Tnb * vel;
    return true;
}


// Return the rate of change of vertical position in the down direction (dPosD/dt) of the body frame origin in m/s
float NavEKF3_core::getPosDownDerivative(void) const
{
    // return the value calculated from a complementary filter applied to the EKF height and vertical acceleration
    // correct for the IMU offset (EKF calculations are at the IMU)
    return vertCompFiltState.vel + velOffsetNED.z;
}

// Write the last estimated NE position of the body frame origin relative to the reference point (m).
// Return true if the estimate is valid
bool NavEKF3_core::getPosNE(Vector2f &posNE) const
{
    // There are three modes of operation, absolute position (GPS fusion), relative position (optical flow fusion) and constant position (no position estimate available)
    if (PV_AidingMode != AID_NONE) {
        // This is the normal mode of operation where we can use the EKF position states
        // correct for the IMU offset (EKF calculations are at the IMU)
        posNE.x = outputDataNew.position.x + posOffsetNED.x;
        posNE.y = outputDataNew.position.y + posOffsetNED.y;
        return true;

    } else {
        // In constant position mode the EKF position states are at the origin, so we cannot use them as a position estimate
        if(validOrigin) {
            auto &gps = dal.gps();
            if ((gps.status(selected_gps) >= AP_DAL_GPS::GPS_OK_FIX_2D)) {
                // If the origin has been set and we have GPS, then return the GPS position relative to the origin
                const struct Location &gpsloc = gps.location(selected_gps);
                const Vector2f tempPosNE = EKF_origin.get_distance_NE(gpsloc);
                posNE.x = tempPosNE.x;
                posNE.y = tempPosNE.y;
                return false;
            } else if (rngBcnAlignmentStarted) {
                // If we are attempting alignment using range beacon data, then report the position
                posNE.x = receiverPos.x;
                posNE.y = receiverPos.y;
                return false;
            } else {
                // If no GPS fix is available, all we can do is provide the last known position
                posNE.x = outputDataNew.position.x;
                posNE.y = outputDataNew.position.y;
                return false;
            }
        } else {
            // If the origin has not been set, then we have no means of providing a relative position
            posNE.x = 0.0f;
            posNE.y = 0.0f;
            return false;
        }
    }
    return false;
}

// Write the last calculated D position of the body frame origin relative to the EKF origin (m).
// Return true if the estimate is valid
bool NavEKF3_core::getPosD(float &posD) const
{
    // The EKF always has a height estimate regardless of mode of operation
    // Correct for the IMU offset (EKF calculations are at the IMU)
    // Also correct for changes to the origin height
    if ((frontend->_originHgtMode & (1<<2)) == 0) {
        // Any sensor height drift corrections relative to the WGS-84 reference are applied to the origin.
        posD = outputDataNew.position.z + posOffsetNED.z;
    } else {
        // The origin height is static and corrections are applied to the local vertical position
        // so that height returned by getLLH() = height returned by getOriginLLH - posD
        posD = outputDataNew.position.z + posOffsetNED.z + 0.01f * (float)EKF_origin.alt - (float)ekfGpsRefHgt;
    }

    // Return the current height solution status
    return filterStatus.flags.vert_pos;

}

// return the estimated height of body frame origin above ground level
bool NavEKF3_core::getHAGL(float &HAGL) const
{
    HAGL = terrainState - outputDataNew.position.z - posOffsetNED.z;
    // If we know the terrain offset and altitude, then we have a valid height above ground estimate
    return !hgtTimeout && gndOffsetValid && healthy();
}

// Return the last calculated latitude, longitude and height in WGS-84
// If a calculated location isn't available, return a raw GPS measurement
// The status will return true if a calculation or raw measurement is available
// The getFilterStatus() function provides a more detailed description of data health and must be checked if data is to be used for flight control
bool NavEKF3_core::getLLH(struct Location &loc) const
{
    Location origin;
    if (getOriginLLH(origin)) {
        float posD;
        if(getPosD(posD) && PV_AidingMode != AID_NONE) {
            // Altitude returned is an absolute altitude relative to the WGS-84 spherioid
            loc.alt = origin.alt - posD*100;
            loc.relative_alt = 0;
            loc.terrain_alt = 0;
            if (filterStatus.flags.horiz_pos_abs || filterStatus.flags.horiz_pos_rel) {
                // The EKF is able to provide a position estimate
                loc.lat = EKF_origin.lat;
                loc.lng = EKF_origin.lng;
                loc.offset(outputDataNew.position.x, outputDataNew.position.y);
                return true;
            } else {
                // We have been be doing inertial dead reckoning for too long so use raw GPS if available
                if (getGPSLLH(loc)) {
                    return true;
                } else {
                    // Return the EKF estimate but mark it as invalid
                    loc.lat = EKF_origin.lat;
                    loc.lng = EKF_origin.lng;
                    loc.offset(outputDataNew.position.x, outputDataNew.position.y);
                    return false;
                }
            }
        } else {
            // Return a raw GPS reading if available and the last recorded positon if not
            if (getGPSLLH(loc)) {
                return true;
            } else {
                loc.lat = EKF_origin.lat;
                loc.lng = EKF_origin.lng;
                loc.offset(lastKnownPositionNE.x, lastKnownPositionNE.y);
                return false;
            }
        }
    } else {
        // The EKF is not navigating so use raw GPS if available
        return getGPSLLH(loc);
    }
}

bool NavEKF3_core::getGPSLLH(struct Location &loc) const
{
    const auto &gps = dal.gps();
    if ((gps.status(selected_gps) >= AP_DAL_GPS::GPS_OK_FIX_3D)) {
        loc = gps.location(selected_gps);
        return true;
    }
    return false;
}

// return the horizontal speed limit in m/s set by optical flow sensor limits
// return the scale factor to be applied to navigation velocity gains to compensate for increase in velocity noise with height when using optical flow
void NavEKF3_core::getEkfControlLimits(float &ekfGndSpdLimit, float &ekfNavVelGainScaler) const
{
    // If in the last 10 seconds we have received flow data and no odometry data, then we are relying on optical flow
    bool relyingOnFlowData = (imuSampleTime_ms - prevBodyVelFuseTime_ms > 1000)
            && (imuSampleTime_ms - flowValidMeaTime_ms <= 10000);

    // If relying on optical flow, limit speed to prevent sensor limit being exceeded and adjust
    // nav gains to prevent body rate feedback into flow rates destabilising the control loop
    if (PV_AidingMode == AID_RELATIVE && relyingOnFlowData) {
        // allow 1.0 rad/sec margin for angular motion
        ekfGndSpdLimit = MAX((frontend->_maxFlowRate - 1.0f), 0.0f) * MAX((terrainState - stateStruct.position[2]), rngOnGnd);
        // use standard gains up to 5.0 metres height and reduce above that
        ekfNavVelGainScaler = 4.0f / MAX((terrainState - stateStruct.position[2]),4.0f);
    } else {
        ekfGndSpdLimit = 400.0f; //return 80% of max filter speed
        ekfNavVelGainScaler = 1.0f;
    }
}


// return the LLH location of the filters NED origin
bool NavEKF3_core::getOriginLLH(struct Location &loc) const
{
    if (validOrigin) {
        loc = EKF_origin;
        // report internally corrected reference height if enabled
        if ((frontend->_originHgtMode & (1<<2)) == 0) {
            loc.alt = (int32_t)(100.0f * (float)ekfGpsRefHgt);
        }
    }
    return validOrigin;
}

// return earth magnetic field estimates in measurement units / 1000
void NavEKF3_core::getMagNED(Vector3f &magNED) const
{
    magNED = stateStruct.earth_magfield * 1000.0f;
}

// return body magnetic field estimates in measurement units / 1000
void NavEKF3_core::getMagXYZ(Vector3f &magXYZ) const
{
    magXYZ = stateStruct.body_magfield*1000.0f;
}

// return magnetometer offsets
// return true if offsets are valid
bool NavEKF3_core::getMagOffsets(uint8_t mag_idx, Vector3f &magOffsets) const
{
    if (!dal.get_compass()) {
        return false;
    }
    // compass offsets are valid if we have finalised magnetic field initialisation, magnetic field learning is not prohibited,
    // primary compass is valid and state variances have converged
    const float maxMagVar = 5E-6f;
    bool variancesConverged = (P[19][19] < maxMagVar) && (P[20][20] < maxMagVar) && (P[21][21] < maxMagVar);
    if ((mag_idx == magSelectIndex) &&
            finalInflightMagInit &&
            !inhibitMagStates &&
            dal.get_compass()->healthy(magSelectIndex) &&
            variancesConverged) {
        magOffsets = dal.get_compass()->get_offsets(magSelectIndex) - stateStruct.body_magfield*1000.0f;
        return true;
    } else {
        magOffsets = dal.get_compass()->get_offsets(magSelectIndex);
        return false;
    }
}

// return the index for the active magnetometer
// return the index for the active airspeed
uint8_t NavEKF3_core::getActiveAirspeed() const
{
    return (uint8_t)selected_airspeed;
}

// return the innovations for the NED Pos, NED Vel, XYZ Mag and Vtas measurements
void  NavEKF3_core::getInnovations(Vector3f &velInnov, Vector3f &posInnov, Vector3f &magInnov, float &tasInnov, float &yawInnov) const
{
    velInnov.x = innovVelPos[0];
    velInnov.y = innovVelPos[1];
    velInnov.z = innovVelPos[2];
    posInnov.x = innovVelPos[3];
    posInnov.y = innovVelPos[4];
    posInnov.z = innovVelPos[5];
    magInnov.x = 1e3f*innovMag[0]; // Convert back to sensor units
    magInnov.y = 1e3f*innovMag[1]; // Convert back to sensor units
    magInnov.z = 1e3f*innovMag[2]; // Convert back to sensor units
    tasInnov   = innovVtas;
    yawInnov   = innovYaw;
}

// return the synthetic air data drag and sideslip innovations
void NavEKF3_core::getSynthAirDataInnovations(Vector2f &dragInnov, float &betaInnov) const
{
#if EK3_FEATURE_DRAG_FUSION
    dragInnov.x = innovDrag[0];
    dragInnov.y = innovDrag[1];
    betaInnov   = innovBeta;
#endif
}

// return the innovation consistency test ratios for the velocity, position, magnetometer and true airspeed measurements
// this indicates the amount of margin available when tuning the various error traps
// also return the delta in position due to the last position reset
void  NavEKF3_core::getVariances(float &velVar, float &posVar, float &hgtVar, Vector3f &magVar, float &tasVar, Vector2f &offset) const
{
    velVar   = sqrtf(velTestRatio);
    posVar   = sqrtf(posTestRatio);
    hgtVar   = sqrtf(hgtTestRatio);
    // If we are using simple compass yaw fusion, populate all three components with the yaw test ratio to provide an equivalent output
    magVar.x = sqrtf(MAX(magTestRatio.x,yawTestRatio));
    magVar.y = sqrtf(MAX(magTestRatio.y,yawTestRatio));
    magVar.z = sqrtf(MAX(magTestRatio.z,yawTestRatio));
    tasVar   = sqrtf(tasTestRatio);
    offset   = posResetNE;
}

// get a particular source's velocity innovations
// returns true on success and results are placed in innovations and variances arguments
bool NavEKF3_core::getVelInnovationsAndVariancesForSource(AP_NavEKF_Source::SourceXY source, Vector3f &innovations, Vector3f &variances) const
{
    switch (source) {
    case AP_NavEKF_Source::SourceXY::GPS:
        // check for timeouts
        if (AP_HAL::millis() - gpsVelInnovTime_ms > 500) {
            return false;
        }
        innovations = gpsVelInnov;
        variances = gpsVelVarInnov;
        return true;
#if EK3_FEATURE_EXTERNAL_NAV
    case AP_NavEKF_Source::SourceXY::EXTNAV:
        // check for timeouts
        if (AP_HAL::millis() - extNavVelInnovTime_ms > 500) {
            return false;
        }
        innovations = extNavVelInnov;
        variances = extNavVelVarInnov;
        return true;
#endif // EK3_FEATURE_EXTERNAL_NAV
    default:
        // variances are not available for this source
        return false;
    }

    // should never get here but just in case
    return false;
}

/*
return the filter fault status as a bitmasked integer
 0 = quaternions are NaN
 1 = velocities are NaN
 2 = badly conditioned X magnetometer fusion
 3 = badly conditioned Y magnetometer fusion
 4 = badly conditioned Z magnetometer fusion
 5 = badly conditioned airspeed fusion
 6 = badly conditioned synthetic sideslip fusion
 7 = filter is not initialised
*/
void  NavEKF3_core::getFilterFaults(uint16_t &faults) const
{
    faults = (stateStruct.quat.is_nan()<<0 |
              stateStruct.velocity.is_nan()<<1 |
              faultStatus.bad_xmag<<2 |
              faultStatus.bad_ymag<<3 |
              faultStatus.bad_zmag<<4 |
              faultStatus.bad_airspeed<<5 |
              faultStatus.bad_sideslip<<6 |
              !statesInitialised<<7);
}

// Return the navigation filter status message
void  NavEKF3_core::getFilterStatus(nav_filter_status &status) const
{
    status = filterStatus;
}

/*
return filter gps quality check status
*/
void  NavEKF3_core::getFilterGpsStatus(nav_gps_status &faults) const
{
    // init return value
    faults.value = 0;

    // set individual flags
    faults.flags.bad_sAcc           = gpsCheckStatus.bad_sAcc; // reported speed accuracy is insufficient
    faults.flags.bad_hAcc           = gpsCheckStatus.bad_hAcc; // reported horizontal position accuracy is insufficient
    faults.flags.bad_vAcc           = gpsCheckStatus.bad_vAcc; // reported vertical position accuracy is insufficient
    faults.flags.bad_yaw            = gpsCheckStatus.bad_yaw; // EKF heading accuracy is too large for GPS use
    faults.flags.bad_sats           = gpsCheckStatus.bad_sats; // reported number of satellites is insufficient
    faults.flags.bad_horiz_drift    = gpsCheckStatus.bad_horiz_drift; // GPS horizontal drift is too large to start using GPS (check assumes vehicle is static)
    faults.flags.bad_hdop           = gpsCheckStatus.bad_hdop; // reported HDoP is too large to start using GPS
    faults.flags.bad_vert_vel       = gpsCheckStatus.bad_vert_vel; // GPS vertical speed is too large to start using GPS (check assumes vehicle is static)
    faults.flags.bad_fix            = gpsCheckStatus.bad_fix; // The GPS cannot provide the 3D fix required
    faults.flags.bad_horiz_vel      = gpsCheckStatus.bad_horiz_vel; // The GPS horizontal speed is excessive (check assumes the vehicle is static)
}

// send an EKF_STATUS message to GCS
void NavEKF3_core::send_status_report(mavlink_channel_t chan) const
{
    // prepare flags
    uint16_t flags = 0;
    if (filterStatus.flags.attitude) {
        flags |= EKF_ATTITUDE;
    }
    if (filterStatus.flags.horiz_vel) {
        flags |= EKF_VELOCITY_HORIZ;
    }
    if (filterStatus.flags.vert_vel) {
        flags |= EKF_VELOCITY_VERT;
    }
    if (filterStatus.flags.horiz_pos_rel) {
        flags |= EKF_POS_HORIZ_REL;
    }
    if (filterStatus.flags.horiz_pos_abs) {
        flags |= EKF_POS_HORIZ_ABS;
    }
    if (filterStatus.flags.vert_pos) {
        flags |= EKF_POS_VERT_ABS;
    }
    if (filterStatus.flags.terrain_alt) {
        flags |= EKF_POS_VERT_AGL;
    }
    if (filterStatus.flags.const_pos_mode) {
        flags |= EKF_CONST_POS_MODE;
    }
    if (filterStatus.flags.pred_horiz_pos_rel) {
        flags |= EKF_PRED_POS_HORIZ_REL;
    }
    if (filterStatus.flags.pred_horiz_pos_abs) {
        flags |= EKF_PRED_POS_HORIZ_ABS;
    }
    if (!filterStatus.flags.initalized) {
        flags |= EKF_UNINITIALIZED;
    }
    if (filterStatus.flags.gps_glitching) {
        flags |= (1<<15);
    }

    // get variances
    float velVar, posVar, hgtVar, tasVar;
    Vector3f magVar;
    Vector2f offset;
    getVariances(velVar, posVar, hgtVar, magVar, tasVar, offset);

    // Only report range finder normalised innovation levels if the EKF needs the data for primary
    // height estimation or optical flow operation. This prevents false alarms at the GCS if a
    // range finder is fitted for other applications
    float temp;
    if (((frontend->_useRngSwHgt > 0) && activeHgtSource == AP_NavEKF_Source::SourceZ::RANGEFINDER) || (PV_AidingMode == AID_RELATIVE && flowDataValid)) {
        temp = sqrtf(auxRngTestRatio);
    } else {
        temp = 0.0f;
    }
    const float mag_max = fmaxf(fmaxf(magVar.x,magVar.y),magVar.z);

    // send message
    mavlink_msg_ekf_status_report_send(chan, flags, velVar, posVar, hgtVar, mag_max, temp, tasVar);
}

// report the reason for why the backend is refusing to initialise
const char *NavEKF3_core::prearm_failure_reason(void) const
{
    if (gpsGoodToAlign) {
        // we are not failing
        return nullptr;
    }
    return prearm_fail_string;
}


// report the number of frames lapsed since the last state prediction
// this is used by other instances to level load
uint8_t NavEKF3_core::getFramesSincePredict(void) const
{
    return framesSincePredict;
}
