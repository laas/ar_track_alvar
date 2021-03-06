/*
  Software License Agreement (BSD License)

  Copyright (c) 2012, Scott Niekum
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.
  * Neither the name of the Willow Garage nor the names of its
  contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

  author: Scott Niekum
*/


#include "ar_track_alvar/CvTestbed.h"
#include "ar_track_alvar/MarkerDetector.h"
#include "ar_track_alvar/MultiMarkerBundle.h"
#include "ar_track_alvar/MultiMarkerInitializer.h"
#include "ar_track_alvar/Shared.h"
#include <cv_bridge/cv_bridge.h>
#include <tf/transform_listener.h>
#include <tf/transform_broadcaster.h>
#include <sensor_msgs/image_encodings.h>
#include <map>
#include <string>
#include <stdlib.h>
#include <dynamic_reconfigure/server.h>
#include <ar_track_alvar/ParamsConfig.h>
#include <std_msgs/Bool.h>
#include "ar_track_alvar/GetPositionAndOrientation.h"
#include "ar_track_alvar/SetCamTopic.h"
#include "ar_track_alvar/standard_service.h"

using namespace alvar;
using namespace std;

#define MAIN_MARKER 1
#define VISIBLE_MARKER 2
#define GHOST_MARKER 3

Camera *cam;
cv_bridge::CvImagePtr cv_ptr_;
image_transport::Subscriber cam_sub_;
ros::Publisher rvizMarkerPub_;
tf::TransformListener *tf_listener;
tf::TransformBroadcaster *tf_broadcaster;
MarkerDetector<MarkerData> marker_detector;
MultiMarkerBundle **multi_marker_bundles=NULL;
Pose *bundlePoses;
int *master_id;
bool *bundles_seen;
std::vector<int> *bundle_indices;
bool init = true;

bool enabled = false;
double max_frequency;
double marker_size;
double max_new_marker_error;
double max_track_error;
int display_unknown_objects;
std::string cam_image_topic;
std::string cam_info_topic;
std::string output_frame;
int n_bundles = 0;
std::map<int, std::string> config;

bool use_ref = false;
ros::ServiceClient ref_client;

bool FindMarker(ar_track_alvar::GetPositionAndOrientation::Request  &req, ar_track_alvar::GetPositionAndOrientation::Response &res);
void configCallback(ar_track_alvar::ParamsConfig &config, uint32_t level);
void enableCallback(const std_msgs::BoolConstPtr& msg);
void ReadConfig (std::map<int,std::string> & config, int* masters_id, int nb_bundles);
void GetMultiMarkerPoses(IplImage *image);
void getCapCallback (const sensor_msgs::ImageConstPtr & image_msg);
void makeMarkerMsgs(int type, int id, Pose &p, sensor_msgs::ImageConstPtr image_msg, tf::StampedTransform &CamToOutput, visualization_msgs::Marker *rvizMarker);

// Updates the bundlePoses of the multi_marker_bundles by detecting markers and using all markers in a bundle to infer the master tag's position
void GetMultiMarkerPoses(IplImage *image) {

  if (marker_detector.Detect(image, cam, true, false, max_new_marker_error, max_track_error, CVSEQ, true)){
    for(int i=0; i<n_bundles; i++)
    {
      marker_detector.SetMarkerSize(multi_marker_bundles[i]->getTagSize());
      multi_marker_bundles[i]->Update(marker_detector.markers, cam, bundlePoses[i]);
    }

    if(marker_detector.DetectAdditional(image, cam, false) > 0){
      for(int i=0; i<n_bundles; i++)
      {
        marker_detector.SetMarkerSize(multi_marker_bundles[i]->getTagSize());
	if ((multi_marker_bundles[i]->SetTrackMarkers(marker_detector, cam, bundlePoses[i], image) > 0))
	  multi_marker_bundles[i]->Update(marker_detector.markers, cam, bundlePoses[i]);
      }
    }
  }
}


// Given the pose of a marker, builds the appropriate ROS messages for later publishing
void makeMarkerMsgs(int type, int id, Pose &p, sensor_msgs::ImageConstPtr image_msg, tf::StampedTransform &CamToOutput, visualization_msgs::Marker *rvizMarker){
  double px,py,pz,qx,qy,qz,qw;

  px = p.translation[0]/100.0;
  py = p.translation[1]/100.0;
  pz = p.translation[2]/100.0;
  qx = p.quaternion[1];
  qy = p.quaternion[2];
  qz = p.quaternion[3];
  qw = p.quaternion[0];

  //Get the marker pose in the camera frame
  tf::Quaternion rotation (qx,qy,qz,qw);
  tf::Vector3 origin (px,py,pz);
  tf::Transform t (rotation, origin);  //transform from cam to marker

  tf::Vector3 markerOrigin (0, 0, 0);
  tf::Transform m (tf::Quaternion::getIdentity (), markerOrigin);
  tf::Transform markerPose = t * m;

  //Publish the cam to marker transform for main marker in each bundle
  if(type==MAIN_MARKER){
    std::string markerFrame = "ar_marker_";
    std::stringstream out;
    out << id;
    std::string id_string = out.str();
    markerFrame += id_string;
    tf::StampedTransform camToMarker (t, image_msg->header.stamp, image_msg->header.frame_id, markerFrame.c_str());
    tf_broadcaster->sendTransform(camToMarker);
  }

  //Create the rviz visualization message
  tf::Transform tagPoseOutput = CamToOutput * markerPose;
  tf::poseTFToMsg (tagPoseOutput, rvizMarker->pose);
  rvizMarker->header.frame_id = image_msg->header.frame_id;
  rvizMarker->header.stamp = image_msg->header.stamp;
  rvizMarker->id = id;

  rvizMarker->scale.x = 1.0 * marker_size/100.0;
  rvizMarker->scale.y = 1.0 * marker_size/100.0;
  rvizMarker->scale.z = 0.2 * marker_size/100.0;

  if(type==MAIN_MARKER)
  {
    if(!use_ref)
    {
      std::map<int,std::string>::iterator it;
      it = config.find(id);
      if (it != config.end())
        rvizMarker->ns = config[id];
      else
      {
        char buff[5];
        sprintf(buff,"%d",id);
        rvizMarker->ns = string("marker_" + string(buff));
      }
    }
    else
    {
      char buff[5];
      sprintf(buff,"%d",id);
      ar_track_alvar::standard_service srv;
      srv.request.action = "get_ref";
      srv.request.param = string(buff);
      if (ref_client.call(srv) && (srv.response.code == 0))
        rvizMarker->ns = srv.response.value;
      else
        rvizMarker->ns = string("marker_" + string(buff));
    }
  }
  else
    rvizMarker->ns = "unknown object";


  rvizMarker->type = visualization_msgs::Marker::CUBE;
  rvizMarker->action = visualization_msgs::Marker::ADD;

  //Determine a color and opacity, based on marker type
  if(type==MAIN_MARKER){
    rvizMarker->color.r = 1.0f;
    rvizMarker->color.g = 0.0f;
    rvizMarker->color.b = 0.0f;
    rvizMarker->color.a = 1.0;
  }
  else if(type==VISIBLE_MARKER){
    rvizMarker->color.r = 0.0f;
    rvizMarker->color.g = 1.0f;
    rvizMarker->color.b = 0.0f;
    rvizMarker->color.a = 0.7;
  }
  else if(type==GHOST_MARKER){
    rvizMarker->color.r = 0.0f;
    rvizMarker->color.g = 0.0f;
    rvizMarker->color.b = 1.0f;
    rvizMarker->color.a = 0.5;
  }

  rvizMarker->lifetime = ros::Duration (1.0);
}


//Callback to handle getting video frames and processing them
void getCapCallback (const sensor_msgs::ImageConstPtr & image_msg)
{
  cv_ptr_ = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);

bool allow_pub = true;

if (enabled) {
  //If we've already gotten the cam info, then go ahead
  if(cam->getCamInfo_){
    try{
      //Get the transformation from the Camera to the output frame for this image capture
      tf::StampedTransform CamToOutput;
      try{
      	tf_listener->waitForTransform(output_frame, image_msg->header.frame_id, image_msg->header.stamp, ros::Duration(1.0));
      	tf_listener->lookupTransform(output_frame, image_msg->header.frame_id, image_msg->header.stamp, CamToOutput);
      }
      catch (tf::TransformException ex){
	        ROS_ERROR("%s",ex.what());
          allow_pub = false;
      }

      visualization_msgs::Marker rvizMarker;

      //Convert the image
      cv_ptr_ = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);
      //Get the estimated pose of the main markers by using all the markers in each bundle

      // GetMultiMarkersPoses expects an IplImage*, but as of ros groovy, cv_bridge gives
      // us a cv::Mat. I'm too lazy to change to cv::Mat throughout right now, so I
      // do this conversion here -jbinney
      IplImage ipl_image = cv_ptr_->image;
      GetMultiMarkerPoses(&ipl_image);

      //Draw the observed markers that are visible and note which bundles have at least 1 marker seen
      for(int i=0; i<n_bundles; i++)
        bundles_seen[i] = false;

      for (size_t i=0; i<marker_detector.markers->size(); i++)
    	{
    	  int id = (*(marker_detector.markers))[i].GetId();

    	  // Draw if id is valid
    	  if(id >= 0)
        {
          //Mark the bundle that marker belongs to as "seen"
          for(int j=0; j<n_bundles; j++){
            for(int k=0; k<bundle_indices[j].size(); k++){
              if(bundle_indices[j][k] == id){
                bundles_seen[j] = true;
                break;
              }
            }
          }

          // Don't draw if it is a master tag...we do this later, a bit differently
          bool should_draw = true;
          for(int k=0; k<n_bundles; k++)
            if(id == master_id[k]) should_draw = false;

          if(should_draw)
          {
            Pose p = (*(marker_detector.markers))[i].pose;
            if(display_unknown_objects==1)
              makeMarkerMsgs(VISIBLE_MARKER, id, p, image_msg, CamToOutput, &rvizMarker);

            if(rvizMarker.header.frame_id != "" && allow_pub)
              rvizMarkerPub_.publish (rvizMarker);
          }
    	  }
    	}

      //Draw the main markers, whether they are visible or not -- but only if at least 1 marker from their bundle is currently seen
      for(int i=0; i<n_bundles; i++)
    	{
    	  if(bundles_seen[i] == true && allow_pub){
    	    makeMarkerMsgs(MAIN_MARKER, master_id[i], bundlePoses[i], image_msg, CamToOutput, &rvizMarker);
    	    rvizMarkerPub_.publish (rvizMarker);
    	  }
    	}

    }
    catch (cv_bridge::Exception& e){
      ROS_ERROR ("Could not convert from '%s' to 'rgb8'.", image_msg->encoding.c_str ());
    }
  }
  }
}

bool ChangeCam(ar_track_alvar::SetCamTopic::Request  &req, ar_track_alvar::SetCamTopic::Response &res, image_transport::ImageTransport &it_, ros::NodeHandle &n)
{
    ROS_INFO ("Subscribing to new image topic");
    delete cam;
    cam = new Camera(n, req.cam_info_topic);

    cam_sub_.shutdown();
    cam_sub_=it_.subscribe(req.cam_topic, 1, &getCapCallback);

    res.result = true;
    return true;
}

bool FindMarker(ar_track_alvar::GetPositionAndOrientation::Request  &req, ar_track_alvar::GetPositionAndOrientation::Response &res)
{
    sensor_msgs::ImagePtr image_msg =cv_ptr_->toImageMsg();

    bool allow_pub = true;

    //If we've already gotten the cam info, then go ahead
        if(cam->getCamInfo_)
        {
            try
            {
                //Get the transformation from the Camera to the output frame for this image capture
                tf::StampedTransform CamToOutput;
                try
                {
                    tf_listener->waitForTransform(output_frame, image_msg->header.frame_id, image_msg->header.stamp, ros::Duration(1.0));
                    tf_listener->lookupTransform(output_frame, image_msg->header.frame_id, image_msg->header.stamp, CamToOutput);
                }
                catch (tf::TransformException ex)
                {
                    ROS_ERROR("%s",ex.what());
                    allow_pub = false;
                }
                visualization_msgs::Marker rvizMarker;

                //Convert the image
                //cv_ptr_ = cv_bridge::toCvCopy(image_msg, sensor_msgs::image_encodings::BGR8);
                //Get the estimated pose of the main markers by using all the markers in each bundle

                // GetMultiMarkersPoses expects an IplImage*, but as of ros groovy, cv_bridge gives
                // us a cv::Mat. I'm too lazy to change to cv::Mat throughout right now, so I
                // do this conversion here -jbinney
                IplImage ipl_image = cv_ptr_->image;
                GetMultiMarkerPoses(&ipl_image);
                //Draw the observed markers that are visible and note which bundles have at least 1 marker seen
                for(int i=0; i<n_bundles; i++)
                    bundles_seen[i] = false;

                for (size_t i=0; i<marker_detector.markers->size(); i++)
                {
                    int id = (*(marker_detector.markers))[i].GetId();

                    // Draw if id is valid
                    if(id >= 0)
                    {

                        //Mark the bundle that marker belongs to as "seen"
                        for(int j=0; j<n_bundles; j++)
                        {
                            for(int k=0; k<bundle_indices[j].size(); k++)
                            {
                                if(bundle_indices[j][k] == id)
                                {
                                    bundles_seen[j] = true;
                                    break;
                                }
                            }
                        }

                        // Don't draw if it is a master tag...we do this later, a bit differently
                        bool should_draw = true;
                        for(int k=0; k<n_bundles; k++)
                        {
                            if(id == master_id[k]) should_draw = false;
                        }
                        if(should_draw)
                        {
                            Pose p = (*(marker_detector.markers))[i].pose;
                            if(display_unknown_objects==1 && allow_pub)
                            {
                                makeMarkerMsgs(VISIBLE_MARKER, id, p, image_msg, CamToOutput, &rvizMarker);
                                rvizMarkerPub_.publish (rvizMarker);
                            }
                        }
                    }
                }
                //Draw the main markers, whether they are visible or not -- but only if at least 1 marker from their bundle is currently seen
                for(int i=0; i<n_bundles; i++)
                {
                    if(bundles_seen[i] == true && allow_pub)
                    {
                        makeMarkerMsgs(MAIN_MARKER, master_id[i], bundlePoses[i], image_msg, CamToOutput, &rvizMarker);
                        rvizMarkerPub_.publish (rvizMarker);
                        res.marker.push_back(rvizMarker);
                    }
                }

            }
            catch (cv_bridge::Exception& e)
            {
                //ROS_ERROR ("Could not convert from '%s' to 'rgb8'.", BuffImage.encoding.c_str ());
                ROS_ERROR ("Could not convert from '%s' to 'rgb8'.", image_msg->encoding.c_str ());

            }
        }
return true;
}


//Load the configuration file (id <-> object)
void ReadConfig (std::map<int,std::string> & config, int* masters_id, int nb_bundles)
{
    FILE * pFile;
    char buffer [100];
    int i=0;
    char name [30];
    std::string path = ros::package::getPath("ar_track_alvar_bundles");
    path+="/bundles/Map_ID_Name.txt";

    ROS_INFO ("Opening Config file Map_ID_Name.txt");

    if ((pFile = fopen (path.c_str(), "r")) == NULL)
    {
        ROS_INFO ("Can't find the file %s- quitting", path.c_str());
        ROS_BREAK ();
    }

    while ( ! feof (pFile) )
    {
        if ( fgets (buffer , 100 , pFile) == NULL ) break;
        if (sscanf (buffer, "%i	%s", &i, name) != 2)
        {
            fclose (pFile);
            ROS_BREAK ();
        }
        config[i]=(string) name;
    }
    fclose (pFile);

    if(config.size() != nb_bundles)
    {
      ROS_ERROR("bundles files configuration : not same number of bundles");
      ROS_BREAK ();
    }
    else
    {
      for(unsigned int i = 0; i < nb_bundles; i++)
      {
        std::cout << masters_id[i] << std::endl;
        std::map<int, std::string>::iterator it;
        it = config.find(masters_id[i]);
        if (it == config.end())
        {
          ROS_ERROR("bundles files configuration : masters id's error");
          ROS_BREAK ();
        }
      }
    }
}

void configCallback(ar_track_alvar::ParamsConfig &config, uint32_t level)
{
  ROS_INFO("AR tracker reconfigured: %s %.2f %.2f %.2f %.2f", config.enabled ? "ENABLED" : "DISABLED",
           config.max_frequency, config.marker_size, config.max_new_marker_error, config.max_track_error);

  //enableSwitched = enabled != config.enabled;  //Useless if we begin with enabled=0;
  //enabled = config.enabled;
  max_frequency = config.max_frequency;
  marker_size = config.marker_size;
  max_new_marker_error = config.max_new_marker_error;
  max_track_error = config.max_track_error;
}


void enableCallback(const std_msgs::BoolConstPtr& msg)
{
    enabled = msg->data;
}

int main(int argc, char *argv[])
{
  ros::init (argc, argv, "marker_detect");
  ros::NodeHandle n,  pn("~"), n2, n3, n4;

  if(argc < 11){
    std::cout << std::endl;
    cout << "Not enough arguments provided." << endl;
    cout << "Usage: ./findMarkerBundles <marker size in cm> <max new marker error> <max track error> <cam image topic> <cam info topic> <output frame> <max frequency> <display unknown objects> <list of bundle XML files...>" << endl;
    std::cout << std::endl;
    return 0;
  }

  // Get params from command line
  marker_size = atof(argv[1]);
  max_new_marker_error = atof(argv[2]);
  max_track_error = atof(argv[3]);
  cam_image_topic = argv[4];
  cam_info_topic = argv[5];
  output_frame = argv[6];
  max_frequency = atof(argv[7]);
  display_unknown_objects = atof(argv[8]);
  int n_args_before_list = 10;
  n_bundles = argc - n_args_before_list;
  cout << n_bundles << " bundles " << endl;

  // Set dynamically configurable parameters so they don't get replaced by default values
  pn.setParam("marker_size", marker_size);
  pn.setParam("max_new_marker_error", max_new_marker_error);
  pn.setParam("max_track_error", max_track_error);

  marker_detector.SetMarkerSize(marker_size);
  multi_marker_bundles = new MultiMarkerBundle*[n_bundles];
  bundlePoses = new Pose[n_bundles];
  master_id = new int[n_bundles];
  bundle_indices = new std::vector<int>[n_bundles];
  bundles_seen = new bool[n_bundles];

  // Load the marker bundle XML files
  for(int i=0; i<n_bundles; i++){
    bundlePoses[i].Reset();
    MultiMarker loadHelper;
    if(loadHelper.Load(argv[i + n_args_before_list], FILE_FORMAT_XML)){
      vector<int> id_vector = loadHelper.getIndices();
      multi_marker_bundles[i] = new MultiMarkerBundle(id_vector);
      multi_marker_bundles[i]->Load(argv[i + n_args_before_list], FILE_FORMAT_XML);
      master_id[i] = multi_marker_bundles[i]->getMasterId();
      bundle_indices[i] = multi_marker_bundles[i]->getIndices();
    }
    else{
      cout<<"Cannot load file "<< argv[i + n_args_before_list] << endl;
      return 0;
    }
  }

  //Load the configuration file (id <-> object)
  if(string(argv[9]) != "none")
  {
    cout << "dynamic mapping on " << argv[9] << endl;
    use_ref = true;
    ref_client = n4.serviceClient<ar_track_alvar::standard_service>(argv[9]);
  }
  else
  {
    cout << "static mapping" << endl;
    ReadConfig(config, master_id, n_bundles);
  }

  // Prepare dynamic reconfiguration
  dynamic_reconfigure::Server < ar_track_alvar::ParamsConfig > server;
  dynamic_reconfigure::Server<ar_track_alvar::ParamsConfig>::CallbackType f;

  f = boost::bind(&configCallback, _1, _2);
  server.setCallback(f);

  // Set up camera, listeners, and broadcasters
  cam = new Camera(n, cam_info_topic);
  tf_listener = new tf::TransformListener(n);
  tf_broadcaster = new tf::TransformBroadcaster();
  rvizMarkerPub_ = n.advertise < visualization_msgs::Marker > ("ar_visualization_marker", 0);

  //Give tf a chance to catch up before the camera callback starts asking for transforms
  ros::Duration(1.0).sleep();
  ros::spinOnce();

  //Subscribe to topics and set up callbacks
  ROS_INFO ("Subscribing to image topic");
  image_transport::ImageTransport it_(n);
  cam_sub_ = it_.subscribe(cam_image_topic, 1, &getCapCallback);


  // Run at the configured rate, discarding pointcloud msgs if necessary
  ros::Rate rate(max_frequency);

  /// Subscriber for enable-topic so that a user can turn off the detection if it is not used without
  /// having to use the reconfigure where he has to know all parameters
  ROS_INFO("Subscribing to enable_detection. Don't forget to publish on this topic if you want ar_track to publish the poses !");
  ros::Subscriber enable_sub_ = pn.subscribe("enable_detection", 1, &enableCallback);

      //Service for marker detection so that a user can turn the detection for a single picture
  ros::ServiceServer service = n2.advertiseService("GetPositionAndOrientation", FindMarker);
  ROS_INFO("Ready To Get Position And Orientation");

      //Service for changing the xam topic during runtime
  //ros::ServiceServer service2 = n3.advertiseService("SetCamTopic", ChangeCam);
  ros::ServiceServer service2 = n3.advertiseService<ar_track_alvar::SetCamTopic::Request, ar_track_alvar::SetCamTopic::Response>("SetCamTopic", boost::bind(ChangeCam, _1, _2, it_, n));
  ROS_INFO("Ready To Set Cam Topic");


  while (ros::ok())
  {
      ros::spinOnce();
      rate.sleep();

      if (std::abs((rate.expectedCycleTime() - ros::Duration(1.0 / max_frequency)).toSec()) > 0.001)
      {
          // Change rate dynamically; if must be above 0, as 0 will provoke a segfault on next spinOnce
          ROS_DEBUG("Changing frequency from %.2f to %.2f", 1.0 / rate.expectedCycleTime().toSec(), max_frequency);
          rate = ros::Rate(max_frequency);
      }

  }

  return 0;
}
