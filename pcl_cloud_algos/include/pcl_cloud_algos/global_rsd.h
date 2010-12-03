#ifndef CLOUD_ALGOS_GRSD_H
#define CLOUD_ALGOS_GRSD_H

// Eigen
//#include <Eigen3/StdVector>
//#include <Eigen3/Array>

//#include <ros/ros.h>
#include <sensor_msgs/PointCloud.h>
//#include <sensor_msgs/PointCloud2.h>
//#include <sensor_msgs/point_cloud_conversion.h>
#include "octomap/octomap.h"
#include "pcl_to_octree/octree/OcTreePCL.h"
#include "pcl_to_octree/octree/OcTreeNodePCL.h"
#include "pcl_to_octree/octree/OcTreeServerPCL.h"
//#include "octomap_server/octomap_server.h"
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <visualization_msgs/MarkerArray.h>

// Cloud geometry
#include <point_cloud_mapping/geometry/angles.h>
#include <point_cloud_mapping/geometry/areas.h>
#include <point_cloud_mapping/geometry/point.h>
#include <point_cloud_mapping/geometry/distances.h>
#include <point_cloud_mapping/geometry/nearest.h>
#include <point_cloud_mapping/geometry/transforms.h>
#include <point_cloud_mapping/geometry/statistics.h>

// Kd Tree
#include <point_cloud_mapping/kdtree/kdtree_ann.h>

// Cloud algos
#include <cloud_algos/cloud_algos.h>

#define NR_CLASS 5
// TODO: use a map to have surface labels and free space map to indices in the transitions matrix

#define _sqr(c) ((c)*(c))
#define _sqr_dist(a,b) ( _sqr(((a).x())-((b).x())) + _sqr(((a).y())-((b).y())) + _sqr(((a).z())-((b).z())) )

namespace cloud_algos
{

struct IntersectedLeaf
{
  double sqr_distance; // square distance from source node
  octomap::point3d centroid; // leaf center coordinates
};

inline bool
  histogramElementCompare (const std::pair<int, IntersectedLeaf> &p1, const std::pair<int, IntersectedLeaf> &p2)
{
  return (p1.second.sqr_distance < p2.second.sqr_distance);
}

class GlobalRSD : public CloudAlgo
{
 public:

  // Input/output type
  typedef sensor_msgs::PointCloud OutputType;
  typedef sensor_msgs::PointCloud InputType;

  // Options
  int point_label_; // label of the object if known, and -1 otherwise
  double width_; // the width of the OcTree cells
  int step_; // specifies how many extra cells in each direction should contribute to local feature
  //int rsd_bins_; // number of divisions to create the RSD histogram
  int min_voxel_pts_; // minimum number of points in a cell to be processed
  bool publish_cloud_centroids_; // should we publish cloud_centroids_?
  bool publish_cloud_vrsd_; // should we publish cloud_vrsd_?
  //bool surface2curvature_; // should we write the results of the local surface classification as the curvature value in the partial results for visualization?

  // Intermediary results for convenient access
  boost::shared_ptr<sensor_msgs::PointCloud> cloud_centroids_;
  boost::shared_ptr<sensor_msgs::PointCloud> cloud_vrsd_;

  // Topic name to subscribe to
  static std::string default_input_topic ()
    {return std::string ("cloud_pcd");}

  // Topic name to advertise
  static std::string default_output_topic ()
    {return std::string ("cloud_grsd");};

  // Node name
  static std::string default_node_name () 
    {return std::string ("global_rsd_node");};

  // Algorithm methods
  void init (ros::NodeHandle&);
  void pre  ();
  void post ();
  std::vector<std::string> requires ();
  std::vector<std::string> provides ();
  std::string process (const boost::shared_ptr<const InputType>&);
  boost::shared_ptr<const OutputType> output ();

  // Constructor-Destructor
  GlobalRSD () : CloudAlgo ()
  {
    point_label_ = -1;
    width_ = 0.03;
    step_ = 0;
    min_voxel_pts_ = 1;
    nr_bins_ = (NR_CLASS+1)*(NR_CLASS+2)/2;
  }
  ~GlobalRSD ()
  {
  }

  ros::Publisher createPublisher (ros::NodeHandle& nh)
  {
    ros::Publisher p = nh.advertise<OutputType> (default_output_topic (), 5);
    return p;
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Compute the min and maximum variation of normal angles by distance and
  // estimates local minimum and maximum radius of surface curvature, then
  // sets a value defining the surface type
  /// @NOTE: taken from 16.06.2010 version of EstimateMinMaxRadius.cc
  // Surface type value:
  //    0 - noise/corner
  //    1 - planar
  //    2 - cylinder (rim)
  //    3 - circle (corner?)
  //    4 - edge
  inline int
    setSurfaceType (boost::shared_ptr<sensor_msgs::PointCloud> cloud, std::vector<int> *indices, std::vector<int> *neighbors, int nxIdx, double max_dist, int regIdx, int rIdx)
  {
    // Fixing binning to 5 and plane radius to 0.2
    int div_d = 5;
    double plane_radius = 0.2;

    // Initialize minimum and maximum angle values in each distance bin
    std::vector<std::vector<double> > min_max_angle_by_dist (div_d);
    for (int di=0; di<div_d; di++)
    {
      min_max_angle_by_dist[di].resize (2);
      min_max_angle_by_dist[di][0] = +DBL_MAX;
      min_max_angle_by_dist[di][1] = -DBL_MAX;
    }

    // Compute distance by normal angle distribution for points
    for (unsigned int i = 0; i < neighbors->size (); i++)
      for (unsigned int j = i; j < neighbors->size (); j++)
      {
        // compute angle between the two lines going through normals (disregard orientation!)
        double cosine = cloud->channels[nxIdx+0].values[neighbors->at(i)] * cloud->channels[nxIdx+0].values[neighbors->at(j)] +
                        cloud->channels[nxIdx+1].values[neighbors->at(i)] * cloud->channels[nxIdx+1].values[neighbors->at(j)] +
                        cloud->channels[nxIdx+2].values[neighbors->at(i)] * cloud->channels[nxIdx+2].values[neighbors->at(j)];
        if (cosine > 1) cosine = 1;
        if (cosine < -1) cosine = -1;
        double angle  = acos (cosine);
        if (angle > M_PI/2) angle = M_PI - angle;
        //cerr << round(RAD2DEG(angle)) << " and ";

        // Compute point to point distance
        double dist = sqrt ((cloud->points[neighbors->at(i)].x - cloud->points[neighbors->at(j)].x) * (cloud->points[neighbors->at(i)].x - cloud->points[neighbors->at(j)].x) +
                            (cloud->points[neighbors->at(i)].y - cloud->points[neighbors->at(j)].y) * (cloud->points[neighbors->at(i)].y - cloud->points[neighbors->at(j)].y) +
                            (cloud->points[neighbors->at(i)].z - cloud->points[neighbors->at(j)].z) * (cloud->points[neighbors->at(i)].z - cloud->points[neighbors->at(j)].z));
        //cerr << dist << ": ";

        // compute bins and increase
        int bin_d = (int) floor (div_d * dist / max_dist);

        // update min-max values for distance bins
        if (min_max_angle_by_dist[bin_d][0] > angle) min_max_angle_by_dist[bin_d][0] = angle;
        if (min_max_angle_by_dist[bin_d][1] < angle) min_max_angle_by_dist[bin_d][1] = angle;
      }

    // Estimate radius from min and max lines
    double Amint_Amin = 0, Amint_d = 0;
    double Amaxt_Amax = 0, Amaxt_d = 0;
    for (int di=0; di<div_d; di++)
    {
      //cerr << di << ": " << min_max_angle_by_dist[di][0] << " - " << min_max_angle_by_dist[di][1] << endl;
      // combute the members of A'*A*r = A'*D
      if (min_max_angle_by_dist[di][1] >= 0)
      {
        double p_min = min_max_angle_by_dist[di][0];
        double p_max = min_max_angle_by_dist[di][1];
        //cerr << p_min << " " << p_max << endl;
        double f = (di+0.5)*max_dist/div_d;
        //cerr << f << endl;
        Amint_Amin += p_min * p_min;
        Amint_d += p_min * f;
        Amaxt_Amax += p_max * p_max;
        Amaxt_d += p_max * f;
      }
    }
    //cerr << Amint_Amin << " " << Amint_d << " " << Amaxt_Amax << " " << Amaxt_d << endl;
    double max_radius;
    if (Amint_Amin == 0) max_radius = plane_radius;
    else max_radius = std::min (Amint_d/Amint_Amin, plane_radius);
    double min_radius;
    if (Amaxt_Amax == 0) min_radius = plane_radius;
    else min_radius = std::min (Amaxt_d/Amaxt_Amax, plane_radius);
    //print_info (stderr, "Estimated minimum and maximum radius is: "); print_value (stderr, "%g - %g\n", min_radius, max_radius);

    // Simple categorization to reduce feature vector size, but should use co-occurance of min-max radius bins
    int type;// = EMPTY_VALUE;
    if (min_radius > 0.045) // 0.066
      type = 1; // plane
    else if ((min_radius < 0.030) && (max_radius < 0.050))
      type = 0; // noise/corner
    else if (max_radius - min_radius < 0.01) // 0.0075
      type = 3; // circle (corner?)
    //else if ((min_radius < 0.020) && (max_radius > 0.175)) // 0.150
    //  type = 4; // edge
    else if (min_radius < 0.030) /// considering small cylinders to be edges
      type = 4; // edge
    else
      type = 2; // cylinder (rim)

    // For safety...
    if (type >= NR_CLASS)
      type = -1;

    // Set values for all points
    for (unsigned int i = 0; i < indices->size (); i++)
    {
      cloud->channels[regIdx].values[indices->at(i)] = type;
      cloud->channels[rIdx+0].values[indices->at(i)] = min_radius;
      cloud->channels[rIdx+1].values[indices->at(i)] = max_radius;
      cloud->channels[rIdx+2].values[indices->at(i)] = max_radius - min_radius;
    }

    // Return type
    return type;
  }

  //*
  // Sets up the OcTree -- copied from Dejan for now
  void
    setOctree (boost::shared_ptr<sensor_msgs::PointCloud> pointcloud_msg, double octree_res, int initial_label, double laser_offset = 0, double octree_maxrange = -1)
  {
    //sensor_msgs::PointCloud2 pointcloud2_msg;
    octomap_server::OctomapBinary octree_msg;

    // Converting from PointCloud msg format to PointCloud2 msg format
    //sensor_msgs::convertPointCloudToPointCloud2(*pointcloud_msg, pointcloud2_msg);
    //pcl::PointCloud<pcl::PointXYZ> pointcloud2_pcl;

    octomap::point3d octomap_3d_point;
    octomap::Pointcloud octomap_pointcloud;

    //Converting PointCloud2 msg format to pcl pointcloud format in order to read the 3d data
    //point_cloud::fromMsg(pointcloud2_msg, pointcloud2_pcl);

    //Reading from pcl point cloud and saving it into octomap point cloud
    for(unsigned int i =0; i < pointcloud_msg->points.size(); i++)
    {
      octomap_3d_point(0) = pointcloud_msg->points[i].x;
      octomap_3d_point(1) = pointcloud_msg->points[i].y;
      octomap_3d_point(2) = pointcloud_msg->points[i].z;
      octomap_pointcloud.push_back(octomap_3d_point);
    }

    // Converting from octomap point cloud to octomap graph
    octomap::pose6d offset_trans(0,0,-laser_offset,0,0,0);
    octomap::pose6d laser_pose(0,0,laser_offset,0,0,0);
    octomap_pointcloud.transform(offset_trans);


    octomap::ScanGraph* octomap_graph = new octomap::ScanGraph();
    octomap_graph->addNode(&octomap_pointcloud, laser_pose);

    //ROS_INFO("Number of points in scene graph: %d", octomap_graph->getNumPoints());

    // Converting from octomap graph to octomap tree (octree)
    octree_ = new octomap::OcTreePCL(octree_res);
    for (octomap::ScanGraph::iterator scan_it = octomap_graph->begin(); scan_it != octomap_graph->end(); scan_it++)
    {
      octree_->insertScan(**scan_it, octree_maxrange, false);
    }
    //octomap_server::octomapMapToMsg(*octree, octree_msg);
    //octree_binary_publisher_.publish(octree_msg);
    //ROS_INFO("Octree built and published");

    std::list<octomap::OcTreeVolume> voxels, leaves;
    //octree->getLeafNodes(leaves, level_);
    octree_->getLeafNodes(leaves);
    std::list<octomap::OcTreeVolume>::iterator it1;
    int cnt = 0;

    //find Leaf Nodes' centroids, assign controid coordinates to Leaf Node
    for( it1 = leaves.begin(); it1 != leaves.end(); ++it1)
    {
      ROS_DEBUG("Leaf Node %d : x = %f y = %f z = %f side length = %f ", cnt++, it1->first.x(), it1->first.y(), it1->first.z(), it1->second);
      octomap::point3d centroid;
      centroid(0) = it1->first.x(),  centroid(1) = it1->first.y(),  centroid(2) = it1->first.z();
      octomap::OcTreeNodePCL *octree_node = octree_->search(centroid);
      octree_node->setCentroid(centroid);
      octree_node->setLabel(initial_label);
    }

    //assign points to Leaf Nodes
    for(unsigned int i = 0; i < pointcloud_msg->points.size(); i++)
    {
      octomap_3d_point(0) = pointcloud_msg->points[i].x;
      octomap_3d_point(1) = pointcloud_msg->points[i].y;
      octomap_3d_point(2) = pointcloud_msg->points[i].z;
      octomap::OcTreeNodePCL * octree_node1 = octree_->search(octomap_3d_point);
      octree_node1->set3DPointInliers(i);
    }
  }
  //*/

 private:

  // ROS stuff
  ros::NodeHandle nh_;
  ros::Publisher pub_cloud_vrsd_;
  ros::Publisher pub_cloud_centroids_;

  // hard-coding these for now
  int nr_bins_;

  // ROS messages
  boost::shared_ptr<sensor_msgs::PointCloud> cloud_grsd_;

  // OcTree stuff
  octomap::OcTreePCL* octree_;
};

}
#endif

