#include "control/controller.hpp"

namespace control {
	Controller::Controller(int frequency)
		: loop_rate_(frequency)
	{
		ROS_INFO("Running controller at %d Hz", frequency);
		model_name_ = "anymal"; // TODO: Replace with node argument
		
		SetVariablesToZero();

		SetupRosTopics();
		SetupRosServices();
		WaitForPublishedTime();
		SpinRosThreads();
		WaitForPublishedState();

		SetRobotMode(kIdle);
	}

	Controller::~Controller()
	{
			ros_node_.shutdown();

			ros_process_queue_.clear();
			ros_process_queue_.disable();
			ros_process_queue_thread_.join();

			ros_publish_queue_.clear();
			ros_publish_queue_.disable();
			ros_publish_queue_thread_.join();
	}

	// **************** //
	// STANDUP SEQUENCE //
	// **************** //

	void Controller::SetJointInitialConfigTraj()
	{
		const std::vector<double> breaks = {
			0.0, seconds_to_initial_config_ 
		};

		std::vector<Eigen::MatrixXd> samples;
		samples.push_back(q_j_);
		samples.push_back(initial_joint_config);

		auto initial_config_pos_traj
			= CreateFirstOrderHoldTraj(breaks, samples);
		auto initial_config_vel_traj
			= initial_config_pos_traj.derivative(1);

		q_j_cmd_traj_ = initial_config_pos_traj;
		q_j_dot_cmd_traj_ = initial_config_vel_traj;
		traj_end_time_s_ = seconds_to_initial_config_;
	}

	void Controller::SetFeetStandupTraj()
	{
		const std::vector<double> breaks = {
			0.0, seconds_to_standup_config_
		};

		Eigen::MatrixXd feet_start_pos(kNumFeetCoords,1);
		feet_start_pos = robot_dynamics_.GetFeetPositions(q_);

		Eigen::MatrixXd feet_standing_pos = feet_start_pos;
		for (int foot_i = 0; foot_i < kNumLegs; ++foot_i)
		{
			int foot_i_z_index = 2 + foot_i * kNumPosDims;
			feet_standing_pos(foot_i_z_index) = standing_height_;
		}

		std::vector<Eigen::MatrixXd> samples;
		samples.push_back(feet_start_pos);
		samples.push_back(feet_standing_pos);

		auto feet_standup_pos_traj
			= CreateFirstOrderHoldTraj(breaks, samples);
		auto feet_standup_vel_traj
			= feet_standup_pos_traj.derivative(1);

		feet_cmd_pos_traj_ = feet_standup_pos_traj;
		feet_cmd_vel_traj_ = feet_standup_vel_traj;
		traj_end_time_s_ = seconds_to_standup_config_;
	}

	// ********** //
	// CONTROLLER //
	// ********** //

	void Controller::UpdateJointCommand()
	{
		seconds_in_mode_ = GetElapsedTimeSince(mode_start_time_);

		switch(control_mode_)	
		{
			case kJointTracking:
				q_j_cmd_ = q_j_cmd_traj_.value(seconds_in_mode_);
				q_j_dot_cmd_ = q_j_dot_cmd_traj_.value(seconds_in_mode_);
				break;
			case kFeetTracking:
				FeetPosControl();
				break;
			default:
				break;
		}

//		std::cout << std::fixed << std::setprecision(2)
//			<< q_j_cmd_.transpose() << std::endl;
			//<< q_j_dot_cmd_.transpose() << std::endl << std::endl;
	}

	void Controller::FeetPosControl()
	{
		gen_coord_vector_t q_with_standard_body_config;
		q_with_standard_body_config
			.block<kNumPoseCoords,1>(0,0) << 0,0,0,1,0,0,0;
		q_with_standard_body_config.block<kNumJoints,1>(kNumPoseCoords,0)
			= q_j_; // TODO: not currently using body pose feedback

		feet_vector_t feet_pos_desired =
			EvalPosTrajAtTime(
					feet_cmd_pos_traj_, seconds_in_mode_, traj_end_time_s_
					);

		feet_vector_t feet_vel_desired = 
			EvalVelTrajAtTime(
					feet_cmd_vel_traj_, seconds_in_mode_, traj_end_time_s_
					);

		feet_vector_t curr_feet_pos =
			robot_dynamics_.GetFeetPositions(q_with_standard_body_config);
		feet_vector_t feet_pos_error = feet_pos_desired - curr_feet_pos;
		
//		ROS_INFO_STREAM(
//				"feet_pos_error: " << std::setprecision(2) << std::fixed
//				<< feet_pos_error.transpose() << std::endl);

		feet_vector_t feet_vel_ff = feet_vel_desired;
//		ROS_INFO_STREAM(
//				"feet_vel_ff: " << std::setprecision(2) << std::fixed
//				<< feet_vel_ff.transpose() << std::endl << std::endl);

		Eigen::Matrix<double,kNumFeetCoords,kNumJoints> J_feet =
			robot_dynamics_
			.GetStackedFeetJacobianPos(q_)
			.block(0,kNumTwistCoords,kNumFeetCoords,kNumJoints);
			// Get rid of Jacobian for body
		Eigen::MatrixXd J_inv = CalcPseudoInverse(J_feet);
		
		// Feet position controller
		q_j_dot_cmd_ = J_inv
			* (k_pos_p_ * feet_pos_error + feet_vel_ff);

		q_j_dot_cmd_integrator_.Integrate(q_j_dot_cmd_);
		q_j_cmd_ = q_j_dot_cmd_integrator_.GetIntegral();
	}


	// ************* //
	// STATE MACHINE // 
	// ************* //

	void Controller::SetRobotMode(RobotMode target_mode)
	{
		controller_ready_ = false;
		switch(target_mode)
		{
			case kIdle:
				ROS_INFO("Setting mode to IDLE");
				SetJointInitialConfigTraj();
				control_mode_ = kJointTracking; 
				break;
			case kStandup:
				ROS_INFO("Setting mode to STANDUP");
				q_j_dot_cmd_integrator_.SetIntegral(q_j_);
				SetFeetStandupTraj();
				control_mode_ = kFeetTracking; 
				break;
			case kWalk:
				break;
			default:
				break;
		}
		SetModeStartTime();
		controller_ready_ = true;
	}

	// *** //
	// ROS //
	// *** //

	void Controller::SetupRosTopics()
	{
		ROS_INFO("Initialized controller node");

		// TODO: All of these constructions can be moved into their own functions, this is essentially just repeated code.

		// Set up advertisements
		ros::AdvertiseOptions q_j_cmd_ao =
			ros::AdvertiseOptions::create<std_msgs::Float64MultiArray>(
					"/q_j_cmd",
					1,
					ros::SubscriberStatusCallback(),
					ros::SubscriberStatusCallback(),
					ros::VoidPtr(),
					&this->ros_publish_queue_
					);

		ros::AdvertiseOptions q_j_dot_cmd_ao =
			ros::AdvertiseOptions::create<std_msgs::Float64MultiArray>(
					"/q_j_dot_cmd",
					1,
					ros::SubscriberStatusCallback(),
					ros::SubscriberStatusCallback(),
					ros::VoidPtr(),
					&this->ros_publish_queue_
					);

		q_j_cmd_pub_= ros_node_.advertise(q_j_cmd_ao);
		q_j_dot_cmd_pub_= ros_node_.advertise(q_j_dot_cmd_ao);

		// Set up subscriptions
		ros::SubscribeOptions gen_coord_so =
			ros::SubscribeOptions::create<std_msgs::Float64MultiArray>(
					"/" + model_name_ + "/gen_coord",
					1,
					boost::bind(&Controller::OnGenCoordMsg, this, _1),
					ros::VoidPtr(),
					&this->ros_process_queue_
					);

		ros::SubscribeOptions gen_vel_so =
			ros::SubscribeOptions::create<std_msgs::Float64MultiArray>(
					"/" + model_name_ + "/gen_vel",
					1,
					boost::bind(&Controller::OnGenVelMsg, this, _1),
					ros::VoidPtr(),
					&this->ros_process_queue_
					);

		gen_coord_sub_ = ros_node_.subscribe(gen_coord_so);
		gen_vel_sub_ = ros_node_.subscribe(gen_vel_so);

		ROS_INFO("Finished setting up ROS topics");
	}

	void Controller::SetupRosServices()
	{
		ros::AdvertiseServiceOptions cmd_standup_aso =
			ros::AdvertiseServiceOptions::create<std_srvs::Empty>(
            "/" + model_name_ + "/standup",
            boost::bind(&Controller::CmdStandupService, this, _1, _2),
            ros::VoidPtr(),
            &this->ros_process_queue_
        );

		cmd_standup_service_ = ros_node_.advertiseService(cmd_standup_aso);
	}

	bool Controller::CmdStandupService(
					const std_srvs::Empty::Request &_req,
					std_srvs::Empty::Response &_res
			)
	{
		SetRobotMode(kStandup);
		return true;
	}

	void Controller::SpinRosThreads()
	{
		ros_process_queue_thread_ = std::thread(
				std::bind(&Controller::ProcessQueueThread, this)
				);
		ros_publish_queue_thread_ = std::thread(
				std::bind(&Controller::PublishQueueThread, this)
				);
	}

	void Controller::ProcessQueueThread()
	{
		static const double timeout = 0.01; // TODO: I should check this number
		while (ros_node_.ok())
		{
			ros_process_queue_.callAvailable(
					ros::WallDuration(timeout)
					);
		}
	}

	void Controller::PublishQueueThread()
	{
		while (ros_node_.ok())
		{
			if (!controller_ready_) continue;

			UpdateJointCommand();
			PublishJointPosCmd();
			PublishJointVelCmd();
			loop_rate_.sleep();	
		}
	}

	void Controller::PublishJointPosCmd()
	{
		std_msgs::Float64MultiArray q_j_cmd_msg;
		tf::matrixEigenToMsg(q_j_cmd_, q_j_cmd_msg);
		q_j_cmd_pub_.publish(q_j_cmd_msg);
	}
	
	void Controller::PublishJointVelCmd()
	{
		std_msgs::Float64MultiArray q_j_dot_cmd_msg;
		tf::matrixEigenToMsg(q_j_dot_cmd_, q_j_dot_cmd_msg);
		q_j_dot_cmd_pub_.publish(q_j_dot_cmd_msg);
	}

	void Controller::OnGenCoordMsg(
			const std_msgs::Float64MultiArrayConstPtr &msg
			)
	{
		received_first_state_msg_ = true;
		SetGenCoords(msg->data);
	}

	void Controller::OnGenVelMsg(
			const std_msgs::Float64MultiArrayConstPtr &msg
			)
	{
		received_first_state_msg_ = true;
		SetGenVels(msg->data);
	}


	// ***************** //
	// SETTERS & GETTERS //
	// ***************** //

	joint_vector_t Controller::GetJointsPos()
	{
		joint_vector_t q_j = 
			q_.block<kNumJoints,1>(kNumPoseCoords,0);
		return q_j;
	}

	joint_vector_t Controller::GetJointsVel()
	{
		joint_vector_t q_j_dot = 
			u_.block<kNumJoints,1>(kNumTwistCoords,0);
		return q_j_dot;
	}

	void Controller::SetGenCoords(const std::vector<double> &gen_coords)
	{
		for (int i = 0; i < kNumGenCoords; ++i)
			q_(i) = gen_coords[i];

		q_j_ = GetJointsPos();
	}

	void Controller::SetGenVels(const std::vector<double> &gen_vels)
	{
		for (int i = 0; i < 18; ++i)
			u_(i) = gen_vels[i];

		q_j_dot_ = GetJointsPos();
	}

	// **************** //
	// HELPER FUNCTIONS //
	// **************** //

	Eigen::MatrixXd Controller::EvalPosTrajAtTime(
			drake::trajectories::PiecewisePolynomial<double> traj,
			double curr_time,
			double end_time)
	{
		if (curr_time > end_time)
		{
			return traj.value(end_time);
		}
		return traj.value(curr_time);
	}

	Eigen::MatrixXd Controller::EvalVelTrajAtTime(
			drake::trajectories::PiecewisePolynomial<double> traj,
			double curr_time,
			double end_time)
	{
		if (curr_time > end_time)
		{
			return Eigen::MatrixXd::Zero(traj.rows(), traj.cols());
		}
		return traj.value(curr_time);
	}

	void Controller::WaitForPublishedTime()
	{
		while (ros::Time::now().toSec() == 0.0); 
		ROS_INFO("Received published time");
	}

	void Controller::WaitForPublishedState()
	{
		while (!received_first_state_msg_);
		ROS_INFO("Received published state");
	}

	drake::trajectories::PiecewisePolynomial<double>
		Controller::CreateFirstOrderHoldTraj(
			std::vector<double> breaks,
			std::vector<Eigen::MatrixXd> samples
			)
	{
		drake::trajectories::PiecewisePolynomial traj
			= drake::trajectories::PiecewisePolynomial<double>
					::FirstOrderHold(breaks, samples);
		return traj;
	}

	void Controller::SetVariablesToZero()
	{
		q_.setZero(); // NOTE: sets quaternion to zero too!
		u_.setZero();

		q_j_.resize(kNumJoints, 1);
		q_j_.setZero();
		q_j_dot_.resize(kNumJoints, 1);
		q_j_dot_.setZero();

		q_j_cmd_.setZero();
		q_j_dot_cmd_.setZero();

		q_j_dot_cmd_integrator_ = Integrator(kNumJoints);
		q_j_dot_cmd_integrator_.Reset();
	}
	
	void Controller::SetModeStartTime()
	{
		mode_start_time_ = ros::Time::now();
	}

	double Controller::GetElapsedTimeSince(ros::Time t)
	{
		double elapsed_time = (ros::Time::now() - t).toSec();
		return elapsed_time; 
	}

	Eigen::MatrixXd Controller::CalcPseudoInverse(Eigen::MatrixXd A)
	{
		// Moore-Penrose right inverse: A^t (A A^t)
		Eigen:: MatrixXd pseudo_inverse =
			A.transpose() * (A * A.transpose()).inverse();
		return pseudo_inverse;
	}

	Eigen::MatrixXd Controller::CalcPseudoInverse(
			Eigen::MatrixXd A, double damping
			)
	{
		// TODO: Test condition number!
		// Same size as A*A^t
		Eigen::MatrixXd eye =
			Eigen::MatrixXd::Identity(A.rows(), A.rows());

		// Moore-Penrose right inverse: A^t (A A^t)
		Eigen:: MatrixXd pseudo_inverse =
			A.transpose() * (A * A.transpose() + damping * eye).inverse();
		return pseudo_inverse;
	}
}
