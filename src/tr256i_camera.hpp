#pragma once

#include <memory>
#include <vector>

#include "camera_session.hpp"

std::vector<CameraDevice> discoverTr256iCameras();
std::unique_ptr<CameraSession> openTr256iCamera(const CameraDevice &device);
