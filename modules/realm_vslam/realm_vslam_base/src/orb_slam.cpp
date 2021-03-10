

#include <realm_vslam_base/orb_slam.h>
#include <realm_core/loguru.h>

using namespace realm;

OrbSlam::OrbSlam(const VisualSlamSettings::Ptr &vslam_set, const CameraSettings::Ptr &cam_set, const ImuSettings::Ptr &imu_set)
: m_prev_keyid(-1),
  m_resizing((*vslam_set)["resizing"].toDouble()),
  m_timestamp_reference(0),
  m_path_vocabulary((*vslam_set)["path_vocabulary"].toString())
{
  // Read the settings files
  cv::Mat K_32f = cv::Mat::eye(3, 3, CV_32F);
  K_32f.at<float>(0, 0) = (*cam_set)["fx"].toFloat() * static_cast<float>(m_resizing);
  K_32f.at<float>(1, 1) = (*cam_set)["fy"].toFloat() * static_cast<float>(m_resizing);
  K_32f.at<float>(0, 2) = (*cam_set)["cx"].toFloat() * static_cast<float>(m_resizing);
  K_32f.at<float>(1, 2) = (*cam_set)["cy"].toFloat() * static_cast<float>(m_resizing);

  cv::Mat dist_coeffs_32f = cv::Mat::zeros(1, 5, CV_32F);
  dist_coeffs_32f.at<float>(0) = (*cam_set)["k1"].toFloat();
  dist_coeffs_32f.at<float>(1) = (*cam_set)["k2"].toFloat();
  dist_coeffs_32f.at<float>(2) = (*cam_set)["p1"].toFloat();
  dist_coeffs_32f.at<float>(3) = (*cam_set)["p2"].toFloat();
  dist_coeffs_32f.at<float>(4) = (*cam_set)["k3"].toFloat();

  ORB_SLAM3::CameraParameters cam{};
  cam.K = K_32f;
  cam.distCoeffs = dist_coeffs_32f;
  cam.fps        = (*cam_set)["fps"].toFloat();
  cam.width      = (*cam_set)["width"].toInt();
  cam.height     = (*cam_set)["height"].toInt();
  cam.isRGB      = false; // BGR

  ORB_SLAM3::OrbParameters orb{};
  orb.nFeatures   = (*vslam_set)["nrof_features"].toInt();
  orb.nLevels     = (*vslam_set)["n_pyr_levels"].toInt();
  orb.scaleFactor = (*vslam_set)["scale_factor"].toFloat();
  orb.minThFast   = (*vslam_set)["min_th_FAST"].toInt();
  orb.iniThFast   = (*vslam_set)["ini_th_FAST"].toInt();

  ORB_SLAM3::ImuParameters imu{};
  if (imu_set != nullptr)
  {
    LOG_F(INFO, "Detected IMU settings. Loading ORB SLAM3 with IMU support.");

    imu.accelWalk  = (*imu_set)["gyro_noise_density"].toFloat();
    imu.gyroWalk   = (*imu_set)["gyro_bias_random_walk_noise_density"].toFloat();
    imu.noiseAccel = (*imu_set)["acc_noise_density"].toFloat();
    imu.noiseGyro  = (*imu_set)["acc_bias_random_walk_noise_density"].toFloat();
    imu.Tbc        = (*imu_set)["T_cam_imu"].toMat();
    imu.freq       = (*imu_set)["freq"].toFloat();
  }

  if (imu_set != nullptr)
    m_slam = new ORB_SLAM3::System(m_path_vocabulary, cam, imu, orb, ORB_SLAM3::System::IMU_MONOCULAR);
  else
    m_slam = new ORB_SLAM3::System(m_path_vocabulary, cam, imu, orb, ORB_SLAM3::System::MONOCULAR);

  //namespace ph = std::placeholders;
  //std::function<void(ORB_SLAM2::KeyFrame*)> kf_update = std::bind(&OrbSlam::keyframeUpdateCb, this, ph::_1);
  //_slam->RegisterKeyTransport(kf_update);
}

OrbSlam::~OrbSlam()
{
  m_slam->Shutdown();
  delete m_slam;
}

void OrbSlam::queueImuData(const ImuData &data)
{
  m_imu_queue.emplace_back(
      ORB_SLAM3::IMU::Point(static_cast<float>(data.acceleration.x), static_cast<float>(data.acceleration.y), static_cast<float>(data.acceleration.z),
                            static_cast<float>(data.gyroscope.x),    static_cast<float>(data.gyroscope.y),    static_cast<float>(data.gyroscope.z),
                            data.timestamp/10e9)
      );
  std::cout << "IMU data received: " << m_imu_queue.back().a << ", " << m_imu_queue.back().w << std::endl;
}

VisualSlamIF::State OrbSlam::track(Frame::Ptr &frame, const cv::Mat &T_c2w_initial)
{
  if (m_timestamp_reference == 0)
  {
    m_timestamp_reference = frame->getTimestamp();
    return State::LOST;
  }

  // Set image resizing accoring to settings
  frame->setImageResizeFactor(m_resizing);

  double timestamp = static_cast<double>(frame->getTimestamp() - m_timestamp_reference)/10e9;
  LOG_IF_F(INFO, true, "Time elapsed since first frame: %4.2f [s]", timestamp);

  // ORB SLAM returns a transformation from the world to the camera frame (T_w2c). In case we provide an initial guess
  // of the current pose, we have to invert this before, because in OpenREALM the standard is defined as T_c2w.

  cv::Mat T_w2c;
  if (T_c2w_initial.empty())
  {
    T_w2c = m_slam->TrackMonocular(frame->getResizedImageRaw(), timestamp, m_imu_queue);

  }
  else
  {
    cv::Mat T_w2c_initial = invertPose(T_c2w_initial);
    T_w2c_initial.convertTo(T_w2c_initial, CV_32F);
    T_w2c = m_slam->TrackMonocular(frame->getResizedImageRaw(), timestamp, T_w2c_initial);
  }

  m_imu_queue.clear();

  // In case tracking was successfull and slam not lost
  if (!T_w2c.empty())
  {
    // Pose definition as 3x4 matrix, calculated as 4x4 with last row (0, 0, 0, 1)
    // ORB SLAM 2 pose is defined as T_w2c, however the more intuitive way to describe
    // it for mapping is T_c2w (camera to world) therefore invert the pose matrix
    cv::Mat T_c2w = invertPose(T_w2c);

    // Also convert to double precision
    T_c2w.convertTo(T_c2w, CV_64F);
    T_c2w.pop_back();
    frame->setVisualPose(T_c2w);

    //std::cout << "Soll:\n" << T_c2w << std::endl;
    //std::cout << "Schätz:\n" << T_c2w_initial << std::endl;

    cv::Mat surface_pts = getTrackedMapPoints();
    frame->setSparseCloud(surface_pts, true);

    // Check if new frame is keyframe by comparing current keyid with last keyid
    auto keyid = static_cast<int32_t>(m_slam->GetLastKeyFrameId());

    // Check current state of the slam
    if (m_prev_keyid == -1)
    {
      m_prev_keyid = keyid;
      return State::INITIALIZED;
    }
    else if (m_prev_keyid != keyid)
    {
      m_prev_keyid = keyid;
      m_orb_to_frame_ids.insert({keyid, frame->getFrameId()});
      return State::KEYFRAME_INSERT;
    }
    else
    {
      return State::FRAME_INSERT;
    }
  }
  return State::LOST;
}

void OrbSlam::reset()
{
  m_slam->Reset();
  m_timestamp_reference = 0;
}

void OrbSlam::close()
{
  m_slam->Shutdown();
}

cv::Mat OrbSlam::getTrackedMapPoints() const
{
  std::vector<ORB_SLAM3::MapPoint*> mappoints;

  mappoints = m_slam->GetTrackedMapPoints();

  size_t n = mappoints.size();

  cv::Mat cvpoints;
  cvpoints.reserve(n);
  for (size_t i = 0; i < n; ++i)
  {
    if (mappoints[i] != nullptr)
    {
      cv::Mat p = mappoints[i]->GetWorldPos().t();
      cvpoints.push_back(p);
    }
  }
  cvpoints.convertTo(cvpoints, CV_64F);
  return cvpoints;
}

bool OrbSlam::drawTrackedImage(cv::Mat &img) const
{
  img = m_slam->DrawTrackedImage();
  return true;
}

void OrbSlam::registerUpdateTransport(const PoseUpdateFuncCb &func)
{
  m_pose_update_func_cb = func;
}

void OrbSlam::registerResetCallback(const ResetFuncCb &func)
{
  if (func)
  {
    //_slam->RegisterResetCallback(func);
  }
}

void OrbSlam::keyframeUpdateCb(ORB_SLAM3::KeyFrame* kf)
{
  if (kf != nullptr && m_pose_update_func_cb)
  {
    auto id = (uint32_t) kf->mnFrameId;

    // Get update on pose
    cv::Mat T_w2c = kf->GetPose();
    cv::Mat T_c2w = invertPose(T_w2c);
    T_c2w.convertTo(T_c2w, CV_64F);
    T_c2w.pop_back();

    // Get update on map points
    std::set<ORB_SLAM3::MapPoint*> map_points = kf->GetMapPoints();
    cv::Mat points;
    points.reserve(map_points.size());
    for (const auto &pt : map_points)
      if (pt != nullptr)
        points.push_back(pt->GetWorldPos().t());
    points.convertTo(points, CV_64F);

    // Transport to update function
    m_pose_update_func_cb(m_orb_to_frame_ids[id], T_c2w, points);
  }
}

cv::Mat OrbSlam::invertPose(const cv::Mat &pose) const
{
  cv::Mat pose_inv = cv::Mat::eye(4, 4, pose.type());
  cv::Mat R_t = (pose.rowRange(0, 3).colRange(0, 3)).t();
  cv::Mat t = -R_t*pose.rowRange(0, 3).col(3);
  R_t.copyTo(pose_inv.rowRange(0, 3).colRange(0, 3));
  t.copyTo(pose_inv.rowRange(0, 3).col(3));
  return pose_inv;
}

void OrbSlam::printSettingsToLog()
{
  LOG_F(INFO, "### OrbSlam2 general settings ###");
  LOG_F(INFO, "- use_viewer: %i", m_use_viewer);
  LOG_F(INFO, "- resizing: %4.2f", m_resizing);
  LOG_F(INFO, "- path settings: %s", m_path_settings.c_str());
  LOG_F(INFO, "- path vocabulary: %s", m_path_vocabulary.c_str());
}