#include <ros/ros.h>
#include <ros/publisher.h>
#include <ros/subscriber.h>

#include <rtabmap_ros/MapData.h>
#include <rtabmap_ros/Info.h>
#include <rtabmap_ros/MsgConversion.h>
#include <rtabmap_ros/AddLink.h>
#include <rtabmap_ros/GetMap.h>

#include <rtabmap/core/Rtabmap.h>
#include <rtabmap/core/Memory.h>
#include <rtabmap/core/VWDictionary.h>
#include <rtabmap/core/util3d.h>
#include <rtabmap/core/RegistrationVis.h>
#include <rtabmap/utilite/UStl.h>

#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/exact_time.h>

#include <external_loop_closure_detection/DetectLoopClosure.h>
#include <cv_bridge/cv_bridge.h>

/*
 * Test:
 * $ roslaunch rtabmap_ros demo_robot_mapping.launch
 * Disable internal loop closure detection, in rtabmapviz->Preferences:
 *    ->Vocabulary, set Max words to -1 (loop closure detection disabled)
 *    ->Proximity Detection, uncheck proximity detection by space
 * $ rosrun rtabmap_ros external_loop_detection_example
 * $ rosbag play --clock demo_mapping.bag
 */

typedef message_filters::sync_policies::ExactTime<rtabmap_ros::MapData, rtabmap_ros::Info> MyInfoMapSyncPolicy;

ros::ServiceClient addLinkSrv;
//ros::ServiceClient getMapSrv;

// This is used to keep in cache the old data of the map
std::map<int, rtabmap::SensorData> localData;

// Use an external service for loop closure detection
class ExternalLoopClosureService
{
	public:
	ExternalLoopClosureService(){};

	~ExternalLoopClosureService(){};

	void init(ros::NodeHandle& nh)
	{
		client_ = nh.serviceClient<external_loop_closure_detection::DetectLoopClosure>("detect_loop_closure");
	}

	bool process(const rtabmap::SensorData& data, const int id)
	{
		ROS_DEBUG("Process Image %d for Loop Closure Detection", id);
		// Image message
		std_msgs::Header header;
		header.seq = id;
		header.stamp = ros::Time::now();
		cv_bridge::CvImage image_bridge = cv_bridge::CvImage(header, sensor_msgs::image_encodings::RGB8, data.imageRaw());
		sensor_msgs::Image image_msg;
		image_bridge.toImageMsg(image_msg);

		// Service request
		external_loop_closure_detection::DetectLoopClosure srv;
		srv.request.image = image_msg;

		if (client_.call(srv))
		{
			loop_closure_id_ = srv.response.detected_loop_closure_id;
			ROS_DEBUG("Loop Closure Detection service success: %d", ((int) srv.response.is_detected));
			return srv.response.is_detected;
		}
		else
		{
			ROS_ERROR("Failed to call loop closure detection service");
			loop_closure_id_ = 0;
			return false;
		}
	}

	int getLoopClosureId()
	{
		return loop_closure_id_;
	}

	private:
	ros::ServiceClient client_;
	int loop_closure_id_ = 0;

};

ExternalLoopClosureService loopClosureDetector;

rtabmap::RegistrationVis reg;

//bool g_localizationMode = false;

void mapDataCallback(const rtabmap_ros::MapDataConstPtr & mapDataMsg, const rtabmap_ros::InfoConstPtr & infoMsg)
{
	ROS_DEBUG("Received map data!");

	rtabmap::Statistics stats;
	rtabmap_ros::infoFromROS(*infoMsg, stats);

	bool smallMovement = (bool)uValue(stats.data(), rtabmap::Statistics::kMemorySmall_movement(), 0.0f);
	bool fastMovement = (bool)uValue(stats.data(), rtabmap::Statistics::kMemoryFast_movement(), 0.0f);

	if(smallMovement || fastMovement)
	{
		// The signature has been ignored from rtabmap, don't process it
		ROS_DEBUG("Ignore keyframe. Small movement=%d, Fast movement=%d", (int)smallMovement, (int)fastMovement);
		return;
	}

	rtabmap::Transform mapToOdom;
	std::map<int, rtabmap::Transform> poses;
	std::multimap<int, rtabmap::Link> links;
	std::map<int, rtabmap::Signature> signatures;
	rtabmap_ros::mapDataFromROS(*mapDataMsg, poses, links, signatures, mapToOdom);

	if(!signatures.empty() &&
		signatures.rbegin()->second.sensorData().isValid() &&
		localData.find(signatures.rbegin()->first) == localData.end())
	{
		int id = signatures.rbegin()->first;
		const rtabmap::SensorData & s =  signatures.rbegin()->second.sensorData();
		cv::Mat rgb;
		//rtabmap::LaserScan scan;
		s.uncompressDataConst(&rgb, 0/*, &scan*/);
		//pcl::PointCloud<pcl::PointXYZ>::Ptr cloud = rtabmap::util3d::laserScanToPointCloud(scan, scan.localTransform());

		if(loopClosureDetector.process(rgb, id))
		{
			if(loopClosureDetector.getLoopClosureId()>0)
			{
				int fromId = id;
				int toId = loopClosureDetector.getLoopClosureId();
				ROS_DEBUG("Detected loop closure between %d and %d", fromId, toId);
				if(localData.find(toId) != localData.end())
				{
					//Compute transformation
					rtabmap::RegistrationInfo regInfo;
					rtabmap::SensorData tmpFrom = s;
					rtabmap::SensorData tmpTo = localData.at(toId);
					tmpFrom.uncompressData();
					tmpTo.uncompressData();
					rtabmap::Transform t = reg.computeTransformation(tmpFrom, tmpTo, rtabmap::Transform(), &regInfo);

					if(!t.isNull())
					{
						rtabmap::Link link(fromId, toId, rtabmap::Link::kUserClosure, t, regInfo.covariance.inv());
						rtabmap_ros::AddLinkRequest req;
						rtabmap_ros::linkToROS(link, req.link);
						rtabmap_ros::AddLinkResponse res;
						if(!addLinkSrv.call(req, res))
						{
							ROS_ERROR("Failed to call %s service", addLinkSrv.getService().c_str());
						}
					}
					else
					{
						ROS_WARN("Could not compute transformation between %d and %d: %s", fromId, toId, regInfo.rejectedMsg.c_str());
					}
				}
				else
				{
					ROS_WARN("Could not compute transformation between %d and %d because node data %d is not in cache.", fromId, toId, toId);
				}
			}
		}

		localData.insert(std::make_pair(id, s));
	}
}

int main(int argc, char** argv)
{

	
	ros::init(argc, argv, "external_loop_detection");

	ULogger::setType(ULogger::kTypeConsole);
	ULogger::setLevel(ULogger::kWarning);

	ros::NodeHandle nh;
	ros::NodeHandle pnh("~");

	loopClosureDetector.init(nh);

	// Registration params
	int min_inliers = 20;
	if (ros::param::has("~min_inliers")) {
		ros::param::get("~min_inliers", min_inliers);
	}
	rtabmap::ParametersMap params;
	params.insert(rtabmap::ParametersPair(rtabmap::Parameters::kVisMinInliers(), std::to_string(min_inliers)));
	reg.parseParameters(params);

	ros::Rate rate(1);

	// service to add link
	addLinkSrv = nh.serviceClient<rtabmap_ros::AddLink>("/rtabmap/add_link");

	// subscription
	message_filters::Subscriber<rtabmap_ros::Info> infoTopic;
	message_filters::Subscriber<rtabmap_ros::MapData> mapDataTopic;
	infoTopic.subscribe(nh, "/rtabmap/info", 1);
	mapDataTopic.subscribe(nh, "/rtabmap/mapData", 1);
	message_filters::Synchronizer<MyInfoMapSyncPolicy> infoMapSync(
			MyInfoMapSyncPolicy(10),
			mapDataTopic,
			infoTopic);
	infoMapSync.registerCallback(&mapDataCallback);
	ROS_INFO("Subscribed to %s and %s", mapDataTopic.getTopic().c_str(), infoTopic.getTopic().c_str());

	ros::spin();

	return 0;
}
