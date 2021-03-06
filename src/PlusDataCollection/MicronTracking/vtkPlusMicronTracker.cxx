/*=Plus=header=begin======================================================
Program: Plus
Copyright (c) Laboratory for Percutaneous Surgery. All rights reserved.
See License.txt for details.
=========================================================Plus=header=end*/

// TODO: need to evaluate if USE_MicronTracker_TIMESTAMPS without filtering
// is better then simply using accurate timestamp with filtering.
// E.g., it could be checked by performing temporal calibration.
// #define USE_MicronTracker_TIMESTAMPS

// Local includes
#include "PlusConfigure.h"
#include "PlusVideoFrame.h"
#include "vtkPlusDataSource.h"
#include "vtkPlusMicronTracker.h"

// Micron interface includes
#include <MicronTrackerInterface.h>
#include <MicronTrackerLogger.h>

// VTK includes
#include <vtkImageData.h>
#include <vtkImageImport.h>
#include <vtkMatrix4x4.h>
#include <vtkObjectFactory.h>

// STL includes
#include <fstream>
#include <iostream>
#include <set>

// Note that "MTC.h" is not included directly, as it causes compilation warnings
// and unnecessary coupling to lower-level MTC functions.
// All MTC internal functions shall be accessed through MicronTrackerInterface.

/****************************************************************************/

vtkStandardNewMacro(vtkPlusMicronTracker);

//----------------------------------------------------------------------------
vtkPlusMicronTracker::vtkPlusMicronTracker()
{
#ifdef USE_MicronTracker_TIMESTAMPS
  this->TrackerTimeToSystemTimeSec = 0;
  this->TrackerTimeToSystemTimeComputed = false;
#endif

  this->IsMicronTrackingInitialized = 0;
  this->MT = new MicronTrackerInterface();
  MicronTrackerLogger::Instance()->SetLogMessageCallback(LogMessageCallback, this);

  // for accurate timing
  this->FrameNumber = 0;

  this->RequirePortNameInDeviceSetConfiguration = true;

  // No callback function provided by the device, so the data capture thread will be used to poll the hardware and add new items to the buffer
  this->StartThreadForInternalUpdates = true;
  this->AcquisitionRate = 20;

  this->IniFile = "MicronTracker.ini";
}

//----------------------------------------------------------------------------
vtkPlusMicronTracker::~vtkPlusMicronTracker()
{
  if (this->IsMicronTrackingInitialized)
  {
    this->MT->mtEnd();
    this->IsMicronTrackingInitialized = false;
  }
  if (this->MT != NULL)
  {
    delete this->MT;
    this->MT = NULL;
  }
}

//----------------------------------------------------------------------------
std::string vtkPlusMicronTracker::GetSdkVersion()
{
  return this->MT->GetSdkVersion();
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::Probe()
{
  if (this->IsMicronTrackingInitialized)
  {
    LOG_ERROR("vtkPlusMicronTracker::Probe should not be called while the device is already initialized");
    return PLUS_FAIL;
  }

  std::string iniFilePath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(this->IniFile);
  LOG_DEBUG("Use MicronTracker ini file: " << iniFilePath);
  if (!vtksys::SystemTools::FileExists(iniFilePath.c_str(), true))
  {
    LOG_DEBUG("Unable to find MicronTracker IniFile file at: " << iniFilePath);
  }
  std::string templateFullPath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(this->TemplateDirectory.c_str());
  LOG_DEBUG("Loading the marker templates from " << templateFullPath);
  if (!vtksys::SystemTools::FileExists(templateFullPath.c_str(), false))
  {
    LOG_DEBUG("Unable to find MicronTracker TemplateDirectory at: " << templateFullPath);
  }

  if (this->MT->mtInit(iniFilePath) != 1)
  {
    LOG_ERROR("Error in initializing Micron Tracker");
    return PLUS_FAIL;
  }

  // Try to attach the cameras till find the cameras
  if (this->MT->mtSetupCameras() != 1)
  {
    LOG_ERROR("Error in initializing Micron Tracker: setup cameras failed. Check the camera connections.");
    return PLUS_FAIL;
  }

  int numOfCameras = this->MT->mtGetNumOfCameras();
  if (numOfCameras == 0)
  {
    LOG_ERROR("Error in initializing Micron Tracker: no cameras attached. Check the camera connections.");
    return PLUS_FAIL;
  }

  LOG_DEBUG("Number of attached cameras: " << numOfCameras);

  this->MT->mtEnd();
  this->IsMicronTrackingInitialized = false;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::InternalStartRecording()
{
  if (!this->IsMicronTrackingInitialized)
  {
    LOG_ERROR("InternalStartRecording failed: MicronTracker has not been initialized");
    return PLUS_FAIL;
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::InternalStopRecording()
{
  // No need to do anything here, as the MicronTracker only performs grabbing on request
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::InternalUpdate()
{
  if (!this->IsMicronTrackingInitialized)
  {
    LOG_ERROR("InternalUpdate failed: MicronTracker has not been initialized");
    return PLUS_FAIL;
  }

  // Generate a frame number, as the tool does not provide a frame number.
  // FrameNumber will be used in ToolTimeStampedUpdate for timestamp filtering
  ++this->FrameNumber;

  // Setting the timestamp
  const double unfilteredTimestamp = vtkPlusAccurateTimer::GetSystemTime();

  if (this->MT->mtGrabFrame() == -1)
  {
    // If grabbing a frame was not successful then just skip this attempt and retry on the next callback
    LOG_WARNING("Failed to grab a new frame (" << this->MT->GetLastErrorString() << "). Maybe the requested frame rate is too high.");
    return PLUS_FAIL;
  }

#ifdef USE_MicronTracker_TIMESTAMPS
  if (!this->TrackerTimeToSystemTimeComputed)
  {
    const double timeSystemSec = unfilteredTimestamp;
    const double timeTrackerSec = this->MT->mtGetLatestFrameTime();
    this->TrackerTimeToSystemTimeSec = timeSystemSec - timeTrackerSec;
    this->TrackerTimeToSystemTimeComputed = true;
  }
  const double timeTrackerSec = this->MT->mtGetLatestFrameTime();
  const double timeSystemSec = timeTrackerSec + this->TrackerTimeToSystemTimeSec;
#endif

  if (this->MT->mtProcessFrame() == -1)
  {
    LOG_ERROR("Error in processing a frame! (" << this->MT->GetLastErrorString() << ")");
    return PLUS_FAIL;
  }

  this->MT->mtFindIdentifiedMarkers();

  int numOfIdentifiedMarkers = this->MT->mtGetIdentifiedMarkersCount();
  LOG_TRACE("Number of identified markers: " << numOfIdentifiedMarkers);

  // Set status and transform for tools with detected markers
  vtkSmartPointer<vtkMatrix4x4> transformMatrix = vtkSmartPointer<vtkMatrix4x4>::New();
  std::set<std::string> identifiedToolSourceIds;
  vtkSmartPointer< vtkMatrix4x4 > mToolToTracker = vtkSmartPointer< vtkMatrix4x4 >::New();
  mToolToTracker->Identity();
  for (int identifedMarkerIndex = 0; identifedMarkerIndex < this->MT->mtGetIdentifiedMarkersCount(); identifedMarkerIndex++)
  {
    char* identifiedTemplateName = this->MT->mtGetIdentifiedTemplateName(identifedMarkerIndex);
    vtkPlusDataSource* tool = NULL;
    if (this->GetToolByPortName(identifiedTemplateName, tool) != PLUS_SUCCESS)
    {
      LOG_DEBUG("Marker " << identifiedTemplateName << " has no associated tool");
      continue;
    }

    GetTransformMatrix(identifedMarkerIndex, mToolToTracker);
#ifdef USE_MicronTracker_TIMESTAMPS
    this->ToolTimeStampedUpdateWithoutFiltering(tool->GetSourceId(), mToolToTracker, TOOL_OK, timeSystemSec, timeSystemSec);
#else
    this->ToolTimeStampedUpdate(tool->GetSourceId(), mToolToTracker, TOOL_OK, this->FrameNumber, unfilteredTimestamp);
#endif

    identifiedToolSourceIds.insert(tool->GetSourceId());
  }

  // Set status for tools with non-detected markers
  transformMatrix->Identity();
  for (DataSourceContainerConstIterator it = this->GetToolIteratorBegin(); it != this->GetToolIteratorEnd(); ++it)
  {
    if (identifiedToolSourceIds.find(it->second->GetSourceId()) != identifiedToolSourceIds.end())
    {
      // this tool has been found and update has been already called with the correct transform
      LOG_TRACE("Tool " << it->second->GetSourceId() << ": found");
      continue;
    }
    LOG_TRACE("Tool " << it->second->GetSourceId() << ": not found");
#ifdef USE_MicronTracker_TIMESTAMPS
    ToolTimeStampedUpdateWithoutFiltering(it->second->GetSourceId(), transformMatrix, TOOL_OUT_OF_VIEW, timeSystemSec, timeSystemSec);
#else
    ToolTimeStampedUpdate(it->second->GetSourceId(), transformMatrix, TOOL_OUT_OF_VIEW, this->FrameNumber, unfilteredTimestamp);
#endif
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::RefreshMarkerTemplates()
{
  std::vector<std::string> vTemplatesName;
  std::vector<std::string> vTemplatesError;

  std::string templateFullPath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(this->TemplateDirectory.c_str());
  LOG_DEBUG("Loading the marker templates from " << templateFullPath);
  if (!vtksys::SystemTools::FileExists(templateFullPath.c_str(), false))
  {
    LOG_WARNING("Unable to find MicronTracker TemplateDirectory at: " << templateFullPath);
  }
  int callResult = this->MT->mtRefreshTemplates(vTemplatesName, vTemplatesError, templateFullPath);
  for (unsigned int i = 0; i < vTemplatesName.size(); i++)
  {
    LOG_DEBUG("Loaded " << vTemplatesName[i]);
  }
  if (callResult != 0)
  {
    LOG_ERROR("Failed to load marker templates from " << templateFullPath);
    for (unsigned int i = 0; i < vTemplatesError.size(); i++)
    {
      LOG_ERROR("Error loading template: " << vTemplatesError[i]);
    }
    return PLUS_FAIL;
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkPlusMicronTracker::GetTransformMatrix(int markerIndex, vtkMatrix4x4* transformMatrix)
{
  std::vector<double> vRotMat;
  this->MT->mtGetRotations(vRotMat, markerIndex);
  std::vector<double> vPos;
  this->MT->mtGetTranslations(vPos, markerIndex);

  transformMatrix->Identity();
  int rotIndex = 0;
  for (int col = 0; col < 3; col++)
  {
    for (int row = 0; row < 3; row++)
    {
      transformMatrix->SetElement(row, col, vRotMat[rotIndex++]);
    }
  }
  // Add the offset to the last column of the transformation matrix
  transformMatrix->SetElement(0, 3, vPos[0]);
  transformMatrix->SetElement(1, 3, vPos[1]);
  transformMatrix->SetElement(2, 3, vPos[2]);
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::GetImage(vtkImageData* leftImage, vtkImageData* rightImage)
{
  PlusLockGuard<vtkPlusRecursiveCriticalSection> updateMutexGuardedLock(this->UpdateMutex);

  unsigned char** leftImageArray = 0;
  unsigned char** rightImageArray = 0;
  if (this->MT->mtGetLeftRightImageArray(leftImageArray, rightImageArray) == -1)
  {
    LOG_ERROR("Error getting images from MicronTracker");
    return PLUS_FAIL;
  }

  int imageWidth = this->MT->mtGetXResolution(-1);
  int imageHeight = this->MT->mtGetYResolution(-1);

  if (leftImage != NULL)
  {
    vtkSmartPointer<vtkImageImport> imageImport = vtkSmartPointer<vtkImageImport>::New();
    imageImport->SetDataScalarTypeToUnsignedChar();
    imageImport->SetImportVoidPointer((unsigned char*)leftImageArray);
    imageImport->SetDataScalarTypeToUnsignedChar();
    imageImport->SetDataExtent(0, imageWidth - 1, 0, imageHeight - 1, 0, 0);
    imageImport->SetWholeExtent(0, imageWidth - 1, 0, imageHeight - 1, 0, 0);
    imageImport->Update();
    leftImage->DeepCopy(imageImport->GetOutput());
  }

  if (rightImage != NULL)
  {
    vtkSmartPointer<vtkImageImport> imageImport = vtkSmartPointer<vtkImageImport>::New();
    imageImport->SetDataScalarTypeToUnsignedChar();
    imageImport->SetImportVoidPointer((unsigned char*)rightImageArray);
    imageImport->SetDataScalarTypeToUnsignedChar();
    imageImport->SetDataExtent(0, imageWidth - 1, 0, imageHeight - 1, 0, 0);
    imageImport->SetWholeExtent(0, imageWidth - 1, 0, imageHeight - 1, 0, 0);
    imageImport->Update();
    rightImage->DeepCopy(imageImport->GetOutput());
  }

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::ReadConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_READING(deviceConfig, rootConfigElement);
  XML_READ_CSTRING_ATTRIBUTE_OPTIONAL(TemplateDirectory, deviceConfig);
  XML_READ_CSTRING_ATTRIBUTE_OPTIONAL(IniFile, deviceConfig);
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::WriteConfiguration(vtkXMLDataElement* rootConfigElement)
{
  XML_FIND_DEVICE_ELEMENT_REQUIRED_FOR_WRITING(trackerConfig, rootConfigElement);

  trackerConfig->SetAttribute("TemplateDirectory", this->TemplateDirectory.c_str());
  trackerConfig->SetAttribute("IniFile", this->IniFile.c_str());

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::InternalConnect()
{
  if (this->IsMicronTrackingInitialized)
  {
    LOG_DEBUG("Already connected to MicronTracker");
    return PLUS_SUCCESS;
  }

  std::string iniFilePath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(this->IniFile);
  LOG_DEBUG("Use MicronTracker ini file: " << iniFilePath);
  if (!vtksys::SystemTools::FileExists(iniFilePath.c_str(), true))
  {
    LOG_WARNING("Unable to find MicronTracker IniFile file at: " << iniFilePath);
  }
  std::string templateFullPath = vtkPlusConfig::GetInstance()->GetDeviceSetConfigurationPath(this->TemplateDirectory.c_str());
  LOG_DEBUG("Loading the marker templates from " << templateFullPath);
  if (!vtksys::SystemTools::FileExists(templateFullPath.c_str(), false))
  {
    LOG_ERROR("Unable to find MicronTracker TemplateDirectory at: " << templateFullPath);
  }

  if (this->MT->mtInit(iniFilePath) != 1)
  {
    LOG_ERROR("Error in initializing Micron Tracker");
    return PLUS_FAIL;
  }

  // Try to attach the cameras till find the cameras
  if (this->MT->mtSetupCameras() != 1)
  {
    LOG_ERROR("Error in initializing Micron Tracker: setup cameras failed. Check the camera connections and INI and Markers file locations.");
    this->MT->mtEnd();
    return PLUS_FAIL;
  }

  int numOfCameras = this->MT->mtGetNumOfCameras();
  if (numOfCameras == 0)
  {
    LOG_ERROR("Error in initializing Micron Tracker: no cameras attached. Check the camera connections and INI and Markers file locations.");
    this->MT->mtEnd();
    return PLUS_FAIL;
  }
  LOG_DEBUG("Number of attached cameras: " << numOfCameras);
  for (int i = 0; i < numOfCameras; i++)
  {
    LOG_DEBUG("Camera " << i << ": "
              << this->MT->mtGetXResolution(i) << "x" << this->MT->mtGetYResolution(i) << ", "
              << this->MT->mtGetNumOfSensors(i) << " sensors "
              << "(serial number: " << this->MT->mtGetSerialNum(i) << ")");
  }

  if (RefreshMarkerTemplates() != PLUS_SUCCESS)
  {
    LOG_ERROR("Error in initializing Micron Tracker: Failed to load marker templates. Check if the marker directory is set correctly.");
    this->MT->mtEnd();
    return PLUS_FAIL;
  }

#ifdef USE_MicronTracker_TIMESTAMPS
  this->TrackerTimeToSystemTimeSec = 0;
  this->TrackerTimeToSystemTimeComputed = false;
#endif

  this->IsMicronTrackingInitialized = 1;

  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
PlusStatus vtkPlusMicronTracker::InternalDisconnect()
{
  if (this->IsMicronTrackingInitialized)
  {
    this->MT->mtEnd();
    this->IsMicronTrackingInitialized = false;
  }
  return PLUS_SUCCESS;
}

//----------------------------------------------------------------------------
void vtkPlusMicronTracker::LogMessageCallback(int level, const char* message, void* /*userdata*/)
{
  switch (level)
  {
    case MicronTrackerLogger::WARNING_LEVEL:
      LOG_WARNING("MicronTracker: " << (message ? message : ""));
      break;
    case MicronTrackerLogger::DEBUG_LEVEL:
      if (message)
      {
        LOG_DEBUG("MicronTracker: " << message);
      }
      break;
    case MicronTrackerLogger::ERROR_LEVEL:
    default:
      LOG_ERROR("MicronTracker: " << (message ? message : ""));
      break;
  }
}
