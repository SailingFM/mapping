/*
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2009, Willow Garage, Inc.
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
 */

/**
  * \author Romain Thibaux, Radu Bogdan Rusu
  * \author modified: Dejan Pangercic
  *
  * @b ptu_calibrate attempts to estimate pan-tilt calibration values as ROS
  * parameters based on point cloud planar segmentation.
  */
// ROS core
#include <ros/ros.h>
// Messages
#include <visualization_msgs/Marker.h>
#include <sensor_msgs/PointCloud2.h>
// PCL stuff
#include <pcl/point_types.h>
#include <pcl/point_cloud.h>
#include <pcl/ros/conversions.h>
#include "pcl/sample_consensus/method_types.h"
#include "pcl/sample_consensus/model_types.h"
#include <pcl/segmentation/sac_segmentation.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/passthrough.h>
#include <pcl/filters/project_inliers.h>
#include <pcl/surface/convex_hull.h>
#include "pcl/filters/extract_indices.h"
#include "pcl/segmentation/extract_polygonal_prism_data.h"
#include "pcl/common/common.h"
#include <pcl/io/pcd_io.h>
#include "pcl/segmentation/extract_clusters.h"
#include <pcl/features/normal_3d.h>
#include <pcl/common/angles.h>

#include <pcl_ros/publisher.h>

#include <tf/transform_broadcaster.h>
#include <tf/transform_listener.h>
#include <tf/message_filter.h>


typedef pcl::PointXYZRGB Point;
typedef pcl::PointCloud<Point> PointCloud;
typedef PointCloud::Ptr PointCloudPtr;
typedef PointCloud::ConstPtr PointCloudConstPtr;
typedef pcl::KdTree<Point>::Ptr KdTreePtr;

// TODO: in the future we should auto-detect the wall, or detect the location of the only
//       moving object, the table
// Equation of a boundary between the table and the wall, in base_link frame
// 'wp' stands for 'wall protection'
// Points on the plane satisfy wp_normal.dot(x) + wp_offset == 0
const tf::Vector3 wp_normal(1, 0, 0);
const double wp_offset = -1.45;

// Waits for a point cloud with pan-tilt close to (0,0) and deduces the position of ptu_base
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
class ExtractClusters 
{
public:
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ExtractClusters (const ros::NodeHandle &nh) : nh_ (nh)
  {
    nh_.param("sac_distance", sac_distance_, 0.03);
    nh_.param("z_min_limit", z_min_limit_, 0.0);
    nh_.param("z_max_limit", z_max_limit_, 1.5);
    nh_.param("max_iter", max_iter_, 500);
    nh_.param("normal_distance_weight", normal_distance_weight_, 0.1);
    nh_.param("eps_angle", eps_angle_, 15.0);
    nh_.param("seg_prob", seg_prob_, 0.99);
    nh_.param("normal_search_radius", normal_search_radius_, 0.05);
    //what area size of the table are we looking for?
    nh_.param("rot_table_frame", rot_table_frame_, std::string("rotating_table"));
    nh_.param("object_cluster_tolerance", object_cluster_tolerance_, 0.03);
    //min 100 points
    nh_.param("object_cluster_min_size", object_cluster_min_size_, 100);
    nh_.param("k", k_, 10);
    nh_.param("base_link_head_tilt_link_angle", base_link_head_tilt_link_angle_, 0.8);
    nh_.param("min_table_inliers", min_table_inliers_, 100);
    nh_.param("cluster_min_height", cluster_min_height_, 0.01);
    nh_.param("cluster_max_height", cluster_max_height_, 0.4);
    nh_.param("nr_cluster", nr_cluster_, 4);
    nh_.param("downsample", downsample_, true);
    nh_.param("voxel_size", voxel_size_, 0.01);
    nh_.param("save_to_files", save_to_files_, false);

    cloud_pub_.advertise (nh_, "table_inliers", 1);
    cloud_extracted_pub_.advertise (nh_, "cloud_extracted", 1);
    cloud_objects_pub_.advertise (nh_, "cloud_objects", 10);

    vgrid_.setFilterFieldName ("z");
    vgrid_.setFilterLimits (z_min_limit_, z_max_limit_);
    //if (downsample_)
      //vgrid_.setLeafSize (0.015, 0.015, 0.015);

    seg_.setDistanceThreshold (sac_distance_);
    seg_.setMaxIterations (max_iter_);
    seg_.setNormalDistanceWeight (normal_distance_weight_);
    seg_.setOptimizeCoefficients (true);
    seg_.setModelType (pcl::SACMODEL_NORMAL_PLANE);
    seg_.setEpsAngle(pcl::deg2rad(eps_angle_));
    seg_.setMethodType (pcl::SAC_RANSAC);
    seg_.setProbability (seg_prob_);

    proj_.setModelType (pcl::SACMODEL_NORMAL_PLANE);
    clusters_tree_ = boost::make_shared<pcl::KdTreeFLANN<Point> > ();
    clusters_tree_->setEpsilon (1);
    normals_tree_ = boost::make_shared<pcl::KdTreeFLANN<Point> > ();

    n3d_.setKSearch (k_);
    n3d_.setSearchMethod (normals_tree_);
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  virtual ~ExtractClusters () 
  {
    for (size_t i = 0; i < table_coeffs_.size (); ++i) 
      delete table_coeffs_[i];
  }

    
  void 
  init (double tolerance, std::string object_name)  // tolerance: how close to (0,0) is good enough?
  {
    std::string point_cloud_topic = nh_.resolveName ("/camera/depth/points2");
    point_cloud_sub_ = nh_.subscribe (point_cloud_topic, 1,  &ExtractClusters::ptuFinderCallback, this);
    object_name_ = object_name;
  }
    
private:
  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  void 
  ptuFinderCallback (const sensor_msgs::PointCloud2ConstPtr &cloud_in)
    {
      ROS_INFO_STREAM ("[" << getName ().c_str () << "] Received cloud: cloud time " << cloud_in->header.stamp);
      
      // Downsample + filter the input dataser
      PointCloud cloud_raw, cloud;
      pcl::fromROSMsg (*cloud_in, cloud_raw);
      vgrid_.setInputCloud (boost::make_shared<PointCloud> (cloud_raw));
      vgrid_.filter (cloud);
      //cloud_pub_.publish(cloud);
      //return;
      
      // Fit a plane (the table)
      pcl::ModelCoefficients table_coeff;
      pcl::PointIndices table_inliers;
      PointCloud cloud_projected;
      pcl::PointCloud<Point> cloud_hull;
      // ---[ Estimate the point normals
      pcl::PointCloud<pcl::Normal> cloud_normals;
      n3d_.setInputCloud (boost::make_shared<PointCloud> (cloud));
      n3d_.compute (cloud_normals);
      //cloud_pub_.publish(cloud_normals);
      //return;
      cloud_normals_.reset (new pcl::PointCloud<pcl::Normal> (cloud_normals));
      
      seg_.setInputCloud (boost::make_shared<PointCloud> (cloud));
      seg_.setInputNormals (cloud_normals_);
      //z axis in Kinect frame
      btVector3 axis(0.0, 0.0, 1.0);
      //rotate axis around x in Kinect frame for an angle between base_link and head_tilt_link + 90deg
      //todo: get angle automatically
      btVector3 axis2 = axis.rotate(btVector3(1.0, 0.0, 0.0), btScalar(base_link_head_tilt_link_angle_ + pcl::deg2rad(90.0)));
      //std::cerr << "axis: " << fabs(axis2.getX()) << " " << fabs(axis2.getY()) << " " << fabs(axis2.getZ()) << std::endl;
      seg_.setAxis (Eigen3::Vector3f(fabs(axis2.getX()), fabs(axis2.getY()), fabs(axis2.getZ())));
      // seg_.setIndices (boost::make_shared<pcl::PointIndices> (selection));
      seg_.segment (table_inliers, table_coeff);
      ROS_INFO ("[%s] Table model: [%f, %f, %f, %f] with %d inliers.", getName ().c_str (), 
                table_coeff.values[0], table_coeff.values[1], table_coeff.values[2], table_coeff.values[3], (int)table_inliers.indices.size ());
      if ((int)table_inliers.indices.size () <= min_table_inliers_)
      {
        ROS_ERROR ("table has to few inliers");
        return;
      }
      // Project the table inliers using the planar model coefficients    
      proj_.setInputCloud (boost::make_shared<PointCloud> (cloud));
      proj_.setIndices (boost::make_shared<pcl::PointIndices> (table_inliers));
      proj_.setModelCoefficients (boost::make_shared<pcl::ModelCoefficients> (table_coeff));
      proj_.filter (cloud_projected);
      //cloud_pub_.publish (cloud_projected);
      
      // Create a Convex Hull representation of the projected inliers
      chull_.setInputCloud (boost::make_shared<PointCloud> (cloud_projected));
      chull_.reconstruct (cloud_hull);
      //ROS_INFO ("Convex hull has: %d data points.", (int)cloud_hull.points.size ());
      //cloud_pub_.publish (cloud_hull);
      
      // //Compute the area of the plane
      // std::vector<double> plane_normal(3);
      // plane_normal[0] = table_coeff.values[0];
      // plane_normal[1] = table_coeff.values[1];
      // plane_normal[2] = table_coeff.values[2];
      // area_ = compute2DPolygonalArea (cloud_hull, plane_normal);
      // //ROS_INFO("[%s] Plane area: %f", getName ().c_str (), area_);
      // if (area_ > (expected_table_area_ + 0.01))
      // {
      //   extract_.setInputCloud (boost::make_shared<PointCloud> (cloud));
      //   extract_.setIndices (boost::make_shared<pcl::PointIndices> (table_inliers));
      //   extract_.setNegative (true);
      //   pcl::PointCloud<Point> cloud_extracted;
      //   extract_.filter (cloud_extracted);
      //   //cloud_extracted_pub_.publish (cloud_extracted);
      //   cloud = cloud_extracted;
      // } 
      //end while
      //ROS_INFO ("[%s] Publishing convex hull with: %d data points and area %lf.", getName ().c_str (), (int)cloud_hull.points.size (), area_);
      cloud_pub_.publish (cloud_hull);
      
      // pcl::PointXYZRGB point_min;
      // pcl::PointXYZRGB point_max;
      // pcl::PointXYZ point_center;
      // pcl::getMinMax3D (cloud_hull, point_min, point_max);
      // //Calculate the centroid of the hull
      // point_center.x = (point_max.x + point_min.x)/2;
      // point_center.y = (point_max.y + point_min.y)/2;
      // point_center.z = (point_max.z + point_min.z)/2;
      
      // tf::Transform transform;
      // transform.setOrigin( tf::Vector3(point_center.x, point_center.y, point_center.z));
      // transform.setRotation( tf::Quaternion(0, 0, 0) );
      // transform_broadcaster_.sendTransform(tf::StampedTransform(transform, ros::Time::now(), 
      //                                                           cloud_raw.header.frame_id, rot_table_frame_));
      
      // ---[ Get the objects on top of the table
      pcl::PointIndices cloud_object_indices;
      prism_.setHeightLimits (cluster_min_height_, cluster_max_height_);
      prism_.setInputCloud (boost::make_shared<PointCloud> (cloud));
      prism_.setInputPlanarHull (boost::make_shared<PointCloud>(cloud_hull));
      prism_.segment (cloud_object_indices);
      //ROS_INFO ("[%s] Number of object point indices: %d.", getName ().c_str (), (int)cloud_object_indices.indices.size ());
      
      pcl::PointCloud<Point> cloud_object;
      pcl::ExtractIndices<Point> extract_object_indices;
      //extract_object_indices.setInputCloud (cloud_all_minus_table_ptr);
      extract_object_indices.setInputCloud (boost::make_shared<PointCloud> (cloud));
//      extract_object_indices.setInputCloud (cloud_downsampled_);
      extract_object_indices.setIndices (boost::make_shared<const pcl::PointIndices> (cloud_object_indices));
      extract_object_indices.filter (cloud_object);
      //ROS_INFO ("[%s ] Publishing number of object point candidates: %d.", getName ().c_str (), 
      //        (int)cloud_objects.points.size ());
      
      
      std::vector<pcl::PointIndices> clusters;
      cluster_.setInputCloud (boost::make_shared<PointCloud>(cloud_object));
      cluster_.setClusterTolerance (object_cluster_tolerance_);
      cluster_.setMinClusterSize (object_cluster_min_size_);
      //    cluster_.setMaxClusterSize (object_cluster_max_size_);
      cluster_.setSearchMethod (clusters_tree_);
      cluster_.extract (clusters);
      
      pcl::PointCloud<Point> cloud_object_clustered;
      if (int(clusters.size()) >= nr_cluster_)
      {
        for (int i = 0; i < nr_cluster_; i++)
        {
          pcl::copyPointCloud (cloud_object, clusters[i], cloud_object_clustered);
          char object_name_angle[100];
          if (save_to_files_)
          {
            sprintf (object_name_angle, "%04d",  i);
            ROS_INFO("Saving cluster to: %s_%s.pcd", object_name_.c_str(), object_name_angle);
            pcd_writer_.write (object_name_ + "_" +  object_name_angle + ".pcd", cloud_object_clustered, true);
          }
          cloud_objects_pub_.publish (cloud_object_clustered);
        }
        ROS_INFO("Published %ld clusters.", clusters.size());
      }
      else
      {
        ROS_ERROR("Only %ld clusters found with size > %d points", clusters.size(), object_cluster_min_size_);
      }
      //std::stringstream ss;
      //ss << (pan_angle_ + 180);
      //    char object_name_angle[100];
//      sprintf (object_name_angle, "%04d",  );
      //ROS_INFO("Saving cluster to: %s.pcd", object_name_.c_str());
//      pcd_writer_.write (object_name_ + ".pcd", cloud_object_clustered, true);
      
      //want to save only once
      if (save_to_files_)
        exit(2);
    }

  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  /** \brief Compute the area of a 2D planar polygon patch - using a given normal
   * \param points the point cloud (planar)
   * \param normal the plane normal
   */
  double
  compute2DPolygonalArea (PointCloud &points, const std::vector<double> &normal)
  {
    int k0, k1, k2;
    
    // Find axis with largest normal component and project onto perpendicular plane
    k0 = (fabs (normal.at (0) ) > fabs (normal.at (1))) ? 0  : 1;
    k0 = (fabs (normal.at (k0)) > fabs (normal.at (2))) ? k0 : 2;
    k1 = (k0 + 1) % 3;
    k2 = (k0 + 2) % 3;
 
    // cos(theta), where theta is the angle between the polygon and the projected plane
    double ct = fabs ( normal.at (k0) );
 
    double area = 0;
    float p_i[3], p_j[3];

    for (unsigned int i = 0; i < points.points.size (); i++)
      {
        p_i[0] = points.points[i].x; p_i[1] = points.points[i].y; p_i[2] = points.points[i].z;
        int j = (i + 1) % points.points.size ();
        p_j[0] = points.points[j].x; p_j[1] = points.points[j].y; p_j[2] = points.points[j].z;

        area += p_i[k1] * p_j[k2] - p_i[k2] * p_j[k1];
      }
    area = fabs (area) / (2 * ct);

    return (area);
  }

  ros::NodeHandle nh_;  // Do we need to keep it?
  tf::TransformBroadcaster transform_broadcaster_;
  tf::TransformListener tf_listener_;
  bool save_to_files_, downsample_;

  double normal_search_radius_;
  double voxel_size_;

  std::string rot_table_frame_, object_name_;
  double object_cluster_tolerance_,  cluster_min_height_, cluster_max_height_;
  int object_cluster_min_size_, object_cluster_max_size_;

  pcl::PCDWriter pcd_writer_;
  double sac_distance_, normal_distance_weight_, z_min_limit_, z_max_limit_;
  double eps_angle_, seg_prob_, base_link_head_tilt_link_angle_;
  int k_, max_iter_, min_table_inliers_, nr_cluster_;
  
  ros::Subscriber point_cloud_sub_;

  std::vector<Eigen3::Vector4d *> table_coeffs_;

  pcl_ros::Publisher<Point> cloud_pub_;
  pcl_ros::Publisher<Point> cloud_extracted_pub_;
  pcl_ros::Publisher<Point> cloud_objects_pub_;

  // PCL objects
  pcl::PassThrough<Point> vgrid_;                   // Filtering + downsampling object
//  pcl::VoxelGrid<Point> vgrid_;                   // Filtering + downsampling object
  pcl::NormalEstimation<Point, pcl::Normal> n3d_;   //Normal estimation
  // The resultant estimated point cloud normals for \a cloud_filtered_
  pcl::PointCloud<pcl::Normal>::ConstPtr cloud_normals_;
  pcl::SACSegmentationFromNormals<Point, pcl::Normal> seg_;               // Planar segmentation object
  pcl::ProjectInliers<Point> proj_;               // Inlier projection object
  pcl::ExtractIndices<Point> extract_;            // Extract (too) big tables
  pcl::ConvexHull2D<Point> chull_;  
  pcl::ExtractPolygonalPrismData<Point> prism_;
  pcl::PointCloud<Point> cloud_objects_;
  pcl::EuclideanClusterExtraction<Point> cluster_;
  KdTreePtr clusters_tree_, normals_tree_;

  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  /** \brief Get a string representation of the name of this class. */
  std::string getName () const { return ("ExtractClusters"); }
};

/* ---[ */
int
main (int argc, char** argv)
{
  ros::init (argc, argv, "extract_clusters");
  ros::NodeHandle nh("~");
  if (argc < 2)
  {
    ROS_ERROR ("usage %s <object_name>", argv[0]);
    exit(2);
  }
  std::string object_name = argv[1];
  ExtractClusters ptu_calibrator (nh);
  ptu_calibrator.init (5, object_name);  // 5 degrees tolerance
  ros::spin ();
}
/* ]--- */
