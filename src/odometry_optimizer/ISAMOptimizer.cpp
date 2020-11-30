#include "ISAMOptimizer.h"
#include "utils.h"
#include <gtsam/geometry/Pose3.h>
#include <gtsam/geometry/Rot3.h>
#include <gtsam/slam/BetweenFactor.h>
#include <gtsam/navigation/CombinedImuFactor.h>
#include <gtsam/navigation/ImuFactor.h>
#include <gtsam/nonlinear/NonlinearFactorGraph.h>
#include <gtsam/nonlinear/Values.h>
#include <gtsam/inference/Symbol.h>
#include <nav_msgs/Path.h>
#include <geometry_msgs/PoseWithCovariance.h>
#include <nav_msgs/Odometry.h>
#include <gtsam/nonlinear/ISAM2Params.h>
#include <iostream>

#include "geometry_msgs/PoseStamped.h"

#include "tf2_ros/transform_listener.h"
#include "tf2_ros/message_filter.h"
#include "message_filters/subscriber.h"
#include "tf2_geometry_msgs/tf2_geometry_msgs.h"

using symbol_shorthand::B;  // Bias  (ax,ay,az,gx,gy,gz)
using symbol_shorthand::V;  // Vel   (xdot,ydot,zdot)
using symbol_shorthand::X;  // Pose3 (x,y,z,r,p,y)

ISAM2Params getParams() {
    ISAM2Params isam2Params;
    isam2Params.relinearizeThreshold = 0.1;
    isam2Params.factorization = ISAM2Params::CHOLESKY;
}

ISAMOptimizer::ISAMOptimizer(ros::Publisher *pub, const boost::shared_ptr<PreintegrationCombinedParams> &imu_params)
        : pub(*pub),
          isam(ISAM2(ISAM2Params())),
          odometryMeasurementProcessor(std::bind(&ISAMOptimizer::processOdometryMeasurement,
                                                 this,
                                                 std::placeholders::_1),
                                       3) {
    auto imuBias = imuBias::ConstantBias(); // Initialize at zero bias
    imuMeasurements = std::make_shared<PreintegratedCombinedMeasurements>(imu_params, imuBias);
    imuMeasurements->resetIntegration();
}

void ISAMOptimizer::safeAddIMUMsgToDeque(const sensor_msgs::Imu &msg) {
    imuQueue.addMeasurement(msg);
}

void ISAMOptimizer::recvIMUMsgAndUpdateState(const sensor_msgs::Imu &msg) {
    mu.lock();
    auto imuTime = ros::Time(msg.header.stamp.sec, msg.header.stamp.nsec);
    auto linearMsg = msg.linear_acceleration;
    auto acc = Vector3(linearMsg.x, linearMsg.y, linearMsg.z);
    auto angularMsg = msg.angular_velocity;
    auto omega = Vector3(angularMsg.x, angularMsg.y, angularMsg.z);
    recvIMUAndUpdateState(acc, omega, imuTime);
    mu.unlock();
}

void ISAMOptimizer::recvIMUAndUpdateState(const Vector3 &acc, const Vector3 &omega, ros::Time imuTime) {
    if (!imuReady) {
        lastIMUTime = imuTime;
        imuReady = true;
        mu.unlock();
        return;
    }
    if (imuTime < lastIMUTime) {
        // Oops, something is out of order: Just ignore the message until we get something new
        ROS_INFO("Got out of order IMU message");
        mu.unlock();
        return;
    }
    auto dt = imuTime - lastIMUTime;
    imuMeasurements->integrateMeasurement(acc, omega, dt.toSec());
    // add to buffer
    imuCount++;
    lastIMUTime = imuTime;
}

void ISAMOptimizer::publishUpdatedPoses() {
    nav_msgs::Path pathMsg;
    for (int j = 1; j < poseNum; ++j) {
        auto pose = isam.calculateEstimate<Pose3>(X(j));
        auto stamp = timestamps[j];
        auto stampedPose = createStampedPoseMsg(pose, stamp);
        stampedPose.header.frame_id = "/map";
        pathMsg.poses.push_back(stampedPose);
    }
    pathMsg.header.frame_id = "/map";
    pub.publish(pathMsg);
    ROS_INFO("Published path");
}

void ISAMOptimizer::publishNewestPose() {
    auto newPose = isam.calculateEstimate<Pose3>(X(poseNum));
    nav_msgs::Odometry msg;
    msg.pose.pose = toPoseMsg(newPose);
    msg.header.frame_id = "/map";
    pub.publish(msg);
    ROS_INFO("Published updated pose");
}

// TODO: Remove
void ISAMOptimizer::incrementTime(const ros::Time &stamp) {
    poseNum++;
    timestamps.push_back(stamp);
}

void ISAMOptimizer::processOdometryMeasurement(const OdometryMeasurement &measurement) {
    mu.lock();
    bool shouldPublish = false;
    geometry_msgs::PoseStamped poseStamped;
    switch (measurement.type) {
        case ODOMETRY_TYPE_ROVIO:
            poseStamped.header = measurement.msg.header;
            poseStamped.pose = measurement.msg.pose.pose;
            shouldPublish = recvRovioOdometryAndUpdateState(poseStamped, noiseModel::Diagonal::Variances(
                    (Vector(6) << 0.1, 0.1, 0.1, 0.1, 0.1, 0.1).finished()));
            break;
        case ODOMETRY_TYPE_LOAM:
            poseStamped.header = measurement.msg.header;
            poseStamped.pose = measurement.msg.pose.pose;
            geometry_msgs::PoseStamped poseMsgInWorldFrame;
            tfListenerNew.transformPose("/map", poseStamped, poseMsgInWorldFrame); // Can fail if newer transform message has arrived
            shouldPublish = recvLidarOdometryAndUpdateState(poseMsgInWorldFrame, noiseModel::Diagonal::Variances(
                    (Vector(6) << 0.2, 0.2, 0.2, 0.2, 0.2, 0.2).finished()));
            break;
    }
    if (shouldPublish) {
        publishNewestPose();
    }
    lastOdometryMeasurement = measurement;
    mu.unlock();
}

void ISAMOptimizer::recvRovioOdometryAndAddToQueue(const nav_msgs::Odometry &msg) {
    OdometryMeasurement measurement{ODOMETRY_TYPE_ROVIO, msg, true};
    odometryMeasurementProcessor.addMeasurement(measurement);
}

void ISAMOptimizer::recvLidarOdometryAndAddToQueue(const nav_msgs::Odometry &msg) {
    OdometryMeasurement measurement{ODOMETRY_TYPE_LOAM, msg, true};
    odometryMeasurementProcessor.addMeasurement(measurement);
}

bool ISAMOptimizer::recvRovioOdometryAndUpdateState(const geometry_msgs::PoseStamped &msg,
                                                    const boost::shared_ptr<noiseModel::Gaussian> &noise) {
    return recvOdometryAndUpdateState(msg, lastRovioPoseNum, lastRovioOdometry, noise);
}

bool ISAMOptimizer::recvLidarOdometryAndUpdateState(const geometry_msgs::PoseStamped &msg,
                                                    const boost::shared_ptr<noiseModel::Gaussian> &noise) {
    return recvOdometryAndUpdateState(msg, lastLidarPoseNum, lastLidarOdometry, noise);
}

void
addPoseVelocityAndBiasValues(int poseNum, const Pose3 &pose, const Vector3 &velocity, const imuBias::ConstantBias &bias,
                             Values &values) {
    values.insert(X(poseNum), pose);
    values.insert(V(poseNum), velocity);
    values.insert(B(poseNum), bias);
}

void addValuesOnIMU(int poseNum, const NavState &navState, const imuBias::ConstantBias &bias, Values &values) {
    addPoseVelocityAndBiasValues(poseNum, navState.pose(), navState.v(), bias, values);
}

void addValuesOnOdometry(int poseNum, const Pose3 &odometry, const Vector3 &velocity, const imuBias::ConstantBias &bias,
                         Values &values) {
    addPoseVelocityAndBiasValues(poseNum, odometry, velocity, bias, values);
}

void addOdometryBetweenFactor(int fromPoseNum, int poseNum, const Pose3 &fromOdometry, const Pose3 &odometry,
                              const boost::shared_ptr<noiseModel::Gaussian> &noise, NonlinearFactorGraph &graph) {
    auto odometryDelta = fromOdometry.between(odometry);
    graph.add(BetweenFactor<Pose3>(X(fromPoseNum), X(poseNum), odometryDelta, noise));
}

void addPriorFactor(const Pose3 &pose, const Vector3 &velocity, const imuBias::ConstantBias &bias,
                    const boost::shared_ptr<noiseModel::Diagonal> &noiseX,
                    const boost::shared_ptr<noiseModel::Isotropic> &noiseV,
                    const boost::shared_ptr<noiseModel::Isotropic> &noiseB, NonlinearFactorGraph &graph) {
    graph.addPrior<Pose3>(X(1), pose, noiseX);
    graph.addPrior<Vector3>(V(1), velocity, noiseV);
    graph.addPrior<imuBias::ConstantBias>(B(1), bias, noiseB);
}

void addCombinedFactor(int poseNum, const Pose3 &fromOdometry, const Pose3 &odometry,
                       const shared_ptr<TangentPreintegration> &imuMeasurements,
                       const boost::shared_ptr<noiseModel::Gaussian> &odometryNoise, NonlinearFactorGraph &graph) {
    auto odometryDelta = fromOdometry.between(odometry);
    auto imuCombined = dynamic_cast<const PreintegratedCombinedMeasurements &>(*imuMeasurements);
    CombinedImuFactor imuFactor(X(poseNum - 1), V(poseNum - 1), X(poseNum), V(poseNum), B(poseNum - 1), B(poseNum),
                                imuCombined);
    graph.add(imuFactor);
}

bool
ISAMOptimizer::recvOdometryAndUpdateState(const geometry_msgs::PoseStamped &msg, int &lastPoseNum, Pose3 &lastOdometry,
                                          const boost::shared_ptr<noiseModel::Gaussian> &noise) {
    Values values;
    NonlinearFactorGraph graph;
    auto odometry = toPose3(msg.pose);
    if (poseNum == 0) {
        poseNum++;
        auto priorNoiseX = noiseModel::Diagonal::Sigmas((Vector(6)
                << 0.0001, 0.0001, 0.0001, 0.0001, 0.0001, 0.0001).finished()); // We are dead sure about starting pos
        auto priorNoiseV = noiseModel::Isotropic::Sigma(3, 0.1);
        auto priorNoiseB = noiseModel::Isotropic::Sigma(6, 2);
        addPriorFactor(odometry, Vector3::Zero(), imuBias::ConstantBias(), priorNoiseX, priorNoiseV, priorNoiseB,
                       graph);
        addPoseVelocityAndBiasValues(poseNum, odometry, Vector3::Zero(), imuBias::ConstantBias(), values);
        isam.update(graph, values);
        imuMeasurements->resetIntegration(); // TODO Remove
        lastOdometry = odometry;
        lastPoseNum = poseNum;
        return true;
    }
    auto haveIMUMeasurements = imuQueue.hasMeasurementsInRange(lastOdometryMeasurement.msg.header.stamp, msg.header.stamp);
    if (haveIMUMeasurements) {
        poseNum++;
        if (lastPoseNum > 0) {
            addOdometryBetweenFactor(lastPoseNum, poseNum, lastOdometry, odometry, noise, graph);
        } else {
            // If we have initialized the other modality, but not this one, we need to add a second hard prior on the pose
            auto priorNoiseX = noiseModel::Diagonal::Sigmas(
                    (Vector(6) << 0.01, 0.01, 0.01, 0.01, 0.01, 0.01).finished());
            graph.addPrior<Pose3>(X(poseNum), odometry, priorNoiseX);
            cout << "Added prior on second modality" << endl;
        }
        imuQueue.integrateIMUMeasurements(imuMeasurements, lastOdometryMeasurement.msg.header.stamp, msg.header.stamp);
        addCombinedFactor(poseNum, lastOdometry, odometry, imuMeasurements, noise, graph);
        NavState navState = imuMeasurements->predict(getPrevIMUState(), getPrevIMUBias());
        getPrevIMUBias().print("prev imu bias: ");
        addValuesOnIMU(poseNum, navState, getPrevIMUBias(), values);
        try {
            isam.update(graph, values);
        } catch (gtsam::IndeterminantLinearSystemException e) {
            cout << "Caught indeterminant linear system exception when adding between factors" << endl;
            auto factors = isam.getFactorsUnsafe();
            auto result = isam.calculateEstimate();
            factors.print("whole graph");
            throw e;
        }
        imuMeasurements->resetIntegrationAndSetBias(isam.calculateEstimate<imuBias::ConstantBias>(B(poseNum)));
        lastOdometry = odometry;
        lastPoseNum = poseNum;
        return true;
    } else {
        // We do not increment poseNum because the time interval since last pose is so small
        cout << "Not enough time between imu measurements: " << lastPoseNum << endl;
        if (lastPoseNum > 0 && poseNum > lastPoseNum) {
            cout << "Adding between factor without imu" << lastPoseNum << endl;
            addOdometryBetweenFactor(lastPoseNum, poseNum, lastOdometry, odometry, noise, graph);
            isam.update(graph, values);
            lastPoseNum = poseNum;
            lastOdometry = odometry;
        }
        return false;
    }
}

NavState ISAMOptimizer::getPrevIMUState() {
    return NavState(isam.calculateEstimate<Pose3>(X(poseNum - 1)), isam.calculateEstimate<Vector3>(V(poseNum - 1)));
}

imuBias::ConstantBias ISAMOptimizer::getPrevIMUBias() {
    return isam.calculateEstimate<imuBias::ConstantBias>(B(poseNum - 1));
}
