/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "XBApplicationEx.h"

#include "AppParamParser.h"
#include "FileItem.h"
#include "PlayListPlayer.h"
#include "commons/Exception.h"
#include "messaging/ApplicationMessenger.h"
#include "utils/XTimeUtils.h"
#include "utils/log.h"

CXBApplicationEx::CXBApplicationEx()
{
  // Variables to perform app timing
  m_bStop = false;
  m_AppFocused = true;
  m_ExitCode = EXITCODE_QUIT;
  m_renderGUI = false;
}

CXBApplicationEx::~CXBApplicationEx() = default;

/* Destroy the app */
void CXBApplicationEx::Destroy()
{
  CLog::Log(LOGINFO, "XBApplicationEx: destroying...");
  // Perform app-specific cleanup
  Cleanup();
}

/* Function that runs the application */
int CXBApplicationEx::Run(const CAppParamParser &params)
{
  CLog::Log(LOGINFO, "Running the application...");

  std::chrono::time_point<std::chrono::steady_clock> lastFrameTime;
  std::chrono::milliseconds frameTime;
  const unsigned int noRenderFrameTime = 15;  // Simulates ~66fps

  if (params.GetPlaylist().Size() > 0)
  {
    CServiceBroker::GetPlaylistPlayer().Add(0, params.GetPlaylist());
    CServiceBroker::GetPlaylistPlayer().SetCurrentPlaylist(0);
    KODI::MESSAGING::CApplicationMessenger::GetInstance().PostMsg(TMSG_PLAYLISTPLAYER_PLAY, -1);
  }

  // Run xbmc
  while (!m_bStop)
  {
    //-----------------------------------------
    // Animate and render a frame
    //-----------------------------------------

    lastFrameTime = std::chrono::steady_clock::now();
    Process();

    if (!m_bStop)
    {
      FrameMove(true, m_renderGUI);
    }

    if (m_renderGUI && !m_bStop)
    {
      Render();
    }
    else if (!m_renderGUI)
    {
      auto now = std::chrono::steady_clock::now();
      frameTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFrameTime);
      if (frameTime.count() < noRenderFrameTime)
        KODI::TIME::Sleep(std::chrono::milliseconds(noRenderFrameTime - frameTime.count()));
    }

  }
  Destroy();

  CLog::Log(LOGINFO, "XBApplicationEx: application stopped!");
  return m_ExitCode;
}
