/*********************************************************************
 *
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2008, 2013, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage, Inc. nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 * Author: Eitan Marder-Eppstein
 *         David V. Lu!!
 *********************************************************************/
#ifndef VOXEL_COSTMAP_PLUGIN_H_
#define VOXEL_COSTMAP_PLUGIN_H_
#include <ros/ros.h>
#include <costmap_2d/layer.h>
#include <costmap_2d/layered_costmap.h>
#include <costmap_2d/observation_buffer.h>
#include <costmap_2d/VoxelGrid.h>
#include <nav_msgs/OccupancyGrid.h>
#include <sensor_msgs/LaserScan.h>
#include <laser_geometry/laser_geometry.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/point_cloud_conversion.h>
#include <tf/message_filter.h>
#include <message_filters/subscriber.h>
#include <dynamic_reconfigure/server.h>
#include <costmap_2d/VoxelPluginConfig.h>
#include <costmap_2d/obstacle_layer.h>
#include <voxel_grid/voxel_grid.h>
#include <vector>

namespace costmap_2d
{
  class ObstaclePoint {
  public:
    unsigned int index;
    double x; 
    double y; 

  ObstaclePoint(unsigned int index_, double x_, double y_):index(index_), x(x_), y(y_){}
  };

  //keeps track of the indices updated for each observation
  class CostMapList {
  public:
    int64_t obs_timestamp; 
    std::string topic;
    std::vector<ObstaclePoint> indices;
  };

  //keeps track of the last time each index location was updated 
  //we should move this out probably to a cpp file - lot of implemetation in the header

  class GridmapLocations {
  public:
    std::map<std::string, double *> last_utimes; 
    //we also need to keep track of the height of these guys 
    std::map<unsigned int, std::set<std::string> > height_map;
    std::map<std::string, unsigned int> topic_height; 
    int size;

  GridmapLocations(int size_=0):size(size_){
      assert(size>=0); 
    }

    ~GridmapLocations(){
      std::map<std::string, double *>::iterator it; 
      for(it = last_utimes.begin(); it != last_utimes.end(); it++){
        delete []it->second;
      }
    }

    void updateObstacleTime(const CostMapList &cm_list){
      double obs_ts = cm_list.obs_timestamp / 1.0e6;
      double *topic_utime = get_values(cm_list.topic);

      for(int j=0; j < cm_list.indices.size(); j++){
        topic_utime[cm_list.indices[j].index] = obs_ts;         
      }
    }

    void touch(double x, double y, double* min_x, double* min_y, double* max_x, double* max_y)
    {
      *min_x = std::min(x, *min_x);
      *min_y = std::min(y, *min_y);
      *max_x = std::max(x, *max_x);
      *max_y = std::max(y, *max_y);
    }

    std::vector<std::string> getOtherLayersAtHeight(std::string topic){
      std::vector<std::string> other_topics; 
      if(topic_height.find(topic) != topic_height.end()){
        unsigned int height =  topic_height.find(topic)->second; 

        std::map<unsigned int, std::set<std::string> >::iterator it = height_map.find(height); 
        if(it != height_map.end()){
          std::set<std::string>::iterator it_tp; 
          for(it_tp = it->second.begin(); it_tp != it->second.end(); it_tp++){
            if(topic.compare(*it_tp)){ //not the same topic
              other_topics.push_back(*it_tp);
            }
          }
        }
      }
      else{
        fprintf(stderr, "Error - topic height not found\n");
      }
      return other_topics;
    }

    std::vector<double *> getOtherValuesAtSameHeight(std::string topic){
      
      std::vector<std::string> other_topics = getOtherLayersAtHeight(topic);
      std::vector<double *> values;
      for(int i=0; i < other_topics.size(); i++){
        values.push_back(get_values(other_topics[i]));
      }
      return values;
    }
    
    void clearObstacleTime(const CostMapList &list, unsigned char* costmap_, 
                           double* min_x, double* min_y, 
                           double* max_x, double* max_y){
      double *topic_utime =  get_values(list.topic);
      double list_time_sec = list.obs_timestamp / 1.0e6;
      
      std::vector<double *> other_layer_values = getOtherValuesAtSameHeight(list.topic);
      if(other_layer_values.size() > 0){
        fprintf(stdout, "[%d] topics found at same height\n", (int) other_layer_values.size());
      }

      for(int j=0; j < list.indices.size(); j++){
	if(topic_utime[list.indices[j].index] == list_time_sec){ //we have to check if there are other layers at the same height 
          //and if so make sure to timeout only if the other is timedout
          //right now this clears everything (irrespective of the height) - this is prob bad if the sensors report different heights 
          bool clear = true;
          topic_utime[list.indices[j].index] = -1;
          if(other_layer_values.size() > 0){
            for(int i=0; i < other_layer_values.size(); i++){
              if(other_layer_values[i][list.indices[j].index] > list_time_sec){ //check if this has timed out on other layers at the same height
                clear = false; 
              }
            }
          }
          if(clear){
            costmap_[list.indices[j].index] = FREE_SPACE; 
            //increase the map update bounds 
            touch(list.indices[j].x, list.indices[j].y, min_x, min_y, max_x, max_y);
          }
	}
      }
    }

    void updateHeightMap(std::string topic, unsigned int height){
      if(topic_height.find(topic) == topic_height.end()){
        topic_height.insert(make_pair(topic, height));
        std::map<unsigned int, std::set<std::string> >::iterator it = height_map.find(height); 
        if(it != height_map.end()){
          std::set<std::string>::iterator it_tp = it->second.find(topic); 
          if(it_tp == it->second.end()){//we havent added it 
            it->second.insert(topic); 
            fprintf(stdout, "Adding Topic : %s - height : %d\n", topic.c_str(), height);
          }
        }
        else{
          fprintf(stdout, "Adding Topic : %s - height : %d\n", topic.c_str(), height);
          std::set<std::string> height_set; 
          height_set.insert(topic);
          height_map.insert(make_pair(height, height_set));
        }
      }
    }

    void addTopic(std::string topic){
      if(last_utimes.find(topic) == last_utimes.end()){
        double *utimes = new double[size];
        for(int i=0; i < size; i++){
          utimes[i] = -1;
        }
        last_utimes.insert(std::make_pair(topic, utimes));
        fprintf(stdout, "Adding Topic %s to location timeout map size : %d\n", topic.c_str(), (int) last_utimes.size());
      }
      else{
        fprintf(stdout, "Topic already present\n");
      }
    }

    void resize(int new_size){
      if(size != new_size){
	size = new_size; 
        std::map<std::string, double *>::iterator it; 
        for(it = last_utimes.begin(); it != last_utimes.end(); it++){
          delete []it->second;
          it->second = new double[size];
        }
      }
      reset();
    }
    
    double *get_values(std::string topic){
      if(last_utimes.find(topic) == last_utimes.end()){
        addTopic(topic); 
      }
      
      std::map<std::string, double *>::iterator it = last_utimes.find(topic); 
      assert(it != last_utimes.end());
      assert(it->second != NULL);
      return it->second; 
    }

    void reset(){
      std::map<std::string, double *>::iterator it; 
      for(it = last_utimes.begin(); it != last_utimes.end(); it++){
        for(int i=0; i < size; i++){
          it->second[i] = -1;
        }
      }
    }
  };

  class ObservationSet{
  public:
  ObservationSet(std::string topic_):topic(topic_){
    }
    std::vector<Observation> marking_observations; 
    std::string topic; 
  };

class VoxelLayer : public ObstacleLayer
{
public:
  VoxelLayer() :
      voxel_grid_(0, 0, 0)
  {
    costmap_ = NULL; // this is the unsigned char* member of parent class's parent class Costmap2D.
  }

  virtual ~VoxelLayer();

  virtual void onInitialize();
  virtual void updateBounds(double robot_x, double robot_y, double robot_yaw, double* min_x, double* min_y, double* max_x,
                             double* max_y);

  void updateOrigin(double new_origin_x, double new_origin_y);
  bool isDiscretized()
  {
    return true;
  }
  virtual void matchSize();
  virtual void reset();

protected:
  /**
   * @brief  Get the observations used to mark space
   * @param marking_observations A reference to a vector that will be populated with the observations
   * @return True if all the observation buffers are current, false otherwise
   */
  bool getMarkingObservations(std::vector<costmap_2d::ObservationSet>& marking_observations) const;

  /**
   * @brief  Get the observations used to clear space
   * @param clearing_observations A reference to a vector that will be populated with the observations
   * @return True if all the observation buffers are current, false otherwise
   */
  bool getClearingObservations(std::vector<costmap_2d::Observation>& clearing_observations) const;


  virtual void setupDynamicReconfigure(ros::NodeHandle& nh);

private:
  void resetOldCosts(double* min_x, double* min_y, 
		     double* max_x, double* max_y); 

  void reconfigureCB(costmap_2d::VoxelPluginConfig &config, uint32_t level);
  void clearNonLethal(double wx, double wy, double w_size_x, double w_size_y, bool clear_no_info);
  virtual void raytraceFreespace(const costmap_2d::Observation& clearing_observation, double* min_x, double* min_y,
                                 double* max_x, double* max_y);

  dynamic_reconfigure::Server<costmap_2d::VoxelPluginConfig> *dsrv_;

  std::vector<CostMapList> new_obs_list; 
  //this needs to be maintained for each topic - since they can have different timeouts 
  GridmapLocations locations_utime;

  double inflation_radius_;
  bool publish_voxel_;
  ros::Publisher voxel_pub_;
  voxel_grid::VoxelGrid voxel_grid_;
  double z_resolution_, origin_z_;
  unsigned int unknown_threshold_, mark_threshold_, size_z_;
  ros::Publisher clearing_endpoints_pub_;
  sensor_msgs::PointCloud clearing_endpoints_;

  inline bool worldToMap3DFloat(double wx, double wy, double wz, double& mx, double& my, double& mz)
  {
    if (wx < origin_x_ || wy < origin_y_ || wz < origin_z_)
      return false;
    mx = ((wx - origin_x_) / resolution_);
    my = ((wy - origin_y_) / resolution_);
    mz = ((wz - origin_z_) / z_resolution_);
    if (mx < size_x_ && my < size_y_ && mz < size_z_)
      return true;

    return false;
  }

  inline bool worldToMap3D(double wx, double wy, double wz, unsigned int& mx, unsigned int& my, unsigned int& mz)
  {
    if (wx < origin_x_ || wy < origin_y_ || wz < origin_z_)
      return false;

    mx = (int)((wx - origin_x_) / resolution_);
    my = (int)((wy - origin_y_) / resolution_);
    mz = (int)((wz - origin_z_) / z_resolution_);

    if (mx < size_x_ && my < size_y_ && mz < size_z_)
      return true;

    return false;
  }

  inline void mapToWorld3D(unsigned int mx, unsigned int my, unsigned int mz, double& wx, double& wy, double& wz)
  {
    //returns the center point of the cell
    wx = origin_x_ + (mx + 0.5) * resolution_;
    wy = origin_y_ + (my + 0.5) * resolution_;
    wz = origin_z_ + (mz + 0.5) * z_resolution_;
  }

  inline double dist(double x0, double y0, double z0, double x1, double y1, double z1)
  {
    return sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
  }

};
}
#endif

