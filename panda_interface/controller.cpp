"""
The main feedback controller (with controller state machine) for the 
panda arm. It utilizes operational space as well as 
joint space control formulations.
The trajectories for each task are also computed here.
1kHz control loop frequency

@authors: Varun Nayak, Lauren Luo, Angela Cheng, Connie Kang
"""

#include "Sai2Model.h"
#include "Sai2Primitives.h"
#include "redis/RedisClient.h"
#include "timer/LoopTimer.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

#include <signal.h>
bool runloop = true;
void sighandler(int sig) { runloop = false; }

using namespace std;
using namespace Eigen;

const string robot_file = "./resources/panda_arm.urdf";

#define JOINT_CONTROLLER 0
#define POSORI_CONTROLLER 1
#define JOINT_CONTROLLER_SHOT 2
#define END_SHOT 3

#define WAIT_MODE 0
#define EXECUTE_MODE 1

int mode = WAIT_MODE;

int state = JOINT_CONTROLLER;

// function prototypes
bool robotReachedGoal(VectorXd x, VectorXd x_desired, VectorXd xdot,
                      VectorXd xddot, VectorXd omega, VectorXd alpha);
Vector3d calculatePointInTrajectory(double t);
bool inRange(double t, double lower, double upper);
Matrix3d calculateRotationInTrajectory(double t, double psi);

// calculates time it takes to swing
double flick_time(double swing_angle, double hit_velocity, double ee_length);
double sinusoidal_trajectory(double angular_velocity, double t,
                             double theta_mid, double swing_angle);
double sinusoidal_velocity(double angular_velocity, double t, double theta_mid,
                           double swing_angle);

// double flick(double t, double time, double start_angle, double end_angle);
void safetyChecks(VectorXd q, VectorXd dq, VectorXd tau, int dof);

// redis keys:
// - read:
std::string JOINT_ANGLES_KEY;
std::string JOINT_VELOCITIES_KEY;
std::string JOINT_TORQUES_SENSED_KEY;
// - write
std::string JOINT_TORQUES_COMMANDED_KEY;

// - model
std::string MASSMATRIX_KEY;
std::string CORIOLIS_KEY;
std::string ROBOT_GRAVITY_KEY;

// state
std::string MODE_CHANGE_KEY = "modechange";

std::string SHOT_ANGLE_KEY = "shotangle";
std::string SHOT_POS_KEY = "shotpos";

// soft safety values
const std::array<double, 7> joint_position_max = {2.7, 1.6, 2.7, -0.2,
                                                  2.7, 3.6, 2.7};
const std::array<double, 7> joint_position_min = {-2.7, -1.6, -2.7, -3.0,
                                                  -2.7, 0.2,  -2.7};
const std::array<double, 7> joint_velocity_limits = {2.0, 2.0, 2.0, 2.0,
                                                     2.5, 2.5, 2.5};
const std::array<double, 7> joint_torques_limits = {85, 85, 85, 85, 10, 10, 10};

unsigned long long controller_counter = 0;

// time slots for which pieces of the trajectory are executed
double t_0 = 0;
double t_1 = 4;
double t_2 = 8;
double t_3 = 12;
double t_4 = 13;
const double ee_length = 17.70 * 0.0254;
double theta_mid = -1.03 + 0.2;

Vector4d cue_start_pos;

bool centershot = false;

const bool flag_simulation = false;
// const bool flag_simulation = true;

const bool inertia_regularization = true;

int main() {

  if (flag_simulation) {
    JOINT_ANGLES_KEY = "sai2::cs225a::panda_robot::sensors::q";
    JOINT_VELOCITIES_KEY = "sai2::cs225a::panda_robot::sensors::dq";
    JOINT_TORQUES_COMMANDED_KEY = "sai2::cs225a::panda_robot::actuators::fgc";
  } else {
    JOINT_TORQUES_COMMANDED_KEY = "sai2::FrankaPanda::actuators::fgc";
    JOINT_ANGLES_KEY = "sai2::FrankaPanda::sensors::q";
    JOINT_VELOCITIES_KEY = "sai2::FrankaPanda::sensors::dq";
    JOINT_TORQUES_SENSED_KEY = "sai2::FrankaPanda::sensors::torques";
    MASSMATRIX_KEY = "sai2::FrankaPanda::sensors::model::massmatrix";
    CORIOLIS_KEY = "sai2::FrankaPanda::sensors::model::coriolis";
    ROBOT_GRAVITY_KEY = "sai2::FrankaPanda::sensors::model::robot_gravity";
  }

  // start redis client
  auto redis_client = RedisClient();
  redis_client.connect();

  // set up signal handler
  signal(SIGABRT, &sighandler);
  signal(SIGTERM, &sighandler);
  signal(SIGINT, &sighandler);

  // load robots
  auto robot = new Sai2Model::Sai2Model(robot_file, false);
  robot->_q = redis_client.getEigenMatrixJSON(JOINT_ANGLES_KEY);
  VectorXd initial_q = robot->_q;
  robot->updateModel();

  // prepare controller
  int dof = robot->dof();
  VectorXd command_torques = VectorXd::Zero(dof);
  MatrixXd N_prec = MatrixXd::Identity(dof, dof);

  // pose task
  const string control_link = "link7";
  const Vector3d control_point =
      Vector3d((-ee_length + 0.0254 / 2) * sin(M_PI / 4.0),
               (ee_length + 0.0254 / 2) * cos(M_PI / 4.0), 0.1070 + 0.0254 * 1);
  auto posori_task =
      new Sai2Primitives::PosOriTask(robot, control_link, control_point);

  // #ifdef USING_OTG
  // posori_task->_use_interpolation_flag = true;
  // #else
  posori_task->_use_velocity_saturation_flag = true;
  // #endif

  VectorXd posori_task_torques = VectorXd::Zero(dof);
  posori_task->_kp_pos = 400.0;
  posori_task->_kv_pos = 25.0;
  posori_task->_kp_ori = 400.0;
  posori_task->_kv_ori = 25.0;

  // joint task
  auto joint_task = new Sai2Primitives::JointTask(robot);

  // #ifdef USING_OTG
  // 	joint_task->_use_interpolation_flag = true;
  // #else
  joint_task->_use_velocity_saturation_flag = true;
  joint_task->_saturation_velocity = M_PI / 3 * VectorXd::Ones(dof);

  VectorXd joint_task_torques = VectorXd::Zero(dof);
  joint_task->_kp = 150.0;
  joint_task->_kv = 20.0;

  VectorXd q_init_desired = initial_q;
  q_init_desired << 0.004, -0.44, 0.315, -1.63, 1.53, 2.15, -0.33;
  joint_task->_desired_position = q_init_desired;

  VectorXd safe_joint_positions = initial_q;
  safe_joint_positions << 0.0, 0.0, 0.0, -1.6, 0.0, 1.9, 0.0;

  // create a timer
  LoopTimer timer;
  timer.initializeTimer();
  timer.setLoopFrequency(1000);
  double start_time = timer.elapsedTime(); // secs
  bool fTimerDidSleep = true;

  // initialize position and velocity in cartesian space
  Vector3d x;     // quantity to store current task space position
  Vector3d xdot;  // quantiy to store current task space velocity]
  Vector3d xddot; // linear acceleration
  Vector3d omega;
  Vector3d alpha;

  double psi = 90 * M_PI / 180.0; // shot angle;
  double hit_velocity;
  double swing_angle;
  double total_time = 1.3;
  double shot_angular_velocity = 0;
  double a;
  double w;
  double command_time;

  while (runloop) {
    // wait for next scheduled loop
    timer.waitForNextLoop();
    double time = timer.elapsedTime() - start_time;

    // read robot state from redis
    robot->_q = redis_client.getEigenMatrixJSON(JOINT_ANGLES_KEY);
    robot->_dq = redis_client.getEigenMatrixJSON(JOINT_VELOCITIES_KEY);

    // update cartesian position of the robot from joint angles
    robot->position(x, control_link, control_point); // position of end effector
    robot->linearVelocity(xdot, control_link,
                          control_point); // velocity of end effector
    robot->linearAcceleration(xddot, control_link, control_point);
    robot->angularVelocity(omega, control_link);
    robot->angularAcceleration(alpha, control_link);

    // calculate current time;
    double dt = 0.001;
    double t = controller_counter * dt;

    if (mode == WAIT_MODE) {
      // cout<<"in wait mode"<<endl;
      joint_task->reInitializeTask();
      N_prec.setIdentity();
      joint_task->updateTaskModel(N_prec);
      joint_task->computeTorques(joint_task_torques);
      command_torques = joint_task_torques;

      if (redis_client.get(MODE_CHANGE_KEY) == "execute") {
        mode = EXECUTE_MODE;
        printf("Going into EXECUTE_MODE\n");

        std::string shot_pos = redis_client.get(SHOT_POS_KEY);
        std::string shot_ang = redis_client.get(SHOT_ANGLE_KEY);

        int delimiter = shot_pos.find(",");
        cue_start_pos << 0.001 * stod(shot_pos.substr(0, delimiter)),
            0.001 * stod((shot_pos).substr(delimiter + 1, shot_pos.length())),
            0, 1;
        cout << "desired cue pos in board frame: " << cue_start_pos << endl;

        psi = stod(shot_ang);
        std::cout << "psi: " << psi << endl;

        if (psi <= 1.571 && psi >= 1.569) {
          centershot = true;
        }

        // retrieve hit velocity through redis
        swing_angle = 120 * M_PI / 180.0;

        // angular velocity
        shot_angular_velocity = hit_velocity / ee_length;
        cout << "shot angular velocity is " << shot_angular_velocity << endl;
        a = swing_angle / 2.0;
        w = shot_angular_velocity / a;
      }

    } else if (mode == EXECUTE_MODE) {

      // update model
      if (flag_simulation) {
        robot->updateModel();
      } else {
        robot->updateKinematics();
        robot->_M = redis_client.getEigenMatrixJSON(MASSMATRIX_KEY);
        if (inertia_regularization) {
          // robot->_M(4,4) += 0.07;
          // robot->_M(5,5) += 0.07;
          // robot->_M(6,6) += 0.07;
        }
        robot->_M_inv = robot->_M.inverse();
      }

      // cout<<"z position is "<<x(2)<<endl;

      if (state == JOINT_CONTROLLER) {
        // update task model and set hierarchy
        joint_task->_desired_position = q_init_desired;
        N_prec.setIdentity();
        joint_task->updateTaskModel(N_prec);
        joint_task->_kp = 250.0;
        // cout << "HERERERERERER" << endl;

        if (inertia_regularization) {
          robot->_M += 0.1 * MatrixXd::Identity(dof, dof);
        }

        // compute torques
        joint_task->computeTorques(joint_task_torques);

        command_torques = joint_task_torques;

        if ((robot->_q - q_init_desired).norm() < 0.15) {
          cout << "Reached JOINT Goal" << endl;
          t = 0;
          controller_counter = 0;
          posori_task->_desired_position = calculatePointInTrajectory(t);
          posori_task->_desired_orientation =
              calculateRotationInTrajectory(t, psi);
          joint_task->_kp = 300.0;
          joint_task->_kv = 25.0;
          posori_task->_kp_pos = 400.0;
          posori_task->_kv_pos = 25.0;
          posori_task->_kp_ori = 400.0;
          posori_task->_kv_ori = 25.0;

          state = POSORI_CONTROLLER;
        }
      }

      else if (state == POSORI_CONTROLLER) {
        // if the robot reaches the desired position and is at rest, come out of
        // the loop
        if (robotReachedGoal(x, calculatePointInTrajectory(100), xdot, xddot,
                             omega, alpha) &&
            t > t_4) // 100 is arbitrarily large, represents last point in traj
        {
          printf("Reached Final Goal \n");
          printf("Going into WAIT_MODE..\n");
          mode = WAIT_MODE;
          redis_client.set(MODE_CHANGE_KEY, "wait");
          state = JOINT_CONTROLLER;
          joint_task->_desired_position = q_init_desired;
        } else {
          joint_task->_desired_position = safe_joint_positions;
        }

        if (t > t_3 && t < t_3 + total_time) {
          cout << "Shooting" << endl;
          state = JOINT_CONTROLLER_SHOT;
          joint_task->reInitializeTask();
          theta_mid = robot->_q(dof - 1);
        }
        joint_task->_use_velocity_saturation_flag = true;
        // update task model and set hierarchy
        N_prec.setIdentity();
        posori_task->updateTaskModel(N_prec);
        N_prec = posori_task->_N;
        joint_task->updateTaskModel(N_prec);

        if (inertia_regularization) {
          posori_task->_Lambda += 0.1 * MatrixXd::Identity(6, 6);
        }

        posori_task->_desired_position = calculatePointInTrajectory(t);
        posori_task->_desired_orientation =
            calculateRotationInTrajectory(t, psi);
        // compute torques
        posori_task->computeTorques(posori_task_torques);
        joint_task->computeTorques(joint_task_torques);

        command_torques = posori_task_torques + joint_task_torques;
        // command_torques = posori_task_torques;
      } else if (state == JOINT_CONTROLLER_SHOT) {
        joint_task->_kp = 400.0;
        if (t > t_3 && t < t_4) {
          joint_task->_desired_position(dof - 1) =
              theta_mid + M_PI / 24; //+ swing_angle/2.0;
          command_time = 0.0;
          // cout<<"joint velocity is "<<robot->_dq(dof-1)<<endl;
        } else if ((t - t_4) <= total_time) {
          joint_task->_use_velocity_saturation_flag = true;
          if (centershot) {
            joint_task->_saturation_velocity << M_PI / 3, M_PI / 3, M_PI / 3,
                M_PI / 3, M_PI / 2, M_PI / 2, 2.33;
            cout << "slowing down for centershot" << endl;
          } else {
            joint_task->_saturation_velocity << M_PI / 3, M_PI / 3, M_PI / 3,
                M_PI / 3, M_PI / 2, M_PI / 2, 3.0;
          }
          joint_task->_desired_position(dof - 1) = theta_mid - M_PI / 4;
          command_time++;

        }

        N_prec.setIdentity();
        joint_task->updateTaskModel(N_prec);
        joint_task->_kp = 250.0;

        if (inertia_regularization) {
          robot->_M += 0.1 * MatrixXd::Identity(dof, dof);
        }

        // compute torques
        joint_task->computeTorques(joint_task_torques);

        command_torques = joint_task_torques;
        if (t > (t_4 + total_time)) {

          joint_task->_use_velocity_saturation_flag = true;
          cout << "Done Shooting" << endl;
          centershot = false;
          posori_task->_desired_position = calculatePointInTrajectory(t);
          posori_task->_desired_orientation =
              calculateRotationInTrajectory(t, psi);
          joint_task->_kp = 200.0;
          joint_task->_kv = 20.0;
          posori_task->_kp_pos = 200.0;
          posori_task->_kv_pos = 20.0;
          posori_task->_kp_ori = 200.0;
          posori_task->_kv_ori = 20.0;
          joint_task->_saturation_velocity = M_PI / 4 * VectorXd::Ones(dof);
          state = POSORI_CONTROLLER;
        }
      }

      // send to redis

      safetyChecks(robot->_q, robot->_dq, command_torques, dof);

      controller_counter++;
    }
    redis_client.setEigenMatrixJSON(JOINT_TORQUES_COMMANDED_KEY,
                                    command_torques);
  }

  command_torques.setZero();
  redis_client.setEigenMatrixJSON(JOINT_TORQUES_COMMANDED_KEY, command_torques);

  double end_time = timer.elapsedTime();
  std::cout << "\n";
  std::cout << "Controller Loop run time  : " << end_time << " seconds\n";
  std::cout << "Controller Loop updates   : " << timer.elapsedCycles() << "\n";
  std::cout << "Controller Loop frequency : "
            << timer.elapsedCycles() / end_time << "Hz\n";

  return 0;
}

bool robotReachedGoal(VectorXd x, VectorXd x_desired, VectorXd xdot,
                      VectorXd xddot, VectorXd omega, VectorXd alpha) {
  double epsilon = 3;
  double error_norm = 100 * xdot.norm() + 10 * (x - x_desired).norm() +
                      1000 * xddot.norm() + 1000 * omega.norm() +
                      1000 * alpha.norm();
  if (error_norm < epsilon) {
    return true;
  }
  return false;
}

/*
This function returns the desired point in the operational space
that the robot needs to track. The plan is to divide it into sections
parametrized by 't'.

From calibration and shot planner, we need (all expressed in robot frame)
1) Home position (xh, yh, zh)
2) Position of cue coin (xc,yc,zc) (also pre-determined)
3) Desired position of cue coin (xcd, ycd, zcd) (get from  shot planner over
redis when mode changes) 4) Backup and Flick Trajectory expressed in the robot
frame (get required params from shot planner and tranform it)
*/
Vector3d calculatePointInTrajectory(double t) {
  // diameter of board is 20.125 in, convert to m:
  double r = 20.125 / 2 * 0.0254;

  double x_offset = 0.7385; // need to calibrate
  double y_offset = 0.1070 + 0.035;
  double z_offset = 0.3120; // need to calibrate
  Vector3d xh;
  xh << 0.2859, 0.2787, 0.4300; // calibrate this
  // Vector3d xc; xc << 0.5,0.35,0.5;
  Vector3d xc;
  xc << r * sin(-M_PI / 4) + x_offset, r * cos(-M_PI / 4) + y_offset,
      z_offset; // calibrate this
  // Vector3d xcd; xcd << r*sin(-1.75*M_PI/4)+x_offset,
  // r*cos(-1.75*M_PI/4)+y_offset, z_offset; //calculate this - get from redis
  MatrixXd T = MatrixXd::Zero(4, 4);
  T << 0, 1, 0, x_offset, -1, 0, 0, y_offset, 0, 0, 1, z_offset, 0, 0, 0, 1;

  VectorXd xcd = VectorXd::Zero(3);
  VectorXd xcd_4d = VectorXd::Zero(4);
  xcd_4d = T * cue_start_pos; // calculate this - get from redis
  xcd = xcd_4d.head(3);
  // std::cout << "xcd: " << xcd << std::endl;

  Vector3d x;

  if (inRange(t, t_0, t_1)) {
    // home position to cue coin position
    x = xh + (xc - xh) * (t - t_0) / (t_1 - t_0);
  } else if (inRange(t, t_1, t_2)) {

    // x = xc;
    // Move cue coin from home to desired position
    double x0 = xc(0);
    double y0 = xc(1);
    double xf = xcd(0);
    double yf = xcd(1);

    double t0 = atan2(x0 - x_offset, y0 - y_offset);
    double tf = atan2(xf - x_offset, yf - y_offset);

    double old_range = t_2 - t_1;
    double new_range = tf - t0;
    double new_t = (((t - t_1) * (new_range)) / old_range) + t0;

    x << r * sin(new_t) + x_offset, r * cos(new_t) + y_offset, xc(2);
  } else if (inRange(t, t_2, t_3)) {
    x = xcd;

  } else if (inRange(t, t_3, t_4)) {
    x = xcd; // shooting
  } else {
    x = xh;
    // cout<<"going home"<<endl;
  }

  return x;
}

/*
From calibration and shot planner, we need:
1) Orientation in home position (point straight and flat maybe?)
2) Angle to which to turn to once we reach the cue coin position (get from shot
planner over redis) 3) Angle to which to point to for the backup and shot (get
from shot planner over redis)
*/
Matrix3d calculateRotationInTrajectory(double t, double psi) {
  Matrix3d rot;
  Matrix3d home_orientation;
  // psi = psi + M_PI/8;

  home_orientation << 0.7360145, 0.6763110, 0.0297644, -0.0413102, 0.0009846,
      0.9991459, 0.6757041, -0.7366155, 0.0286632;

  if (inRange(t, t_0, t_1)) {
    rot = AngleAxisd(-M_PI / 4 * (t - t_0) / (t_1 - t_0), Vector3d::UnitZ())
              .toRotationMatrix() *
          home_orientation;
  } else if (inRange(t, t_1, t_2)) {
    // rotate -90 degrees to gather the coin
    rot = AngleAxisd(-M_PI / 4, Vector3d::UnitZ()).toRotationMatrix() *
          home_orientation;

  } else if (inRange(t, t_2, t_3)) {
    // double psi; psi = 135*M_PI/180.0; //get psi from redis
    Matrix3d hit_rot;
    hit_rot << cos(-M_PI / 2 + psi), -sin(-M_PI / 2 + psi), 0,
        sin(-M_PI / 2 + psi), cos(-M_PI / 2 + psi), 0, 0, 0, 1;
    rot = hit_rot * home_orientation;

  } else if (inRange(t, t_3, t_4)) {
    // double psi; psi = 135*M_PI/180.0; //get psi from redis
    Matrix3d hit_rot;
    hit_rot << cos(-M_PI / 2 + psi), -sin(-M_PI / 2 + psi), 0,
        sin(-M_PI / 2 + psi), cos(-M_PI / 2 + psi), 0, 0, 0, 1;
    rot = hit_rot * home_orientation;
  } else {
    rot = home_orientation;
  }

  return rot;
}

double flick_time(double swing_angle, double hit_velocity, double ee_length) {
  double time;
  double angle_range;
  angle_range = swing_angle;
  time = angle_range * ee_length / hit_velocity;
  return time;
}

double sinusoidal_trajectory(double angular_velocity, double t,
                             double theta_mid, double swing_angle) {
  double pos;
  double a;
  double w;
  a = swing_angle / 2.0;
  w = angular_velocity / a;
  pos = -a * sin(w * t - M_PI / 2.0) + theta_mid;
  return pos;
}

double sinusoidal_velocity(double angular_velocity, double t, double theta_mid,
                           double swing_angle) {
  double vel;
  double a;
  double w;
  a = swing_angle / 2.0;
  w = angular_velocity / a;
  vel = -a * w * cos(w * t - M_PI / 2.0);
  return vel;
}

// return true if t lies in between lower and upper limits
bool inRange(double t, double lower, double upper) {
  return ((t < upper) && (t >= lower));
}

// soft limit safetycheck as per the driver
void safetyChecks(VectorXd q, VectorXd dq, VectorXd tau, int dof) {
  for (int i = 0; i < dof; i++) {
    if (q[i] > joint_position_max[i])
      cout << "------!! VIOLATED MAX JOINT POSITION SOFT LIMIT !!------- for "
              "joint "
           << i + 1 << endl;
    if (q[i] < joint_position_min[i])
      cout << "------!! VIOLATED MIN JOINT POSITION SOFT LIMIT !!------- for "
              "joint"
           << i + 1 << endl;
    if (abs(dq[i]) > joint_velocity_limits[i])
      cout << "------!! VIOLATED MAX JOINT VELOCITY SOFT LIMIT !!------- for "
              "joint"
           << i + 1 << endl;
    if (abs(tau[i]) > joint_torques_limits[i])
      cout
          << "------!! VIOLATED MAX JOINT TORQUE SOFT LIMIT !!------- for joint"
          << i + 1 << endl;
  }
}