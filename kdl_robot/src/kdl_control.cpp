#include "kdl_ros_control/kdl_control.h"

KDLController::KDLController(KDLRobot &_robot)
{
    robot_ = &_robot;
}

Eigen::VectorXd KDLController::idCntr(KDL::JntArray &_qd,
                                      KDL::JntArray &_dqd,
                                      KDL::JntArray &_ddqd,
                                      double _Kp, double _Kd){
    // read current state
    Eigen::VectorXd q = robot_->getJntValues();
    Eigen::VectorXd dq = robot_->getJntVelocities();

    // calculate errors
    Eigen::VectorXd e = _qd.data - q;
    Eigen::VectorXd de = _dqd.data - dq;
    Eigen::VectorXd ddqd = _ddqd.data;
    return robot_->getJsim() * (ddqd + _Kd*de + _Kp*e)
            + robot_->getCoriolis() + robot_->getGravity() /*friction compensation?*/;
}

Eigen::VectorXd KDLController::idCntr(KDL::Frame &_desPos,
                                      KDL::Twist &_desVel,
                                      KDL::Twist &_desAcc,
                                      double _Kpp, double _Kpo,
                                      double _Kdp, double _Kdo){
   /// Inverse Dynamic Control in operational space ///

   // calculate gain matrices
   Eigen::Matrix<double,6,6> Kp = Eigen::Matrix<double,6,6>::Zero();
   Eigen::Matrix<double,6,6> Kd = Eigen::Matrix<double,6,6>::Zero();
   Kp.block(0,0,3,3) = _Kpp*Eigen::Matrix3d::Identity();
   Kp.block(3,3,3,3) = _Kpo*Eigen::Matrix3d::Identity();
   Kd.block(0,0,3,3) = _Kdp*Eigen::Matrix3d::Identity();
   Kd.block(3,3,3,3) = _Kdo*Eigen::Matrix3d::Identity();

   // read current state
   Eigen::Matrix<double,6,7> J = robot_->getEEJacobian().data;
   Eigen::Matrix<double,7,7> I = Eigen::Matrix<double,7,7>::Identity();
   Eigen::Matrix<double,7,7> B = robot_->getJsim();
   // Eigen::Matrix<double,7,6> Jpinv = weightedPseudoInverse(B,J);
   Eigen::Matrix<double,7,6> Jpinv = pseudoinverse(J);

   // position
   Eigen::Vector3d p_d(_desPos.p.data);
   Eigen::Vector3d p_e(robot_->getEEFrame().p.data);
   Eigen::Matrix<double,3,3,Eigen::RowMajor> R_d(_desPos.M.data);
   Eigen::Matrix<double,3,3,Eigen::RowMajor> R_e(robot_->getEEFrame().M.data);
   R_d = matrixOrthonormalization(R_d);
   R_e = matrixOrthonormalization(R_e);

   // velocity
   Eigen::Vector3d dot_p_d(_desVel.vel.data);
   Eigen::Vector3d dot_p_e(robot_->getEEVelocity().vel.data);
   Eigen::Vector3d omega_d(_desVel.rot.data);
   Eigen::Vector3d omega_e(robot_->getEEVelocity().rot.data);

   // acceleration
   Eigen::Matrix<double,6,1> dot_dot_x_d = Eigen::Matrix<double,6,1>::Zero();
   Eigen::Matrix<double,3,1> dot_dot_p_d(_desAcc.vel.data);
   Eigen::Matrix<double,3,1> dot_dot_r_d(_desAcc.rot.data);

   // compute linear errors
   Eigen::Matrix<double,3,1> e_p = computeLinearError(p_d,p_e);
   Eigen::Matrix<double,3,1> dot_e_p = computeLinearError(dot_p_d,dot_p_e);

   // shared control
   // Eigen::Vector3d lin_acc;
   // lin_acc << _desAcc.vel.x(), _desAcc.vel.y(), _desAcc.vel.z(); //use desired acceleration
   // lin_acc << dot_dot_p_d + _Kdp*(dot_e_p) + _Kpp*(e_p); // assuming no friction no loads
   // Eigen::Matrix<double,3,3> R_sh = shCntr(lin_acc);

   // compute orientation errors
   Eigen::Matrix<double,3,1> e_o = computeOrientationError(R_d,R_e);
   Eigen::Matrix<double,3,1> dot_e_o = computeOrientationVelocityError(omega_d, omega_e, R_d, R_e);
   Eigen::Matrix<double,6,1> x_tilde = Eigen::Matrix<double,6,1>::Zero();
   Eigen::Matrix<double,6,1> dot_x_tilde = Eigen::Matrix<double,6,1>::Zero();
   x_tilde << e_p, e_o;
   dot_x_tilde << dot_e_p, dot_e_o;  //-omega_e
   dot_dot_x_d << dot_dot_p_d, dot_dot_r_d;

   // null space control
   // double cost;
   // Eigen::VectorXd grad = gradientJointLimits(robot_->getJntValues(),robot_->getJntLimits(),cost);

   // inverse dynamics
   Eigen::Matrix<double,6,1> y = Eigen::Matrix<double,6,1>::Zero();
   Eigen::Matrix<double,6,1> J_dot_q_dot = robot_->getEEJacDotqDot();
   y << (dot_dot_x_d - J_dot_q_dot + Kd*dot_x_tilde + Kp*x_tilde);
   Eigen::Matrix<double,7,1> tau = Eigen::Matrix<double,7,1>::Zero();
   // tau = B*(Jpinv*y) + robot_->getGravity() + robot_->getCoriolis();

   tau = B*(Jpinv*y) + (I-Jpinv*J)*(/*- 10*grad */ - 1.0*robot_->getJntVelocities()) + robot_->getGravity() + robot_->getCoriolis();

   // std::cout << "----------DEBUG-----------" << std::endl;

   // std::cout << "Kp: " << std::endl << Kp << std::endl;
   // std::cout << "Kd: " << std::endl << Kd << std::endl;

   // std::cout << "jacobian: " << std::endl << J << std::endl;
   // std::cout << "jsim: " << std::endl << B << std::endl;
   // std::cout << "jpinv: " << std::endl << Jpinv << std::endl;

   // std::cout << "p_d: " << std::endl << p_d << std::endl;
   // std::cout << "p_e: " << std::endl << p_e << std::endl;
   // std::cout << "R_d: " << std::endl << R_d << std::endl;
   // std::cout << "R_e: " << std::endl << R_e << std::endl;

   // std::cout << "dot_p_d: " << std::endl << dot_p_d << std::endl;
   // std::cout << "dot_p_e: " << std::endl << dot_p_e << std::endl;
   // std::cout << "omega_d: " << std::endl << omega_d << std::endl;
   // std::cout << "omega_e: " << std::endl << omega_e << std::endl;

   // std::cout << "dot_dot_x_d: " << std::endl << dot_dot_x_d << std::endl;
   // std::cout << "dot_dot_p_d: " << std::endl << dot_dot_p_d << std::endl;
   // std::cout << "dot_dot_r_d: " << std::endl << dot_dot_r_d << std::endl;

   // std::cout << "e_p: " << std::endl << e_p << std::endl;
   // std::cout << "dot_e_p: " << std::endl << dot_e_p << std::endl;

   // std::cout << "e_o: " << std::endl << e_o << std::endl;
   // std::cout << "dot_e_o: " << std::endl << dot_e_o << std::endl;

   // std::cout << "x_tilde: " << std::endl << x_tilde << std::endl;
   // std::cout << "dot_x_tilde: " << std::endl << dot_x_tilde << std::endl;
   // std::cout << "dot_dot_x_d: " << std::endl << dot_dot_x_d << std::endl;

   // std::cout << "dot_dot_x_d: " << std::endl << dot_dot_x_d << std::endl;

   // std::cout << "J_dot_q_dot: " << std::endl << J_dot_q_dot << std::endl;
   // std::cout << "y: " << std::endl << y << std::endl;
   // std::cout << "tau: " << std::endl << tau << std::endl;3

   // std::cout << "Errors: " << std::endl << x_tilde << std::endl;

   return tau;
}
