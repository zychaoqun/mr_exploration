#include "graph2occupancy.h"

using namespace std;
using namespace Eigen;
using namespace g2o;


#define MAP_IDX(sx, i, j) ((sx) * (j) + (i))

bool Graph2occupancy::mapCallback(nav_msgs::GetMap::Request  &req, nav_msgs::GetMap::Response &res ){


  //header (uint32 seq, time stamp, string frame_id)
 res.map.header.frame_id = _topicName;
  
  //info (time map_load_time  float32 resolution   uint32 width  uint32 height   geometry_msgs/Pose origin)

  res.map.info = _gridMsg.info;
  res.map.info.map_load_time = ros::Time::now();


  //data (int8[] data)
  res.map.data = _gridMsg.data;

  return true;
}


Graph2occupancy::Graph2occupancy(OptimizableGraph *graph, int idRobot, SE2 gtPose, string topicName, float resolution, float threhsold, float rows, float cols, float maxRange, float usableRange, float gain, float squareSize, float angle, float freeThrehsold){
  
    _graph = graph;
    _resolution = resolution;
    _threshold = threhsold;
    _rows = rows;
    _cols = cols;
    _maxRange = maxRange;
    _usableRange = usableRange;
    _gain = gain;
    _squareSize = squareSize;
    _angle = angle;
    _freeThreshold = freeThrehsold;

    _groundTruthInitialPose = gtPose;




    std::stringstream fullTopicName;
    //fullTopicName << "/robot_" << idRobot << "/" << topicName;
    fullTopicName << topicName;
    _topicName = fullTopicName.str();

    _pubOccupGrid = _nh.advertise<nav_msgs::OccupancyGrid>(_topicName,1);

    _pubActualCoord = _nh.advertise<geometry_msgs::Pose2D>("map_pose",1);

    _server = _nh.advertiseService("map", &Graph2occupancy::mapCallback, this);


    _gridMsg.header.frame_id = _topicName;
    _gridMsg.info.resolution = _resolution;
    geometry_msgs::Pose poseMsg;
    poseMsg.position.x = 0.0;
    poseMsg.position.y = 0.0;
    poseMsg.position.z = 0.0;
    poseMsg.orientation.x = 0;
    poseMsg.orientation.y = 0; 
    poseMsg.orientation.z = 0.0; 
    poseMsg.orientation.w = 1.0;

    _gridMsg.info.origin = poseMsg;


    _tfListener.waitForTransform("odom", "base_footprint", ros::Time::now(), ros::Duration(2.0));


}


void Graph2occupancy::computeMap(){

    // Sort verteces
    vector<int> vertexIds(_graph->vertices().size());
    int k = 0;
    for(OptimizableGraph::VertexIDMap::iterator it = _graph->vertices().begin(); it != _graph->vertices().end(); ++it) {
      vertexIds[k++] = (it->first);
    }  
    sort(vertexIds.begin(), vertexIds.end());


  /************************************************************************
   *                          Compute map size                            *
   ************************************************************************/
  // Check the entire graph to find map bounding box
  Matrix2d boundingBox = Matrix2d::Zero();
  std::vector<RobotLaser*> robotLasers;
  std::vector<SE2> robotPoses;
  double xmin=std::numeric_limits<double>::max();
  double xmax=std::numeric_limits<double>::min();
  double ymin=std::numeric_limits<double>::max();
  double ymax=std::numeric_limits<double>::min();

  SE2 baseTransform(0,0,_angle);

  for(size_t i = 0; i < vertexIds.size(); ++i) {
    OptimizableGraph::Vertex *_v = _graph->vertex(vertexIds[i]);
    VertexSE2 *v = dynamic_cast<VertexSE2*>(_v);
    if(!v) { continue; }
    v->setEstimate(baseTransform*v->estimate());
    OptimizableGraph::Data *d = v->userData();

    while(d) {
      RobotLaser *robotLaser = dynamic_cast<RobotLaser*>(d);
      if(!robotLaser) {
  d = d->next();
  continue;
      }      
      robotLasers.push_back(robotLaser);
      robotPoses.push_back(v->estimate());
      double x = v->estimate().translation().x();
      double y = v->estimate().translation().y();
      
      xmax = xmax > x+_usableRange ? xmax : x+_usableRange;
      ymax = ymax > y+_usableRange ? ymax : y+_usableRange;
      xmin = xmin < x-_usableRange ? xmin : x-_usableRange;
      ymin = ymin < y-_usableRange ? ymin : y-_usableRange;
 
      d = d->next();
    }
  }

  boundingBox(0,0)=xmin;
  boundingBox(0,1)=xmax;
  boundingBox(1,0)=ymin;
  boundingBox(1,1)=ymax;


  //std::cout << "Found " << robotLasers.size() << " laser scans"<< std::endl;
  //std::cout << "Bounding box: " << std::endl << boundingBox << std::endl; 
  if(robotLasers.size() == 0)  {
    std::cout << "No laser scans found ... quitting!" << std::endl;
    return;
  }

  /************************************************************************
   *                          Compute the map                             *
   ************************************************************************/
  // Create the map
  Vector2i size;
  if(_rows != 0 && _cols != 0) { size = Vector2i(_rows, _cols); }
  else {
    size = Vector2i((boundingBox(0, 1) - boundingBox(0, 0))/ _resolution,
         (boundingBox(1, 1) - boundingBox(1, 0))/ _resolution);
    } 
 // std::cout << "Map size: " << size.transpose() << std::endl;
  if(size.x() == 0 || size.y() == 0) {
    std::cout << "Zero map size ... quitting!" << std::endl;
   return;
  }

  

  //Vector2f offset(-size.x() * _resolution / 2.0f, -size.y() * _resolution / 2.0f);
  _offset<<boundingBox(0, 0),boundingBox(1, 0);
  if (_initialOffset == Vector2f{0,0}){
    _initialOffset = _offset;
  }
  FrequencyMapCell unknownCell;
  
  _map = FrequencyMap(_resolution, _offset, size, unknownCell);

  for(size_t i = 0; i < vertexIds.size(); ++i) {
    OptimizableGraph::Vertex *_v = _graph->vertex(vertexIds[i]);
    VertexSE2 *v = dynamic_cast<VertexSE2*>(_v);
    if(!v) { continue; }
    OptimizableGraph::Data *d = v->userData();
    SE2 robotPose = v->estimate();
    
    while(d) {
      RobotLaser *robotLaser = dynamic_cast<RobotLaser*>(d);
      if(!robotLaser) {
  d = d->next();
  continue;
      }      
      _map.integrateScan(robotLaser, robotPose, _maxRange, _usableRange, _gain, _squareSize);
      d = d->next();
    }
  }



  /************************************************************************
   *                  Convert frequency map into int[8]                   *
   ************************************************************************/

  _gridMsg.data.resize(_map.rows() * _map.cols());

  _gridMsg.info.width = _map.cols();
  _gridMsg.info.height = _map.rows();

    for(int r = 0; r < _map.rows(); r++) {
      for(int c = 0; c < _map.cols(); c++) {
          if(_map(r, c).misses() == 0 && _map(r, c).hits() == 0) {
            _gridMsg.data[MAP_IDX(_map.cols(),c, _map.rows() - r - 1)] = _unknownColor;   }
          else {
            float fraction = (float)_map(r, c).hits()/(float)(_map(r, c).hits()+_map(r, c).misses());
            if (_freeThreshold && fraction < _freeThreshold){
              _gridMsg.data[MAP_IDX(_map.cols(),c, _map.rows() - r - 1)] = _freeColor;   }
            else if (_threshold && fraction > _threshold){
              _gridMsg.data[MAP_IDX(_map.cols(),c, _map.rows() - r - 1)] = _occupiedColor;   }
            else {
              _gridMsg.data[MAP_IDX(_map.cols(),c, _map.rows() - r - 1)] = _unknownColor;      }
                }
          
          }
        } 


}


void Graph2occupancy::publishMapPose(SE2 actualPose){

  geometry_msgs::Pose2D poseMsg;

  Vector2D translation = actualPose.translation();
  Vector2D groundTruthStartPose = _groundTruthInitialPose.translation();

  float startX = groundTruthStartPose[0] - _initialOffset[0];
  float startY = groundTruthStartPose[1] - _initialOffset[1];

  float relativeX = translation[0] - groundTruthStartPose[0];
  float relativeY = translation[1] - groundTruthStartPose[1];

  float mapY = startX - relativeX;
  float mapX = startY + relativeY;


  poseMsg.x = mapX;
  poseMsg.y = mapY;
  poseMsg.theta = actualPose.rotation().angle() - _groundTruthInitialPose.rotation().angle();

  _pubActualCoord.publish(poseMsg);
}

void Graph2occupancy::publishTF(SE2 actualPose) {


  _tfListener.lookupTransform("odom", "base_footprint", ros::Time(0), _tfOdom2Footprint);

  float gtX = _groundTruthInitialPose.translation().x();
  float gtY = _groundTruthInitialPose.translation().y();
  float gtTheta = _groundTruthInitialPose.rotation().angle();


  tf::Transform correctedOdom;
  Vector2f correctedOdomOrigin = {actualPose.translation()[0] - gtX, actualPose.translation()[1] - gtY};
  Rotation2D<float> correctedRot(-gtTheta);
  Vector2f rotatedCorrectedOdomOrigin = correctedRot*correctedOdomOrigin;
  correctedOdom.setOrigin(tf::Vector3(rotatedCorrectedOdomOrigin[0],rotatedCorrectedOdomOrigin[1], 0.0));
  tf::Quaternion qCorrected;
  qCorrected.setRPY(0, 0, actualPose.rotation().angle() - gtTheta);
  correctedOdom.setRotation(qCorrected);

  tf::Transform differenceSLAM;
  Vector2f differenceOrigin = {correctedOdom.getOrigin().x() - _tfOdom2Footprint.getOrigin().x(),correctedOdom.getOrigin().y() - _tfOdom2Footprint.getOrigin().y()};
  float yawDiff = tf::getYaw(correctedOdom.getRotation()) - tf::getYaw(_tfOdom2Footprint.getRotation());
  tf::Quaternion diffQuaternion;
  Rotation2D<float> rototo(yawDiff);
  Vector2f rotatedDifference = rototo*differenceOrigin;

  diffQuaternion.setRPY(0,0,0); //YAWDIFF????!!!!!
  
  differenceSLAM.setOrigin(tf::Vector3(rotatedDifference[0], rotatedDifference[1], 0.0));
  differenceSLAM.setRotation(diffQuaternion);

  Rotation2D<float> rot(gtTheta);
  Vector2f origin = {gtY + gtX - _initialOffset[1], -gtX + gtY + _initialOffset[0]};
  Vector2f rotatedOrigin = rot*origin;

  tf::Transform map2Trajectory;
  map2Trajectory.setOrigin(tf::Vector3(rotatedOrigin[0],rotatedOrigin[1], 0.0));
  tf::Quaternion map2TrajectoryQuaternions;
  map2TrajectoryQuaternions.setRPY(0, 0, -gtTheta);
  map2Trajectory.setRotation(map2TrajectoryQuaternions);
  _tfBroadcaster.sendTransform(tf::StampedTransform(map2Trajectory, ros::Time::now(), "map", "trajectory"));


  tf::Transform staticMap2Odom;
  staticMap2Odom.setOrigin(tf::Vector3(gtX - _initialOffset[0],gtY - _initialOffset[1], 0.0));
  tf::Quaternion staticMap2OdomQuaternions;
  staticMap2OdomQuaternions.setRPY(0, 0, 0);
  staticMap2Odom.setRotation(staticMap2OdomQuaternions);


  tf::Transform corretedMap2Odom;
  //corretedMap2Odom.mult(differenceSLAM, staticMap2Odom);
  corretedMap2Odom.setOrigin(differenceSLAM.getOrigin() + staticMap2Odom.getOrigin() );
  //diffQuaternion.setRPY(0,0,0); 
  //corretedMap2Odom.setRotation(diffQuaternion);
  corretedMap2Odom.setRotation(tf::createQuaternionFromYaw(tf::getYaw(staticMap2Odom.getRotation()) + tf::getYaw(differenceSLAM.getRotation())));


  float diffX = corretedMap2Odom.getOrigin().x() - _lastMap2Odom.getOrigin().x();  
  float diffY = corretedMap2Odom.getOrigin().y() - _lastMap2Odom.getOrigin().y();  
  float diffAngle = tf::getYaw(corretedMap2Odom.getRotation()) -  tf::getYaw(_lastMap2Odom.getRotation());


  float xyDrift = sqrt(pow(diffX,2) + pow(diffY,2));
  float yawDrift = fabs(diffAngle);

  if (_first){
    _lastMap2Odom = staticMap2Odom;
    _first = false;
  }

  if ((xyDrift > 0.25)||(yawDrift > 0.15)){
      _tfBroadcaster.sendTransform(tf::StampedTransform(corretedMap2Odom, ros::Time::now(), "map", "odom"));
      std::cout<<"CORRECTED MAP2ODOM: from "<< _lastMap2Odom.getOrigin().x()<<" "<< _lastMap2Odom.getOrigin().x()<<" to "<<corretedMap2Odom.getOrigin().x() <<" "<<corretedMap2Odom.getOrigin().y()<<std::endl;
      _lastMap2Odom = corretedMap2Odom; 

  }
  else {
       _tfBroadcaster.sendTransform(tf::StampedTransform(_lastMap2Odom, ros::Time::now(), "map", "odom")); 

  }

}


void Graph2occupancy::publishMap() {

  //header (uint32 seq, time stamp, string frame_id)
  
  //info (time map_load_time  float32 resolution   uint32 width  uint32 height   geometry_msgs/Pose origin)
  _gridMsg.info.map_load_time = ros::Time::now();

  _pubOccupGrid.publish(_gridMsg);


}

  void Graph2occupancy::setResolution (const float resolution){
    _resolution = resolution;
  }
  void Graph2occupancy::setThreshold (const float threshold){
    _threshold = threshold;
  }
  void Graph2occupancy::setRows (const float rows){
    _rows = rows;
  }
  void Graph2occupancy::setCols (const float cols){
    _cols = cols;
  }
  void Graph2occupancy::setMaxRange (const float maxRange){
    _maxRange = maxRange;
  }
  void Graph2occupancy::setUsableRange (const float usableRange){
    _usableRange = usableRange;
  }
  void Graph2occupancy::setGain (const float gain){
    _gain = gain;
  }
  void Graph2occupancy::setSquareSize (const float squareSize) {
    _squareSize = squareSize;
  }
  void Graph2occupancy::setAngle (const float angle){
    _angle = angle;
  }
  void Graph2occupancy::setFreeThreshold (const float freeThrehsold){
    _freeThreshold = freeThrehsold;
  }
  void Graph2occupancy::setTopicName (const string topicName){
    _topicName = topicName;
  }



  float Graph2occupancy::getResolution (){
    return _resolution;
  }
  float Graph2occupancy::getThreshold (){
    return _threshold;
  }
  float Graph2occupancy::getRows (){
    return _rows;
  }
  float Graph2occupancy::getCols (){
    return _cols;
  }
  float Graph2occupancy::getMaxRange (){
    return _maxRange;
  }
  float Graph2occupancy::getUsableRange (){
    return _usableRange;
  }
  float Graph2occupancy::getGain (){
    return _gain;
  }
  float Graph2occupancy::getSquareSize (){
    return _squareSize;
  }
  float Graph2occupancy::getAngle (){
    return _angle;
  }
  float Graph2occupancy::getFreeThreshold (){
    return _freeThreshold;
  }
  string Graph2occupancy::getTopicName (){
    return _topicName;
  }

  Vector2f Graph2occupancy::getOffset(){
    return _offset;
  }



void Graph2occupancy::showMap() {}



void Graph2occupancy::saveMap(string outputFileName) {

  *_mapImage = cv::Mat(_map.rows(), _map.cols(), CV_8UC1);
  _mapImage->setTo(cv::Scalar(0));

    for(int c = 0; c < _map.cols(); c++) {
      for(int r = 0; r < _map.rows(); r++) {
          if(_map(r, c).misses() == 0 && _map(r, c).hits() == 0) {
            _mapImage->at<unsigned char>(r, c) = _unknownImageColor; }
          else {
            float fraction = (float)_map(r, c).hits()/(float)(_map(r, c).hits()+_map(r, c).misses());
            if (_freeThreshold && fraction < _freeThreshold){
              _mapImage->at<unsigned char>(r, c) = _freeImageColor; }
            else if (_threshold && fraction > _threshold){
              _mapImage->at<unsigned char>(r, c) = _occupiedImageColor; }
            else {
            _mapImage->at<unsigned char>(r, c) = _unknownImageColor; }
              }
          
          }
      }



  cv::imwrite(outputFileName + ".png", *_mapImage);


  std::ofstream ofs(string(outputFileName + ".yaml").c_str());
  Eigen::Vector3f origin(0.0f, 0.0f, 0.0f);
  ofs << "image: " << outputFileName << ".png" << endl
      << "resolution: " << _resolution << endl
      << "origin: [" << origin.x() << ", " << origin.y() << ", " << origin.z() << "]" << endl
      << "negate: 0" << endl
      << "occupied_thresh: " << _threshold << endl
      << "free_thresh: " << _freeThreshold << endl;




}

