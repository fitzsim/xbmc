/*
 *  Copyright (C) 2012-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "PVRManager.h"

#include "ServiceBroker.h"
#include "guilib/LocalizeStrings.h"
#include "interfaces/AnnouncementManager.h"
#include "messaging/ApplicationMessenger.h"
#include "pvr/PVRDatabase.h"
#include "pvr/PVRPlaybackState.h"
#include "pvr/addons/PVRClient.h"
#include "pvr/addons/PVRClients.h"
#include "pvr/channels/PVRChannel.h"
#include "pvr/channels/PVRChannelGroup.h"
#include "pvr/channels/PVRChannelGroupInternal.h"
#include "pvr/channels/PVRChannelGroups.h"
#include "pvr/channels/PVRChannelGroupsContainer.h"
#include "pvr/epg/EpgInfoTag.h"
#include "pvr/guilib/PVRGUIActions.h"
#include "pvr/guilib/PVRGUIChannelIconUpdater.h"
#include "pvr/guilib/PVRGUIProgressHandler.h"
#include "pvr/guilib/guiinfo/PVRGUIInfo.h"
#include "pvr/providers/PVRProvider.h"
#include "pvr/providers/PVRProviders.h"
#include "pvr/recordings/PVRRecording.h"
#include "pvr/recordings/PVRRecordings.h"
#include "pvr/timers/PVRTimerInfoTag.h"
#include "pvr/timers/PVRTimers.h"
#include "settings/Settings.h"
#include "utils/JobManager.h"
#include "utils/Stopwatch.h"
#include "utils/StringUtils.h"
#include "utils/URIUtils.h"
#include "utils/log.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace PVR;
using namespace KODI::MESSAGING;
using namespace std::chrono_literals;

namespace
{

class CPVRJob
{
public:
  virtual ~CPVRJob() = default;

  virtual bool DoWork() = 0;
  virtual const std::string GetType() const = 0;

protected:
};

template<typename F>
class CPVRLambdaJob : public CPVRJob
{
public:
  CPVRLambdaJob() = delete;
  CPVRLambdaJob(const std::string& type, F&& f) : m_type(type), m_f(std::forward<F>(f)) {}

  bool DoWork() override
  {
    m_f();
    return true;
  }

  const std::string GetType() const override { return m_type; }

private:
  std::string m_type;
  F m_f;
};

} // unnamed namespace

namespace PVR
{

class CPVRManagerJobQueue
{
public:
  CPVRManagerJobQueue() : m_triggerEvent(false) {}

  void Start();
  void Stop();
  void Clear();

  template<typename F>
  void Append(const std::string& type, F&& f)
  {
    AppendJob(new CPVRLambdaJob<F>(type, std::forward<F>(f)));
  }

  void ExecutePendingJobs();

  bool WaitForJobs(unsigned int milliSeconds)
  {
    return m_triggerEvent.Wait(std::chrono::milliseconds(milliSeconds));
  }


private:
  void AppendJob(CPVRJob* job);

  CCriticalSection m_critSection;
  CEvent m_triggerEvent;
  std::vector<CPVRJob*> m_pendingUpdates;
  bool m_bStopped = true;
};

} // namespace PVR

void CPVRManagerJobQueue::Start()
{
  CSingleLock lock(m_critSection);
  m_bStopped = false;
  m_triggerEvent.Set();
}

void CPVRManagerJobQueue::Stop()
{
  CSingleLock lock(m_critSection);
  m_bStopped = true;
  m_triggerEvent.Reset();
}

void CPVRManagerJobQueue::Clear()
{
  CSingleLock lock(m_critSection);
  for (CPVRJob* updateJob : m_pendingUpdates)
    delete updateJob;

  m_pendingUpdates.clear();
  m_triggerEvent.Set();
}

void CPVRManagerJobQueue::AppendJob(CPVRJob* job)
{
  CSingleLock lock(m_critSection);

  // check for another pending job of given type...
  for (CPVRJob* updateJob : m_pendingUpdates)
  {
    if (updateJob->GetType() == job->GetType())
    {
      delete job;
      return;
    }
  }

  m_pendingUpdates.push_back(job);
  m_triggerEvent.Set();
}

void CPVRManagerJobQueue::ExecutePendingJobs()
{
  std::vector<CPVRJob*> pendingUpdates;

  {
    CSingleLock lock(m_critSection);

    if (m_bStopped)
      return;

    pendingUpdates = std::move(m_pendingUpdates);
    m_triggerEvent.Reset();
  }

  CPVRJob* job = nullptr;
  while (!pendingUpdates.empty())
  {
    job = pendingUpdates.front();
    pendingUpdates.erase(pendingUpdates.begin());

    job->DoWork();
    delete job;
  }
}

CPVRManager::CPVRManager()
  : CThread("PVRManager"),
    m_providers(new CPVRProviders),
    m_channelGroups(new CPVRChannelGroupsContainer),
    m_recordings(new CPVRRecordings),
    m_timers(new CPVRTimers),
    m_addons(new CPVRClients),
    m_guiInfo(new CPVRGUIInfo),
    m_guiActions(new CPVRGUIActions),
    m_epgContainer(m_events),
    m_pendingUpdates(new CPVRManagerJobQueue),
    m_database(new CPVRDatabase),
    m_parentalTimer(new CStopWatch),
    m_playbackState(new CPVRPlaybackState),
    m_settings({CSettings::SETTING_PVRPOWERMANAGEMENT_ENABLED,
                CSettings::SETTING_PVRPOWERMANAGEMENT_SETWAKEUPCMD,
                CSettings::SETTING_PVRPARENTAL_ENABLED, CSettings::SETTING_PVRPARENTAL_DURATION})
{
  CServiceBroker::GetAnnouncementManager()->AddAnnouncer(this);
  m_actionListener.Init(*this);

  CLog::LogFC(LOGDEBUG, LOGPVR, "PVR Manager instance created");
}

CPVRManager::~CPVRManager()
{
  m_actionListener.Deinit(*this);
  CServiceBroker::GetAnnouncementManager()->RemoveAnnouncer(this);

  CLog::LogFC(LOGDEBUG, LOGPVR, "PVR Manager instance destroyed");
}

void CPVRManager::Announce(ANNOUNCEMENT::AnnouncementFlag flag,
                           const std::string& sender,
                           const std::string& message,
                           const CVariant& data)
{
  if (!IsStarted())
    return;

  if ((flag & (ANNOUNCEMENT::GUI)))
  {
    if (message == "OnScreensaverActivated")
      m_addons->OnPowerSavingActivated();
    else if (message == "OnScreensaverDeactivated")
      m_addons->OnPowerSavingDeactivated();
  }
}

std::shared_ptr<CPVRDatabase> CPVRManager::GetTVDatabase() const
{
  CSingleLock lock(m_critSection);
  if (!m_database || !m_database->IsOpen())
    CLog::LogF(LOGERROR, "Failed to open the PVR database");

  return m_database;
}

std::shared_ptr<CPVRProviders> CPVRManager::Providers() const
{
  CSingleLock lock(m_critSection);
  return m_providers;
}

std::shared_ptr<CPVRChannelGroupsContainer> CPVRManager::ChannelGroups() const
{
  CSingleLock lock(m_critSection);
  return m_channelGroups;
}

std::shared_ptr<CPVRRecordings> CPVRManager::Recordings() const
{
  CSingleLock lock(m_critSection);
  return m_recordings;
}

std::shared_ptr<CPVRTimers> CPVRManager::Timers() const
{
  CSingleLock lock(m_critSection);
  return m_timers;
}

std::shared_ptr<CPVRClients> CPVRManager::Clients() const
{
  // note: m_addons is const (only set/reset in ctor/dtor). no need for a lock here.
  return m_addons;
}

std::shared_ptr<CPVRClient> CPVRManager::GetClient(const CFileItem& item) const
{
  int iClientID = PVR_INVALID_CLIENT_ID;

  if (item.HasPVRChannelInfoTag())
    iClientID = item.GetPVRChannelInfoTag()->ClientID();
  else if (item.HasPVRRecordingInfoTag())
    iClientID = item.GetPVRRecordingInfoTag()->m_iClientId;
  else if (item.HasPVRTimerInfoTag())
    iClientID = item.GetPVRTimerInfoTag()->m_iClientId;
  else if (item.HasEPGInfoTag())
    iClientID = item.GetEPGInfoTag()->ClientID();
  else if (URIUtils::IsPVRChannel(item.GetPath()))
  {
    const std::shared_ptr<CPVRChannel> channel = m_channelGroups->GetByPath(item.GetPath());
    if (channel)
      iClientID = channel->ClientID();
  }
  else if (URIUtils::IsPVRRecording(item.GetPath()))
  {
    const std::shared_ptr<CPVRRecording> recording = m_recordings->GetByPath(item.GetPath());
    if (recording)
      iClientID = recording->ClientID();
  }
  return GetClient(iClientID);
}

std::shared_ptr<CPVRClient> CPVRManager::GetClient(int iClientId) const
{
  std::shared_ptr<CPVRClient> client;
  if (iClientId != PVR_INVALID_CLIENT_ID)
    m_addons->GetCreatedClient(iClientId, client);

  return client;
}

std::shared_ptr<CPVRGUIActions> CPVRManager::GUIActions() const
{
  // note: m_guiActions is const (only set/reset in ctor/dtor). no need for a lock here.
  return m_guiActions;
}

std::shared_ptr<CPVRPlaybackState> CPVRManager::PlaybackState() const
{
  // note: m_playbackState is const (only set/reset in ctor/dtor). no need for a lock here.
  return m_playbackState;
}

CPVREpgContainer& CPVRManager::EpgContainer()
{
  // note: m_epgContainer is const (only set/reset in ctor/dtor). no need for a lock here.
  return m_epgContainer;
}

void CPVRManager::Clear()
{
  m_playbackState->Clear();
  m_pendingUpdates->Clear();

  CSingleLock lock(m_critSection);

  m_guiInfo.reset();
  m_timers.reset();
  m_recordings.reset();
  m_providers.reset();
  m_channelGroups.reset();
  m_parentalTimer.reset();
  m_database.reset();

  m_bEpgsCreated = false;
}

void CPVRManager::ResetProperties()
{
  CSingleLock lock(m_critSection);
  Clear();

  m_database.reset(new CPVRDatabase);
  m_providers.reset(new CPVRProviders);
  m_channelGroups.reset(new CPVRChannelGroupsContainer);
  m_recordings.reset(new CPVRRecordings);
  m_timers.reset(new CPVRTimers);
  m_guiInfo.reset(new CPVRGUIInfo);
  m_parentalTimer.reset(new CStopWatch);
}

void CPVRManager::Init()
{
  // initial check for enabled addons
  // if at least one pvr addon is enabled, PVRManager start up
  CJobManager::GetInstance().Submit([this] {
    Clients()->Start();
    return true;
  });
}

void CPVRManager::Start()
{
  CSingleLock initLock(m_startStopMutex);

  // Prevent concurrent starts
  if (IsInitialising())
    return;

  // Note: Stop() must not be called while holding pvr manager's mutex. Stop() calls
  // StopThread() which can deadlock if the worker thread tries to acquire pvr manager's
  // lock while StopThread() is waiting for the worker to exit. Thus, we introduce another
  // lock here (m_startStopMutex), which only gets hold while starting/restarting pvr manager.
  Stop(true);

  if (!m_addons->HasCreatedClients())
    return;

  CLog::Log(LOGINFO, "PVR Manager: Starting");
  SetState(ManagerState::STATE_STARTING);

  /* create the pvrmanager thread, which will ensure that all data will be loaded */
  Create();
  SetPriority(ThreadPriority::BELOW_NORMAL);
}

void CPVRManager::Stop(bool bRestart /* = false */)
{
  CSingleLock initLock(m_startStopMutex);

  // Prevent concurrent stops
  if (IsStopped())
    return;

  /* stop playback if needed */
  if (!bRestart && m_playbackState->IsPlaying())
  {
    CLog::LogFC(LOGDEBUG, LOGPVR, "Stopping PVR playback");
    CApplicationMessenger::GetInstance().SendMsg(TMSG_MEDIA_STOP);
  }

  CLog::Log(LOGINFO, "PVR Manager: Stopping");
  SetState(ManagerState::STATE_SSTOPPING);

  StopThread();
}

void CPVRManager::Unload()
{
  // stop pvr manager thread and clear all pvr data
  Stop();
  Clear();
}

void CPVRManager::Deinit()
{
  SetWakeupCommand();
  Unload();

  // release addons
  m_addons.reset();
}

CPVRManager::ManagerState CPVRManager::GetState() const
{
  CSingleLock lock(m_managerStateMutex);
  return m_managerState;
}

void CPVRManager::SetState(CPVRManager::ManagerState state)
{
  {
    CSingleLock lock(m_managerStateMutex);
    if (m_managerState == state)
      return;

    m_managerState = state;
  }

  PVREvent event;
  switch (state)
  {
    case ManagerState::STATE_ERROR:
      event = PVREvent::ManagerError;
      break;
    case ManagerState::STATE_STOPPED:
      event = PVREvent::ManagerStopped;
      break;
    case ManagerState::STATE_STARTING:
      event = PVREvent::ManagerStarting;
      break;
    case ManagerState::STATE_SSTOPPING:
      event = PVREvent::ManagerStopped;
      break;
    case ManagerState::STATE_INTERRUPTED:
      event = PVREvent::ManagerInterrupted;
      break;
    case ManagerState::STATE_STARTED:
      event = PVREvent::ManagerStarted;
      break;
    default:
      return;
  }

  PublishEvent(event);
}

void CPVRManager::PublishEvent(PVREvent event)
{
  m_events.Publish(event);
}

void CPVRManager::Process()
{
  m_addons->Continue();
  m_database->Open();

  if (!IsInitialising())
  {
    CLog::Log(LOGINFO, "PVR Manager: Start aborted");
    return;
  }

  UnloadComponents();

  if (!IsInitialising())
  {
    CLog::Log(LOGINFO, "PVR Manager: Start aborted");
    return;
  }

  // Wait for at least one client to come up and load/update data
  std::vector<std::shared_ptr<CPVRClient>> knownClients;
  UpdateComponents(knownClients, ManagerState::STATE_STARTING);

  if (!IsInitialising())
  {
    CLog::Log(LOGINFO, "PVR Manager: Start aborted");
    return;
  }

  // Load EPGs from database.
  m_epgContainer.Load();

  // Reinit playbackstate
  m_playbackState->ReInit();

  m_guiInfo->Start();
  m_epgContainer.Start();
  m_timers->Start();
  m_pendingUpdates->Start();

  SetState(ManagerState::STATE_STARTED);
  CLog::Log(LOGINFO, "PVR Manager: Started");

  bool bRestart(false);
  XbmcThreads::EndTime<> cachedImagesCleanupTimeout(30s); // first timeout after 30 secs

  while (IsStarted() && m_addons->HasCreatedClients() && !bRestart)
  {
    // In case any new client connected, load from db and fetch data update from new client(s)
    UpdateComponents(knownClients, ManagerState::STATE_STARTED);

    if (cachedImagesCleanupTimeout.IsTimePast())
    {
      // We don't know for sure what to delete if there are not (yet) connected clients
      if (m_addons->HasIgnoredClients())
      {
        cachedImagesCleanupTimeout.Set(10s); // try again in 10 secs
      }
      else
      {
        // start a job to erase stale texture db entries and image files
        TriggerCleanupCachedImages();
        cachedImagesCleanupTimeout.Set(12h); // following timeouts after 12 hours
      }
    }

    /* first startup */
    if (m_bFirstStart)
    {
      {
        CSingleLock lock(m_critSection);
        m_bFirstStart = false;
      }

      /* start job to search for missing channel icons */
      TriggerSearchMissingChannelIcons();

      /* try to play channel on startup */
      TriggerPlayChannelOnStartup();
    }

    if (m_addons->AnyClientSupportingRecordingsSize())
      TriggerRecordingsSizeInProgressUpdate();

    /* execute the next pending jobs if there are any */
    try
    {
      m_pendingUpdates->ExecutePendingJobs();
    }
    catch (...)
    {
      CLog::LogF(LOGERROR, "An error occurred while trying to execute the last PVR update job, trying to recover");
      bRestart = true;
    }

    if (IsStarted() && !bRestart)
      m_pendingUpdates->WaitForJobs(1000);
  }

  m_addons->Stop();
  m_pendingUpdates->Stop();
  m_timers->Stop();
  m_epgContainer.Stop();
  m_guiInfo->Stop();

  SetState(ManagerState::STATE_INTERRUPTED);

  UnloadComponents();
  m_database->Close();

  ResetProperties();

  CLog::Log(LOGINFO, "PVR Manager: Stopped");
  SetState(ManagerState::STATE_STOPPED);
}

bool CPVRManager::SetWakeupCommand()
{
#if !defined(TARGET_DARWIN_EMBEDDED) && !defined(TARGET_WINDOWS_STORE)
  if (!m_settings.GetBoolValue(CSettings::SETTING_PVRPOWERMANAGEMENT_ENABLED))
    return false;

  const std::string strWakeupCommand(m_settings.GetStringValue(CSettings::SETTING_PVRPOWERMANAGEMENT_SETWAKEUPCMD));
  if (!strWakeupCommand.empty() && m_timers)
  {
    time_t iWakeupTime;
    const CDateTime nextEvent = m_timers->GetNextEventTime();
    if (nextEvent.IsValid())
    {
      nextEvent.GetAsTime(iWakeupTime);

      std::string strExecCommand = StringUtils::Format("{} {}", strWakeupCommand, iWakeupTime);

      const int iReturn = system(strExecCommand.c_str());
      if (iReturn != 0)
        CLog::LogF(LOGERROR, "PVR Manager failed to execute wakeup command '{}': {} ({})",
                   strExecCommand, strerror(iReturn), iReturn);

      return iReturn == 0;
    }
  }
#endif
  return false;
}

void CPVRManager::OnSleep()
{
  PublishEvent(PVREvent::SystemSleep);

  SetWakeupCommand();

  m_epgContainer.OnSystemSleep();

  m_addons->OnSystemSleep();
}

void CPVRManager::OnWake()
{
  m_addons->OnSystemWake();

  m_epgContainer.OnSystemWake();

  PublishEvent(PVREvent::SystemWake);

  /* start job to search for missing channel icons */
  TriggerSearchMissingChannelIcons();

  /* try to play channel on startup */
  TriggerPlayChannelOnStartup();

  /* trigger PVR data updates */
  TriggerChannelGroupsUpdate();
  TriggerProvidersUpdate();
  TriggerChannelsUpdate();
  TriggerRecordingsUpdate();
  TriggerEpgsCreate();
  TriggerTimersUpdate();
}

void CPVRManager::UpdateComponents(std::vector<std::shared_ptr<CPVRClient>>& knownClients,
                                   ManagerState stateToCheck)
{
  XbmcThreads::EndTime<> progressTimeout(30s);
  std::unique_ptr<CPVRGUIProgressHandler> progressHandler(
      new CPVRGUIProgressHandler(g_localizeStrings.Get(19235))); // PVR manager is starting up

  // Wait for at least one client to come up and load/update data
  while (!UpdateComponents(knownClients, stateToCheck, progressHandler) &&
         m_addons->HasCreatedClients() && (stateToCheck == GetState()))
  {
    CThread::Sleep(1000ms);

    if (progressTimeout.IsTimePast())
      progressHandler.reset();
  }
}

bool CPVRManager::UpdateComponents(std::vector<std::shared_ptr<CPVRClient>>& knownClients,
                                   ManagerState stateToCheck,
                                   const std::unique_ptr<CPVRGUIProgressHandler>& progressHandler)
{
  // find clients which disappeared since last check and remove them from known clients
  for (auto it = knownClients.begin(); it != knownClients.end();)
  {
    if ((*it)->IgnoreClient())
    {
      it = knownClients.erase(it);
      CLog::LogFC(LOGDEBUG, LOGPVR,
                  "PVR client '{}' disconnected. Erasing from list of known clients.", (*it)->ID());
    }
    else
      ++it;
  }

  // find clients which appeared since last check and update them
  CPVRClientMap clientMap;
  m_addons->GetCreatedClients(clientMap);
  if (clientMap.empty())
  {
    CLog::LogFC(LOGDEBUG, LOGPVR, "All created PVR clients gone!");
    return false;
  }

  std::vector<std::shared_ptr<CPVRClient>> newClients;
  for (const auto& entry : clientMap)
  {
    // skip not (yet) connected clients
    if (entry.second->IgnoreClient())
    {
      CLog::LogFC(LOGDEBUG, LOGPVR, "Skipping not (yet) connected PVR client '{}'",
                  entry.second->ID());
      continue;
    }

    if (knownClients.empty() || std::find_if(knownClients.cbegin(), knownClients.cend(),
                                             [&entry](const std::shared_ptr<CPVRClient>& client) {
                                               return entry.first == client->GetID();
                                             }) == knownClients.cend())
    {
      knownClients.emplace_back(entry.second);
      newClients.emplace_back(entry.second);

      CLog::LogFC(LOGDEBUG, LOGPVR, "Adding new PVR client '{}' to list of known clients",
                  entry.second->ID());
    }
  }

  if (newClients.empty())
    return !knownClients.empty();

  // Load all channels and groups
  if (progressHandler)
    progressHandler->UpdateProgress(g_localizeStrings.Get(19236), 0); // Loading channels and groups

  if (!m_providers->Update(newClients) || (stateToCheck != GetState()))
  {
    CLog::LogF(LOGERROR, "Failed to load PVR providers.");
    knownClients.clear(); // start over
    return false;
  }

  if (!m_channelGroups->Update(newClients) || (stateToCheck != GetState()))
  {
    CLog::LogF(LOGERROR, "Failed to load PVR channels / groups.");
    knownClients.clear(); // start over
    return false;
  }

  PublishEvent(PVREvent::ChannelGroupsLoaded);

  // Load all timers
  if (progressHandler)
    progressHandler->UpdateProgress(g_localizeStrings.Get(19237), 50); // Loading timers

  if (!m_timers->Update(newClients) || (stateToCheck != GetState()))
  {
    CLog::LogF(LOGERROR, "Failed to load PVR timers.");
    knownClients.clear(); // start over
    return false;
  }

  // Load all recordings
  if (progressHandler)
    progressHandler->UpdateProgress(g_localizeStrings.Get(19238), 75); // Loading recordings

  if (!m_recordings->Update(newClients) || (stateToCheck != GetState()))
  {
    CLog::LogF(LOGERROR, "Failed to load PVR recordings.");
    knownClients.clear(); // start over
    return false;
  }

  return true;
}

void CPVRManager::UnloadComponents()
{
  m_recordings->Unload();
  m_timers->Unload();
  m_channelGroups->Unload();
  m_providers->Unload();
  m_epgContainer.Unload();
}

void CPVRManager::TriggerPlayChannelOnStartup()
{
  if (IsStarted())
  {
    CJobManager::GetInstance().Submit([this] {
      return GUIActions()->PlayChannelOnStartup();
    });
  }
}

void CPVRManager::RestartParentalTimer()
{
  if (m_parentalTimer)
    m_parentalTimer->StartZero();
}

bool CPVRManager::IsParentalLocked(const std::shared_ptr<CPVREpgInfoTag>& epgTag) const
{
  return m_channelGroups &&
         epgTag &&
         IsCurrentlyParentalLocked(m_channelGroups->GetByUniqueID(epgTag->UniqueChannelID(), epgTag->ClientID()),
                                   epgTag->IsParentalLocked());
}

bool CPVRManager::IsParentalLocked(const std::shared_ptr<CPVRChannel>& channel) const
{
  return channel &&
         IsCurrentlyParentalLocked(channel, channel->IsLocked());
}

bool CPVRManager::IsCurrentlyParentalLocked(const std::shared_ptr<CPVRChannel>& channel, bool bGenerallyLocked) const
{
  bool bReturn = false;

  if (!channel || !bGenerallyLocked)
    return bReturn;

  const std::shared_ptr<CPVRChannel> currentChannel = m_playbackState->GetPlayingChannel();

  if (// if channel in question is currently playing it must be currently unlocked.
      (!currentChannel || channel != currentChannel) &&
      // parental control enabled
      m_settings.GetBoolValue(CSettings::SETTING_PVRPARENTAL_ENABLED))
  {
    float parentalDurationMs = m_settings.GetIntValue(CSettings::SETTING_PVRPARENTAL_DURATION) * 1000.0f;
    bReturn = m_parentalTimer &&
        (!m_parentalTimer->IsRunning() ||
          m_parentalTimer->GetElapsedMilliseconds() > parentalDurationMs);
  }

  return bReturn;
}

void CPVRManager::OnPlaybackStarted(const CFileItemPtr& item)
{
  m_playbackState->OnPlaybackStarted(item);
  m_guiActions->OnPlaybackStarted(item);
  m_epgContainer.OnPlaybackStarted();
}

void CPVRManager::OnPlaybackStopped(const CFileItemPtr& item)
{
  // Playback ended due to user interaction
  if (m_playbackState->OnPlaybackStopped(item))
    PublishEvent(PVREvent::ChannelPlaybackStopped);

  m_guiActions->OnPlaybackStopped(item);
  m_epgContainer.OnPlaybackStopped();
}

void CPVRManager::OnPlaybackEnded(const CFileItemPtr& item)
{
  // Playback ended, but not due to user interaction
  OnPlaybackStopped(item);
}

void CPVRManager::LocalizationChanged()
{
  CSingleLock lock(m_critSection);
  if (IsStarted())
  {
    static_cast<CPVRChannelGroupInternal *>(m_channelGroups->GetGroupAllRadio().get())->CheckGroupName();
    static_cast<CPVRChannelGroupInternal *>(m_channelGroups->GetGroupAllTV().get())->CheckGroupName();
  }
}

bool CPVRManager::EpgsCreated() const
{
  CSingleLock lock(m_critSection);
  return m_bEpgsCreated;
}

void CPVRManager::TriggerEpgsCreate()
{
  m_pendingUpdates->Append("pvr-create-epgs", [this]() {
    return CreateChannelEpgs();
  });
}

void CPVRManager::TriggerRecordingsSizeInProgressUpdate()
{
  m_pendingUpdates->Append("pvr-update-recordings-size", [this]() {
    return Recordings()->UpdateInProgressSize();
  });
}

void CPVRManager::TriggerRecordingsUpdate(int clientId)
{
  m_pendingUpdates->Append("pvr-update-recordings-" + std::to_string(clientId), [this, clientId]() {
    const std::shared_ptr<CPVRClient> client = GetClient(clientId);
    if (client)
      Recordings()->UpdateFromClients({client});
  });
}

void CPVRManager::TriggerRecordingsUpdate()
{
  m_pendingUpdates->Append("pvr-update-recordings",
                           [this]() { Recordings()->UpdateFromClients({}); });
}

void CPVRManager::TriggerTimersUpdate(int clientId)
{
  m_pendingUpdates->Append("pvr-update-timers-" + std::to_string(clientId), [this, clientId]() {
    const std::shared_ptr<CPVRClient> client = GetClient(clientId);
    if (client)
      Timers()->UpdateFromClients({client});
  });
}

void CPVRManager::TriggerTimersUpdate()
{
  m_pendingUpdates->Append("pvr-update-timers", [this]() { Timers()->UpdateFromClients({}); });
}

void CPVRManager::TriggerProvidersUpdate(int clientId)
{
  m_pendingUpdates->Append("pvr-update-channel-providers-" + std::to_string(clientId),
                           [this, clientId]() {
                             const std::shared_ptr<CPVRClient> client = GetClient(clientId);
                             if (client)
                               Providers()->UpdateFromClients({client});
                           });
}

void CPVRManager::TriggerProvidersUpdate()
{
  m_pendingUpdates->Append("pvr-update-channel-providers",
                           [this]() { Providers()->UpdateFromClients({}); });
}

void CPVRManager::TriggerChannelsUpdate(int clientId)
{
  m_pendingUpdates->Append("pvr-update-channels-" + std::to_string(clientId), [this, clientId]() {
    const std::shared_ptr<CPVRClient> client = GetClient(clientId);
    if (client)
      ChannelGroups()->UpdateFromClients({client}, true);
  });
}

void CPVRManager::TriggerChannelsUpdate()
{
  m_pendingUpdates->Append("pvr-update-channels",
                           [this]() { ChannelGroups()->UpdateFromClients({}, true); });
}

void CPVRManager::TriggerChannelGroupsUpdate(int clientId)
{
  m_pendingUpdates->Append("pvr-update-channelgroups-" + std::to_string(clientId),
                           [this, clientId]() {
                             const std::shared_ptr<CPVRClient> client = GetClient(clientId);
                             if (client)
                               ChannelGroups()->UpdateFromClients({client}, false);
                           });
}

void CPVRManager::TriggerChannelGroupsUpdate()
{
  m_pendingUpdates->Append("pvr-update-channelgroups",
                           [this]() { ChannelGroups()->UpdateFromClients({}, false); });
}

void CPVRManager::TriggerSearchMissingChannelIcons()
{
  m_pendingUpdates->Append("pvr-search-missing-channel-icons", [this]() {
    CPVRGUIChannelIconUpdater updater(
        {ChannelGroups()->GetGroupAllTV(), ChannelGroups()->GetGroupAllRadio()}, true);
    updater.SearchAndUpdateMissingChannelIcons();
    return true;
  });
}

void CPVRManager::TriggerSearchMissingChannelIcons(const std::shared_ptr<CPVRChannelGroup>& group)
{
  m_pendingUpdates->Append("pvr-search-missing-channel-icons-" + std::to_string(group->GroupID()),
                           [group]() {
                             CPVRGUIChannelIconUpdater updater({group}, false);
                             updater.SearchAndUpdateMissingChannelIcons();
                             return true;
                           });
}

void CPVRManager::TriggerCleanupCachedImages()
{
  m_pendingUpdates->Append("pvr-cleanup-cached-images", [this]() {
    int iCleanedImages = 0;
    CLog::Log(LOGINFO, "PVR Manager: Starting cleanup of cached images.");
    iCleanedImages += Recordings()->CleanupCachedImages();
    iCleanedImages += ChannelGroups()->CleanupCachedImages();
    iCleanedImages += Providers()->CleanupCachedImages();
    iCleanedImages += EpgContainer().CleanupCachedImages();
    CLog::Log(LOGINFO, "PVR Manager: Cleaned up {} cached images.", iCleanedImages);
    return true;
  });
}

void CPVRManager::ConnectionStateChange(CPVRClient* client,
                                        const std::string& connectString,
                                        PVR_CONNECTION_STATE state,
                                        const std::string& message)
{
  CJobManager::GetInstance().Submit([this, client, connectString, state, message] {
    Clients()->ConnectionStateChange(client, connectString, state, message);
    return true;
  });
}

bool CPVRManager::CreateChannelEpgs()
{
  if (EpgsCreated())
    return true;

  bool bEpgsCreated = m_channelGroups->CreateChannelEpgs();

  CSingleLock lock(m_critSection);
  m_bEpgsCreated = bEpgsCreated;
  return m_bEpgsCreated;
}
